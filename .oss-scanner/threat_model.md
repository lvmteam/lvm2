# Threat model (LVM2 userspace)

## What this project does

LVM2 is the userspace stack for Linux Logical Volume Manager and device-mapper
control: command-line tools (`lvm`, `pv*`, `vg*`, `lv*`), `libdevmapper`,
metadata read/write on disk, activation/deactivation via ioctl to `/dev/mapper`,
and daemons (`dmeventd`, `lvmpolld`, optional `lvmlockd` when built).

Untrusted input enters primarily as:

- On-disk LVM metadata (PV/VG/LV labels, text and binary formats).
- Device nodes and sysfs/block layer data when scanning PVs.
- Configuration files under `etc/lvm` (often root-controlled; treat hostile
  config as in scope when not root-only).
- IPC: D-Bus (when enabled), daemon sockets, `lvmpolld` protocol.
- Environment and command-line arguments to CLI tools and daemons.

Kernel device-mapper targets (RAID, thin, cache, etc.) live in the Linux kernel;
this repository contains userspace management and monitoring for those features.

## Components that matter most

High priority for review:

- Metadata parsing and validation (`lib/metadata`, `lib/format_text`, related).
- `libdm` ioctl helpers and device name handling.
- Privilege boundaries: tools run as root on typical systems; any path that
  allows less-privileged callers to influence root-owned operations.
- Daemon code: `dmeventd`, `lvmpolld`, D-Bus service code.
- Setuid/setgid or file creation modes on device nodes (see `configure`
  device uid/gid options).

Medium priority:

- Reporting, filtering, and display code that may mishandle untrusted strings.
- Optional segment modules (thin, cache, raid, vdo userspace helpers).

Lower priority / usually out of scope for this tree:

- `test/shell` integration harness (requires root, dm, loop devices; not run
  in the OSS Scanner image).
- `test/cluster` VM-based tests.
- Vendored or external trees not shipped in the default configure used by
  `.oss-scanner/Dockerfile`.
- Linux kernel `drivers/md` (use kernel security process separately).

## How to exercise it

After build (see `.oss-scanner/Dockerfile`):

- `make run-unit-test` runs `test/unit/unit-test` (parsing, data structures,
  some daemon plugin logic with mocks).
- Built binaries under the build tree: `tools/lvm`, `libdm/dmsetup`, daemons
  under `daemons/`.
- Fuzzing targets: metadata dumps, `pvck`/`vgck` on crafted images, config
  parser inputs.

Full `make check` / `test/shell` needs the lvmtest framework, device-mapper
in the host kernel, and root; do not expect that suite inside the scanner VM.

## Severity guidance

Use CVSS-style impact on a typical server where root may run LVM:

- **Critical**: Unauthenticated remote code execution (unusual for this stack);
  or local unprivileged to root code execution via default-installed setuid
  paths or always-on services without admin action.
- **High**: Memory corruption with plausible control in metadata or libdm paths
  when processing untrusted storage (e.g. USB disk, multipath LUN, image mount)
  as root; or authenticated escape from confined helper to full root.
- **Medium**: Denial of service (crash/hang) on crafted metadata; bugs requiring
  existing root; information leaks of sensitive metadata.
- **Low**: Bugs only in optional features disabled in the scanner Dockerfile;
  test-only code; clearly theoretical issues without a plausible trigger on
  supported distros.

Buffer overflows, use-after-free, or double-free in metadata or ioctl marshalling
are at least **high** when root processes the input; raise to **critical** if
exploitability to arbitrary code execution is demonstrated.

DoS via malformed on-disk metadata is typically **medium** unless it affects
default boot or initramfs paths without admin interaction.

## Out of scope / noise to avoid

- Issues that require loading attacker-controlled kernel modules or modifying
  the kernel DM target implementations (not in this repo).
- Warnings from static analysis on generated or autotools output under `autom4te.cache`.
- Test-only helpers under `test/` except when the same code ships in production
  binaries.

## Reports and patches

Prefer minimal patches consistent with existing LVM2 style (C, tabs, K&R braces,
ASCII-only). Upstream: https://gitlab.com/lvmteam/lvm2 and
https://github.com/lvmteam/lvm2. Include reproducer commands and, for metadata
bugs, a small binary or text fixture where possible.
