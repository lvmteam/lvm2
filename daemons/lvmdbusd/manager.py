# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from .automatedproperties import AutomatedProperties

from . import utils
from .cfg import MANAGER_INTERFACE
import dbus
from . import cfg
from . import cmdhandler
from .fetch import load_pvs, load_vgs
from .request import RequestEntry
from .refresh import event_add


# noinspection PyPep8Naming
class Manager(AutomatedProperties):
	_Version_meta = ("t", MANAGER_INTERFACE)

	def __init__(self, object_path):
		super(Manager, self).__init__(object_path)
		self.set_interface(MANAGER_INTERFACE)

	@property
	def Version(self):
		return '1.0.0'

	@staticmethod
	def _pv_create(device, create_options):

		# Check to see if we are already trying to create a PV for an existing
		# PV
		pv = cfg.om.get_object_path_by_lvm_id(
			device, device, None, False)
		if pv:
			raise dbus.exceptions.DBusException(
				MANAGER_INTERFACE, "PV Already exists!")

		created_pv = []
		rc, out, err = cmdhandler.pv_create(create_options, [device])
		if rc == 0:
			pvs = load_pvs([device], emit_signal=True)[0]
			for p in pvs:
				created_pv = p.dbus_object_path()
		else:
			raise dbus.exceptions.DBusException(
				MANAGER_INTERFACE,
				'Exit code %s, stderr = %s' % (str(rc), err))

		return created_pv

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='sia{sv}',
		out_signature='(oo)',
		async_callbacks=('cb', 'cbe'))
	def PvCreate(self, device, tmo, create_options, cb, cbe):
		utils.validate_device_path(MANAGER_INTERFACE, device)
		r = RequestEntry(
			tmo, Manager._pv_create,
			(device, create_options), cb, cbe)
		cfg.worker_q.put(r)

	@staticmethod
	def _create_vg(name, pv_object_paths, create_options):
		pv_devices = []

		for p in pv_object_paths:
			pv = cfg.om.get_object_by_path(p)
			if pv:
				pv_devices.append(pv.Name)
			else:
				raise dbus.exceptions.DBusException(
					MANAGER_INTERFACE, 'object path = %s not found' % p)

		rc, out, err = cmdhandler.vg_create(create_options, pv_devices, name)
		created_vg = "/"

		if rc == 0:
			vgs = load_vgs([name], emit_signal=True)[0]
			for v in vgs:
				created_vg = v.dbus_object_path()

			# Update the PVS
			load_pvs(refresh=True, emit_signal=True, cache_refresh=False)
		else:
			raise dbus.exceptions.DBusException(
				MANAGER_INTERFACE,
				'Exit code %s, stderr = %s' % (str(rc), err))
		return created_vg

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='saoia{sv}',
		out_signature='(oo)',
		async_callbacks=('cb', 'cbe'))
	def VgCreate(self, name, pv_object_paths, tmo, create_options, cb, cbe):
		utils.validate_vg_name(MANAGER_INTERFACE, name)
		r = RequestEntry(
			tmo, Manager._create_vg,
			(name, pv_object_paths, create_options,),
			cb, cbe)
		cfg.worker_q.put(r)

	@staticmethod
	def _refresh():
		utils.log_debug('Manager.Refresh - entry')

		# This is a diagnostic and should not be run in normal operation, so
		# lets remove the log entries for refresh as it's implied.
		rc = cfg.load(log=False)

		if rc != 0:
			utils.log_debug('Manager.Refresh - exit %d' % (rc),
							'bg_black', 'fg_light_red')
		else:
			utils.log_debug('Manager.Refresh - exit %d' % (rc))
		return rc

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		out_signature='t',
		async_callbacks=('cb', 'cbe'))
	def Refresh(self, cb, cbe):
		"""
		Take all the objects we know about and go out and grab the latest
		more of a test method at the moment to make sure we are handling object
		paths correctly.

		:param cb   Callback for result
		:param cbe  Callback for errors

		Returns the number of changes, object add/remove/properties changed
		"""
		r = RequestEntry(-1, Manager._refresh, (), cb, cbe, False)
		cfg.worker_q.put(r)

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='s',
		out_signature='o')
	def LookUpByLvmId(self, key):
		"""
		Given a lvm id in one of the forms:

		/dev/sda
		some_vg
		some_vg/some_lv
		Oe1rPX-Pf0W-15E5-n41N-ZmtF-jXS0-Osg8fn

		return the object path in O(1) time.

		:param key: The lookup value
		:return: Return the object path.  If object not found you will get '/'
		"""
		p = cfg.om.get_object_path_by_lvm_id(
			key, key, gen_new=False)
		if p:
			return p
		return '/'

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='b')
	def UseLvmShell(self, yes_no):
		"""
		Allow the client to enable/disable lvm shell, used for testing
		:param yes_no:
		:return: Nothing
		"""
		cmdhandler.set_execution(yes_no)

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='s', out_signature='i')
	def ExternalEvent(self, command):

		event_add((command,))
		return dbus.Int32(0)

	@staticmethod
	def _pv_scan(activate, cache, device_path, major_minor, scan_options):

		rc, out, err = cmdhandler.pv_scan(
			activate, cache, device_path,
			major_minor, scan_options)

		if rc == 0:
			# This could potentially change the state quite a bit, so lets
			# update everything to be safe
			cfg.load()
			return '/'
		else:
			raise dbus.exceptions.DBusException(
				MANAGER_INTERFACE,
				'Exit code %s, stderr = %s' % (str(rc), err))

	@dbus.service.method(
		dbus_interface=MANAGER_INTERFACE,
		in_signature='bbasa(ii)ia{sv}',
		out_signature='o',
		async_callbacks=('cb', 'cbe'))
	def PvScan(self, activate, cache, device_paths, major_minors,
			tmo, scan_options, cb, cbe):
		"""
		Scan all supported LVM block devices in the system for physical volumes
		NOTE: major_minors & device_paths only usable when cache == True
		:param activate: If True, activate any newly found LVs
		:param cache:    If True, update lvmetad
		:param device_paths: Array of device paths or empty
		:param major_minors: Array of structures (major,minor)
		:param tmo: Timeout for operation
		:param scan_options:  Additional options to pvscan
		:param cb: Not visible in API (used for async. callback)
		:param cbe: Not visible in API (used for async. error callback)
		:return: '/' if operation done, else job path
		"""
		for d in device_paths:
			utils.validate_device_path(MANAGER_INTERFACE, d)

		r = RequestEntry(
			tmo, Manager._pv_scan,
			(activate, cache, device_paths, major_minors,
			scan_options), cb, cbe, False)
		cfg.worker_q.put(r)

	@property
	def lvm_id(self):
		"""
		Intended to be overridden by classes that inherit
		"""
		return str(id(self))

	@property
	def Uuid(self):
		"""
		Intended to be overridden by classes that inherit
		"""
		import uuid
		return uuid.uuid1()
