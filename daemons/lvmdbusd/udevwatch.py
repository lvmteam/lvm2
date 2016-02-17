# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import pyudev
from .refresh import event_add
from . import cfg

observer = None


# noinspection PyUnusedLocal
def filter_event(action, device):
	# Filter for events of interest and add a request object to be processed
	# when appropriate.
	refresh = False

	if '.ID_FS_TYPE_NEW' in device:
		fs_type_new = device['.ID_FS_TYPE_NEW']

		if 'LVM' in fs_type_new:
			refresh = True
		elif fs_type_new == '':
			# Check to see if the device was one we knew about
			if 'DEVNAME' in device:
				found = cfg.om.get_object_by_lvm_id(device['DEVNAME'])
				if found:
					refresh = True

	if 'DM_LV_NAME' in device:
		refresh = True

	if refresh:
		event_add(('udev',))


def add():
	global observer
	context = pyudev.Context()
	monitor = pyudev.Monitor.from_netlink(context)
	monitor.filter_by('block')
	observer = pyudev.MonitorObserver(monitor, filter_event)
	observer.start()


def remove():
	global observer
	observer.stop()
	observer = None
