%define import() %include %(test -e %{S:%1} && echo %{S:%1} || echo %{_sourcedir}/%1)

%import source.inc

# PatchN: nnn.patch goes here

%prep
%setup -q -n LVM2.%{version}

%import build.inc
%import packages.inc

%changelog
