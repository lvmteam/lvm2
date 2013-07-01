#!/bin/sh
# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

. lib/test

#If you change this change the unit test case too.
aux prepare_pvs 6

#Locate the python binding library to use.
python_lib=`find $abs_top_builddir -name lvm.so`
if [ "$python_lib" != "" ]
then
	export PYTHONPATH=`dirname $python_lib`:$PYTHONPATH
	python_lvm_unit.py -v
	#nemiver python ../api/python_lvm_unit.py -v -f
else
	echo "Unable to test python bindings as library not available"
fi
