#!/usr/bin/env python3

# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
import string
import random
import functools
import xml.etree.ElementTree as Et
from collections import OrderedDict
import dbus

BUSNAME = "com.redhat.lvmdbus1"
MANAGER_INT = BUSNAME + '.Manager'
MANAGER_OBJ = '/' + BUSNAME.replace('.', '/') + '/Manager'
PV_INT = BUSNAME + ".Pv"
VG_INT = BUSNAME + ".Vg"
LV_INT = BUSNAME + ".Lv"
THINPOOL_INT = BUSNAME + ".ThinPool"
SNAPSHOT_INT = BUSNAME + ".Snapshot"
LV_COMMON_INT = BUSNAME + ".LvCommon"
JOB_INT = BUSNAME + ".Job"
CACHE_POOL_INT = BUSNAME + ".CachePool"
CACHE_LV_INT = BUSNAME + ".CachedLv"

THINPOOL_LV_PATH = '/' + THINPOOL_INT.replace('.', '/')


def rs(length, suffix, character_set=string.ascii_lowercase):
	return ''.join(random.choice(character_set)
				for _ in range(length)) + suffix


def mib(s):
	return 1024 * 1024 * s


class DbusIntrospection(object):
	@staticmethod
	def introspect(xml_representation):
		interfaces = {}

		root = Et.fromstring(xml_representation)

		for c in root:
			if c.tag == "interface":
				in_f = c.attrib['name']
				interfaces[in_f] = \
					dict(methods=OrderedDict(), properties={})
				for nested in c:
					if nested.tag == "method":
						mn = nested.attrib['name']
						interfaces[in_f]['methods'][mn] = OrderedDict()

						for arg in nested:
							if arg.tag == 'arg':
								arg_dir = arg.attrib['direction']
								if arg_dir == 'in':
									n = arg.attrib['name']
								else:
									n = 'RETURN_VALUE'

								arg_type = arg.attrib['type']

								if n:
									v = dict(
											name=mn,
											a_dir=arg_dir,
											a_type=arg_type)
									interfaces[in_f]['methods'][mn][n] = v

					elif nested.tag == 'property':
						pn = nested.attrib['name']
						p_access = nested.attrib['access']
						p_type = nested.attrib['type']

						interfaces[in_f]['properties'][pn] = \
							dict(p_access=p_access, p_type=p_type)
					else:
						pass

		# print('Interfaces...')
		# for k, v in list(interfaces.items()):
		# 	print('Interface %s' % k)
		# 	if v['methods']:
		# 		for m, args in list(v['methods'].items()):
		# 			print('    method: %s' % m)
		# 			for a, aa in args.items():
		# 				print('         method arg: %s type %s' %
		# 					  (a, aa['a_type']))
		# 	if v['properties']:
		# 		for p, d in list(v['properties'].items()):
		# 			print('    Property: %s type= %s' % (p, d['p_type']))
		# print('End interfaces')

		return interfaces


def btsr(value):
	t = type(value)
	if t == dbus.Boolean:
		return 'b'
	elif t == dbus.ObjectPath:
		return 'o'
	elif t == dbus.String:
		return 's'
	elif t == dbus.Byte:
		return 'y'
	elif t == dbus.Int16:
		return 'n'
	elif t == dbus.Int32:
		return 'i'
	elif t == dbus.Int64:
		return 'x'
	elif t == dbus.UInt16:
		return 'q'
	elif t == dbus.UInt32:
		return 'u'
	elif t == dbus.UInt64:
		return 't'
	elif t == dbus.Double:
		return 'd'
	elif t == dbus.Struct:
		rc = '('
		for vt in value:
			rc += btsr(vt)
		rc += ')'
		return rc
	elif t == dbus.Array:
		rc = "a"
		for i in value:
			rc += btsr(i)
			break
		return rc
	else:
		raise RuntimeError("Unhandled type %s" % str(t))


def verify_type(value, dbus_str_rep):
	actual_str_rep = btsr(value)

	if dbus_str_rep != actual_str_rep:
		# print("%s ~= %s" % (dbus_str_rep, actual_str_rep))
		# Unless we have a full filled out type we won't match exactly
		if not dbus_str_rep.startswith(actual_str_rep):
			raise RuntimeError("Incorrect type, expected= %s actual "
								"= %s object= %s" %
								(dbus_str_rep, actual_str_rep,
								str(type(value))))


class RemoteObject(object):
	def _set_props(self, props=None):
		# print 'Fetching properties'
		if not props:
			# prop_fetch = dbus.Interface(self.bus.get_object(
			#    BUSNAME, self.object_path), 'org.freedesktop.DBus.Properties')

			for i in range(0, 3):
				try:
					prop_fetch = dbus.Interface(self.bus.get_object(
						BUSNAME, self.object_path),
						'org.freedesktop.DBus.Properties')
					props = prop_fetch.GetAll(self.interface)
					break
				except dbus.exceptions.DBusException as dbe:
					if "GetAll" not in str(dbe):
						raise dbe
		if props:
			for kl, vl in list(props.items()):
				# Verify type is correct!
				verify_type(vl,
					self.introspect[self.interface]['properties'][kl]['p_type'])
				setattr(self, kl, vl)

	def __init__(self, specified_bus, object_path, interface, introspect,
				 properties=None):
		self.object_path = object_path
		self.interface = interface
		self.bus = specified_bus
		self.introspect = introspect

		self.dbus_method = dbus.Interface(specified_bus.get_object(
			BUSNAME, self.object_path), self.interface)

		self._set_props(properties)

	def __getattr__(self, item):
		if hasattr(self.dbus_method, item):
			return functools.partial(self._wrapper, item)
		else:
			return functools.partial(self, item)

	def _wrapper(self, _method_name, *args, **kwargs):
		result = getattr(self.dbus_method, _method_name)(*args, **kwargs)
		# print("DEBUG: %s.%s result %s" %
		# (self.interface, _method_name, str(type(result))))

		if 'RETURN_VALUE' in self.introspect[
				self.interface]['methods'][_method_name]:
			r_type = self.introspect[
				self.interface]['methods'][
				_method_name]['RETURN_VALUE']['a_type']

			verify_type(result, r_type)

		return result

	def update(self):
		self._set_props()


class ClientProxy(object):
	@staticmethod
	def _intf_short_name(nm):
		return nm.split('.')[-1:][0]

	def __init__(self, specified_bus, object_path, interface=None, props=None):
		i = dbus.Interface(specified_bus.get_object(
			BUSNAME, object_path), 'org.freedesktop.DBus.Introspectable')

		introspection_xml = i.Introspect()

		# import xml.dom.minidom
		#
		# xml = xml.dom.minidom.parseString(introspection_xml)
		# print(xml.toprettyxml())

		self.intro_spect = DbusIntrospection.introspect(introspection_xml)

		for k in self.intro_spect.keys():
			sn = ClientProxy._intf_short_name(k)
			# print('Client proxy has interface: %s %s' % (k, sn))

			if interface and interface == k and props is not None:
				ro = RemoteObject(specified_bus, object_path, k,
								  self.intro_spect, props)
			else:
				ro = RemoteObject(specified_bus, object_path, k,
								  self.intro_spect)

			setattr(self, sn, ro)

		self.object_path = object_path

	def update(self):
		# Go through all interfaces and update them
		for int_f in self.intro_spect.keys():
			sn = ClientProxy._intf_short_name(int_f)
			getattr(self, sn).update()
