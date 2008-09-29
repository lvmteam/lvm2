# Copyright (C) 2007 Red Hat, Inc. All rights reserved.
# Copyright (C) 2007 NEC Corporation
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. ./test-utils.sh

dmsetup_has_dm_devdir_support_ || exit 200

pv_() {
    eval "echo \$dev$1"
}

lv_is_on_ ()
{
  local lv=$vg/$1
  shift
  local pvs=$*

  echo "Check if $lv is exactly on PVs $pvs"
  rm -f out1 out2
  echo $pvs | sed 's/ /\n/g' | sort | uniq > out1

  lvs -a -odevices --noheadings $lv | \
    sed 's/([^)]*)//g; s/[ ,]/\n/g' | sort | uniq > out2

  diff --ignore-blank-lines out1 out2
}

mimages_are_on_ ()
{
  local lv=$1
  shift
  local pvs="$*"
  local mimages
  local i

  echo "Check if mirror images of $lv are on PVs $pvs"
  rm -f out1 out2
  echo $pvs | sed 's/ /\n/g' | sort | uniq > out1

  mimages=$(lvs --noheadings -a -o lv_name $vg | grep "${lv}_mimage_" | \
             sed 's/\[//g; s/\]//g')
  for i in $mimages; do
    echo "Checking $vg/$i"
    lvs -a -odevices --noheadings $vg/$i | \
      sed 's/([^)]*)//g; s/ //g; s/,/ /g' | sort | uniq >> out2
  done

  diff --ignore-blank-lines out1 out2
}

mirrorlog_is_on_()
{
  local lv="$1"_mlog
  shift
  lv_is_on_ $lv $*
}

lv_is_linear_()
{
  echo "Check if $1 is linear LV (i.e. not a mirror)"
  lvs -o stripes,attr --noheadings $vg/$1 | sed 's/ //g'
  lvs -o stripes,attr --noheadings $vg/$1 | sed 's/ //g' | grep -q '^1-'
}

rest_pvs_()
{
  local index=$1
  local num=$2
  local rem=""
  local n

  for n in $(seq 1 $(($index - 1))) $(seq $((index + 1)) $num); do
    rem="$rem $(pv_ $n)"
  done

  echo "$rem"
}

# ---------------------------------------------------------------------
# Initialize PVs and VGs

aux prepare_vg 5

# ---------------------------------------------------------------------
# Common environment setup/cleanup for each sub testcases

prepare_lvs_()
{
  lvremove -ff $vg;
  :
}

check_and_cleanup_lvs_()
{
  lvs -a -o+devices $vg &&
  lvremove -ff $vg
}

recover_vg_()
{
  enable_dev $* &&
  pvcreate -ff $* &&
  vgextend $vg $* &&
  check_and_cleanup_lvs_
}

test_expect_success "check environment setup/cleanup" \
  'prepare_lvs_ &&
   check_and_cleanup_lvs_'

# ---------------------------------------------------------------------
# one of mirror images has failed

# basic: fail the 2nd mirror image of 2-way mirrored LV
prepare_lvs_
lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 3):0-1
lvchange -an $vg/$lv1
aux mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2)
mirrorlog_is_on_ $lv1 $(pv_ 3)
disable_dev $(pv_ 2)
vgreduce --removemissing --force $vg
lv_is_linear_ $lv1
lv_is_on_ $lv1 $(pv_ 1)

test_expect_success "cleanup" \
  'recover_vg_ $(pv_ 2)'

# ---------------------------------------------------------------------
# LV has 3 images in flat,
# 1 out of 3 images fails

# test_3way_mirror_fail_1_ <PV# to fail>
test_3way_mirror_fail_1_()
{
   local index=$1

   lvcreate -l2 -m2 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4):0-1
   lvchange -an $vg/$lv1
   aux mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3)
   mirrorlog_is_on_ $lv1 $(pv_ 4)
   disable_dev $(pv_ $index)
   vgreduce --removemissing --force $vg
   lvs -a -o+devices $vg
   cat $G_root_/etc/lvm.conf
   mimages_are_on_ $lv1 $(rest_pvs_ $index 3)
   mirrorlog_is_on_ $lv1 $(pv_ 4)
}

for n in $(seq 1 3); do
    # fail mirror image $(($n - 1)) of 3-way mirrored LV" \
    prepare_lvs_
    test_3way_mirror_fail_1_ $n
    recover_vg_ $(pv_ $n)
