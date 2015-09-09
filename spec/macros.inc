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

%define enableif() \
%global configure_flags %{?configure_flags} --%(if test %1 -gt 0; then echo enable-%2; else echo disable-%2; fi)

%define with() \
%global configure_flags %(echo -n "%{?configure_flags} " | sed -e "s,--with-%1=[^ ]*,,"; test -n "%{?2}" && echo --with-%1=%2) \
%global with_flags %(echo -n "%{?with_flags} " | sed -e "s,%1,,"; test -n "%{?2}" && test "%{?2}" != none && echo %1)

%global services monitor
%define service() \
%global services %(echo -n "%{?services} " | sed -e s,%1,,; test "%2" = 1 && echo %1)

%define maybe() \
%if %(test -n "%{?2}" && echo 1 || echo 0) \
%* \
%endif

%define have_with() %(if echo %{with_flags} | grep -q %1; then echo 1; else echo 0; fi)
%define have_service() %(if echo %{services} | grep -q %1; then echo 1; else echo 0; fi)

%define daemon_reload \
%if %{enable_systemd} \
systemctl daemon-reload > /dev/null 2>&1 || : \
%endif \
: \
%{nil}

%define enable(s:t:) \
%if %{have_service %{-s*}} \
%if %{enable_systemd} \
if [ $1 = 1 ]; then \
	systemctl preset lvm2-%{-s*}.%{-t*} > /dev/null 2>&1 || : \
fi \
%else \
/sbin/chkconfig --add lvm2-%{-s*} \
%endif \
%endif \
: \
%{nil}

%define disable(s:t:) \
%if %{have_service %{-s*}} \
%if %{enable_systemd} \
if [ $1 = 0 ]; then \
	systemctl --no-reload disable lvm2-%{-s*}.%{-t*} > /dev/null 2>&1 || : \
	%if %{-t*} == socket \
	systemctl --no-reload disable lvm2-%{-s*}.service > /dev/null 2>&1 || : \
	%endif \
	systemctl stop lvm2-%{-s*}.%{-t*} > /dev/null 2>&1 || : \
	%if %{-t*} == socket \
	systemctl stop lvm2-%{-s*}.service > /dev/null 2>&1 || : \
	%endif \
fi \
%else \
/sbin/chkconfig --del lvm2-%{-s*} \
%endif \
%endif \
: \
%{nil}

%define try_restart(s:t:) \
%if %{have_service %{-s*}} && %{enable_systemd} \
if [ $1 = 1 ]; then \
	systemctl try-restart lvm2-%{-s*}.%{-t*} > /dev/null 2>&1 || : \
fi \
%endif \
: \
%{nil}
