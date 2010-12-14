. ./test-utils.sh
aux prepare_devs 2
vgcreate -c n -s 4k $vg $devs
lvcreate -n foo $vg -l 5
lvcreate -s -n snap $vg/foo -l 2 -c 4k
lvcreate -s -n snap2 $vg/foo -l 6 -c 4k
dd if=/dev/urandom of=$DM_DEV_DIR/$vg/snap2 count=1 bs=1024
lvcreate -m 1 -n mirr $vg -l 1 --mirrorlog core
lvs
apitest percent $vg