done

# ---------------------------------------------------------------------
# LV has 3 images in flat,
# 2 out of 3 images fail

# test_3way_mirror_fail_2_ <PV# NOT to fail>
test_3way_mirror_fail_2_()
{
   local index=$1

   lvcreate -l2 -m2 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4):0-1
   lvchange -an $vg/$lv1
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3)
   mirrorlog_is_on_ $lv1 $(pv_ 4)
   rest_pvs_ $index 3
   disable_dev $(rest_pvs_ $index 3)
   vgreduce --force --removemissing $vg
   lvs -a -o+devices $vg
   aux lv_is_linear_ $lv1
   lv_is_on_ $lv1 $(pv_ $index)
}

for n in $(seq 1 3); do
    # fail mirror images other than mirror image $(($n - 1)) of 3-way mirrored LV
    prepare_lvs_
    test_3way_mirror_fail_2_ $n
    recover_vg_ $(rest_pvs_ $n 3)
done

# ---------------------------------------------------------------------
# LV has 4 images, 1 of them is in the temporary mirror for syncing.
# 1 out of 4 images fails

# test_3way_mirror_plus_1_fail_1_ <PV# to fail>
test_3way_mirror_plus_1_fail_1_()
{
   local index=$1

   lvcreate -l2 -m2 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+1 $vg/$lv1 $(pv_ 4) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ $index) &&
   vgreduce --removemissing --force $vg &&
   lvs -a -o+devices $vg &&
   mimages_are_on_ $lv1 $(rest_pvs_ $index 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5)
}

for n in $(seq 1 4); do
  test_expect_success "fail mirror image $(($n - 1)) of 4-way (1 converting) mirrored LV" \
    "prepare_lvs_ &&
     test_3way_mirror_plus_1_fail_1_ $n"
  test_expect_success "cleanup" \
    "recover_vg_ $(pv_ $n)"
done

# ---------------------------------------------------------------------
# LV has 4 images, 1 of them is in the temporary mirror for syncing.
# 3 out of 4 images fail

# test_3way_mirror_plus_1_fail_3_ <PV# NOT to fail>
test_3way_mirror_plus_1_fail_3_()
{
   local index=$1

   lvcreate -l2 -m2 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+1 $vg/$lv1 $(pv_ 4) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(rest_pvs_ $index 4) &&
   vgreduce --removemissing --force $vg &&
   lvs -a -o+devices $vg &&
   (mimages_are_on_ $lv1 $(pv_ $index) || lv_is_on_ $lv1 $(pv_ $index)) &&
   ! mirrorlog_is_on_ $lv1 $(pv_ 5)
}

for n in $(seq 1 4); do
  test_expect_success "fail mirror images other than mirror image $(($n - 1)) of 4-way (1 converting) mirrored LV" \
    "prepare_lvs_ &&
     test_3way_mirror_plus_1_fail_3_ $n"
  test_expect_success "cleanup" \
    "recover_vg_ $(rest_pvs_ $n 4)"
done

# ---------------------------------------------------------------------
# LV has 4 images, 2 of them are in the temporary mirror for syncing.
# 1 out of 4 images fail

# test_2way_mirror_plus_2_fail_1_ <PV# to fail>
test_2way_mirror_plus_2_fail_1_()
{
   local index=$1

   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+2 $vg/$lv1 $(pv_ 3) $(pv_ 4) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ $index) &&
   vgreduce --removemissing --force $vg &&
   lvs -a -o+devices $vg &&
   mimages_are_on_ $lv1 $(rest_pvs_ $index 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5)
}

for n in $(seq 1 4); do
  test_expect_success "fail mirror image $(($n - 1)) of 4-way (2 converting) mirrored LV" \
    "prepare_lvs_ &&
     test_2way_mirror_plus_2_fail_1_ $n"
  test_expect_success "cleanup" \
    "recover_vg_ $(pv_ $n)"
done

# ---------------------------------------------------------------------
# LV has 4 images, 2 of them are in the temporary mirror for syncing.
# 3 out of 4 images fail

