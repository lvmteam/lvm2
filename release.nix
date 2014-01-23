{ nixpkgs ? <nixpkgs>, lvm2Src, release ? false,
  rawhide32 ? "" , rawhide64 ? "" ,
  fc19_32_updates ? "", fc19_64_updates ? "",
  fc18_32_updates ? "", fc18_64_updates ? "",
  lvm2Nix ? lvm2Src, T ? "" }:

let
  pkgs = import nixpkgs {};
  mkVM = { VM, extras ? [], diskFun, kernel }:
   VM rec {
     inherit kernel;
     name = "lvm2";
     fullName = "LVM2";
     src = jobs.tarball;
     diskImage = diskFun { extraPackages = extras; };
     memSize = 768;
     # fc16 lcov is broken and el6 has none... be creative
     prepareImagePhase = ''
      rpm -Uv ${pkgs.fetchurl {
       url="ftp://ftp.isu.edu.tw/pub/Linux/Fedora/linux/updates/16/i386/lcov-1.9-2.fc16.noarch.rpm";
       sha256 = "0ycdh5mb7p5ll76mqk0p6gpnjskvxxgh3a3bfr1crh94nvpwhp4z"; }}
      dmesg -n 1 # avoid spilling dmesg into the main log, we capture it in harness
     '';
     postBuild = ''
      cd `cat /tmp/build-location`
      mv test/results/list test/results/list-rpm
      rpm -Uvh /tmp/rpmout/RPMS/*/*.rpm # */
      (/usr/lib/systemd/systemd-udevd || /usr/lib/udev/udevd || /sbin/udevd || find / -xdev -name \*udevd) &
      make check_system QUIET=1 T=${T} || touch $out/nix-support/failed
      mv test/results/list test/results/list-system
      cat test/results/list-* > test/results/list
      cp -R test/results $out/test-results && \
          echo "report tests $out/test-results" >> $out/nix-support/hydra-build-products || \
          true
      make lcov || true
      cp -R lcov_reports $out/coverage && \
          echo "report coverage $out/coverage" >> $out/nix-support/hydra-build-products || \
          true # not really fatal, although kinda disappointing
     '';
   };

  rootmods = [ "virtio_pci" "virtio_blk" "virtio_balloon" "ext4" "unix"
               "cifs" "virtio_net" "unix" "hmac" "md4" "ecb" "des_generic" "sha256" ];

  centos_url = ver: arch: if ver == "6.5"
       then "http://ftp.fi.muni.cz/pub/linux/centos/${ver}/os/${arch}/"
       else "http://vault.centos.org/${ver}/os/${arch}/";
  fedora_url = ver: arch: if pkgs.lib.eqStrings ver "rawhide" || pkgs.lib.eqStrings ver "19"
                       then "ftp://ftp.fi.muni.cz/pub/linux/fedora/linux/development/${ver}/${arch}/os/"
                       else "mirror://fedora/linux/releases/${ver}/Everything/${arch}/os/";
  fedora_update_url = ver: arch: "mirror://fedora/linux/updates/${ver}/${arch}";
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
        unifiedSystemDir = true;
      };
      rawhide = version: arch: repodata: import (pkgs.runCommand "rawhide-${version}-${arch}.nix" {} ''
        sha=$(grep primary.xml ${repodata} | sed -re 's:.* ([0-9a-f]+)-primary.*:\1:')
        echo '{fedora}: fedora { version = "${version}"; sha = "'$sha'"; arch = "${arch}"; }' > $out
      '') { inherit fedora; };
      update = version: arch: repodata: orig: orig // (import (pkgs.runCommand "updates-fedora.nix" {} ''
          sha=$(grep primary.xml ${repodata} | sed -re 's:.* ([0-9a-f]+)-primary.*:\1:')
          echo fedora ${version} updates sha: $sha
          (echo 'fetchurl: orig: { packagesLists = [ orig.packagesList ('
           echo "fetchurl { "
           echo "  url = \"${fedora_update_url version arch}/repodata/$sha-primary.xml.gz\";"
           echo "  sha256 = \"$sha\";"
           echo '} ) ]; urlPrefixes = [ orig.urlPrefix "${fedora_update_url version arch}" ]; }'
          ) > $out
          echo built $out 1>&2
        '')) pkgs.fetchurl orig;
    in {
      rawhidex86_64 = rawhide "rawhide" "x86_64" rawhide64;
      rawhidei386 = rawhide "rawhide" "i386" rawhide32;
      fedora19ux86_64 = update "19" "x86_64" fc19_64_updates pkgs.vmTools.rpmDistros.fedora19x86_64;
      fedora19ui386 = update "19" "i386" fc19_32_updates pkgs.vmTools.rpmDistros.fedora19i386;
      fedora18ux86_64 = update "18" "x86_64" fc18_64_updates pkgs.vmTools.rpmDistros.fedora18x86_64;
      fedora18ui386 = update "18" "i386" fc18_32_updates pkgs.vmTools.rpmDistros.fedora18i386;

      #centos63x86_64 = centos {
      #  version="6.3"; arch="x86_64";
      #  sha="4d3cddf382e81c20b167a8d13c7c92067040a1947dbb3c29cfafa01a74a26a2b";
      #};

      #centos63i386 = centos {
      #  version="6.3"; arch="i386";
      #  sha="5cee0e0c4d7e2dcb997f123ce9107dedbc424d80dd7f2b2471b3b348f3e1754c";
      #};

      centos64x86_64 = centos {
        version="6.4"; arch="x86_64";
        sha="4d4030b92f010f466eb4f004312b9f532b9e85e60c5e6421e8b429c180ac1efe";
      };

      centos64i386 = centos {
        version="6.4"; arch="i386";
        sha="87aa4c4e19f9a3ec93e3d820f1ea6b6ece8810cb45f117a16354465e57a1b50d";
      };

      centos65i386 = centos {
        version="6.5"; arch="i386";
        sha="a89f27cc7d3cea431f3bd605a1e9309c32d5d409abc1b51a7b5c71c05f18a0c2";
      };

      centos65x86_64 = centos {
        version="6.5"; arch="x86_64";
        sha="3353e378f5cb4bb6c3b3dd2ca266c6d68a1e29c36cf99f76aea3d8e158626024";
      };
    };

  vm = pkgs: xmods: with pkgs.lib; rec {
    tools = import "${nixpkgs}/pkgs/build-support/vm/default.nix" {
      inherit pkgs; rootModules = rootmods ++ xmods ++
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
      centos65 = centos64;
      fedora16 = [ "clusterlib-devel" "openaislib-devel" "cman" "systemd-devel" "libudev-devel" ];
      fedora17 = [ "dlm-devel" "corosynclib-devel" "device-mapper-persistent-data"
                   "dlm" "systemd-devel" "perl-Digest-MD5" "libudev-devel" ];
      fedora18 = [ "dlm-devel" "corosynclib-devel" "device-mapper-persistent-data"
                   "dlm" "systemd-devel" "perl-Digest-MD5" ];
      fedora18u = fedora18;
      fedora19 = [ "dlm-devel" "dlm" "corosynclib-devel" "perl-Digest-MD5" "systemd-devel" "procps-ng" ];
      fedora19u = fedora19;
      rawhide = fedora19;
    };

  mkRPM = { arch, image }: with pkgs.lib;
    let use = vm (if eqStrings arch "i386" then pkgs.pkgsi686Linux else pkgs)
                 (if image == "centos64" || image == "centos65" then [] else [ "9p" "9pnet_virtio" ]);
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
      autoconfPhase = ":";
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
         echo "make check QUIET=1 T=${T} || touch \$out/nix-support/failed \\"
	 echo "pwd > /tmp/build-location \\"
	 echo "touch rpm-no-clean") >> source.inc
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

    fc18u_x86_64 = mkRPM { arch = "x86_64"; image = "fedora18u"; };
    fc18u_i386   = mkRPM { arch = "i386"; image = "fedora18u"; };
    fc19u_x86_64 = mkRPM { arch = "x86_64"; image = "fedora19u"; };
    fc19u_i386   = mkRPM { arch = "i386"; image = "fedora19u"; };

    #centos63_i386 = mkRPM { arch = "i386"  ; image = "centos63"; };
    #centos63_x86_64 = mkRPM { arch = "x86_64" ; image = "centos63"; };
    centos64_i386 = mkRPM { arch = "i386"  ; image = "centos64"; };
    centos64_x86_64 = mkRPM { arch = "x86_64" ; image = "centos64"; };
    centos65_i386 = mkRPM { arch = "i386"  ; image = "centos65"; };
    centos65_x86_64 = mkRPM { arch = "x86_64" ; image = "centos65"; };

    rawhide_i386 = mkRPM { arch = "i386"  ; image = "rawhide"; };
    rawhide_x86_64 = mkRPM { arch = "x86_64" ; image = "rawhide"; };
  };
in jobs
