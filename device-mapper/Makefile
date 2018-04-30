# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

DM_SOURCE=\
	device-mapper/vdo/status.c

DM_DEPENDS=$(subst .c,.d,$(DM_SOURCE))
DM_OBJECTS=$(DM_SOURCE:%.c=%.o)
CLEAN_TARGETS+=$(DM_DEPENDS) $(DM_OBJECTS)

-include $(DM_DEPENDS)
