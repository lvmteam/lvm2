%import macros.inc

##############################################################
# Defaults (rawhide)... 

%global enable_profiling 0
%global enable_testsuite 1
%global enable_dbusd 1
%global enable_udev 1
%global enable_systemd 1
%global enable_cmirror 1
%global enable_lvmlockd 1
%global enable_lvmpolld 1
%global enable_dmfilemapd 1
%global enable_lvmlockd_dlm 0
%global enable_lvmlockd_sanlock 0

%with default-event-activation %{lvm2_testsuite_build}

%if %{enable_udev}
%service lvmpolld 1
%endif

########################################################
# Normally clustering is maintained via resource agents
#
# enable service only if you know what you are doing
#
%if %{enable_cmirror}
#service cmirrord 1
%endif

%global dlm_version 3.99.1-1
%global corosync_version 1.99.9-1

%global buildreq_cluster corosync-devel >= %{corosync_version}, dlm-devel >= %{dlm_version}
%global req_cluster corosync >= %{corosync_version}, dlm >= %{dlm_version}

%with cache internal
%with thin internal
%with thin_check %{_sbindir}/thin_check
%with thin_repair %{_sbindir}/thin_repair
%with thin_dump %{_sbindir}/thin_dump
# TODO disable vdo and writecache for older releases
%with vdo internal
%with vdo-format %{_bindir}/vdoformat
%with writecache internal
%with integrity internal

# NOTE: Required by fedora 38+, maybe older
%global buildreq_udev systemd-devel, systemd-units
%global req_udev udev >= 181-1


%if %{fedora} >= 35 || %{rhel} >= 9
  %global enable_readline 0
  %global enable_editline 1
%else
  %global enable_readline 1
  %global enable_editline 0
%endif

%if %{fedora} >= 27 || %{rhel} >= 7
 %service lvmlockd 1
# For RH7 require 7.4 for sanlock
 %if %{rhel} > 7 || 0%{?rhel_minor_version} > 4
   %global enable_lvmlockd_sanlock 1
 %endif
 %global sanlock_version 3.7.0-1
 %if %{rhel} <= 9
   %global enable_lvmlockd_dlm 1
 %endif
 # FIXME: Check these conditions against RHEL builds!
 %if %{rhel}
  %ifnarch i686 x86_64 s390x
   %global enable_lvmlockd_dlm 0
  %endif
  %ifnarch x86_64 ppc64le ppc64 aarch64
   %global enable_lvmlockd_sanlock 0
  %endif
 %endif
%else
 %if %{fedora} >= 22
  %service lvmlockd 1
  %global enable_lvmlockd_dlm 1
 %endif
%endif
%if %{enable_lvmlockd_dlm}
 %global buildreq_lvmlockd_dlm dlm-devel >= %{dlm_version}
%endif
%if %{enable_lvmlockd_sanlock}
 %global buildreq_lvmlockd_sanlock sanlock-devel >= %{sanlock_version}
%endif

##############################################################

%if %{rhel} < 7 && %{fedora} < 25
  %global enable_dmfilemapd 0
%endif

##############################################################

%if %{rhel} < 8 && %{fedora} < 23
  %global enable_dbusd 0
%endif

%if %{enable_dbusd}
  %global buildreq_python3_devel python3-devel
  %global buildreq_python3_setuptools python3-setuptools
  %global buildreq_python3_dbus python3-dbus
  %global buildreq_python3_pyudev python3-pyudev
%endif

##############################################################

%if %{fedora} == 16 || %{rhel} == 6
%global enable_systemd 0

%global buildreq_udev libudev-devel
%global buildreq_cluster openaislib-devel >= 1.1.1-1, clusterlib-devel >= 3.0.6-1, corosynclib-devel >= 1.2.0-1

%global req_udev udev >= 158-1
%global req_cluster openais >= 1.1.1-1, cman >= 3.0.6-1, corosync >= 1.2.0-1

%global _udevbasedir /lib/udev
%global _udevdir %{_udevbasedir}/rules.d
%endif

%if %{fedora} == 16
%with cache none
%with thin none
%with thin_check
%with thin_repair
%with thin_dump
%endif

%if %{rhel} >= 9
%with default-use-devices-file 1
%endif

##############################################################

%if %{fedora} == 17
%global buildreq_udev systemd-devel
%global buildreq_cluster corosync-devel >= 1.99.9-1, dlm-devel >= 3.99.1-1

%global req_udev udev >= 181-1
%global req_dm_persistent device-mapper-persistent-data >= 0.1.4
%global req_cluster corosync >= 1.99.9-1, dlm >= 3.99.2-1
%endif

##############################################################
# same as FC 16 above, only with older udev

%if %{rhel} == 6
%global req_udev udev >= 147-2
%global req_dm_persistent device-mapper-persistent-data >= 0.1.4
%endif

##############################################################

# Do not reset Release to 1 unless both lvm2 and device-mapper
# versions are increased together.

%global device_mapper_version 1.02.97

Summary: Userland logical volume management tools
Name: lvm2
Version: 2.02.120
Release: 1%{?dist}
License: GPLv2
Group: System Environment/Base
URL: http://sources.redhat.com/lvm2
Source0: ftp://sources.redhat.com/pub/lvm2/LVM2.%{version}.tgz
Source91: source.inc
Source92: build.inc
Source93: packages.inc
Source94: macros.inc

BuildRequires: make
BuildRequires: gcc
%if %{enable_testsuite}
BuildRequires: gcc-c++
%endif
BuildRequires: libselinux-devel >= 1.30.19-4, libsepol-devel
%if %{enable_readline}
BuildRequires: ncurses-devel
BuildRequires: readline-devel
%endif
%if %{enable_editline}
BuildRequires: libedit-devel
%endif
BuildRequires: module-init-tools
BuildRequires: pkgconfig
# TODO: Should these be conditional?
BuildRequires: libaio-devel
BuildRequires: libblkid-devel

# Expands to nothing unless at least 2 arguments are given
%define maybe() \
%if %(test -n "%{?2}" && echo 1 || echo 0) \
%* \
%endif
%define ifwith() \
%if %(if echo %{with_flags} | grep -q %1; then echo 1; else echo 0; fi)

%maybe BuildRequires: %{?buildreq_udev}
%maybe BuildRequires: %{?buildreq_cluster}
%maybe BuildRequires: %{?buildreq_lvmlockd_dlm}
%maybe BuildRequires: %{?buildreq_lvmlockd_sanlock}
%maybe BuildRequires: %{?buildreq_python3_devel}
%maybe BuildRequires: %{?buildreq_python3_setuptools}
%maybe BuildRequires: %{?buildreq_python3_dbus}
%maybe BuildRequires: %{?buildreq_python3_pyudev}

%description
LVM2 includes all of the support for handling read/write operations on
physical volumes (hard disks, RAID-Systems, magneto optical, etc.,
multiple devices (MD), see mdadm(8) or even loop devices, see
losetup(8)), creating volume groups (kind of virtual disks) from one
or more physical volumes and creating one or more logical volumes
(kind of logical partitions) in volume groups.
