#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='verify semaphore exhaustion does not leave devices suspended'

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

# This test requires udev synchronization and SysV semaphore support
test "$DM_UDEV_SYNCHRONIZATION" -eq 1 || skip "Needs udev synchronization"
test -e /proc/sysvipc/sem || skip "No SysV semaphore support"
test -w /proc/sys/kernel/sem || skip "Cannot modify semaphore limits"

aux prepare_vg 1

# Save original semaphore limits
ORIG_SEM=$(cat /proc/sys/kernel/sem)
read -r SEMMSL SEMMNS SEMOPM SEMMNI <<< "$ORIG_SEM"

# Restore semaphore limits on any exit path - the test framework
# teardown needs semaphores for udev transactions during cleanup.
cleanup_and_teardown() {
	echo "$ORIG_SEM" > /proc/sys/kernel/sem
	test -n "${HELD_SEM-}" && ipcrm -s "$HELD_SEM" 2>/dev/null
	HELD_SEM=""
	aux teardown
}
trap cleanup_and_teardown EXIT

restore_sem_() {
	echo "$ORIG_SEM" > /proc/sys/kernel/sem
	test -n "${HELD_SEM-}" && ipcrm -s "$HELD_SEM" 2>/dev/null
	HELD_SEM=""
}

# Block new semaphore creation: hold one set, then lower SEMMNI
# to current usage so no new sets can be allocated.
exhaust_sem_() {
	HELD_SEM=$(ipcmk -S 1 | awk '{print $NF}')
	CURRENT=$(( $(wc -l < /proc/sysvipc/sem) - 1 ))
	echo "$SEMMSL $SEMMNS $SEMOPM $CURRENT" > /proc/sys/kernel/sem
}

lvcreate -an -L4 -n $lv1 $vg

# --- Test 1: activation under exhaustion ---
exhaust_sem_
not lvchange -ay $vg/$lv1
check inactive $vg $lv1
restore_sem_

lvchange -ay $vg/$lv1
check active $vg $lv1

# --- Test 2: refresh (suspend+resume) under exhaustion ---
exhaust_sem_
not lvchange --refresh $vg/$lv1
# Device must NOT be left in suspended state
dmsetup info "$vg-$lv1" | tee out
grep "State:.*ACTIVE" out
restore_sem_

lvchange --refresh $vg/$lv1
check active $vg $lv1

# --- Test 3: deactivation under exhaustion ---
exhaust_sem_
not lvchange -an $vg/$lv1
check active $vg $lv1
restore_sem_

lvchange -an $vg/$lv1
check inactive $vg $lv1

# --- Test 4: lvcreate with activation under exhaustion ---
lvremove -f $vg/$lv1
exhaust_sem_
not lvcreate -L4 -n $lv2 $vg
# LV must not exist after failed create
not lvs $vg/$lv2
restore_sem_

lvcreate -L4 -n $lv2 $vg
check active $vg $lv2

# --- Test 5: snapshot creation under exhaustion ---
# Snapshot creation suspends the origin to add the COW exception store,
# then resumes both origin and snapshot - exercises compound suspend path.
exhaust_sem_
not lvcreate -s -L4 -n $lv3 $vg/$lv2
# Origin must stay active and not suspended
dmsetup info "$vg-$lv2" | tee out
grep "State:.*ACTIVE" out
# Snapshot must not exist
not lvs $vg/$lv3
restore_sem_

lvcreate -s -L4 -n $lv3 $vg/$lv2
check active $vg $lv2
check active $vg $lv3

# --- Test 6: --noudevsync bypasses cookie, operations must succeed ---
lvchange -an $vg/$lv2
lvremove -f $vg/$lv3 $vg/$lv2

lvcreate -an -L4 -n $lv1 $vg
exhaust_sem_

lvchange --noudevsync -ay $vg/$lv1
check active $vg $lv1

lvchange --noudevsync --refresh $vg/$lv1
dmsetup info "$vg-$lv1" | tee out
grep "State:.*ACTIVE" out

lvchange --noudevsync -an $vg/$lv1
check inactive $vg $lv1

restore_sem_

vgremove -ff $vg
