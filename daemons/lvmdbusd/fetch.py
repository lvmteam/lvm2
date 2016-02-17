# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from .pv import load_pvs
from .vg import load_vgs
from .lv import load_lvs
from . import cfg


def load(refresh=True, emit_signal=True, cache_refresh=True, log=True):
	num_total_changes = 0

	# Go through and load all the PVs, VGs and LVs
	if cache_refresh:
		cfg.db.refresh(log)

	num_total_changes += load_pvs(refresh=refresh, emit_signal=emit_signal,
									cache_refresh=False)[1]
	num_total_changes += load_vgs(refresh=refresh, emit_signal=emit_signal,
									cache_refresh=False)[1]
	num_total_changes += load_lvs(refresh=refresh, emit_signal=emit_signal,
									cache_refresh=False)[1]

	return num_total_changes
