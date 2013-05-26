{ nixpkgs ? <nixpkgs>, lvm2Src, release ? false, rawhide32 ? "" , rawhide64 ? "" , fc19_32 ? "" , fc19_64 ? "", lvm2Nix ? lvm2Src }:

let
  pkgs = import nixpkgs {};
  mkVM = { VM, extras ? [], diskFun, kernel }:
   VM rec {
     inherit kernel;
     name = "lvm2";
     fullName = "LVM2";
     src = jobs.tarball;
     diskImage = diskFun { extraPackages = extras; };
     memSize = 512;
     # fc16 lcov is broken and el6 has none... be creative
     prepareImagePhase = ''
      rpm -Uv ${pkgs.fetchurl {
       url="ftp://fr2.rpmfind.net/linux/fedora/linux/updates/16/i386/lcov-1.9-2.fc16.noarch.rpm";
       sha256 = "0ycdh5mb7p5ll76mqk0p6gpnjskvxxgh3a3bfr1crh94nvpwhp4z"; }}
     '';
     postBuild = ''
      cp -R /tmp/lcov $out/coverage && \
      echo "report coverage $out/coverage" >> $out/nix-support/hydra-build-products || \
      true # not really fatal, although kinda disappointing
     '';
   };

  rootmods = [ "cifs" "virtio_net" "virtio_pci" "virtio_blk" "virtio_balloon" "nls_utf8" "ext2" "ext3"
      "unix" "hmac" "md4" "ecb" "des_generic" ];

  centos_url = ver: arch: "http://ftp.fi.muni.cz/pub/linux/centos/${ver}/os/${arch}/";
  fedora_url = ver: arch: if pkgs.lib.eqStrings ver "rawhide" || pkgs.lib.eqStrings ver "19"
                       then "ftp://ftp.fi.muni.cz/pub/linux/fedora/linux/development/${ver}/${arch}/os/"
                       else "mirror://fedora/linux/releases/${ver}/Everything/${arch}/os/";
  extra_distros = with pkgs.lib; let
      centos = { version, sha, arch }: {
        name = "centos-${version}-${arch}";
        fullName = "CentOS ${version} (${arch})";
        packagesList = pkgs.fetchurl {
          url = centos_url version arch + "repodata/${sha}-primary.xml.gz";
          sha256 = sha;
        };
        urlPrefix = centos_url version arch;
        archs = ["noarch" arch] ++ (if eqStrings arch "i386" then ["i586" "i686"] else []);
        packages = filter (n: !(eqStrings n "fedora-release")) pkgs.vmTools.commonFedoraPackages ++
                     [ "centos-release" ];
      };
      fedora = { version, sha, arch }: rec {
        name = "fedora-${version}-${arch}";
        fullName = "Fedora ${version} (${arch})";
        packagesList = pkgs.fetchurl {
            url = fedora_url version arch + "repodata/${sha}-primary.xml.gz";
            sha256 = sha;
        };
        urlPrefix = fedora_url version arch;
        archs = ["noarch" arch] ++ (if eqStrings arch "i386" then ["i586" "i686"] else []);
        packages = pkgs.vmTools.commonFedoraPackages;
        mergeUsr = true;
      };
      rawhide = version: arch: repodata: import (pkgs.runCommand "rawhide-${version}-${arch}.nix" {} ''
        sha=$(grep primary.xml ${repodata} | sed -re 's:.* ([0-9a-f]+)-primary.*:\1:')
        echo '{fedora}: fedora { version = "${version}"; sha = "'$sha'"; arch = "${arch}"; }' > $out
      '') { inherit fedora; };
    in {
      rawhidex86_64 = rawhide "rawhide" "x86_64" rawhide64;
      rawhidei386 = rawhide "rawhide" "i386" rawhide32;
      fedora19x86_64 = rawhide "19" "x86_64" fc19_64;
      fedora19i386 = rawhide "19" "i386" fc19_32;

      centos63x86_64 = centos {
        version="6.3"; arch="x86_64";
        sha="4d3cddf382e81c20b167a8d13c7c92067040a1947dbb3c29cfafa01a74a26a2b";
      };

      centos63i386 = centos {
        version="6.3"; arch="i386";
        sha="5cee0e0c4d7e2dcb997f123ce9107dedbc424d80dd7f2b2471b3b348f3e1754c";
      };

      centos64x86_64 = centos {
        version="6.4"; arch="x86_64";
        sha="4d4030b92f010f466eb4f004312b9f532b9e85e60c5e6421e8b429c180ac1efe";
      };

      centos64i386 = centos {
        version="6.4"; arch="i386";
        sha="87aa4c4e19f9a3ec93e3d820f1ea6b6ece8810cb45f117a16354465e57a1b50d";
      };
    };

  vm = pkgs: with pkgs.lib; rec {
    tools = import "${nixpkgs}/pkgs/build-support/vm/default.nix" {
      inherit pkgs; rootModules = rootmods ++
        [ "loop" "dm_mod" "dm_snapshot" "dm_mirror" "dm_zero" "dm_raid" "dm_thin_pool" ]; };
    release = import "${nixpkgs}/pkgs/build-support/release/default.nix" {
      pkgs = pkgs // { vmTools = tools; }; };
    imgs = pkgs.vmTools.diskImageFuns //
            mapAttrs (n: a: b: pkgs.vmTools.makeImageFromRPMDist (a // b)) extra_distros;
    rpmdistros = pkgs.vmTools.rpmDistros // extra_distros;
    rpmbuild = release.rpmBuild;
  };

  extra_rpms = rec {
      common = [ "libselinux-devel" "libsepol-devel" "ncurses-devel" "readline-devel"
                 "corosynclib-devel"
                 "redhat-rpm-config" # needed for rpmbuild of lvm
                 "which" "e2fsprogs" # needed for fsadm
                 "perl-GD" # for lcov
               ];
      centos63 = [ "clusterlib-devel" "openaislib-devel" "cman" "libudev-devel" ];
      centos64 = centos63;
      fedora16 = [ "clusterlib-devel" "openaislib-devel" "cman" "systemd-devel" "libudev-devel" ];
      fedora17 = [ "dlm-devel" "corosynclib-devel" "device-mapper-persistent-data"
                   "dlm" "systemd-devel" "perl-Digest-MD5" "libudev-devel" ];
      fedora18 = [ "dlm-devel" "corosynclib-devel" "device-mapper-persistent-data"
                   "dlm" "systemd-devel" "perl-Digest-MD5" ];
      fedora19 = [ "dlm-devel" "dlm" "corosynclib-devel" "perl-Digest-MD5" "systemd-devel" "procps-ng" ];
      rawhide = fedora19;
    };

  mkRPM = { arch, image }: with pkgs.lib;
    let use = if eqStrings arch "i386" then vm pkgs.pkgsi686Linux else vm pkgs;
     in mkVM {
           VM = use.rpmbuild;
           diskFun = builtins.getAttr "${image}${arch}" use.imgs;
           extras = extra_rpms.common ++ builtins.getAttr image extra_rpms;
           kernel = use.tools.makeKernelFromRPMDist (builtins.getAttr "${image}${arch}" use.rpmdistros);
        };

  jobs = rec {
    tarball = pkgs.releaseTools.sourceTarball rec {
      name = "lvm2-tarball";
      versionSuffix = if lvm2Src ? revCount
                         then ".pre${toString lvm2Src.revCount}"
                         else "";
      src = lvm2Src;
      distPhase = ''
        set -x
        make distclean
        version=`cat VERSION | cut "-d(" -f1`${versionSuffix}
        version_dm=`cat VERSION_DM | cut "-d-" -f1`${versionSuffix}
        sed -e s,-git,${versionSuffix}, -i VERSION VERSION_DM
        rm -rf spec; cp -R ${lvm2Nix}/spec/* .
        chmod u+w *
        (echo "%define enable_profiling 1";
         echo "%define check_commands \\";
         echo "make lcov-reset \\";
         echo "dmsetup targets\\";
         echo "make check || touch \$out/nix-support/failed \\"
         echo "make lcov && cp -R lcov_reports /tmp/lcov") >> source.inc
        sed -e "s,\(device_mapper_version\) [0-9.]*$,\1 $version_dm," \
            -e "s,^\(Version:[^0-9%]*\)[0-9.]*$,\1 $version," \
            -e "s,^\(Release:[^0-9%]*\)[0-9.]\+,\1 0.HYDRA," \
            -e "s:%with clvmd corosync:%with clvmd corosync,singlenode:" \
            -i source.inc
        sed -e '/^%changelog/,$d' \
            -i lvm2.spec
        echo "%changelog" >> lvm2.spec;
        echo "* `date +"%a %b %d %Y"` Petr Rockai <prockai@redhat.com> - $version" >> lvm2.spec;
        echo "- AUTOMATED BUILD BY Hydra" >> lvm2.spec
        mkdir ../LVM2.$version
        mv * ../LVM2.$version
        ensureDir $out/tarballs
        cd ..
        tar cvzf $out/tarballs/LVM2.$version.tgz LVM2.$version
      '';
    };

    fc19_x86_64 = mkRPM { arch = "x86_64"; image = "fedora19"; };
    fc19_i386   = mkRPM { arch = "i386"  ; image = "fedora19"; };
    fc18_x86_64 = mkRPM { arch = "x86_64"; image = "fedora18"; };
    fc18_i386   = mkRPM { arch = "i386"  ; image = "fedora18"; };
    fc17_x86_64 = mkRPM { arch = "x86_64"; image = "fedora17"; };
    fc17_i386   = mkRPM { arch = "i386"  ; image = "fedora17"; };
    fc16_x86_64 = mkRPM { arch = "x86_64"; image = "fedora16"; };
    fc16_i386   = mkRPM { arch = "i386"  ; image = "fedora16"; };

    centos63_i386 = mkRPM { arch = "i386"  ; image = "centos63"; };
    centos63_x86_64 = mkRPM { arch = "x86_64" ; image = "centos63"; };
    centos64_i386 = mkRPM { arch = "i386"  ; image = "centos64"; };
    centos64_x86_64 = mkRPM { arch = "x86_64" ; image = "centos64"; };

    rawhide_i386 = mkRPM { arch = "i386"  ; image = "rawhide"; };
    rawhide_x86_64 = mkRPM { arch = "x86_64" ; image = "rawhide"; };
  };
in jobs
