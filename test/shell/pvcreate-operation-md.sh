# Copyright (C) 2009 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# skip this test if mdadm or sfdisk (or others) aren't available
which mdadm || exit 200
which sfdisk || exit 200
which perl || exit 200
which awk || exit 200
which cut || exit 200

test -f /proc/mdstat && grep -q raid0 /proc/mdstat || \
modprobe raid0 || exit 200

. lib/test

aux lvmconf 'devices/filter = [ "a|/dev/md.*|", "a/dev\/mapper\/.*$/", "r/.*/" ]'
aux prepare_devs 2

# Have MD use a non-standard name to avoid colliding with an existing MD device
# - mdadm >= 3.0 requires that non-standard device names be in /dev/md/
# - newer mdadm _completely_ defers to udev to create the associated device node
mdadm_maj=$(mdadm --version 2>&1 | perl -pi -e 's|.* v(\d+).*|\1|')
[ $mdadm_maj -ge 3 ] && \
    mddev=/dev/md/md_lvm_test0 || \
    mddev=/dev/md_lvm_test0

cleanup_md() {
    # sleeps offer hack to defeat: 'md: md127 still in use'
    # see: https://bugzilla.redhat.com/show_bug.cgi?id=509908#c25
    aux udev_wait
    mdadm --stop $mddev || true
    aux udev_wait
    if [ -b "$mddev" ]; then
        # mdadm doesn't always cleanup the device node
	sleep 2
	rm -f $mddev
    fi
}

cleanup_md_and_teardown() {
    cleanup_md
    aux teardown
}

# create 2 disk MD raid0 array (stripe_width=128K)
test -b "$mddev" && exit 200
mdadm --create --metadata=1.0 $mddev --auto=md --level 0 --raid-devices=2 --chunk 64 $dev1 $dev2
trap 'cleanup_md_and_teardown' EXIT # cleanup this MD device at the end of the test
test -b "$mddev" || exit 200

# Test alignment of PV on MD without any MD-aware or topology-aware detection
# - should treat $mddev just like any other block device
pv_align="1.00m"
pvcreate --metadatasize 128k \
    --config 'devices {md_chunk_alignment=0 data_alignment_detection=0 data_alignment_offset_detection=0}' \
    $mddev
check pv_field $mddev pe_start $pv_align

# Test md_chunk_alignment independent of topology-aware detection
pv_align="1.00m"
pvcreate --metadatasize 128k \
    --config 'devices {data_alignment_detection=0 data_alignment_offset_detection=0}' \
    $mddev
check pv_field $mddev pe_start $pv_align


# Test newer topology-aware alignment detection
# - first added to 2.6.31 but not "reliable" until 2.6.33
if kernel_at_least 2 6 33 ; then
    pv_align="1.00m"
    # optimal_io_size=131072, minimum_io_size=65536
    pvcreate --metadatasize 128k \
	--config 'devices { md_chunk_alignment=0 }' $mddev
    check pv_field $mddev pe_start $pv_align
fi

# partition MD array directly, depends on blkext in Linux >= 2.6.28
if kernel_at_least 2 6 28 ; then
    # create one partition
    sfdisk $mddev <<EOF
,,83
EOF
    # make sure partition on MD is _not_ removed
    # - tests partition -> parent lookup via sysfs paths
    not pvcreate --metadatasize 128k $mddev

    # verify alignment_offset is accounted for in pe_start
    # - topology infrastructure is available in Linux >= 2.6.31
    # - also tests partition -> parent lookup via sysfs paths

    # Oh joy: need to lookup /sys/block/md127 rather than /sys/block/md_lvm_test0
    mddev_maj_min=$(ls -lL $mddev | awk '{ print $5 $6 }' | perl -pi -e 's|,|:|')
    mddev_p_sysfs_name=$(echo /sys/dev/block/${mddev_maj_min}/*p1)
    base_mddev_p=`basename $mddev_p_sysfs_name`
    mddev_p=/dev/${base_mddev_p}

    # in case the system is running without devtmpfs /dev
    # wait here for created device node on tmpfs
    aux udev_wait $mddev_p
    test -b $mddev_p || exit 200

    # Checking for 'alignment_offset' in sysfs implies Linux >= 2.6.31
    # but reliable alignment_offset support requires kernel.org Linux >= 2.6.33
    sysfs_alignment_offset=/sys/dev/block/${mddev_maj_min}/${base_mddev_p}/alignment_offset
    [ -f $sysfs_alignment_offset ] && kernel_at_least 2 6 33 && \
	alignment_offset=`cat $sysfs_alignment_offset` || \
	alignment_offset=0

    if [ $alignment_offset -gt 0 ]; then
        # default alignment is 1M, add alignment_offset
	pv_align=$((1048576+$alignment_offset))B
	pvcreate --metadatasize 128k $mddev_p
	check pv_field $mddev_p pe_start $pv_align "--units b"
	pvremove $mddev_p
    fi
fi

# Test newer topology-aware alignment detection w/ --dataalignment override
if kernel_at_least 2 6 33 ; then
    cleanup_md
    pvcreate -f $dev1
    pvcreate -f $dev2

    # create 2 disk MD raid0 array (stripe_width=2M)
    test -b "$mddev" && exit 200
    mdadm --create --metadata=1.0 $mddev --auto=md --level 0 --raid-devices=2 --chunk 1024 $dev1 $dev2
    test -b "$mddev" || exit 200

    # optimal_io_size=2097152, minimum_io_size=1048576
    pv_align="2.00m"
    pvcreate --metadatasize 128k \
	--config 'devices { md_chunk_alignment=0 }' $mddev
    check pv_field $mddev pe_start $pv_align

    # now verify pe_start alignment override using --dataalignment
    pv_align="192.00k"
    pvcreate --dataalignment 64k --metadatasize 128k \
	--config 'devices { md_chunk_alignment=0 }' $mddev
    check pv_field $mddev pe_start $pv_align
fi
