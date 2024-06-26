### MAIN PACKAGE (lvm2)

%post
%daemon_reload
%enable -s monitor -t service
%if %{have_service lvmpolld}
%enable -s lvmpolld -t socket
%endif

%preun
%if %{have_service lvmpolld}
%disable -s lvmpolld -t socket
%endif
%disable -s monitor -t service

%postun
%try_restart -s monitor -t service
%if %{have_service lvmpolld}
%try_restart -s lvmpolld -t service
%endif
if [ $1 = 0 ]; then
%daemon_reload
fi

%triggerun -- %{name} < 2.02.86-2
%{_bindir}/systemd-sysv-convert --save lvm2-monitor >/dev/null 2>&1 || :
/bin/systemctl --no-reload enable lvm2-monitor.service > /dev/null 2>&1 || :
/sbin/chkconfig --del lvm2-monitor > /dev/null 2>&1 || :
/bin/systemctl try-restart lvm2-monitor.service > /dev/null 2>&1 || :
# files in the main package

%files
%defattr(-,root,root,-)
%doc COPYING COPYING.LIB INSTALL README VERSION WHATS_NEW
%doc doc/lvm_fault_handling.txt
%{_sbindir}/fsadm
%{_libexecdir}/lvresize_fs_helper
%{_sbindir}/lvchange
%{_sbindir}/lvconvert
%{_sbindir}/lvcreate
%{_sbindir}/lvdisplay
%{_sbindir}/lvextend
%{_sbindir}/lvm
%{_sbindir}/lvmconfig
%{_sbindir}/lvmdevices
%{_sbindir}/lvmdiskscan
%{_sbindir}/lvmdump
%{_sbindir}/lvmsadc
%{_sbindir}/lvmsar
%{_sbindir}/lvreduce
%{_sbindir}/lvremove
%{_sbindir}/lvrename
%{_sbindir}/lvresize
%{_sbindir}/lvs
%{_sbindir}/lvscan
%{_sbindir}/pvchange
%{_sbindir}/pvck
%{_sbindir}/pvcreate
%{_sbindir}/pvdisplay
%{_sbindir}/pvmove
%{_sbindir}/pvremove
%{_sbindir}/pvresize
%{_sbindir}/pvs
%{_sbindir}/pvscan
%if %{have_with vdo}
%{_sbindir}/lvm_import_vdo
%endif
%{_sbindir}/vgcfgbackup
%{_sbindir}/vgcfgrestore
%{_sbindir}/vgchange
%{_sbindir}/vgck
%{_sbindir}/vgconvert
%{_sbindir}/vgcreate
%{_sbindir}/vgdisplay
%{_sbindir}/vgexport
%{_sbindir}/vgextend
%{_sbindir}/vgimport
%{_sbindir}/vgimportclone
%{_sbindir}/vgimportdevices
%{_sbindir}/vgmerge
%{_sbindir}/vgmknodes
%{_sbindir}/vgreduce
%{_sbindir}/vgremove
%{_sbindir}/vgrename
%{_sbindir}/vgs
%{_sbindir}/vgscan
%{_sbindir}/vgsplit
%if %{have_service lvmpolld}
 %{_sbindir}/lvmpolld
