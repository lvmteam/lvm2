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

# Set of basic unit tests for the python bindings.
#
# *** WARNING ***
#
# This test tries to only modify configuration for the list of allowed
# PVs, but an error in it could potentially cause data loss if run on a
# production system.  Therefore it is strongly advised that this unit test
# not be run on a system that contains data of value.


def rs(l=10):
	"""
	Generate a random string
	"""
	return ''.join(random.choice(string.ascii_uppercase) for x in range(l))


def _get_allowed_devices():
	rc = os.environ.get('PY_UNIT_PVS')
	if rc is not None:
		rc = rc.split(' ')
		rc.sort()
	return rc


def compare_pv(r, l):
	r_name = r.getName()
	l_name = l.getName()

	if r_name > l_name:
		return 1
	elif r_name == l_name:
		return 0
	else:
		return -1


class AllowedPVS(object):
	"""
	We are only allowed to muck with certain PV, filter to only
	the ones we can use.
	"""

	def __init__(self):
		self.handle = None

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
		rc.sort(compare_pv)
		return rc

	def __exit__(self, t_type, value, traceback):
		if self.handle:
			self.pvs_all = None
			self.handle.close()


class TestLvm(unittest.TestCase):

	VG_P = 'py_unit_test_'

	def _get_pv_device_names(self):
		rc = []
		with AllowedPVS() as pvs:
			for p in pvs:
				rc.append(p.getName())
		return rc

	def _createThickLV(self, device_list, name):
		vg = lvm.vgCreate(TestLvm.VG_P + "_" + name)

		for d in device_list:
			vg.extend(d)

		vg.createLvLinear(name, vg.getSize()/2)
		vg.close()
		vg = None

	def _createThinPool(self, device_list, pool_name):
		vg = lvm.vgCreate(TestLvm.VG_P + "_" + pool_name)

		for d in device_list:
			vg.extend(d)

		vg.createLvThinpool(pool_name, vg.getSize()/2, 0, 0,
							lvm.THIN_DISCARDS_PASSDOWN, 1)
		return vg

	def _createThinLV(self, pv_devices, name):
		thin_pool_name = 'thin_vg_pool_' + rs(4)
		vg = self._createThinPool(pv_devices, thin_pool_name)
		vg.createLvThin(thin_pool_name, name, vg.getSize()/8)
		vg.close()
		vg = None

	def _vg_names(self):
		rc = []
		vg_names = lvm.listVgNames()

		for i in vg_names:
			if i[0:len(TestLvm.VG_P)] == TestLvm.VG_P:
				rc.append(i)

		return rc

	def _get_lv(self, lv_vol_type=None, lv_name=None):
		vg_name_list = self._vg_names()
		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, "w")
			lvs = vg.listLVs()

			for l in lvs:
				attr = l.getAttr()
				if lv_vol_type or lv_name:
					if lv_vol_type is not None and attr[0] == lv_vol_type:
						return l, vg
					elif lv_name is not None and lv_name == l.getName():
						return l, vg
				else:
					return l, vg
			vg.close()
		return None, None

	def _remove_VG(self, vg_name):
		vg = lvm.vgOpen(vg_name, 'w')

		pvs = vg.listPVs()

		pe_devices = []

		#Remove old snapshots first, then lv
		for l in vg.listLVs():
			attr = l.getAttr()
			if attr[0] == 's':
				l.remove()

		lvs = vg.listLVs()

		#Now remove any thin lVs
		for l in vg.listLVs():
			attr = l.getAttr()
			if attr[0] == 'V':
				l.remove()

		#now remove the rest
		for l in vg.listLVs():
			name = l.getName()

			#Don't remove the hidden ones
			if 'tmeta' not in name and 'tdata' not in name:
				l.remove()

		for p in pvs:
			pe_devices.append(p.getName())

		for pv in pe_devices:
			vg.reduce(pv)

		vg.remove()
		vg.close()

	def _clean_up(self):
		#Clear out the testing PVs, but only if they contain stuff
		#this unit test created
		for vg_n in self._vg_names():
			self._remove_VG(vg_n)

	def setUp(self):
		device_list = self._get_pv_device_names()

		#Make sure we have an adequate number of PVs to use
		self.assertTrue(len(device_list) >= 4)
		self._clean_up()

	def tearDown(self):
		self._clean_up()

	def testPVresize(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			curr_size = pv.getSize()
			dev_size = pv.getDevSize()
			self.assertTrue(curr_size == dev_size)
			pv.resize(curr_size/2)
		with AllowedPVS() as pvs:
			pv = pvs[0]
			resized_size = pv.getSize()
			self.assertTrue(resized_size != curr_size)
			pv.resize(dev_size)

	def testPVlifecycle(self):
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

	def testPvMethods(self):
		with AllowedPVS() as pvs:
			for p in pvs:
				p.getName()
				p.getUuid()
				p.getMdaCount()
				p.getSize()
				p.getDevSize()
				p.getFree()
				p = None

	def testVersion(self):
		version = lvm.getVersion()
		self.assertNotEquals(version, None)
		self.assertEquals(type(version), str)
		self.assertTrue(len(version) > 0)

	def testPvGetters(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			self.assertEqual(type(pv.getName()), str)
			self.assertTrue(len(pv.getName()) > 0)

			self.assertEqual(type(pv.getUuid()), str)
			self.assertTrue(len(pv.getUuid()) > 0)

			self.assertTrue(type(pv.getMdaCount()) == int or
							type(pv.getMdaCount()) == long)

			self.assertTrue(type(pv.getSize()) == int or
							type(pv.getSize()) == long)

			self.assertTrue(type(pv.getDevSize()) == int or
							type(pv.getSize()) == long)

			self.assertTrue(type(pv.getFree()) == int or
							type(pv.getFree()) == long)

	def _test_prop(self, prop_obj, prop, var_type, settable):
		result = prop_obj.getProperty(prop)

		self.assertEqual(type(result[0]), var_type)
		self.assertEqual(type(result[1]), bool)
		self.assertTrue(result[1] == settable)

	def testPvSegs(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			pv_segs = pv.listPVsegs()

			#LVsegs returns a tuple, (value, bool settable)
			#TODO: Test other properties of pv_seg
			for i in pv_segs:
				self._test_prop(i, 'pvseg_start', long, False)

	def testPvProperty(self):
		with AllowedPVS() as pvs:
			pv = pvs[0]
			self._test_prop(pv, 'pv_mda_count', long, False)

	def testLvProperty(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)
		self._test_prop(lv, 'seg_count', long, False)
		vg.close()

	def testLvTags(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)
		self._testTags(lv)
		vg.close()

	def testLvActiveInactive(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)
		lv.deactivate()
		self.assertTrue(lv.isActive() is False)
		lv.activate()
		self.assertTrue(lv.isActive() is True)
		vg.close()

	def testLvRename(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)

		current_name = lv.getName()
		new_name = rs()
		lv.rename(new_name)
		self.assertEqual(lv.getName(), new_name)
		lv.rename(current_name)
		vg.close()

	def testLvSnapshot(self):

		thin_lv = 'thin_lv'
		thick_lv = 'thick_lv'

		device_names = self._get_pv_device_names()

		self._createThinLV(device_names[0:2], thin_lv)
		self._createThickLV(device_names[2:4], thick_lv)

		lv, vg = self._get_lv(None, thick_lv)
		lv.snapshot('thick_snap_shot', 1024*1024)
		vg.close()

		thick_ss, vg = self._get_lv(None, 'thick_snap_shot')
		self.assertTrue(thick_ss is not None)
		vg.close()

		thin_lv, vg = self._get_lv(None, thin_lv)
		thin_lv.snapshot('thin_snap_shot')
		vg.close()

		thin_ss, vg = self._get_lv(None, 'thin_snap_shot')
		self.assertTrue(thin_ss is not None)

		origin = thin_ss.getOrigin()
		self.assertTrue(thin_lv, origin)

		vg.close()

	def testLvSuspend(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)

		result = lv.isSuspended()
		self.assertTrue(type(result) == bool)
		vg.close()

	def testLvSize(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)

		result = lv.getSize()
		self.assertTrue(type(result) == int or type(result) == long)
		vg.close()

	def testLvResize(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)

		curr_size = lv.getSize()
		lv.resize(curr_size+(1024*1024))
		latest = lv.getSize()
		self.assertTrue(curr_size != latest)

	def testLvSeg(self):
		lv_name = 'lv_test'
		self._createThinLV(self._get_pv_device_names(), lv_name)
		lv, vg = self._get_lv(None, lv_name)

		lv_segs = lv.listLVsegs()

		#LVsegs returns a tuple, (value, bool settable)
		#TODO: Test other properties of lv_seg
		for i in lv_segs:
			self._test_prop(i, 'seg_start_pe', long, False)

		vg.close()

	def testGetSetExtentSize(self):
		thick_lv = 'get_set_prop'
		device_names = self._get_pv_device_names()
		self._createThickLV(device_names[0:2], thick_lv)
		lv, vg = self._get_lv(None, thick_lv)

		new_extent = 1024 * 1024 * 4

		self.assertFalse(vg.getExtentSize() != new_extent,
						 "Cannot determine if it works if they are the same")

		vg.setExtentSize(new_extent)
		self.assertEqual(vg.getExtentSize(), new_extent)
		vg.close()

	def testVGsetGetProp(self):
		thick_lv = 'get_set_prop'
		device_names = self._get_pv_device_names()
		self._createThickLV(device_names[0:2], thick_lv)
		lv, vg = self._get_lv(None, thick_lv)

		self.assertTrue(vg is not None)
		if vg:
			vg_mda_copies = vg.getProperty('vg_mda_copies')
			vg.setProperty('vg_mda_copies', vg_mda_copies[0])
			vg.close()

	def testVGremoveRestore(self):
		#Store off the list of physical devices
		pv_devices = []

		thick_lv = 'get_set_prop'
		device_names = self._get_pv_device_names()
		self._createThickLV(device_names[0:2], thick_lv)
		lv, vg = self._get_lv(None, thick_lv)

		vg_name = vg.getName()

		pvs = vg.listPVs()
		for p in pvs:
			pv_devices.append(p.getName())
		vg.close()

		self._remove_VG(vg_name)
		self._createThickLV(pv_devices, thick_lv)

	def testVgNames(self):
		vg = lvm.listVgNames()
		self.assertTrue(isinstance(vg, tuple))

	def testDupeLvCreate(self):
		"""
		Try to create a lv with the same name expecting a failure
		Note: This was causing a seg. fault previously
		"""
		thick_lv = 'dupe_name'
		device_names = self._get_pv_device_names()
		self._createThickLV(device_names[0:2], thick_lv)
		lv, vg = self._get_lv(None, thick_lv)

		self.assertTrue(vg is not None)

		if vg:
			lvs = vg.listLVs()

			if len(lvs):
				lv = lvs[0]
				lv_name = lv.getName()
				self.assertRaises(lvm.LibLVMError, vg.createLvLinear, lv_name,
								  lv.getSize())
			vg.close()

	def testVgUuids(self):

		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

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

	def testPvLookupFromVG(self):
		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

		vg_names = self._vg_names()

		self.assertTrue(len(vg_names) > 0)

		for vg_name in vg_names:
			vg = lvm.vgOpen(vg_name, 'w')
			pvs = vg.listPVs()

			for p in pvs:
				name = p.getName()
				uuid = p.getUuid()

				pv_name_lookup = vg.pvFromName(name)
				pv_uuid_lookup = vg.pvFromUuid(uuid)

				self.assertTrue(pv_name_lookup.getName() ==
								pv_uuid_lookup.getName())
				self.assertTrue(pv_name_lookup.getUuid() ==
								pv_uuid_lookup.getUuid())

				self.assertTrue(name == pv_name_lookup.getName())
				self.assertTrue(uuid == pv_uuid_lookup.getUuid())

				pv_name_lookup = None
				pv_uuid_lookup = None
				p = None

			pvs = None
			vg.close()

	def testPercentToFloat(self):
		self.assertEqual(lvm.percentToFloat(0), 0.0)
		self.assertEqual(lvm.percentToFloat(1000000), 1.0)
		self.assertEqual(lvm.percentToFloat(1000000 / 2), 0.5)

	def testScan(self):
		self.assertEqual(lvm.scan(), None)

	def testConfigReload(self):
		self.assertEqual(lvm.configReload(), None)

	def testConfig_override(self):
		self.assertEquals(lvm.configOverride("global.test = 1"), None)

	def testConfigFindBool(self):
		either_or = lvm.configFindBool("global/fallback_to_local_locking")
		self.assertTrue(type(either_or) == bool)
		self.assertTrue(lvm.configFindBool("global/locking_type"))

	def testVgFromPVLookups(self):
		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

		vgname_list = self._vg_names()

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

	def testVgGetName(self):
		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

		vgname_list = self._vg_names()

		self.assertTrue(len(vgname_list) > 0)

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')
			self.assertEqual(vg.getName(), vg_name)
			vg.close()

	def testVgGetUuid(self):
		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

		vgname_list = self._vg_names()

		self.assertTrue(len(vgname_list) > 0)

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')
			uuid = vg.getUuid()
			self.assertNotEqual(uuid, None)
			self.assertTrue(len(uuid) > 0)
			vg.close()

	RETURN_NUMERIC = ["getSeqno", "getSize", "getFreeSize", "getFreeSize",
					  "getExtentSize", "getExtentCount", "getFreeExtentCount",
					  "getPvCount", "getMaxPv", "getMaxLv"]

	def testVgGetters(self):
		device_names = self._get_pv_device_names()
		self._createThinLV(device_names[0:2], 'thin')
		self._createThickLV(device_names[2:4], 'thick')

		vg_name_list = self._vg_names()

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

	def _testTags(self, tag_obj):
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

	def testVgTags(self):
		device_names = self._get_pv_device_names()

		i = 0
		for d in device_names:
			if i % 2 == 0:
				self._createThinLV([d],  "thin_lv%d" % i)
			else:
				self._createThickLV([d], "thick_lv%d" % i)
			i += 1

		for vg_name in self._vg_names():
			vg = lvm.vgOpen(vg_name, 'w')
			self._testTags(vg)
			vg.close()

if __name__ == "__main__":
	unittest.main()
