#!/bin/sh
# Copyright (C) 2014-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip
aux prepare_vg 3

aux lvmconf 'global/cache_disabled_features = [ "policy_smq" ]'

lvcreate --type cache-pool -an -v -L 2 -n cpool $vg
lvcreate -H -L 4 -n corigin --cachepool $vg/cpool
lvcreate -n noncache -l 1 $vg

# cannot change major minor for pools
not lvchange --yes -M y --minor 235 --major 253 $vg/cpool
not lvchange -M n $vg/cpool

not lvchange --cachepolicy mq $vg/noncache
not lvchange --cachesettings foo=bar $vg/noncache

lvchange --cachepolicy cleaner $vg/corigin
check lv_field  $vg/corigin kernel_cache_policy "cleaner"

lvchange --cachepolicy mq --cachesettings migration_threshold=333 $vg/corigin

# TODO once mq->smq happens we will get here some 0 for mq settings
check lv_field $vg/corigin kernel_cache_policy "mq"
get lv_field $vg/corigin kernel_cache_settings | grep 'migration_threshold=333'

lvchange --refresh $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | grep 'migration_threshold=333'
lvchange -an $vg
lvchange -ay $vg
get lv_field $vg/corigin kernel_cache_settings | grep 'migration_threshold=333'

lvchange --cachesettings 'migration_threshold = 233 sequential_threshold = 13' $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=233' out
grep 'sequential_threshold=13' out

lvchange --cachesettings 'migration_threshold = 17' $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=17' out
grep 'sequential_threshold=13' out

lvchange --cachesettings 'migration_threshold = default' $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=2048' out
grep 'sequential_threshold=13' out

lvchange --cachesettings 'migration_threshold = 233 sequential_threshold = 13 random_threshold = 1' $vg/corigin
lvchange --cachesettings 'random_threshold = default migration_threshold = default' $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=2048' out
grep 'sequential_threshold=13' out
grep 'random_threshold=4' out

lvchange --cachesettings migration_threshold=233 --cachesettings sequential_threshold=13 --cachesettings random_threshold=1 $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=233' out
grep 'sequential_threshold=13' out
grep 'random_threshold=1' out

lvchange --cachesettings random_threshold=default --cachesettings migration_threshold=default $vg/corigin
get lv_field $vg/corigin kernel_cache_settings | tee out
grep 'migration_threshold=2048' out
grep 'sequential_threshold=13' out
grep 'random_threshold=4' out

vgremove -f $vg
