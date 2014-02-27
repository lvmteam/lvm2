#!/bin/sh
set -ex
rm -f result
rm -f divine-snapshot.tar.gz
rm -rf lvm-snapshot
mkdir lvm-snapshot
git ls-tree -r HEAD --name-only | xargs cp --parents --target-directory=lvm-snapshot
tar cvzf lvm-snapshot.tar.gz lvm-snapshot
nix-build nix/ \
    --arg lvm2Src "`pwd`/lvm-snapshot.tar.gz" \
    --arg lvm2Nix `pwd` -A "$@"
