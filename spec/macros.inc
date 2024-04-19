%global _default_pid_dir /run
%global _default_dm_run_dir /run
%global _default_run_dir /run/lvm
%global _default_locking_dir /run/lock/lvm
%global _udevbasedir %{_prefix}/lib/udev
%global _udevdir %{_udevbasedir}/rules.d
%global _tmpfilesdir %{_prefix}/lib/tmpfiles.d

%if !0%{?fedora}
%global fedora 0
%endif

%if !0%{?rhel}
%global rhel 0
%endif

%global enableif() \
%%global configure_flags %%{?configure_flags} --%%(if test %%1 -gt 0; then echo enable-%%2; else echo disable-%%2; fi)

%global with() \
%%global configure_flags %%(echo -n "%%{?configure_flags} " | sed -e "s,--with-%%1=[^ ]*,,"; test -n "%%{?2}" && echo --with-%%1=%%2) \
%%global with_flags %%(echo -n "%%{?with_flags} " | sed -e "s,%%1,,"; test -n "%%{?2}" && test "%%{?2}" != none && echo %%1)

%global services monitor
%global service() \
%%global services %%(echo -n "%%{?services} " | sed -e s,%%1,,; test "%%2" = 1 && echo %%1)

%global maybe() \
%%if %%(test -n "%%{?2}" && echo 1 || echo 0) \
%%* \
%%endif

%global have_with() %%(if echo %%{with_flags} | grep -q %%1; then echo 1; else echo 0; fi)
%global have_service() %%(if echo %%{services} | grep -q %%1; then echo 1; else echo 0; fi)

%global daemon_reload \
%%if %%{enable_systemd} \
systemctl daemon-reload > /dev/null 2>&1 || : \
%%endif \
: \
%%{nil}

%global enable(s:t:) \
%%if %%{have_service %%{-s*}} \
%%if %%{enable_systemd} \
if [ $1 = 1 ]; then \
	systemctl preset lvm2-%%{-s*}.%%{-t*} > /dev/null 2>&1 || : \
fi \
%%else \
/sbin/chkconfig --add lvm2-%%{-s*} \
%%endif \
%%endif \
: \
%%{nil}

%global disable(s:t:) \
%%if %%{have_service %%{-s*}} \
%%if %%{enable_systemd} \
if [ $1 = 0 ]; then \
	systemctl --no-reload disable lvm2-%%{-s*}.%%{-t*} > /dev/null 2>&1 || : \
	%%if "%%{-t*}" == "socket" \
	systemctl --no-reload disable lvm2-%%{-s*}.service > /dev/null 2>&1 || : \
	%%endif \
	systemctl stop lvm2-%%{-s*}.%%{-t*} > /dev/null 2>&1 || : \
	%%if "%%{-t*}" == "socket" \
	systemctl stop lvm2-%%{-s*}.service > /dev/null 2>&1 || : \
	%%endif \
fi \
%%else \
/sbin/chkconfig --del lvm2-%%{-s*} \
%%endif \
%%endif \
: \
%%{nil}

%global try_restart(s:t:) \
%%if %%{have_service %%{-s*}} && %%{enable_systemd} \
if [ $1 = 1 ]; then \
	systemctl try-restart lvm2-%%{-s*}.%%{-t*} > /dev/null 2>&1 || : \
fi \
%%endif \
: \
%%{nil}

# Resolve RHEL minor version from redhat-release
%if 0%{?rhel} > 0
%define rhel_minor_version %(rhel_version_file="/etc/redhat-release";
if  [ -f "$rhel_version_file" ]; then cat "$rhel_version_file" |
cut -f2 -d. | cut -f1 -d' '; else echo 0; fi)
%endif

#############################################################
#
# Disable event_activation when creating packages for system
# with installed lvm2-testsuite
#
# This is used for internal package testing to avoid
# updating lvm.conf after test suite installation.
#
%define lvm2_testsuite_build %( test -d /usr/share/lvm2-testsuite && echo 0 || echo 1 )