%endif
%{_mandir}/man5/lvm.conf.5.gz
%{_mandir}/man7/lvmautoactivation.7.gz
%{_mandir}/man7/lvmsystemid.7.gz
%{_mandir}/man7/lvmreport.7.gz
%{_mandir}/man7/lvmraid.7.gz
%{_mandir}/man8/fsadm.8.gz
%{_mandir}/man8/lvchange.8.gz
%{_mandir}/man8/lvconvert.8.gz
%{_mandir}/man8/lvcreate.8.gz
%{_mandir}/man8/lvdisplay.8.gz
%{_mandir}/man8/lvextend.8.gz
%{_mandir}/man8/lvm-config.8.gz
%{_mandir}/man8/lvm-dumpconfig.8.gz
%{_mandir}/man8/lvm.8.gz
%{_mandir}/man8/lvmconfig.8.gz
%{_mandir}/man8/lvmdevices.8.gz
%{_mandir}/man8/lvmdiskscan.8.gz
%{_mandir}/man8/lvmdump.8.gz
%{_mandir}/man8/lvm-fullreport.8.gz
%{_mandir}/man8/lvmsadc.8.gz
%{_mandir}/man8/lvmsar.8.gz
%{_mandir}/man8/lvreduce.8.gz
%{_mandir}/man8/lvremove.8.gz
%{_mandir}/man8/lvrename.8.gz
%{_mandir}/man8/lvresize.8.gz
%{_mandir}/man8/lvs.8.gz
%{_mandir}/man8/lvscan.8.gz
%{_mandir}/man8/pvchange.8.gz
%{_mandir}/man8/pvck.8.gz
%{_mandir}/man8/pvcreate.8.gz
%{_mandir}/man8/pvdisplay.8.gz
%{_mandir}/man8/pvmove.8.gz
%{_mandir}/man8/pvremove.8.gz
%{_mandir}/man8/pvresize.8.gz
%{_mandir}/man8/pvs.8.gz
%{_mandir}/man8/pvscan.8.gz
%if %{have_with vdo}
%{_mandir}/man8/lvm_import_vdo.8.gz
%endif
%{_mandir}/man8/vgcfgbackup.8.gz
%{_mandir}/man8/vgcfgrestore.8.gz
%{_mandir}/man8/vgchange.8.gz
%{_mandir}/man8/vgck.8.gz
%{_mandir}/man8/vgconvert.8.gz
%{_mandir}/man8/vgcreate.8.gz
%{_mandir}/man8/vgdisplay.8.gz
%{_mandir}/man8/vgexport.8.gz
%{_mandir}/man8/vgextend.8.gz
%{_mandir}/man8/vgimport.8.gz
%{_mandir}/man8/vgimportclone.8.gz
%{_mandir}/man8/vgimportdevices.8.gz
%{_mandir}/man8/vgmerge.8.gz
%{_mandir}/man8/vgmknodes.8.gz
%{_mandir}/man8/vgreduce.8.gz
%{_mandir}/man8/vgremove.8.gz
%{_mandir}/man8/vgrename.8.gz
%{_mandir}/man8/vgs.8.gz
%{_mandir}/man8/vgscan.8.gz
%{_mandir}/man8/vgsplit.8.gz
%if %{have_with cache}
  %{_mandir}/man7/lvmcache.7.gz
%endif
%if %{have_with thin}
  %{_mandir}/man7/lvmthin.7.gz
%endif
%if %{have_service lvmpolld}
  %{_mandir}/man8/lvmpolld.8.gz
  %{_mandir}/man8/lvm-lvpoll.8.gz
%endif
%if %{have_with vdo}
  %{_mandir}/man7/lvmvdo.7.gz
%endif
%if %{enable_udev}
 %{_udevdir}/11-dm-lvm.rules
 %{_udevdir}/69-dm-lvm.rules
%endif
%dir %{_sysconfdir}/lvm
%ghost %{_sysconfdir}/lvm/cache/.cache
%attr(644, -, -) %config(noreplace) %verify(not md5 mtime size) %{_sysconfdir}/lvm/lvm.conf
%attr(644, -, -) %config(noreplace) %verify(not md5 mtime size) %{_sysconfdir}/lvm/lvmlocal.conf
%dir %{_sysconfdir}/lvm/profile
%{_sysconfdir}/lvm/profile/command_profile_template.profile
%{_sysconfdir}/lvm/profile/metadata_profile_template.profile
%{_sysconfdir}/lvm/profile/thin-generic.profile
%{_sysconfdir}/lvm/profile/thin-performance.profile
%{_sysconfdir}/lvm/profile/cache-mq.profile
%{_sysconfdir}/lvm/profile/cache-smq.profile
%{_sysconfdir}/lvm/profile/lvmdbusd.profile
%if %{have_with vdo}
 %{_sysconfdir}/lvm/profile/vdo-small.profile
