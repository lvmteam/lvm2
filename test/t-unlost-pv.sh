. ./test-utils.sh

aux prepare_vg 3

lvcreate -m 1 -l 1 -n mirror $vg
lvchange -a n $vg/mirror

check() {
vgscan 2>&1 | tee vgscan.out
grep "Inconsistent metadata found for VG $vg" vgscan.out
vgscan 2>&1 | tee vgscan.out
not grep "Inconsistent metadata found for VG $vg" vgscan.out
}

# try orphaning a missing PV
disable_dev $dev1
vgreduce --removemissing --force $vg
enable_dev $dev1
check

exit 0 # FIXME, bug demonstration code follows
# try to just change metadata; we expect the new version (with MISSING_PV set
# on the reappeared volume) to be written out to the previously missing PV
vgextend $vg $dev1
disable_dev $dev1
lvremove $vg/mirror
enable_dev $dev1
check
