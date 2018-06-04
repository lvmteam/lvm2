# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This file is part of the device-mapper userspace tools.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU Lesser General Public License v.2.1.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

BASE_SOURCE=\
	base/data-struct/radix-tree.c \

BASE_DEPENDS=$(addprefix $(top_builddir)/,$(subst .c,.d,$(BASE_SOURCE)))
BASE_OBJECTS=$(addprefix $(top_builddir)/,$(subst .c,.o,$(BASE_SOURCE)))
CLEAN_TARGETS+=$(BASE_DEPENDS) $(BASE_OBJECTS)

-include $(BASE_DEPENDS)

$(BASE_OBJECTS): INCLUDES+=-I$(top_srcdir)/base/

$(top_builddir)/base/libbase.a: $(BASE_OBJECTS)
	@echo "    [AR] $@"
	$(Q) $(RM) $@
	$(Q) $(AR) rsv $@ $(BASE_OBJECTS) > /dev/null

CLEAN_TARGETS+=$(top_builddir)/base/libbase.a