%endif
%dir %{_sysconfdir}/lvm/backup
%dir %{_sysconfdir}/lvm/cache
%dir %{_sysconfdir}/lvm/archive
%dir %{_sysconfdir}/lvm/devices
%dir %{_default_locking_dir}
%dir %{_default_run_dir}
%if %{enable_systemd}
 %{_tmpfilesdir}/%{name}.conf
 %{_unitdir}/blk-availability.service
 %{_unitdir}/lvm2-monitor.service
 %if %{have_service lvmpolld}
  %{_unitdir}/lvm2-lvmpolld.service
  %{_unitdir}/lvm2-lvmpolld.socket
 %endif
 %{_unitdir}/lvm-devices-import.service
 %{_unitdir}/lvm-devices-import.path
%else
 %{_sysconfdir}/rc.d/init.d/blk-availability
 %{_sysconfdir}/rc.d/init.d/lvm2-monitor
 %if %{have_service lvmpolld}
  %{_sysconfdir}/rc.d/init.d/lvm2-lvmpolld
 %endif
%endif

##############################################################################
# Library and Development subpackages
##############################################################################
%package devel
Summary: Development libraries and headers
Group: Development/Libraries
License: LGPLv2
Requires: %{name} = %{version}-%{release}
Requires: device-mapper-devel >= %{device_mapper_version}-%{release}
Requires: device-mapper-event-devel >= %{device_mapper_version}-%{release}
Requires: pkgconfig

%description devel
This package contains files needed to develop applications that use
the lvm2 libraries.

%files devel
%defattr(-,root,root,-)
%{_libdir}/liblvm2cmd.so
%{_includedir}/lvm2cmd.h
%{_libdir}/libdevmapper-event-lvm2.so

%package libs
Summary: Shared libraries for lvm2
License: LGPLv2
Group: System Environment/Libraries
Requires: device-mapper-event >= %{device_mapper_version}-%{release}

%description libs
This package contains shared lvm2 libraries for applications.

%post libs -p /sbin/ldconfig

%postun libs -p /sbin/ldconfig

%files libs
%defattr(-,root,root,-)
%attr(755,root,root) %{_libdir}/liblvm2cmd.so.*
%attr(755,root,root) %{_libdir}/libdevmapper-event-lvm2.so.*
%dir %{_libdir}/device-mapper
%{_libdir}/device-mapper/libdevmapper-event-lvm2mirror.so
%{_libdir}/device-mapper/libdevmapper-event-lvm2snapshot.so
%{_libdir}/device-mapper/libdevmapper-event-lvm2raid.so
%if %{have_with thin}
%{_libdir}/device-mapper/libdevmapper-event-lvm2thin.so
%{_libdir}/libdevmapper-event-lvm2thin.so
%endif
%{_libdir}/libdevmapper-event-lvm2mirror.so
%{_libdir}/libdevmapper-event-lvm2snapshot.so
%{_libdir}/libdevmapper-event-lvm2raid.so
%if %{have_with vdo}
%{_libdir}/device-mapper/libdevmapper-event-lvm2vdo.so
%{_libdir}/libdevmapper-event-lvm2vdo.so
%endif


##############################################################################
# LVM locking daemon
##############################################################################
%if %{have_service lvmlockd}
%package lockd
Summary: LVM locking daemon
Group: System Environment/Base
Requires: lvm2 = %{version}-%{release}
%if %{enable_lvmlockd_dlm}
Requires: dlm-lib >= %{dlm_version}
%endif
%if %{enable_lvmlockd_sanlock}
Requires: sanlock-lib >= %{sanlock_version}
%endif
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

%description lockd
LVM commands use lvmlockd to coordinate access to shared storage.

%post lockd
%daemon_reload
%enable -s lvmlockd -t service

%preun lockd
%disable -s lvmlockd -t service

%postun lockd
if [ $1 = 0 ]; then
%daemon_reload
fi

%files lockd
%{_sbindir}/lvmlockd
%{_sbindir}/lvmlockctl
%{_mandir}/man8/lvmlockd.8.gz
%{_mandir}/man8/lvmlockctl.8.gz
%dir %{_default_locking_dir}
%{_unitdir}/lvmlockd.service
%{_unitdir}/lvmlocks.service
%endif

##############################################################################
# Cluster mirror subpackage
##############################################################################
%if %{enable_cmirror}

