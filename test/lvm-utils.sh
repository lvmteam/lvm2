# Put lvm-related utilities here.
# This file is sourced from test-lib.sh.

export LVM_SUPPRESS_FD_WARNINGS=1

ME=$(basename "$0")
warn() { echo >&2 "$ME: $@"; }


unsafe_losetup_()
{
  f=$1
  # Prefer the race-free losetup from recent util-linux-ng.
  dev=$(losetup --find --show "$f" 2>/dev/null) \
      && { echo "$dev"; return 0; }

  # If that fails, try to use util-linux-ng's -f "find-device" option.
  dev=$(losetup -f 2>/dev/null) \
      && losetup "$dev" "$f" \
      && { echo "$dev"; return 0; }

  # Last resort: iterate through /dev/loop{,/}{0,1,2,3,4,5,6,7,8,9}
  for slash in '' /; do
    for i in 0 1 2 3 4 5 6 7 8 9; do
      dev=/dev/loop$slash$i
      losetup $dev > /dev/null 2>&1 && continue;
      losetup "$dev" "$f" > /dev/null && { echo "$dev"; return 0; }
      break
    done
  done

  return 1
}

loop_setup_()
{
  file=$1
  dd if=/dev/zero of="$file" bs=1M count=1 seek=1000 > /dev/null 2>&1 \
    || { warn "loop_setup_ failed: Unable to create tmp file $file"; return 1; }

  # NOTE: this requires a new enough version of losetup
  dev=$(unsafe_losetup_ "$file" 2>/dev/null) \
    || { warn "loop_setup_ failed: Unable to create loopback device"; return 1; }

  echo "$dev"
  return 0;
}
