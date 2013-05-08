%import macros.inc

##############################################################
# Defaults (rawhide)... 

%global enable_profiling 0
%global enable_udev 1
%global enable_systemd 1
%global enable_cmirror 1

%global buildreq_cluster corosync-devel >= 1.99.9-1, dlm-devel >= 3.99.1-1
%global req_cluster corosync >= 1.99.9-1, dlm >= 3.99.2-1
%with clvmd corosync

# TODO %global req_dm_persistent device-mapper-persistent-data >= 0.1.4
%with thin internal
%with thin_check %{_sbindir}/thin_check

%global buildreq_udev systemd-devel
%global req_udev udev >= 181-1

%service lvmetad 1

##############################################################

%if %{fedora} == 16 || %{rhel} == 6
%global enable_systemd 0

%global buildreq_udev libudev-devel
%global buildreq_cluster openaislib-devel >= 1.1.1-1, clusterlib-devel >= 3.0.6-1, corosynclib-devel >= 1.2.0-1

%global req_udev udev >= 158-1
%global req_cluster openais >= 1.1.1-1, cman >= 3.0.6-1, corosync >= 1.2.0-1
%with thin
%with thin_check
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
%define req_udev udev >= 147-2
%endif

##############################################################

# Do not reset Release to 1 unless both lvm2 and device-mapper
# versions are increased together.

%define device_mapper_version 1.02.75

Summary: Userland logical volume management tools
Name: lvm2
Version: 2.02.96
Release: 4%{?dist}
License: GPLv2
Group: System Environment/Base
URL: http://sources.redhat.com/lvm2
Source0: ftp://sources.redhat.com/pub/lvm2/LVM2.%{version}.tgz
Source91: source.inc
Source92: build.inc
Source93: packages.inc
Source94: macros.inc

BuildRequires: libselinux-devel >= 1.30.19-4, libsepol-devel
BuildRequires: ncurses-devel
BuildRequires: readline-devel
BuildRequires: module-init-tools
BuildRequires: pkgconfig

# Expands to nothing unless at least 2 arguments are given
%define maybe() \
%if %(test -n "%{?2}" && echo 1 || echo 0) \
%* \
%endif
%define ifwith() \
%if %(if echo %{with_flags} | grep -q %1; then echo 1; else echo 0; fi)

%maybe BuildRequires: %{?buildreq_udev}
%maybe BuildRequires: %{?buildreq_cluster}

%description
LVM2 includes all of the support for handling read/write operations on
physical volumes (hard disks, RAID-Systems, magneto optical, etc.,
multiple devices (MD), see mdadd(8) or even loop devices, see
losetup(8)), creating volume groups (kind of virtual disks) from one
or more physical volumes and creating one or more logical volumes
(kind of logical partitions) in volume groups.
