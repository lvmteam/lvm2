# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

# Try and minimize the refreshes we do.

import threading
from .request import RequestEntry
from . import cfg
from . import utils

_rlock = threading.RLock()
_count = 0


def handle_external_event(command):
	utils.log_debug("External event: '%s'" % command)
	event_complete()
	cfg.load()


def event_add(params):
	global _rlock
	global _count
	with _rlock:
		if _count == 0:
			_count += 1
			r = RequestEntry(
				-1, handle_external_event,
				params, None, None, False)
			cfg.worker_q.put(r)


def event_complete():
	global _rlock
	global _count
	with _rlock:
		if _count > 0:
			_count -= 1
		return _count