%package -n cmirror
Summary: Daemon for device-mapper-based clustered mirrors
Group: System Environment/Base
Requires(post): chkconfig
Requires(preun): chkconfig
Requires: corosync >= %{corosync_version}
Requires: device-mapper >= %{device_mapper_version}-%{release}

%description -n cmirror
Daemon providing device-mapper-based mirrors in a shared-storage cluster.

%post -n cmirror
%daemon_reload
%enable -s cmirrord -t service

%preun -n cmirror
%disable -s cmirrord -t service

%postun -n cmirror
%try_restart -s cmirrord -t service
if [ $1 = 0 ]; then
%daemon_reload
fi

%files -n cmirror
%defattr(-,root,root,-)
%attr(755,root,root) /usr/sbin/cmirrord
%{_mandir}/man8/cmirrord.8.gz
%if %{enable_systemd}
  %{_unitdir}/lvm2-cmirrord.service
%else
  %{_sysconfdir}/rc.d/init.d/cmirrord
%endif

%endif

##############################################################################
# LVM D-Bus daemon
##############################################################################
%if %{enable_dbusd}
%package dbusd
Summary: LVM2 D-Bus daemon
License: GPLv2
Group: System Environment/Base
Requires: lvm2 >= %{version}-%{release}
Requires: dbus
Requires: python3-dbus
Requires: python3-pyudev
Requires: python3-gobject-base
Requires(post): systemd-units >= %{systemd_version}
Requires(preun): systemd-units >= %{systemd_version}
Requires(postun): systemd-units >= %{systemd_version}

%description dbusd

Daemon for access to LVM2 functionality through a D-Bus interface.

%post dbusd
%systemd_post lvm2-lvmdbusd.service

%preun dbusd
%systemd_preun lvm2-lvmdbusd.service

%postun dbusd
%systemd_postun lvm2-lvmdbusd.service

