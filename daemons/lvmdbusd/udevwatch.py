# Copyright (C) 2015-2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import pyudev
import threading
import os
from . import cfg
from .request import RequestEntry
from . import utils

observer = None
observer_lock = threading.RLock()

_udev_lock = threading.RLock()
_udev_count = 0


def udev_add():
	global _udev_count
	with _udev_lock:
		if _udev_count == 0:
			_udev_count += 1

			# Place this on the queue so any other operations will sequence
			# behind it
			r = RequestEntry(
				-1, _udev_event, (), None, None, False)
			cfg.worker_q.put(r)


def udev_complete():
	global _udev_count
	with _udev_lock:
		if _udev_count > 0:
			_udev_count -= 1


def _udev_event():
	utils.log_debug("Processing udev event")
	udev_complete()
	cfg.load()


# noinspection PyUnusedLocal
def filter_event(action, device):
	# Filter for events of interest and add a request object to be processed
	# when appropriate.
	refresh = False

	# Debug: Uncomment to log all udev events
	#devlinks_str = device.get('DEVLINKS', '')
	#utils.log_debug("Udev event: action='%s', DEVNAME='%s', ID_FS_TYPE='%s', subsystem='%s', DEVLINKS='%s'" %
	#	(action, device.get('DEVNAME', 'N/A'), device.get('ID_FS_TYPE', 'N/A'),
	#	device.get('SUBSYSTEM', 'N/A'), devlinks_str[:100] if devlinks_str else 'N/A'))

	# Ignore everything but change
	if action != 'change':
		return

	# Helper to lookup device with automatic path translation for test environments
	dm_dev_dir = os.environ.get('DM_DEV_DIR', '/dev')

	def lookup_with_translation(device):
		"""Lookup device by name, with fallback to translated path if needed.

		Try direct lookup first (fast path for production).
		If not found and using test environment (DM_DEV_DIR != /dev):
		  - Extract dm-name from DEVLINKS (/dev/disk/by-id/dm-name-XXX)
		  - Construct path: $DM_DEV_DIR/mapper/XXX
		  - Try lookup again

		Returns the found object or None.
		"""
		devname = device.get('DEVNAME', '')
		obj = cfg.om.get_object_by_lvm_id(devname)
		if not obj and dm_dev_dir != '/dev' and devname.startswith('/dev/dm-'):
			devlinks = device.get('DEVLINKS', '')
			if devlinks:
				# Parse DEVLINKS to find dm-name-XXX
				for link in devlinks.split():
					if 'dm-name-' in link:
						# Extract device-mapper name from /dev/disk/by-id/dm-name-XXX
						dm_name = link.split('dm-name-', 1)[1]
						# Construct path in DM_DEV_DIR and try lookup
						mapped_path = os.path.join(dm_dev_dir, 'mapper', dm_name)
						#utils.log_debug("Translating %s to %s (via dm-name)" % (devname, mapped_path))
						obj = cfg.om.get_object_by_lvm_id(mapped_path)
						break
		return obj

	if 'ID_FS_TYPE' in device:
		fs_type_new = device['ID_FS_TYPE']
		if 'LVM' in fs_type_new:
			# If we get a lvm related udev event for a block device
			# we don't know about, it's either a pvcreate which we
			# would handle with the dbus notification or something
			# copied a pv signature onto a block device, this is
			# required to catch the latter.
			if not lookup_with_translation(device):
				refresh = True
		elif fs_type_new == '':
			# Check to see if the device was one we knew about
			if 'DEVNAME' in device:
				if lookup_with_translation(device):
					refresh = True
	else:
		# This handles the wipefs -a path
		if not refresh and 'DEVNAME' in device:
			found_obj = lookup_with_translation(device)

			# Also check device symlinks - udev might report /dev/dm-X but
			# the PV is tracked under a different name
			if not found_obj:
				devlinks = device.get('DEVLINKS', '')
				if devlinks:
					for link in devlinks.split():
						found_obj = cfg.om.get_object_by_lvm_id(link)
						if found_obj:
							break

			if found_obj:
				refresh = True

	if refresh:
		udev_add()


def add():
	with observer_lock:
		global observer
		context = pyudev.Context()
		# Use source='udev' to get processed udev events, not raw kernel events
		monitor = pyudev.Monitor.from_netlink(context, source='udev')
		monitor.filter_by('block')
		observer = pyudev.MonitorObserver(monitor, filter_event)
		observer.start()


def remove():
	with observer_lock:
		global observer
		if observer:
			observer.stop()
			observer = None
			return True
		return False
