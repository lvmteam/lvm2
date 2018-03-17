#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise creation of cache without cache_check

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_cache 1 3 0 || skip

# FIXME: parallel cache metadata allocator is crashing when used value 8000!
aux prepare_vg 5 80000

aux lvmconf 'global/cache_disabled_features = [ "policy_smq" ]' \
	    'global/cache_check_executable = "./fake-tool.sh"'

rm -f fake-tool.sh

lvcreate -l1 -n $lv1 $vg
lvcreate -H -l2 $vg/$lv1

lvchange -an $vg |& tee out
grep "Check is skipped" out

lvchange -ay $vg
grep "Check is skipped" out

# prepare fake version of cache_check tool that reports old version
cat <<- EOF >fake-tool.sh
#!/bin/sh
echo "0.1.0"
exit 1
EOF
chmod +x fake-tool.sh

lvchange -an $vg |& tee out
grep "upgrade" out

lvchange -ay $vg
grep "upgrade" out

# prepare fake version of cache_check tool that reports garbage
cat <<- EOF >fake-tool.sh
#!/bin/sh
echo "garbage"
exit 1
EOF
chmod +x fake-tool.sh

lvchange -an $vg |& tee out
grep "parse" out

lvchange -ay $vg
grep "parse" out

# prepare fake version of cache_check tool with high version
cat <<- EOF >fake-tool.sh
#!/bin/sh
echo "9.0.0"
exit 1
EOF
chmod +x fake-tool.sh

# Integrity check fails, but deactivation is OK
lvchange -an $vg |& tee out
grep "failed" out
# Activation must fail
fail lvchange -ay $vg

vgremove -ff $vg
