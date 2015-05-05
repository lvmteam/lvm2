#!/usr/bin/env python

# Copyright (C) 2012-2013 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

import unittest
import random
import string
import lvm
import os
import itertools
import sys

if sys.version_info[0] > 2:
	long = int

# Set of basic unit tests for the python bindings.
#
# *** WARNING ***
#
# This test tries to only modify configuration for the list of allowed
# PVs, but an error in it could potentially cause data loss if run on a
# production system.  Therefore it is strongly advised that this unit test
# not be run on a system that contains data of value.

fh = None


def l(txt):
	if os.environ.get('PY_UNIT_LOG') is not None:
		global fh
		if fh is None:
			fh = open('/tmp/lvm_py_unit_test_' + rs(10), "a")
		fh.write(txt + "\n")
		fh.flush()


def rs(rand_len=10):
	"""
	Generate a random string
	"""
	return ''.join(
		random.choice(string.ascii_uppercase)for x in range(rand_len))


def _get_allowed_devices():
	rc = os.environ.get('PY_UNIT_PVS')
	if rc is not None:
		rc = rc.splitlines()
		rc.sort()
	return rc


class AllowedPVS(object):
	"""
	We are only allowed to muck with certain PV, filter to only
	the ones we can use.
	"""

	def __init__(self):
		self.handle = None
		self.pvs_all = None

	def __enter__(self):
		rc = []

		allowed_dev = _get_allowed_devices()

		if allowed_dev:
			self.handle = lvm.listPvs()
			self.pvs_all = self.handle.open()

			for p in self.pvs_all:
				if p.getName() in allowed_dev:
					rc.append(p)

		#Sort them consistently
		rc.sort(key=lambda x: x.getName())
		return rc

	def __exit__(self, t_type, value, traceback):
		if self.handle:
			self.pvs_all = None
			self.handle.close()


