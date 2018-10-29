%with default-dm-run-dir %{_default_dm_run_dir}
%with default-run-dir %{_default_run_dir}
%with default-pid-dir %{_default_pid_dir}
%with default-locking-dir %{_default_locking_dir}
%with usrlibdir %{_libdir}
%enableif 1 lvm1_fallback
%enableif 1 fsadm
%with pool internal
%with user
%with group
%with device-uid 0
%with device-gid 6
%with device-mode 0660
%enableif 1 pkgconfig
%enableif 1 applib
%enableif 1 cmdlib
%enableif 1 dmeventd
%enableif 1 write_install

%with udevdir %{_udevdir}
%enableif %{enable_cmirror} cmirrord
%enableif %{enable_udev} udev_sync
%enableif %{enable_profiling} profiling
%global enable_lvmetad %(if echo %{services} | grep -q lvmetad; then echo 1; else echo 0; fi)
%enableif %{enable_lvmetad} lvmetad
%global enable_lvmpolld %(if echo %{services} | grep -q lvmpolld; then echo 1; else echo 0; fi)
%enableif %{enable_lvmpolld} lvmpolld
%global enable_lvmlockd %(if echo %{services} | grep -q lvmlockd; then echo 1; else echo 0; fi)
%if %{enable_lvmlockd}
%enableif %{enable_lvmlockd_dlm} lvmlockd-dlm
%enableif %{enable_lvmlockd_sanlock} lvmlockd-sanlock
%endif
%enableif %{enable_python} python2-bindings
%enableif %{enable_python3} python3-bindings
# Must use this, or applib will be enabled and disabled depending in python[23] availability
%if %{enable_python3} || %{enable_python}
%enableif 1 applib
%endif
%enableif %{enable_dbusd} dbus-service
%enableif %{enable_dbusd} notify-dbus
%enableif %{enable_dmfilemapd} dmfilemapd

%build

%configure %{configure_flags}

make %{?_smp_mflags}
%{?extra_build_commands}

%install
make install DESTDIR=$RPM_BUILD_ROOT
make install_system_dirs DESTDIR=$RPM_BUILD_ROOT
%if %{enable_systemd}
make install_systemd_units DESTDIR=$RPM_BUILD_ROOT
make install_systemd_generators DESTDIR=$RPM_BUILD_ROOT
make install_tmpfiles_configuration DESTDIR=$RPM_BUILD_ROOT
%else
make install_initscripts DESTDIR=$RPM_BUILD_ROOT
%endif
%if %{enable_testsuite}
make -C test install DESTDIR=$RPM_BUILD_ROOT
%endif

# when building an src.rpm from freestanding specfiles
test -e %{_sourcedir}/source.inc || cp source.inc build.inc packages.inc macros.inc %{_sourcedir}

%check
%{?check_commands}

%clean
pwd
test -n rpm-no-clean || rm -rf $RPM_BUILD_ROOT
