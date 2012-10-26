#!/usr/bin/env python

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
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

# Set of basic unit tests for the python bindings.

def rs(l=10):
	"""
	Generate a random string
	"""
	return ''.join(random.choice(string.ascii_uppercase) for x in range(l))


class TestLvm(unittest.TestCase):
	def setUp(self):
		pass

	def tearDown(self):
		pass

	def testOpenClose(self):
		pass

	def testVersion(self):
		version = lvm.getVersion()
		self.assertNotEquals(version, None)
		self.assertEquals(type(version), str)
		self.assertTrue(len(version) > 0)

	def testVgOpen(self):
		vg_names = lvm.listVgNames()

		for i in vg_names:
			vg = lvm.vgOpen(i)
			vg.close()

	def _get_lv_test(self, mode='r'):
		vg_name_list = lvm.listVgNames()
		for vgname in vg_name_list:
			vg = lvm.vgOpen(vgname, mode)
			lvs = vg.listLVs()
			if len(lvs):
				return lvs[0]
		return None

	def _get_pv_test(self, mode='r'):
		vg_name_list = lvm.listVgNames()
		for vgname in vg_name_list:
			vg = lvm.vgOpen(vgname, mode)
			pvs = vg.listPVs()
			if len(pvs):
				return pvs[0]
		return None

	def testPvGetters(self):
		pv = self._get_pv_test()

		self.assertEqual(type(pv.getName()), str)
		self.assertTrue(len(pv.getName()) > 0)

		self.assertEqual(type(pv.getUuid()), str)
		self.assertTrue(len(pv.getUuid()) > 0)

		self.assertEqual(type(pv.getMdaCount()), int)
		self.assertEqual(type(pv.getMdaCount()), int)

		self.assertEqual(type(pv.getSize()), int)

		self.assertEqual(type(pv.getDevSize()), int)

		self.assertEqual(type(pv.getFree()), int)

	def _test_prop(self, prop_obj, prop, var_type, settable):
		result = prop_obj.getProperty(prop)

		self.assertEqual(type(result[0]), var_type)
		self.assertEqual(type(result[1]), bool)
		self.assertTrue(result[1] == settable)

	def testPvSegs(self):
		pv = self._get_pv_test("r")
		pv_segs = pv.listPVsegs()

		#LVsegs returns a tuple, (value, bool settable)

		#TODO: Test other properties of pv_seg
		for i in pv_segs:
			self._test_prop(i, 'pvseg_start', long, False)

	def testPvProperty(self):
		pv = self._get_pv_test("r")
		self._test_prop(pv, 'pv_mda_count', long, False)

	def testLvProperty(self):
		lv = self._get_lv_test("r")
		self._test_prop(lv, 'seg_count', long, False)

	def testLvTags(self):
		lv = self._get_lv_test("w")
		self._testTags(lv)

	def testLvActiveInactive(self):
		lv = self._get_lv_test("w")
		lv.deactivate()
		self.assertTrue(lv.isActive() == False)
		lv.activate()
		self.assertTrue(lv.isActive() == True)

	def testLvRename(self):
		lv = self._get_lv_test("w")

		current_name = lv.getName()
		new_name = rs()
		lv.rename(new_name)
		self.assertEqual(lv.getName(), new_name)
		lv.rename(current_name)

	def testLvSuspend(self):
		lv = self._get_lv_test("r")

		result = lv.isSuspended()
		self.assertTrue(type(result), bool)

	def testLvSize(self):
		lv = self._get_lv_test("r")
		result = lv.getSize()
		self.assertTrue(type(result), bool)

	def testLvResize(self):
		pass    #Not implemented!

	def testPvResize(self):
		pass    #Patch available, not committed

	def testLvSeg(self):
		lv = self._get_lv_test("r")

		lv_segs = lv.listLVsegs()

		#LVsegs returns a tuple, (value, bool settable)

		#TODO: Test other properties of lv_seg
		for i in lv_segs:
			self._test_prop(i, 'seg_start_pe', long, False)

	def testLvMisc(self):
		#Need to look at lack of vg_write in vg create

		#For this to work cleanly we will remove an existing lv & vg and then
		#put it back so that the test framework can clean it up.
		vg_name_list = lvm.listVgNames()

		if len(vg_name_list):
			vg_name = vg_name_list[0]

			vg = lvm.vgOpen(vg_name, "w")

			vg_mda_copies = vg.getProperty('vg_mda_copies')
			vg.setProperty('vg_mda_copies', vg_mda_copies[0])

			pvs = vg.listPVs()
			lvs = vg.listLVs()

			pe_devices = []
			for p in pvs:
				pe_devices.append(p.getName())

			self.assertEquals(len(lvs), 1)

			lv = lvs[0]

			lv_name = lv.getName()
			lv_size = lv.getSize()

			lv.remove()
			lv = None

			vg.reduce(pe_devices[0])

			vg.remove()
			vg.close()

			nvg = lvm.vgCreate(vg_name)
			for p in pe_devices:
				nvg.extend(p)

			#2MiB extent size
			new_extent = 1024 * 1024 * 2

			nvg.setExtentSize(new_extent)
			self.assertEqual(nvg.getExtentSize(), new_extent)

			v = nvg.createLvLinear(lv_name, lv_size)

			lv_find_name = nvg.lvFromName(lv_name)
			lv_find_uuid = nvg.lvFromUuid(v.getUuid())

			self.assertTrue(lv_find_name.getName() == v.getName())
			self.assertTrue(lv_find_uuid.getUuid() == v.getUuid())

			nvg.close()

	def testVgNames(self):
		vg = lvm.listVgNames()
		self.assertTrue(isinstance(vg, tuple))

	def testDupeLvCreate(self):
		"""
		Try to create a lv with the same name expecting a failure
		Note: This was causing a seg. fault previously
		"""
		vgs = lvm.listVgNames()

		if len(vgs):
			vg_name = vgs[0]
			vg = lvm.vgOpen(vg_name, "w")

			lvs = vg.listLVs()

			if len(lvs):
				lv = lvs[0]
				lv_name = lv.getName()
				self.assertRaises(lvm.LibLVMError, vg.createLvLinear, lv_name,
					lv.getSize())

	def testVgUuids(self):
		vgs_uuids = lvm.listVgUuids()

		self.assertTrue(isinstance(vgs_uuids, tuple))

		vgs_uuids = list(vgs_uuids)

		vgs_names = lvm.listVgNames()

		for vg_name in vgs_names:
			vg = lvm.vgOpen(vg_name, "r")

			#TODO Write/fix BUG, vg uuid don't match between lvm.listVgUuids
			# and vg.getUuid()
			vg_uuid_search = vg.getUuid().replace('-', '')

			self.assertTrue(vg_uuid_search in vgs_uuids)
			vgs_uuids.remove(vg_uuid_search)

		self.assertTrue(len(vgs_uuids) == 0)

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
		self.assertTrue(lvm.configFindBool("global/locking_type"))
		self.assertFalse(lvm.configFindBool("global/fallback_to_local_locking"))

	def testVgFromPVLookups(self):
		vgname_list = lvm.listVgNames()
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
		vgname_list = lvm.listVgNames()

		for vg_name in vgname_list:
			vg = lvm.vgOpen(vg_name, 'r')
			self.assertEqual(vg.getName(), vg_name)
			vg.close()

	def testVgGetUuid(self):
		vgname_list = lvm.listVgNames()

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
		vg_name_list = lvm.listVgNames()

		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, 'r')
			self.assertTrue(type(vg.isClustered()) == bool)
			self.assertTrue(type(vg.isExported()) == bool)
			self.assertTrue(type(vg.isPartial()) == bool)

			#Loop through the list invoking the method
			for method_name in TestLvm.RETURN_NUMERIC:
				method = getattr(vg, method_name)
				result = method()
				self.assertTrue(type(result) == int)

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
			tag_to_remove = created_tags[random.randint(0, len(created_tags) - 1)]

			created_tags.remove(tag_to_remove)

			tag_obj.removeTag(tag_to_remove)

			current_tags = tag_obj.getTags()
			self.assertFalse(tag_to_remove in current_tags)

		current_tags = tag_obj.getTags()
		self.assertTrue(len(current_tags) == len(existing_tags))
		for e in existing_tags:
			self.assertTrue(e in current_tags)

	def testVgTags(self):
		vg_name_list = lvm.listVgNames()

		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, 'w')
			self._testTags(vg)
			vg.close()

if __name__ == "__main__":
	unittest.main()