%files dbusd
%defattr(555,root,root,-)
%{_sbindir}/lvmdbusd
%defattr(444,root,root,-)
%{_sysconfdir}/dbus-1/system.d/com.redhat.lvmdbus1.conf
%{_datadir}/dbus-1/system-services/com.redhat.lvmdbus1.service
%{_mandir}/man8/lvmdbusd.8.gz
%{_unitdir}/lvm2-lvmdbusd.service
%{python3_sitelib}/lvmdbusd/*

%endif

##############################################################################
# Testsuite subpackage
##############################################################################
%if %{enable_testsuite}
%package testsuite
Summary: LVM2 Testsuite
License: LGPLv2
Group: Development

%description testsuite
An extensive functional testsuite for LVM2.

%files testsuite
%defattr(-,root,root,-)
%{_datadir}/lvm2-testsuite/
%{_libexecdir}/lvm2-testsuite/
%{_bindir}/lvm2-testsuite
%endif

##############################################################################
# Device-mapper subpackages
##############################################################################
%package -n device-mapper
Summary: Device mapper utility
Version: %{device_mapper_version}
Release: %{release}
License: GPLv2
Group: System Environment/Base
URL: http://sources.redhat.com/dm
Requires: device-mapper-libs = %{device_mapper_version}-%{release}
Requires: util-linux >= 2.15
%maybe Requires: %{req_udev}
%if %{enable_udev}
# We need dracut to install required udev rules if udev_sync
# feature is turned on so we don't lose required notifications.
Conflicts: dracut < 002-18
%endif

%description -n device-mapper
This package contains the supporting userspace utility, dmsetup,
for the kernel device-mapper.

%files -n device-mapper
%defattr(-,root,root,-)
%doc COPYING COPYING.LIB WHATS_NEW_DM VERSION_DM README INSTALL
%{_sbindir}/blkdeactivate
%attr(755,root,root) %{_sbindir}/dmsetup
%{_sbindir}/dmstats
%{_mandir}/man8/blkdeactivate.8.gz
%{_mandir}/man8/dmsetup.8.gz
%{_mandir}/man8/dmstats.8.gz
%if %{enable_dmfilemapd}
%{_sbindir}/dmfilemapd
%{_mandir}/man8/dmfilemapd.8.gz
%endif
%if %{enable_udev}
%doc udev/12-dm-permissions.rules
%dir %{_udevbasedir}
%dir %{_udevdir}
%{_udevdir}/10-dm.rules
%{_udevdir}/13-dm-disk.rules
%{_udevdir}/95-dm-notify.rules
%endif

%package -n device-mapper-devel
Summary: Development libraries and headers for device-mapper
Version: %{device_mapper_version}
Release: %{release}
License: LGPLv2
Group: Development/Libraries
Requires: device-mapper = %{device_mapper_version}-%{release}
Requires: pkgconfig

%description -n device-mapper-devel
This package contains files needed to develop applications that use
the device-mapper libraries.

%files -n device-mapper-devel
%defattr(-,root,root,-)
%{_libdir}/libdevmapper.so
%{_includedir}/libdevmapper.h
%{_libdir}/pkgconfig/devmapper.pc

%package -n device-mapper-libs
Summary: Device-mapper shared library
Version: %{device_mapper_version}
Release: %{release}
License: LGPLv2
Group: System Environment/Libraries
Requires: device-mapper = %{device_mapper_version}-%{release}

%description -n device-mapper-libs
This package contains the device-mapper shared library, libdevmapper.

%post -n device-mapper-libs -p /sbin/ldconfig

%postun -n device-mapper-libs -p /sbin/ldconfig

%files -n device-mapper-libs
%attr(755,root,root) %{_libdir}/libdevmapper.so.*

%package -n device-mapper-event
Summary: Device-mapper event daemon
Group: System Environment/Base
Version: %{device_mapper_version}
Release: %{release}
Requires: device-mapper = %{device_mapper_version}-%{release}
Requires: device-mapper-event-libs = %{device_mapper_version}-%{release}
%if %{enable_systemd}
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units
%endif

%description -n device-mapper-event
This package contains the dmeventd daemon for monitoring the state
of device-mapper devices.

%post -n device-mapper-event
%daemon_reload
%if %{enable_systemd}
systemctl preset dm-event.socket > /dev/null 2>&1 || :
%endif

%preun -n device-mapper-event
%if %{enable_systemd}
if [ $1 = 0 ]; then
	systemctl --no-reload disable dm-event.service dm-event.socket > /dev/null 2>&1 || :
	systemctl stop dm-event.service dm-event.socket> /dev/null 2>&1 || :
fi
%endif

%postun -n device-mapper-event
if [ $1 = 0 ]; then
%daemon_reload
fi

%posttrans -n device-mapper-event
if [ -e %{_default_pid_dir}/dmeventd.pid ]; then
        %{_sbindir}/dmeventd -R || echo "Failed to restart dmeventd daemon. Please, try manual restart."
fi

%files -n device-mapper-event
%defattr(-,root,root,-)
%{_sbindir}/dmeventd
%{_mandir}/man8/dmeventd.8.gz
%if %{enable_systemd}
%{_unitdir}/dm-event.socket
%{_unitdir}/dm-event.service
%endif

%package -n device-mapper-event-libs
Summary: Device-mapper event daemon shared library
Version: %{device_mapper_version}
Release: %{release}
License: LGPLv2
Group: System Environment/Libraries

%description -n device-mapper-event-libs
This package contains the device-mapper event daemon shared library,
libdevmapper-event.

%post -n device-mapper-event-libs -p /sbin/ldconfig

%postun -n device-mapper-event-libs -p /sbin/ldconfig

%files -n device-mapper-event-libs
%attr(755,root,root) %{_libdir}/libdevmapper-event.so.*

%package -n device-mapper-event-devel
Summary: Development libraries and headers for the device-mapper event daemon
Version: %{device_mapper_version}
Release: %{release}
License: LGPLv2
Group: Development/Libraries
Requires: device-mapper-event = %{device_mapper_version}-%{release}
Requires: pkgconfig

%description -n device-mapper-event-devel
This package contains files needed to develop applications that use
the device-mapper event library.

%files -n device-mapper-event-devel
%defattr(-,root,root,-)
%{_libdir}/libdevmapper-event.so
%{_includedir}/libdevmapper-event.h
%{_libdir}/pkgconfig/devmapper-event.pc

