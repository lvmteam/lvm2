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

DEVICE_MAPPER_SOURCE=\
	device_mapper/datastruct/bitset.c \
	device_mapper/libdm-common.c \
	device_mapper/libdm-config.c \
	device_mapper/libdm-deptree.c \
	device_mapper/libdm-file.c \
	device_mapper/libdm-report.c \
	device_mapper/libdm-string.c \
	device_mapper/libdm-targets.c \
	device_mapper/libdm-timestamp.c \
	device_mapper/mm/pool.c \
	device_mapper/regex/matcher.c \
	device_mapper/regex/parse_rx.c \
	device_mapper/regex/ttree.c \
	device_mapper/ioctl/libdm-iface.c \
	device_mapper/vdo/vdo_target.c \
	device_mapper/vdo/status.c

DEVICE_MAPPER_DEPENDS=$(addprefix $(top_builddir)/,$(subst .c,.d,$(DEVICE_MAPPER_SOURCE)))
DEVICE_MAPPER_OBJECTS=$(addprefix $(top_builddir)/,$(subst .c,.o,$(DEVICE_MAPPER_SOURCE)))
CLEAN_TARGETS+=$(DEVICE_MAPPER_DEPENDS) $(DEVICE_MAPPER_OBJECTS)

#$(DEVICE_MAPPER_DEPENDS): INCLUDES+=$(VDO_INCLUDES)
#$(DEVICE_MAPPER_OBJECTS): INCLUDES+=$(VDO_INCLUDES)

ifeq ("$(USE_TRACKING)","yes")
ifeq (,$(findstring $(MAKECMDGOALS),cscope.out cflow clean distclean lcov \
 help check check_local check_cluster check_lvmetad check_lvmpolld))
	-include $(DEVICE_MAPPER_DEPENDS)
endif
endif

$(DEVICE_MAPPER_OBJECTS): INCLUDES+=-I$(top_srcdir)/device_mapper/

$(top_builddir)/device_mapper/libdevice-mapper.a: $(DEVICE_MAPPER_OBJECTS)
	@echo "    [AR] $@"
	$(Q) $(RM) $@
	$(Q) $(AR) rsv $@ $(DEVICE_MAPPER_OBJECTS) > /dev/null

CLEAN_TARGETS+=$(top_builddir)/device_mapper/libdevice-mapper.a
