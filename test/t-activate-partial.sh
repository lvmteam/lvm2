. ./test-utils.sh

aux prepare_vg 3

lvcreate -m 1 -l 1 -n mirror $vg
lvchange -a n $vg/mirror
disable_dev $dev1

not vgreduce --removemissing $vg
not lvchange -v -a y $vg/mirror
lvchange -v --partial -a y $vg/mirror
not lvchange -v --refresh $vg/mirror
lvchange -v --refresh --partial $vg/mirror

# also check that vgchange works
vgchange -a n --partial $vg
vgchange -a y --partial $vg

# check vgremove
vgremove -f $vg