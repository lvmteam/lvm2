# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import sys
import threading
import traceback
import dbus
from . import cfg
from .utils import log_debug
from .automatedproperties import AutomatedProperties


# noinspection PyPep8Naming
class ObjectManager(AutomatedProperties):
	"""
	Implements the org.freedesktop.DBus.ObjectManager interface
	"""

	def __init__(self, object_path, interface):
		super(ObjectManager, self).__init__(object_path, interface)
		self.set_interface(interface)
		self._ap_o_path = object_path
		self._objects = {}
		self._id_to_object_path = {}
		self.rlock = threading.RLock()

	@dbus.service.method(
		dbus_interface="org.freedesktop.DBus.ObjectManager",
		out_signature='a{oa{sa{sv}}}')
	def GetManagedObjects(self):
		with self.rlock:
			rc = {}
			try:
				for k, v in list(self._objects.items()):
					path, props = v[0].emit_data()
					rc[path] = props
			except Exception:
				traceback.print_exc(file=sys.stdout)
				sys.exit(1)
			return rc

	def locked(self):
		"""
		If some external code need to run across a number of different
		calls into ObjectManager while blocking others they can use this method
		to lock others out.
		:return:
		"""
		return ObjectManagerLock(self.rlock)

	@dbus.service.signal(
		dbus_interface="org.freedesktop.DBus.ObjectManager",
		signature='oa{sa{sv}}')
	def InterfacesAdded(self, object_path, int_name_prop_dict):
		log_debug(
			('SIGNAL: InterfacesAdded(%s, %s)' %
			(str(object_path), str(int_name_prop_dict))))

	@dbus.service.signal(
		dbus_interface="org.freedesktop.DBus.ObjectManager",
		signature='oas')
	def InterfacesRemoved(self, object_path, interface_list):
		log_debug(('SIGNAL: InterfacesRemoved(%s, %s)' %
			(str(object_path), str(interface_list))))

	def _lookup_add(self, obj, path, lvm_id, uuid):
		"""
		Store information about what we added to the caches so that we
		can remove it cleanly
		:param obj:     The dbus object we are storing
		:param lvm_id:  The user name for the asset
		:param uuid:    The uuid for the asset
		:return:
		"""
		# Note: Only called internally, lock implied

		# We could have a temp entry from the forward creation of a path
		self._lookup_remove(path)

		self._objects[path] = (obj, lvm_id, uuid)
		self._id_to_object_path[lvm_id] = path

		if uuid:
			self._id_to_object_path[uuid] = path

	def _lookup_remove(self, obj_path):
		# Note: Only called internally, lock implied
		if obj_path in self._objects:
			(obj, lvm_id, uuid) = self._objects[obj_path]
			del self._id_to_object_path[lvm_id]
			del self._id_to_object_path[uuid]
			del self._objects[obj_path]

	def lookup_update(self, dbus_obj, new_uuid, new_lvm_id):
		with self.rlock:
			obj_path = dbus_obj.dbus_object_path()
			self._lookup_remove(obj_path)
			self._lookup_add(
				dbus_obj, obj_path,
				new_lvm_id, new_uuid)

	def object_paths_by_type(self, o_type):
		with self.rlock:
			rc = {}

			for k, v in list(self._objects.items()):
				if isinstance(v[0], o_type):
					rc[k] = True
			return rc

	def register_object(self, dbus_object, emit_signal=False):
		"""
		Given a dbus object add it to the collection
		:param dbus_object: Dbus object to register
		:param emit_signal: If true emit a signal for interfaces added
		"""
		with self.rlock:
			path, props = dbus_object.emit_data()

			# print 'Registering object path %s for %s' %
			# (path, dbus_object.lvm_id)

			# We want fast access to the object by a number of different ways
			# so we use multiple hashs with different keys
			self._lookup_add(dbus_object, path, dbus_object.lvm_id,
				dbus_object.Uuid)

			if emit_signal:
				self.InterfacesAdded(path, props)

	def remove_object(self, dbus_object, emit_signal=False):
		"""
		Given a dbus object, remove it from the collection and remove it
		from the dbus framework as well
		:param dbus_object:  Dbus object to remove
		:param emit_signal:  If true emit the interfaces removed signal
		"""
		with self.rlock:
			# Store off the object path and the interface first
			path = dbus_object.dbus_object_path()
			interfaces = dbus_object.interface()

			# print 'UN-Registering object path %s for %s' % \
			#      (path, dbus_object.lvm_id)

			self._lookup_remove(path)

			# Remove from dbus library
			dbus_object.remove_from_connection(cfg.bus, path)

			# Optionally emit a signal
			if emit_signal:
				self.InterfacesRemoved(path, interfaces)

	def get_object_by_path(self, path):
		"""
		Given a dbus path return the object registered for it
		:param path: The dbus path
		:return: The object
		"""
		with self.rlock:
			if path in self._objects:
				return self._objects[path][0]
			return None

	def get_object_by_uuid_lvm_id(self, uuid, lvm_id):
		with self.rlock:
			return self.get_object_by_path(
				self.get_object_path_by_lvm_id(uuid, lvm_id, None, False))

	def get_object_by_lvm_id(self, lvm_id):
		"""
		Given an lvm identifier, return the object registered for it
		:param lvm_id: The lvm identifier
		"""
		with self.rlock:
			if lvm_id in self._id_to_object_path:
				return self.get_object_by_path(self._id_to_object_path[lvm_id])
			return None

	def _uuid_verify(self, path, lvm_id, uuid):
		"""
		Ensure uuid is present for a successful lvm_id lookup
		NOTE: Internal call, assumes under object manager lock
		:param path: 		Path to object we looked up
		:param lvm_id:		lvm_id used to find object
		:param uuid: 		lvm uuid to verify
		:return: None
		"""
		# This gets called when we found an object based on lvm_id, ensure
		# uuid is correct too, as they can change
		if lvm_id != uuid:
			if uuid not in self._id_to_object_path:
				obj = self.get_object_by_path(path)
				self._lookup_add(obj, path, lvm_id, uuid)

	def get_object_path_by_lvm_id(self, uuid, lvm_id, path_create=None,
								gen_new=True):
		"""
		For a given lvm asset return the dbus object registered to it.  If the
		object is not found and gen_new == True and path_create is a valid
		function we will create a new path, register it and return it.
		:param uuid: The uuid for the lvm object
		:param lvm_id: The lvm name
		:param path_create: If true create an object path if not found
		:param gen_new: The function used to create the new path
		"""
		with self.rlock:
			assert lvm_id
			assert uuid

			if gen_new:
				assert path_create

			path = None

			if lvm_id in self._id_to_object_path:
				path = self._id_to_object_path[lvm_id]
				self._uuid_verify(path, lvm_id, uuid)
				return path
			if "/" in lvm_id:
				vg, lv = lvm_id.split("/", 1)
				int_lvm_id = vg + "/" + ("[%s]" % lv)
				if int_lvm_id in self._id_to_object_path:
					path = self._id_to_object_path[int_lvm_id]
					self._uuid_verify(path, int_lvm_id, uuid)
					return path

			if uuid and uuid in self._id_to_object_path:
				# If we get here it indicates that we found the object, but
				# the lvm_id lookup failed.  In the case of a rename, the uuid
				# will be correct, but the lvm_id will be wrong and vise versa.
				# If the lvm_id does not equal the uuid, lets fix up the table
				# so that lookups will be handled correctly.
				path = self._id_to_object_path[uuid]

				# In some cases we are looking up by one or the other, don't
				# update when they are the same.
				if uuid != lvm_id:
					obj = self.get_object_by_path(path)
					self._lookup_add(obj, path, lvm_id, uuid)
			else:
				if gen_new:
					path = path_create()
					self._lookup_add(None, path, lvm_id, uuid)

			# pprint('get_object_path_by_lvm_id(%s, %s, %s, %s: return %s' %
			#       (uuid, lvm_id, str(path_create), str(gen_new), path))

			return path


class ObjectManagerLock(object):
	"""
	The sole purpose of this class is to allow other code the ability to
	lock the object manager using a `with` statement, eg.

	with cfg.om.locked():
		# Do stuff with object manager

	This will ensure that the lock is always released (assuming this is done
	correctly)
	"""

	def __init__(self, recursive_lock):
		self._lock = recursive_lock

	def __enter__(self):
		# Acquire lock
		self._lock.acquire()

	# noinspection PyUnusedLocal
	def __exit__(self, e_type, e_value, e_traceback):
		# Release lock
		self._lock.release()
		self._lock = None
