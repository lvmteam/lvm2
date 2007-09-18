#!/bin/sh

test_description='Exercise some lvcreate diagnostics'
privileges_required_=1

. ./test-lib.sh

cleanup_()
{
  test -n "$vg" && {
    vgchange -an "$vg"
    lvremove -ff "$vg"
    vgremove "$vg"
  } > /dev/null
  test -n "$d1" && losetup -d "$d1"
  test -n "$d2" && losetup -d "$d2"
  rm -f "$f1" "$f2"
}

test_expect_success \
  'set up temp files, loopback devices, PVs, and a VG' \
  'f1="$(pwd)/1" && d1=$(loop_setup_ "$f1") &&
   f2="$(pwd)/2" && d2=$(loop_setup_ "$f2") &&
   pvcreate $d1 $d2      &&
   vg=$(this_test_)-test-vg-$$  &&
   vgcreate $vg $d1 $d2'

lv=lvcreate-usage-$$

test_expect_success \
  'lvcreate w/negative stripesize must fail' \
  'lvcreate -L 64M -n $lv -i2 --stripesize -4 $vg 2>err; test $? = 3 &&
   grep "^  Negative stripesize is invalid\$" err'

test_expect_success \
  'lvcreate w/too-large stripesize must fail' \
  'lvcreate -L 64M -n $lv -i2 --stripesize 4294967291 $vg 2>err; test $? = 3 &&
   grep "^  Stripe size cannot be larger than 512.00 GB\$" err'

test_expect_success \
  'lvcreate w/single stripe succeeds with diagnostics to stdout' \
  'lvcreate -L 64M -n $lv -i1 --stripesize 4 $vg >out 2>err &&
   grep "^  Redundant stripes argument: default is 1\$" out &&
   grep "^  Ignoring stripesize argument with single stripe\$" out &&
   lvdisplay $vg &&
   lvremove -ff $vg'

test_expect_success \
  'lvcreate w/no stripe size succeeds with diagnostics to stdout' \
  'lvcreate -L 64M -n $lv -i2 $vg > out &&
   grep "^  Using default stripesize" out &&
   lvdisplay $vg &&
   lvremove -ff $vg'

test_expect_success \
  'lvcreate w/invalid number of stripes must fail' \
  'lvcreate -L 64M -n $lv -i129 $vg 2>err; test $? = 3 &&
   grep "^  Number of stripes (129) must be between 1 and 128\$" err'

# The case on lvdisplay output is to verify that the LV was not created.
test_expect_success \
  'lvcreate w/invalid stripe size must fail' \
  'lvcreate -L 64M -n $lv -i2 --stripesize 3 $vg 2>err; test $? = 3 &&
   grep "^  Invalid stripe size 3\.00 KB\$" err &&
   case "$(lvdisplay $vg)" in "") true ;; *) false ;; esac'

test_done
# Local Variables:
# indent-tabs-mode: nil
# End:
