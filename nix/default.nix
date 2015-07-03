# -*- mode: nix; indent-tabs-mode: nil -*-
{ nixpkgs ? <nixpkgs>, lvm2Src, release ? false,
  rawhide32 ? "" , rawhide64 ? "" ,
  fc20_32_updates ? "", fc20_64_updates ? "",
  fc19_32_updates ? "", fc19_64_updates ? "",
  fc18_32_updates ? "", fc18_64_updates ? "",
  T ? "", ENV ? "", timeout ? 60,
  overrides ? { pkgs }: { install_rpms = {}; distros = {}; configs = {}; } }:

let
  pkgs = import nixpkgs {};
  lib = pkgs.lib;
  over = overrides { inherit pkgs; };
  install_lcov = ''
     rpm -Uv ${pkgs.fetchurl {
        url = "http://archives.fedoraproject.org/pub/archive/fedora/linux/updates/16/i386/lcov-1.9-2.fc16.noarch.rpm";
        sha256 = "0ycdh5mb7p5ll76mqk0p6gpnjskvxxgh3a3bfr1crh94nvpwhp4z"; }}
  '';

  mkTest = args: pkgs.stdenv.mkDerivation rec {
     name = "lvm2-test-${(args.diskFun {}).name}";

     builder = pkgs.writeScript "lvm2-collect-results" ''
       #!${pkgs.bash}/bin/bash
       . $stdenv/setup
       mkdir -p $out/test-results
       for i in ${lib.concatStringsSep " " buildInputs}; do
         cat $i/test-results/list >> $out/test-results/list
         cp $i/test-results'/'*.txt $out/test-results/ || true
       done
       mkdir -p $out/nix-support
       grep '\<failed\>' $out/test-results/list && touch $out/nix-support/failed || true
     '';

     buildInputs = map (x: runTest (args // { flavour = x; }))
       [ "ndev-vanilla" "ndev-lvmetad" "ndev-cluster" "udev-vanilla" "udev-lvmetad" "udev-cluster" ];
  };

  runTest = { build, diskFun, extras ? [], kernel, vmtools, flavour, ... }: pkgs.stdenv.mkDerivation rec {
     diskImage = diskFun { extraPackages = extras; };
     name = "lvm2-test-${diskImage.name}-${flavour}";

     # this is the builder that runs in the guest
     origBuilder = pkgs.writeScript "vm-test-guest" ''
         #!/bin/bash
         export PATH=/usr/bin:/bin:/usr/sbin:/sbin

         # we always run in a fresh image, so need to install everything again
         ls ${build}/rpms/*/*.rpm | grep -v sysvinit | xargs rpm -Uv --oldpackage # */
         ${install_lcov}

         mkdir -p /xchg/results
         touch /xchg/booted

         dmsetup targets

         export LVM_TEST_BACKING_DEVICE=/dev/sdb
         ulimit -c unlimited

         watch=
         if echo ${flavour} | grep -q udev; then
           (/usr/lib/systemd/systemd-udevd || /usr/lib/udev/udevd || /sbin/udevd || \
            find / -xdev -name \*udevd) >> /xchg/udevd.log 2>&1 &
           watch="--watch /xchg/udevd.log"
         fi

         export ${ENV}
         lvm2-testsuite --batch --outdir /xchg/results --continue \
             --timeout ${toString timeout} --fatal-timeouts --heartbeat /xchg/heartbeat \
             --flavours ${flavour} $watch --kmsg ${if lib.eqStrings T "" then "" else "--only ${T}"}

         # TODO: coverage reports
         # make lcov || true
         # cp -R lcov_reports $out/coverage && \
         #     echo "report coverage $out/coverage" >> $out/nix-support/hydra-build-products || \
         #     true # not really fatal, although kinda disappointing
     '';

     buildInputs = [ pkgs.coreutils pkgs.bash pkgs.utillinux ];

     # make a qcow copy of the main image
     preVM = ''
       diskImage=$(pwd)/disk-image.qcow2
       origImage=${diskImage}
       if test -d "$origImage"; then origImage="$origImage/disk-image.qcow2"; fi
       ${vmtools.qemu}/bin/qemu-img create -b "$origImage" -f qcow2 $diskImage
     '';

     builder = pkgs.writeScript "vm-test" ''
       #!${pkgs.bash}/bin/bash
       . $stdenv/setup

       export QEMU_OPTS="-drive file=/dev/shm/testdisk.img,if=ide -m 256M"
       export QEMU_DRIVE_OPTS=",if=ide"
       export KERNEL_OPTS="log_buf_len=131072 loglevel=1"
       export mountDisk=1

       mkdir -p $out/test-results $out/nix-support
       touch $out/nix-support/failed

       monitor() {
           set +e
           counter=0
           rm -f j.current j.last t.current t.last
           while true; do
               if ! test -f pid; then
                   counter=0
                   sleep 60
                   continue
               fi

               cat xchg/results/journal > j.current 2> /dev/null
               cat xchg/heartbeat > hb.current 2> /dev/null
               if diff j.current j.last >& /dev/null; then
                   counter=$(($counter + 1));
               else
                   counter=0
               fi
               if test $counter -eq 10 || test $(wc -c <hb.current) -eq $(wc -c <hb.last); then
                   echo
                   echo "VM got stuck; heartbeat: $(wc -c <hb.current) $(wc -c <hb.last), counter = $counter."
                   echo "last journal entry: $(tail -n 1 j.current), previously $(tail -n 1 j.last)"
                   kill -- -$(cat pid)
               fi
               sleep 60
               mv j.current j.last >& /dev/null
               mv hb.current hb.last >& /dev/null
           done
       }

       monitor &

       for i in `seq 1 20`; do # we allow up to 20 VM restarts
           rm -f xchg/booted
           ${vmtools.qemu}/bin/qemu-img create -f qcow2 /dev/shm/testdisk.img 4G
           setsid bash -e ${vmtools.vmRunCommand (vmtools.qemuCommandLinux kernel)} &
           pid=$!

           # give the VM some time to get up and running
           slept=0
           while test $slept -le 180 && test ! -e xchg/booted; do
               sleep 10
               slept=$(($slept + 10))
           done
           echo $pid > pid # monitor go
           wait $pid || true
           rm -f pid # disarm the monitor process

           # if we have any new results, stash them
           mv xchg/results'/'*.txt $out/test-results/ || true

           if test -n "$(cat xchg/in-vm-exit)"; then # the VM is done
               test 0 -eq "$(cat xchg/in-vm-exit)" && rm -f $out/nix-support/failed
               break
           fi

           sleep 10 # wait for the VM to clean up before starting up a new one
       done

       cat xchg/results/list > $out/test-results/list || true
     '';
  };

  mkTarball = profiling: pkgs.releaseTools.sourceTarball rec {
    name = "lvm2-tarball";
    versionSuffix = if lvm2Src ? revCount
                       then ".pre${toString lvm2Src.revCount}"
                       else "";
    src = lvm2Src;
    autoconfPhase = ":";
    distPhase = ''
      make distclean

      version=`cat VERSION | cut "-d(" -f1`${versionSuffix}
      version_dm=`cat VERSION_DM | cut "-d-" -f1`${versionSuffix}

      chmod u+w *

      # set up versions
      sed -e s,-git,${versionSuffix}, -i VERSION VERSION_DM
      sed -e "s,\(device_mapper_version\) [0-9.]*$,\1 $version_dm," \
          -e "s,^\(Version:[^0-9%]*\)[0-9.]*$,\1 $version," \
          -e "s,^\(Release:[^0-9%]*\)[0-9.]\+,\1 0.HYDRA," \
          -i spec/source.inc

      # tweak RPM configuration
      echo   "%define enable_profiling ${profiling}" >> spec/source.inc
      echo   "%define enable_testsuite 1" >> spec/source.inc
      sed -e "s:%with clvmd corosync:%with clvmd corosync,singlenode:" -i spec/source.inc

      # synthesize a changelog
      sed -e '/^%changelog/,$d' -i spec/lvm2.spec
      (echo "%changelog";
       echo "* `date +"%a %b %d %Y"` Petr Rockai <prockai@redhat.com> - $version";
       echo "- AUTOMATED BUILD BY Hydra") >> spec/lvm2.spec

      cp spec/* . # */ # RPM needs the spec file in the source root

      # make a tarball
      mkdir ../LVM2.$version
      mv * ../LVM2.$version
      ensureDir $out/tarballs
      cd ..
      tar cvzf $out/tarballs/LVM2.$version.tgz LVM2.$version
    '';
  };

  mkBuild = { src, VM, extras ? [], diskFun, ... }:
   VM rec {
     name = "lvm2-build-${diskImage.name}";
     fullName = "lvm2-build-${diskImage.name}";

     inherit src;
     diskImage = diskFun { extraPackages = extras; };
     memSize = 512;
     checkPhase = ":";

     preConfigure = install_lcov;

     postInstall = ''
      mkdir -p $out/nix-support
      for i in $out/rpms/*/*.rpm; do # */
        if echo $i | grep -vq "\.src\.rpm$"; then
          echo "file rpm $i" >> $out/nix-support/hydra-build-products
        else
          echo "file srpm $i" >> $out/nix-support/hydra-build-products
        fi
      done
     '';
   };

  rootmods = [ "virtio_pci" "virtio_blk" "virtio_balloon" "ext4" "unix"
               "cifs" "virtio_net" "unix" "hmac" "md4" "ecb" "des_generic" "sha256"
               "ata_piix" "sd_mod" ];

  centos_url = ver: arch: if ver == "6.6" || ver == "7"
       then "http://ftp.fi.muni.cz/pub/linux/centos/${ver}/os/${arch}/"
       else "http://vault.centos.org/${ver}/os/${arch}/";
  fedora_url = ver: arch: if lib.eqStrings ver "rawhide" || lib.eqStrings ver "19"
                       then "ftp://ftp.fi.muni.cz/pub/linux/fedora/linux/development/${ver}/${arch}/os/"
                       else "mirror://fedora/linux/releases/${ver}/Everything/${arch}/os/";
  fedora_update_url = ver: arch: "mirror://fedora/linux/updates/${ver}/${arch}";

  distros = with lib; let
      centos = { version, sha, arch }: {
        name = "centos-${version}-${arch}";
        fullName = "CentOS ${version} (${arch})";
        packagesList = pkgs.fetchurl {
          url = centos_url version arch + "repodata/${sha}-primary.xml.gz";
          sha256 = sha;
        };
        urlPrefix = centos_url version arch;
        archs = ["noarch" arch] ++ (if eqStrings arch "i386" then ["i586" "i686"] else []);
        packages = pkgs.vmTools.commonCentOSPackages;
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
        sha=$(grep primary.xml ${repodata} | sed -re 's:.* ([0-9a-f]+)-primary.*:\1:' | head -n 1)
        echo '{fedora}: fedora { version = "${version}"; sha = "'$sha'"; arch = "${arch}"; }' > $out
      '') { inherit fedora; };
      update = version: arch: repodata: orig: orig // (import (pkgs.runCommand "updates-fedora.nix" {} ''
          sha=$(grep primary.xml ${repodata} | sed -re 's:.* ([0-9a-f]+)-primary.*:\1:' | head -n 1)
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
      fedora20ux86_64 = update "20" "x86_64" fc20_64_updates pkgs.vmTools.rpmDistros.fedora20x86_64;
      fedora20ui386 = update "20" "i386" fc20_32_updates pkgs.vmTools.rpmDistros.fedora20i386;
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

      centos66i386 = centos {
        version="6.6"; arch="i386";
        sha="a8b935fcac1c8515c6d8dab3c43c53b3e461f89eb7a93b1914303784e28fcd17";
      };

      centos66x86_64 = centos {
        version="6.6"; arch="x86_64";
        sha="7651b16a9a2a8a5fbd0ad3ff8bbbe6f2409a64850ccfd83a6a3f874f13d8622f";
      };

      centos70x86_64 = centos {
        version="7"; arch="x86_64";
        sha="1a7dd0d315b39ad504f54ea88676ab502a48064cb2d875ae3ae29431e175861c";
      };
    } // over.distros;

  vm = { pkgs, xmods, dmmods ? false }: with lib; rec {
    tools = import "${nixpkgs}/pkgs/build-support/vm/default.nix" {
      inherit pkgs; rootModules = rootmods ++ xmods ++
        (if dmmods then [ "loop" "dm_mod" "dm_snapshot" "dm_mirror" "dm_zero" "dm_raid" "dm_thin_pool" ]
                   else []); };
    release = import "${nixpkgs}/pkgs/build-support/release/default.nix" {
      pkgs = pkgs // { vmTools = tools; }; };
    imgs = tools.diskImageFuns //
            mapAttrs (n: a: b: pkgs.vmTools.makeImageFromRPMDist (a // b)) distros;
    rpmdistros = tools.rpmDistros // distros;
    rpmbuild = tools.buildRPM;
  };

  install_rpms = rec {
      common = [ "libselinux-devel" "libsepol-devel" "ncurses-devel" "readline-devel"
                 "valgrind" "valgrind-devel" "gdb" "strace"
                 "redhat-rpm-config" # needed for rpmbuild of lvm
                 "which" "e2fsprogs" # needed for fsadm
                 "e2fsprogs-libs" "e2fsprogs-devel"
                 "perl-GD" # for lcov
                 "mdadm" # for tests with lvm2 and mdadm
                 "device-mapper-persistent-data" # thin and cache
                 "pkgconfig" # better support for config
                 "kernel"
               ];
      centos63 = [ "clusterlib-devel" "openaislib-devel" "cman" "libudev-devel" "procps" "nc" ];
      centos64 = centos63 ++ [ "corosynclib-devel" ];
      centos65 = centos64;
      centos66 = centos65;
      centos70 = [ "dlm-devel" "dlm" "corosynclib-devel" "perl-Digest-MD5" "systemd-devel"
                   "socat" # used by test suite lvmpolld
                   # "sanlock" # used by lvmlockd. Required version present in 7.2 only
                   "procps-ng" ];

      fedora17_18 = [ "dlm-devel" "corosynclib-devel" "libblkid" "libblkid-devel"
                      "dlm" "systemd-devel" "perl-Digest-MD5" "kernel-modules-extra" ];
      fedora17 = fedora17_18 ++ [ "libudev-devel" "nc" ];

      fedora18 = fedora17_18 ++ [ "socat" ];
      fedora18u = fedora18;

      fedora19 = centos70 ++ [ "kernel-modules-extra" ];
      fedora19u = fedora19;

      fedora20 = fedora19;
      fedora20u = fedora20;

      rawhide = fedora20;
    } // over.install_rpms;

  wrapper = fun: { arch, image, build ? {}, istest ? false, src ? jobs.tarball }: with lib;
    let use = vm { pkgs = if eqStrings arch "i386" then pkgs.pkgsi686Linux else pkgs;
                   xmods = if istest && (image == "centos64" || image == "centos65")
                              then [] else [ "9p" "9pnet_virtio" ];
                   dmmods = istest; };
     in fun {
           inherit build istest src;
           VM = use.rpmbuild;
           diskFun = builtins.getAttr "${image}${arch}" use.imgs;
           extras = install_rpms.common ++ builtins.getAttr image install_rpms;
           vmtools = use.tools;
           kernel = use.tools.makeKernelFromRPMDist (builtins.getAttr "${image}${arch}" use.rpmdistros);
        };

  configs = {
    fc20p_x86_64 = { arch = "x86_64"; image = "fedora20"; src = jobs.tarball_prof; };
    fc20p_i386   = { arch = "i386"  ; image = "fedora20"; src = jobs.tarball_prof; };
    fc20_x86_64 = { arch = "x86_64"; image = "fedora20"; };
    fc20_i386   = { arch = "i386"  ; image = "fedora20"; };
    fc19_x86_64 = { arch = "x86_64"; image = "fedora19"; };
    fc19_i386   = { arch = "i386"  ; image = "fedora19"; };
    fc18_x86_64 = { arch = "x86_64"; image = "fedora18"; };
    fc18_i386   = { arch = "i386"  ; image = "fedora18"; };
    fc17_x86_64 = { arch = "x86_64"; image = "fedora17"; };
    fc17_i386   = { arch = "i386"  ; image = "fedora17"; };

    fc18u_x86_64 = { arch = "x86_64"; image = "fedora18u"; };
    fc18u_i386   = { arch = "i386"; image = "fedora18u"; };
    fc19u_x86_64 = { arch = "x86_64"; image = "fedora19u"; };
    fc19u_i386   = { arch = "i386"; image = "fedora19u"; };

    #centos63_i386   = { arch = "i386"  ; image = "centos63"; };
    #centos63_x86_64 = { arch = "x86_64" ; image = "centos63"; };
    centos64_i386   = { arch = "i386"  ; image = "centos64"; };
    centos64_x86_64 = { arch = "x86_64" ; image = "centos64"; };
    centos65_i386   = { arch = "i386"  ; image = "centos65"; };
    centos65_x86_64 = { arch = "x86_64" ; image = "centos65"; };
    centos66_i386   = { arch = "i386"  ; image = "centos66"; };
    centos66_x86_64 = { arch = "x86_64" ; image = "centos66"; };

    centos70_x86_64 = { arch = "x86_64" ; image = "centos70"; };

    rawhide_i386   = { arch = "i386"  ; image = "rawhide"; };
    rawhide_x86_64 = { arch = "x86_64" ; image = "rawhide"; };
  } // over.configs;

  rpms = lib.mapAttrs (n: v: wrapper mkBuild v) configs;
  tests = let make = n: v: wrapper mkTest (v // { build = builtins.getAttr n rpms; istest = true; });
           in lib.mapAttrs make configs;

  jobs = tests // {
     tarball_prof = mkTarball "1";
     tarball = mkTarball "0";
  };
in jobs