# test_2way_mirror_plus_2_fail_3_ <PV# NOT to fail>
test_2way_mirror_plus_2_fail_3_()
{
   local index=$1

   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+2 $vg/$lv1 $(pv_ 3) $(pv_ 4) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(rest_pvs_ $index 4) &&
   vgreduce --removemissing --force $vg &&
   lvs -a -o+devices $vg &&
   (mimages_are_on_ $lv1 $(pv_ $index) || lv_is_on_ $lv1 $(pv_ $index)) &&
   ! mirrorlog_is_on_ $lv1 $(pv_ 5)
}

for n in $(seq 1 4); do
  test_expect_success "fail mirror images other than mirror image $(($n - 1)) of 4-way (2 converting) mirrored LV" \
    "prepare_lvs_ &&
     test_2way_mirror_plus_2_fail_3_ $n"
  test_expect_success "cleanup" \
    "recover_vg_ $(rest_pvs_ $n 4)"
done

# ---------------------------------------------------------------------
# log device is gone (flat mirror and stacked mirror)

test_expect_success "fail mirror log of 2-way mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ 5) &&
   vgreduce --removemissing --force $vg &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   ! mirrorlog_is_on_ $lv1 $(pv_ 5)'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 5)"

test_expect_success "fail mirror log of 3-way (1 converting) mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+1 $vg/$lv1 $(pv_ 3) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ 5) &&
   vgreduce --removemissing --force $vg &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) &&
   ! mirrorlog_is_on_ $lv1 $(pv_ 5)'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 5)"

# ---------------------------------------------------------------------
# all images are gone (flat mirror and stacked mirror)

test_expect_success "fail all mirror images of 2-way mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ 1) $(pv_ 2) &&
   vgreduce --removemissing --force $vg &&
   ! lvs $vg/$lv1'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 1) $(pv_ 2)"

test_expect_success "fail all mirror images of 3-way (1 converting) mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvconvert -m+1 $vg/$lv1 $(pv_ 3) &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) $(pv_ 3) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   disable_dev $(pv_ 1) $(pv_ 2) $(pv_ 3) &&
   vgreduce --removemissing --force $vg &&
   ! lvs $vg/$lv1'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 1) $(pv_ 2) $(pv_ 3)"

# ---------------------------------------------------------------------
# Multiple LVs

test_expect_success "fail a mirror image of one of mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvcreate -l2 -m1 -n $lv2 $vg $(pv_ 3) $(pv_ 4) $(pv_ 5):1-1 &&
   lvchange -an $vg/$lv2 &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mimages_are_on_ $lv2 $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   mirrorlog_is_on_ $lv2 $(pv_ 5) &&
   disable_dev $(pv_ 2) &&
   vgreduce --removemissing --force $vg &&
   mimages_are_on_ $lv2 $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv2 $(pv_ 5) &&
   lv_is_linear_ $lv1 &&
   lv_is_on_ $lv1 $(pv_ 1)'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 2)"

test_expect_success "fail mirror images, one for each mirrored LV" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   lvcreate -l2 -m1 -n $lv2 $vg $(pv_ 3) $(pv_ 4) $(pv_ 5):1-1 &&
   lvchange -an $vg/$lv2 &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mimages_are_on_ $lv2 $(pv_ 3) $(pv_ 4) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   mirrorlog_is_on_ $lv2 $(pv_ 5) &&
   disable_dev $(pv_ 2) &&
   disable_dev $(pv_ 4) &&
   vgreduce --removemissing --force $vg &&
   lv_is_linear_ $lv1 &&
   lv_is_on_ $lv1 $(pv_ 1) &&
   lv_is_linear_ $lv2 &&
   lv_is_on_ $lv2 $(pv_ 3)'
test_expect_success "cleanup" \
  "recover_vg_ $(pv_ 2) $(pv_ 4)"

# ---------------------------------------------------------------------
# no failure

test_expect_success "no failures" \
  'prepare_lvs_ &&
   lvcreate -l2 -m1 -n $lv1 $vg $(pv_ 1) $(pv_ 2) $(pv_ 5):0-1 &&
   lvchange -an $vg/$lv1 &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5) &&
   vgreduce --removemissing --force $vg &&
   mimages_are_on_ $lv1 $(pv_ 1) $(pv_ 2) &&
   mirrorlog_is_on_ $lv1 $(pv_ 5)'
test_expect_success "cleanup" \
  'check_and_cleanup_lvs_'

# ---------------------------------------------------------------------

test_done