class TestLvm(unittest.TestCase):

	VG_P = os.environ.get('PREFIX')

	@staticmethod
	def _get_pv_device_names():
		rc = []
		with AllowedPVS() as pvs:
			for p in pvs:
				rc.append(p.getName())
		return rc

	@staticmethod
	def _create_thick_lv(device_list, name):
		vg = lvm.vgCreate(TestLvm.VG_P + "_" + name)

		for d in device_list:
			vg.extend(d)

		vg.createLvLinear(name, vg.getSize() / 2)
		vg.close()
		vg = None

	@staticmethod
	def _create_thin_pool(device_list, pool_name):
		vg = lvm.vgCreate(TestLvm.VG_P + "_" + pool_name)

		for d in device_list:
			vg.extend(d)

		vg.createLvThinpool(
			pool_name, vg.getSize() / 2, 0, 0, lvm.THIN_DISCARDS_PASSDOWN, 1)
		return vg

	@staticmethod
	def _create_thin_lv(pv_devices, name):
		thin_pool_name = 'thin_vg_pool_' + rs(4)
		vg = TestLvm._create_thin_pool(pv_devices, thin_pool_name)
		vg.createLvThin(thin_pool_name, name, vg.getSize() / 8)
		vg.close()
		vg = None

	@staticmethod
	def _vg_names():
		rc = []
		vg_names = lvm.listVgNames()

		for i in vg_names:
			if i[0:len(TestLvm.VG_P)] == TestLvm.VG_P:
				rc.append(i)

		return rc

	@staticmethod
	def _get_lv(lv_vol_type=None, lv_name=None):
		vg_name_list = TestLvm._vg_names()
		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, "w")
			lvs = vg.listLVs()

			for lv in lvs:
				attr = lv.getAttr()
				if lv_vol_type or lv_name:
					if lv_vol_type is not None and attr[0] == lv_vol_type:
						return lv, vg
					elif lv_name is not None and lv_name == lv.getName():
						return lv, vg
				else:
					return lv, vg
			vg.close()
		return None, None

	@staticmethod
	def _remove_vg(vg_name):
		vg = lvm.vgOpen(vg_name, 'w')

		pvs = vg.listPVs()

		pe_devices = []

		#Remove old snapshots first, then lv
		for lv in vg.listLVs():
			attr = lv.getAttr()
			if attr[0] == 's':
				lv.remove()

		lvs = vg.listLVs()

		#Now remove any thin lVs
		for lv in vg.listLVs():
			attr = lv.getAttr()
			if attr[0] == 'V':
				lv.remove()

		#now remove the rest
		for lv in vg.listLVs():
			name = lv.getName()

			#Don't remove the hidden ones
			if '_tmeta' not in name and '_tdata' not in name:
				lv.remove()

		for p in pvs:
			pe_devices.append(p.getName())

		for pv in pe_devices[:-1]:
			vg.reduce(pv)

		vg.remove()
		vg.close()

	@staticmethod
	def _clean_up():
		#Clear out the testing PVs, but only if they contain stuff
		#this unit test created
		for vg_n in TestLvm._vg_names():
			TestLvm._remove_vg(vg_n)

		for d in TestLvm._get_pv_device_names():
			lvm.pvRemove(d)
			lvm.pvCreate(d)

	def setUp(self):
		device_list = TestLvm._get_pv_device_names()

		#Make sure we have an adequate number of PVs to use
		self.assertTrue(len(device_list) >= 4)
		TestLvm._clean_up()

	def tearDown(self):
		TestLvm._clean_up()

	def test_pv_resize(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			curr_size = pv.getSize()
			dev_size = pv.getDevSize()
			self.assertTrue(curr_size == dev_size)
			pv.resize(curr_size / 2)
		with AllowedPVS() as pvs:
			pv = pvs[0]
			resized_size = pv.getSize()
			self.assertTrue(resized_size != curr_size)
			pv.resize(dev_size)

	def test_pv_life_cycle(self):
		"""
		Test removing and re-creating a PV
		"""
		target_name = None

		with AllowedPVS() as pvs:
			pv = pvs[0]
			target_name = pv.getName()
			lvm.pvRemove(target_name)

		with AllowedPVS() as pvs:
			for p in pvs:
				self.assertTrue(p.getName() != target_name)

		lvm.pvCreate(target_name, 0)

		with AllowedPVS() as pvs:
			found = False
			for p in pvs:
				if p.getName() == target_name:
					found = True

		self.assertTrue(found)

	@staticmethod
	def test_pv_methods():
		with AllowedPVS() as pvs:
			for p in pvs:
				p.getName()
				p.getUuid()
				p.getMdaCount()
				p.getSize()
				p.getDevSize()
				p.getFree()
				p = None

	def test_version(self):
		version = lvm.getVersion()
		self.assertNotEquals(version, None)
		self.assertEquals(type(version), str)
		self.assertTrue(len(version) > 0)

	def test_pv_getters(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			self.assertEqual(type(pv.getName()), str)
			self.assertTrue(len(pv.getName()) > 0)

			self.assertEqual(type(pv.getUuid()), str)
			self.assertTrue(len(pv.getUuid()) > 0)

			self.assertTrue(
				type(pv.getMdaCount()) == int or
				type(pv.getMdaCount()) == long)

			self.assertTrue(
				type(pv.getSize()) == int or
				type(pv.getSize()) == long)

			self.assertTrue(
				type(pv.getDevSize()) == int or
				type(pv.getSize()) == long)

			self.assertTrue(
				type(pv.getFree()) == int or
				type(pv.getFree()) == long)

	def _test_prop(self, prop_obj, prop, var_type, settable):
		result = prop_obj.getProperty(prop)

		#If we have no string value we can get a None type back
		if result[0] is not None:
			self.assertEqual(type(result[0]), var_type)
		else:
			self.assertTrue(str == var_type)
		self.assertEqual(type(result[1]), bool)
		self.assertTrue(result[1] == settable)

	def test_pv_segs(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			pv_segs = pv.listPVsegs()

			#LVsegs returns a tuple, (value, bool settable)
			#TODO: Test other properties of pv_seg
			for i in pv_segs:
				self._test_prop(i, 'pvseg_start', long, False)

	def test_pv_property(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			self._test_prop(pv, 'pv_mda_count', long, False)

	def test_lv_property(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		lv_seg_properties = [
			('chunk_size', long, False), ('devices', str, False),
			('discards', str, False), ('region_size', long, False),
			('segtype', str, False), ('seg_pe_ranges', str, False),
			('seg_size', long, False), ('seg_size_pe', long, False),
			('seg_start', long, False), ('seg_start_pe', long, False),
			('seg_tags', str, False), ('stripes', long, False),
			('stripe_size', long, False), ('thin_count', long, False),
			('transaction_id', long, False), ('zero', long, False)]

		lv_properties = [
			('convert_lv', str, False), ('copy_percent', long, False),
			('data_lv', str, False), ('lv_attr', str, False),
			('lv_host', str, False), ('lv_kernel_major', long, False),
			('lv_kernel_minor', long, False),
			('lv_kernel_read_ahead', long, False),
			('lv_major', long, False), ('lv_minor', long, False),
			('lv_name', str, False), ('lv_path', str, False),
			('lv_profile', str, False), ('lv_read_ahead', long, False),
			('lv_size', long, False), ('lv_tags', str, False),
			('lv_time', str, False), ('lv_uuid', str, False),
			('metadata_lv', str, False), ('mirror_log', str, False),
			('lv_modules', str, False), ('move_pv', str, False),
			('origin', str, False), ('origin_size', long, False),
			('pool_lv', str, False), ('raid_max_recovery_rate', long, False),
			('raid_min_recovery_rate', long, False),
			('raid_mismatch_count', long, False),
			('raid_sync_action', str, False),
			('raid_write_behind', long, False), ('seg_count', long, False),
			('snap_percent', long, False), ('sync_percent', long, False)]

		# Generic test case, make sure we get what we expect
		for t in lv_properties:
			self._test_prop(lv, *t)

		segments = lv.listLVsegs()
		if segments and len(segments):
			for s in segments:
				for t in lv_seg_properties:
					self._test_prop(s, *t)

		# Test specific cases
		tag = 'hello_world'
		lv.addTag(tag)
		tags = lv.getProperty('lv_tags')
		self.assertTrue(tag in tags[0])
		vg.close()

	def test_lv_tags(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)
		self._test_tags(lv)
		vg.close()

	def test_lv_active_inactive(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)
		lv.deactivate()
		self.assertTrue(lv.isActive() is False)
		lv.activate()
		self.assertTrue(lv.isActive() is True)
		vg.close()

	def test_lv_rename(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		current_name = lv.getName()
		new_name = rs()
		lv.rename(new_name)
		self.assertEqual(lv.getName(), new_name)
		lv.rename(current_name)
		vg.close()

	def test_lv_persistence(self):
		# Make changes to the lv, close the vg and re-open to make sure that
		# the changes persist
		lv_name = 'lv_test_persist'
		TestLvm._create_thick_lv(TestLvm._get_pv_device_names(), lv_name)

		# Test rename
		lv, vg = TestLvm._get_lv(None, lv_name)
		current_name = lv.getName()
		new_name = rs()
		lv.rename(new_name)

		vg.close()
		vg = None

		lv, vg = TestLvm._get_lv(None, new_name)

		self.assertTrue(lv is not None)

		if lv and vg:
			lv.rename(lv_name)
			vg.close()
			vg = None

		# Test lv tag add
		tag = 'hello_world'

		lv, vg = TestLvm._get_lv(None, lv_name)
		lv.addTag(tag)
		vg.close()
		vg = None

		lv, vg = TestLvm._get_lv(None, lv_name)
		tags = lv.getTags()

		self.assertTrue(tag in tags)
		vg.close()
		vg = None

		# Test lv tag delete
		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)

		if lv and vg:
			tags = lv.getTags()

			for t in tags:
				lv.removeTag(t)

			vg.close()
			vg = None

		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)

		if lv and vg:
			tags = lv.getTags()

			if tags:
				self.assertEqual(len(tags), 0)
			vg.close()
			vg = None

		# Test lv deactivate
		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)

		if lv and vg:
			lv.deactivate()
			vg.close()
			vg = None

		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)
		if lv and vg:
			self.assertFalse(lv.isActive())
			vg.close()
			vg = None

		# Test lv activate
		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)
		if lv and vg:
			lv.activate()
			vg.close()
			vg = None

		lv, vg = TestLvm._get_lv(None, lv_name)
		self.assertTrue(lv is not None and vg is not None)
		if lv and vg:
			self.assertTrue(lv.isActive())
			vg.close()
			vg = None

	def test_lv_snapshot(self):

		thin_lv = 'thin_lv'
		thick_lv = 'thick_lv'

		device_names = TestLvm._get_pv_device_names()

		TestLvm._create_thin_lv(device_names[0:2], thin_lv)
		TestLvm._create_thick_lv(device_names[2:4], thick_lv)

		lv, vg = TestLvm._get_lv(None, thick_lv)
# FIXME		lv.snapshot('thick_snap_shot', 1024*1024)
		vg.close()

# FIXME		thick_ss, vg = TestLvm._get_lv(None, 'thick_snap_shot')
# FIXME		self.assertTrue(thick_ss is not None)
# FIXME		vg.close()

		thin_lv, vg = TestLvm._get_lv(None, thin_lv)
		thin_lv.snapshot('thin_snap_shot')
		vg.close()

		thin_ss, vg = TestLvm._get_lv(None, 'thin_snap_shot')
		self.assertTrue(thin_ss is not None)

		origin = thin_ss.getOrigin()
		self.assertTrue(thin_lv, origin)

		vg.close()

	def test_lv_suspend(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		result = lv.isSuspended()
		self.assertTrue(type(result) == bool)
		vg.close()

	def test_lv_size(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		result = lv.getSize()
		self.assertTrue(type(result) == int or type(result) == long)
		vg.close()

	def test_lv_resize(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		curr_size = lv.getSize()
		lv.resize(curr_size + (1024 * 1024))
		latest = lv.getSize()
		self.assertTrue(curr_size != latest)

	def test_lv_seg(self):
		lv_name = 'lv_test'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		lv_segs = lv.listLVsegs()

		#LVsegs returns a tuple, (value, bool settable)
		#TODO: Test other properties of lv_seg
		for i in lv_segs:
			self._test_prop(i, 'seg_start_pe', long, False)

		vg.close()

	def test_get_set_extend_size(self):
		thick_lv = 'get_set_prop'
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thick_lv(device_names[0:2], thick_lv)
		lv, vg = TestLvm._get_lv(None, thick_lv)

		new_extent = 1024 * 1024 * 4

		self.assertFalse(
			vg.getExtentSize() != new_extent,
			"Cannot determine if it works if they are the same")

		vg.setExtentSize(new_extent)
		self.assertEqual(vg.getExtentSize(), new_extent)
		vg.close()

	def test_vg_get_set_prop(self):
		thick_lv = 'get_set_prop'
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thick_lv(device_names[0:2], thick_lv)
		lv, vg = TestLvm._get_lv(None, thick_lv)

		self.assertTrue(vg is not None)
		if vg:
			vg_mda_copies = vg.getProperty('vg_mda_copies')
			vg.setProperty('vg_mda_copies', vg_mda_copies[0])
			vg.close()

	def test_vg_remove_restore(self):
		#Store off the list of physical devices
		pv_devices = []

		thick_lv = 'get_set_prop'
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thick_lv(device_names[0:2], thick_lv)
		lv, vg = TestLvm._get_lv(None, thick_lv)

		vg_name = vg.getName()

		pvs = vg.listPVs()
		for p in pvs:
			pv_devices.append(p.getName())
		vg.close()

		TestLvm._remove_vg(vg_name)
		self._create_thick_lv(pv_devices, thick_lv)

	def test_vg_names(self):
		vg = lvm.listVgNames()
		self.assertTrue(isinstance(vg, tuple))

	def test_dupe_lv_create(self):
		"""
		Try to create a lv with the same name expecting a failure
		Note: This was causing a seg. fault previously
		"""
		thick_lv = 'dupe_name'
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thick_lv(device_names[0:2], thick_lv)
		lv, vg = TestLvm._get_lv(None, thick_lv)

		self.assertTrue(vg is not None)

		if vg:
			lvs = vg.listLVs()

			if len(lvs):
				lv = lvs[0]
				lv_name = lv.getName()
				self.assertRaises(
					lvm.LibLVMError, vg.createLvLinear, lv_name, lv.getSize())
			vg.close()

	def test_vg_uuids(self):

		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vgs_uuids = lvm.listVgUuids()

		self.assertTrue(len(vgs_uuids) > 0)
		self.assertTrue(isinstance(vgs_uuids, tuple))

		vgs_uuids = list(vgs_uuids)
		vgs_names = lvm.listVgNames()

		for vg_name in vgs_names:
			vg = lvm.vgOpen(vg_name, "r")

			#TODO Write/fix BUG, vg uuid don't match between
			#lvm.listVgUuids and vg.getUuid()
			vg_uuid_search = vg.getUuid().replace('-', '')

			self.assertTrue(vg_uuid_search in vgs_uuids)
			vgs_uuids.remove(vg_uuid_search)
			vg.close()

		self.assertTrue(len(vgs_uuids) == 0)

	def test_pv_lookup_from_vg(self):
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vg_names = TestLvm._vg_names()

		self.assertTrue(len(vg_names) > 0)

		for vg_name in vg_names:
			vg = lvm.vgOpen(vg_name, 'w')
			pvs = vg.listPVs()

			for p in pvs:
				name = p.getName()
				uuid = p.getUuid()

				pv_name_lookup = vg.pvFromName(name)
				pv_uuid_lookup = vg.pvFromUuid(uuid)

				self.assertTrue(
					pv_name_lookup.getName() == pv_uuid_lookup.getName())
				self.assertTrue(
					pv_name_lookup.getUuid() == pv_uuid_lookup.getUuid())

				self.assertTrue(name == pv_name_lookup.getName())
				self.assertTrue(uuid == pv_uuid_lookup.getUuid())

				pv_name_lookup = None
				pv_uuid_lookup = None
				p = None

			pvs = None
			vg.close()

	def test_percent_to_float(self):
		self.assertEqual(lvm.percentToFloat(0), 0.0)
		self.assertEqual(lvm.percentToFloat(1000000), 1.0)
		self.assertEqual(lvm.percentToFloat(1000000 / 2), 0.5)

	def test_scan(self):
		self.assertEqual(lvm.scan(), None)

	def test_config_reload(self):
		self.assertEqual(lvm.configReload(), None)

	def test_config_override(self):
		self.assertEquals(lvm.configOverride("global.test = 1"), None)

	def test_config_find_bool(self):
		either_or = lvm.configFindBool("global/fallback_to_local_locking")
		self.assertTrue(type(either_or) == bool)
		self.assertTrue(lvm.configFindBool("global/locking_type"))

	def test_vg_from_pv_lookups(self):
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vgname_list = TestLvm._vg_names()

		self.assertTrue(len(vgname_list) > 0)

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')

			vg_name = vg.getName()

			pv_list = vg.listPVs()
			for pv in pv_list:
				vg_name_from_pv = lvm.vgNameFromPvid(pv.getUuid())
				self.assertEquals(vg_name, vg_name_from_pv)
				self.assertEqual(vg_name, lvm.vgNameFromDevice(pv.getName()))
			vg.close()

	def test_vg_get_name(self):
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vgname_list = TestLvm._vg_names()

		self.assertTrue(len(vgname_list) > 0)

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')
			self.assertEqual(vg.getName(), vg_name)
			vg.close()

	def test_vg_get_uuid(self):
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vgname_list = TestLvm._vg_names()

		self.assertTrue(len(vgname_list) > 0)

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')
			uuid = vg.getUuid()
			self.assertNotEqual(uuid, None)
			self.assertTrue(len(uuid) > 0)
			vg.close()

	RETURN_NUMERIC = [
		"getSeqno", "getSize", "getFreeSize", "getFreeSize",
		"getExtentSize", "getExtentCount", "getFreeExtentCount",
		"getPvCount", "getMaxPv", "getMaxLv"]

	def test_vg_getters(self):
		device_names = TestLvm._get_pv_device_names()
		TestLvm._create_thin_lv(device_names[0:2], 'thin')
		TestLvm._create_thick_lv(device_names[2:4], 'thick')

		vg_name_list = TestLvm._vg_names()

		self.assertTrue(len(vg_name_list) > 0)

		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, 'r')
			self.assertTrue(type(vg.isClustered()) == bool)
			self.assertTrue(type(vg.isExported()) == bool)
			self.assertTrue(type(vg.isPartial()) == bool)

			#Loop through the list invoking the method
			for method_name in TestLvm.RETURN_NUMERIC:
				method = getattr(vg, method_name)
				result = method()
				self.assertTrue(type(result) == int or type(result) == long)

			vg.close()

	def _test_tags(self, tag_obj):
		existing_tags = tag_obj.getTags()
		self.assertTrue(type(existing_tags) == tuple)

		num_tags = random.randint(2, 40)
		created_tags = []

		for i in range(num_tags):
			tag_name = rs(random.randint(1, 128))
			tag_obj.addTag(tag_name)
			created_tags.append(tag_name)

		tags = tag_obj.getTags()
		self.assertTrue(len(existing_tags) + len(created_tags) == len(tags))

		num_remove = len(created_tags)

		for i in range(num_remove):
			tag_to_remove = created_tags[
				random.randint(0, len(created_tags) - 1)]

			created_tags.remove(tag_to_remove)

			tag_obj.removeTag(tag_to_remove)

			current_tags = tag_obj.getTags()
			self.assertFalse(tag_to_remove in current_tags)

		current_tags = tag_obj.getTags()
		self.assertTrue(len(current_tags) == len(existing_tags))
		for e in existing_tags:
			self.assertTrue(e in current_tags)

	def test_vg_tags(self):
		device_names = TestLvm._get_pv_device_names()

		i = 0
		for d in device_names:
			if i % 2 == 0:
				TestLvm._create_thin_lv([d], "thin_lv%d" % i)
			else:
				TestLvm._create_thick_lv([d], "thick_lv%d" % i)
			i += 1

		for vg_name in TestLvm._vg_names():
			vg = lvm.vgOpen(vg_name, 'w')
			self._test_tags(vg)
			vg.close()

	@staticmethod
	def test_listing():

		env = os.environ

		for k, v in env.items():
			l("%s:%s" % (k, v))

		with lvm.listPvs() as pvs:
			for p in pvs:
				l('pv= %s' % p.getName())

		l('Checking for VG')
		for v in lvm.listVgNames():
			l('vg= %s' % v)

	def test_pv_empty_listing(self):
		#We had a bug where we would seg. fault if we had no PVs.

		l('testPVemptylisting entry')

		device_names = TestLvm._get_pv_device_names()

		for d in device_names:
			l("Removing %s" % d)
			lvm.pvRemove(d)

		count = 0

		with lvm.listPvs() as pvs:
			for p in pvs:
				count += 1
				l('pv= %s' % p.getName())

		self.assertTrue(count == 0)

		for d in device_names:
			lvm.pvCreate(d)

	def test_pv_create(self):
		size = [0, 1024 * 1024 * 4]
		pvmeta_copies = [0, 1, 2]
		pvmeta_size = [0, 255, 512, 1024]
		data_alignment = [0, 2048, 4096]
		data_alignment_offset = [1, 1, 1]
		zero = [0, 1]

		device_names = TestLvm._get_pv_device_names()

		for d in device_names:
			lvm.pvRemove(d)

		d = device_names[0]

		#Test some error cases
		self.assertRaises(TypeError, lvm.pvCreate, None)
		self.assertRaises(lvm.LibLVMError, lvm.pvCreate, '')
		self.assertRaises(lvm.LibLVMError, lvm.pvCreate, d, 4)
		self.assertRaises(lvm.LibLVMError, lvm.pvCreate, d, 0, 4)
		self.assertRaises(lvm.LibLVMError, lvm.pvCreate, d, 0, 0, 0, 2 ** 34)
		self.assertRaises(
			lvm.LibLVMError, lvm.pvCreate, d, 0, 0, 0, 4096, 2 ** 34)

		#Try a number of combinations and permutations
		for s in size:
			lvm.pvCreate(d, s)
			lvm.pvRemove(d)
			for copies in pvmeta_copies:
				lvm.pvCreate(d, s, copies)
				lvm.pvRemove(d)
				for pv_size in pvmeta_size:
					lvm.pvCreate(d, s, copies, pv_size)
					lvm.pvRemove(d)
					for align in data_alignment:
						lvm.pvCreate(d, s, copies, pv_size, align)
						lvm.pvRemove(d)
						for align_offset in data_alignment_offset:
							lvm.pvCreate(
								d, s, copies, pv_size, align,
								align * align_offset)
							lvm.pvRemove(d)
							for z in zero:
								lvm.pvCreate(
									d, s, copies, pv_size, align,
									align * align_offset, z)
								lvm.pvRemove(d)

		#Restore
		for d in device_names:
			lvm.pvCreate(d)

	def test_vg_reduce(self):
		# Test the case where we try to reduce a vg where the last PV has
		# no metadata copies.  In this case the reduce should fail.
		vg_name = TestLvm.VG_P + 'reduce_test'

		device_names = TestLvm._get_pv_device_names()

		for d in device_names:
			lvm.pvRemove(d)

		lvm.pvCreate(device_names[0], 0, 0)  # Size all, pvmetadatacopies 0
		lvm.pvCreate(device_names[1])
		lvm.pvCreate(device_names[2])
		lvm.pvCreate(device_names[3])

		vg = lvm.vgCreate(vg_name)

		vg.extend(device_names[3])
		vg.extend(device_names[2])
		vg.extend(device_names[1])
		vg.extend(device_names[0])
		vg.close()

		vg = None

		vg = lvm.vgOpen(vg_name, 'w')

		vg.reduce(device_names[3])
		vg.reduce(device_names[2])

		self.assertRaises(lvm.LibLVMError, vg.reduce, device_names[1])

		vg.close()
		vg = None

		vg = lvm.vgOpen(vg_name, 'w')
		vg.remove()
		vg.close()

	@staticmethod
	def _test_valid_names(method):
		sample = 'azAZ09._-+'

		method('x' * 127)
		method('.X')
		method('..X')

		for i in range(1, 7):
			tests = (''.join(i) for i in itertools.product(sample, repeat=i))
			for t in tests:
				if t == '.' or t == '..':
					t += 'X'
				elif t.startswith('-'):
					t = 'H' + t
				method(t)

	def _test_bad_names(self, method, dupe_name):
		# Test for duplicate name
		self.assertRaises(lvm.LibLVMError, method, dupe_name)

		# Test for too long a name
		self.assertRaises(lvm.LibLVMError, method, ('x' * 128))

		# Test empty
		self.assertRaises(lvm.LibLVMError, method, '')

		# Invalid characters
		self.assertRaises(lvm.LibLVMError, method, '&invalid^char')

		# Cannot start with .. and no following characters
		self.assertRaises(lvm.LibLVMError, method, '..')

		# Cannot start with . and no following characters
		self.assertRaises(lvm.LibLVMError, method, '.')

		# Cannot start with a hyphen
		self.assertRaises(lvm.LibLVMError, method, '-not_good')

	def _lv_reserved_names(self, method):
		prefixes = ['snapshot', 'pvmove']
		reserved = [
			'_mlog', '_mimage', '_pmspare', '_rimage', '_rmeta',
			'_vorigin', '_tdata', '_tmeta']

		for p in prefixes:
			self.assertRaises(lvm.LibLVMError, method, p + rs(3))

		for r in reserved:
			self.assertRaises(lvm.LibLVMError, method, rs(3) + r + rs(1))
			self.assertRaises(lvm.LibLVMError, method, r + rs(1))

	def test_vg_lv_name_validate(self):
		lv_name = 'vg_lv_name_validate'
		TestLvm._create_thin_lv(TestLvm._get_pv_device_names(), lv_name)
		lv, vg = TestLvm._get_lv(None, lv_name)

		self._test_bad_names(lvm.vgNameValidate, vg.getName())
		self._test_bad_names(vg.lvNameValidate, lv.getName())

		# Test good values
		TestLvm._test_valid_names(lvm.vgNameValidate)
		TestLvm._test_valid_names(vg.lvNameValidate)
		self._lv_reserved_names(vg.lvNameValidate)

		vg.close()

if __name__ == "__main__":
	unittest.main()
