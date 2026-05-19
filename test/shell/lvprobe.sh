#!/bin/sh
# Verify the test runner's accounting columns: perform a few known,
# measurable operations and publish independent ground-truth numbers
# ("TRUTH_<tag> <MiB|SKIP>") that a harness compares against the runner's
# M mem / M in / M out / M ram summary line.
#
#   TRUTH_MEM  K MiB held as an anonymous string inside the direct child
#   TRUTH_RAM  delta of this test's own memory cgroup during the run
#   TRUTH_OUT  K MiB written to a loop device (block-layer write sectors)
#   TRUTH_IN   K MiB read back from that loop device (read sectors)
#   TRUTH_BRD  K MiB written to a fresh brd; must NOT appear in diskstats
#
# ground truth comes from different sources than the runner uses
# (memory.current vs memory.peak; the loop's own /sys/block stat vs the
# summed leaf /proc/diskstats), so the two cannot silently agree.

K=${LVM2_PROBE_MIB:-16}

cgpath=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup 2>/dev/null)
if [ -r "$cgpath/memory.current" ]; then
    # cgroup files count bytes; report kB for the arithmetic below
    cg_kb() { echo $(( $(cat "$cgpath/memory.current") / 1024 )); }
else
    cg_kb() { echo 0; }
fi

ram0=$(cg_kb)

# 1) hold K MiB of anonymous memory inside the runner's direct child
#    (what ru_maxrss / the "M mem" column should report).
#    /dev/zero carries NUL bytes which command substitution would drop,
#    so translate them to plain 'x' before collecting.
bigvar=$(dd if=/dev/zero bs=1M count=$K status=none 2>/dev/null | tr '\0' 'x')
TRUTH_MEM=$(( ${#bigvar} / 1048576 ))

# 2) K MiB file on tmpfs, charged to this test's memory cgroup
tmpf=/dev/shm/lvprobe.$$
dd if=/dev/zero of=$tmpf bs=1M count=$K conv=fdatasync status=none 2>/dev/null

# 3) K MiB of block I/O through a loop device (real block-layer counters)
TRUTH_OUT=SKIP
TRUTH_IN=SKIP
if command -v losetup >/dev/null 2>&1; then
    mkf=/dev/shm/lvprobe_loop.$$
    dd if=/dev/zero of=$mkf bs=1M count=$(( K + 1 )) status=none 2>/dev/null
    dev=$(losetup -f --show "$mkf" 2>/dev/null) || dev=
    if [ -n "$dev" ]; then
        st=/sys/block/${dev#/dev/}/stat
        w0=$(awk '{print $7}' "$st")
        r0=$(awk '{print $3}' "$st")
        dd if=/dev/zero of="$dev" bs=1M count=$K conv=fdatasync status=none 2>/dev/null
        TRUTH_OUT=$(( ( $(awk '{print $7}' "$st") - w0 ) / 2048 ))
        dd if="$dev" of=/dev/null bs=1M count=$K status=none 2>/dev/null
        TRUTH_IN=$(( ( $(awk '{print $3}' "$st") - r0 ) / 2048 ))
        losetup -d "$dev" 2>/dev/null
    fi
fi

# 4) RAM-hosted backend (brd): writes must NOT move /proc/diskstats but
#    must be charged to this test's cgroup; only with a clean brd
TRUTH_BRD=SKIP
if [ ! -e /dev/ram0 ] && command -v modprobe >/dev/null 2>&1; then
    if modprobe brd rd_nr=1 rd_size=$(( K * 1024 + 1024 )) 2>/dev/null; then
        b0=$(cg_kb)
        # O_DIRECT: allocates the brd pages from the issuing process (this
        # cgroup); a buffered write would hand them to the flusher instead
        dd if=/dev/zero of=/dev/ram0 bs=1M count=$K oflag=direct conv=fdatasync status=none 2>/dev/null
        TRUTH_BRD=$(( ( $(cg_kb) - b0 + 512 ) / 1024 ))
        rmmod brd 2>/dev/null
    fi
fi

# capture everything before tearing the fixtures down
ram1=$(cg_kb)
TRUTH_RAM=$(( ( ram1 - ram0 + 512 ) / 1024 ))

echo "TRUTH_MEM $TRUTH_MEM"
echo "TRUTH_RAM $TRUTH_RAM"
echo "TRUTH_OUT $TRUTH_OUT"
echo "TRUTH_IN $TRUTH_IN"
echo "TRUTH_BRD $TRUTH_BRD"

rm -f -- "$tmpf" "$mkf"
exit 0