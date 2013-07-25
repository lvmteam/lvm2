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
	(FULL_PV, THIN_PV_A, THIN_PV_B, RESIZE_PV) = (0, 1, 2, 3)

	def _get_pv_devices(self):
		rc = []
		with lvm.listPvs() as pvs:
			for p in pvs:
				name = p.getName()
				self.assertTrue(name is not None and len(name) > 0)
				rc.append(name)
				p = None
		return rc

	def _createThick(self, device_list):
		vg = lvm.vgCreate('full_vg')

		for d in device_list:
			vg.extend(d)

		new_extent = 1024 * 1024 * 2
		vg.setExtentSize(new_extent)
		self.assertEqual(vg.getExtentSize(), new_extent)

		vg.createLvLinear('thick_lv', vg.getSize()/2)
		vg.close()
		vg = None

	def _removeThick(self):
		vg_name = 'full_vg'
		vg = lvm.vgOpen(vg_name, 'w')

		pvs = vg.listPVs()
		lvs = vg.listLVs()

		pe_devices = []

		#Remove old snapshots first, then lv
		for l in lvs:
			attr = l.getAttr()
			if attr[0] == 's':
				l.remove()

		for l in vg.listLVs():
			l.remove()

		for p in pvs:
			pe_devices.append(p.getName())

		for pv in pe_devices:
			vg.reduce(pv)

		vg.remove()
		vg.close()

	def setUp(self):
		device_list = self._get_pv_devices()

		#Make sure our prepare script is doing as expected.
		self.assertTrue(len(device_list) >= 4)

		vg_names = lvm.listVgNames()

		#If we don't have any volume groups lets setup one for
		#those tests that are expecting one
		if len(vg_names) == 0:
			self._createThick([device_list[TestLvm.FULL_PV]])

			vg = lvm.vgCreate('thin_vg')
			vg.extend(device_list[TestLvm.THIN_PV_A])
			vg.extend(device_list[TestLvm.THIN_PV_B])
			vg.createLvThinpool('thin_pool', vg.getSize()/2, 0, 0,
								lvm.THIN_DISCARDS_PASSDOWN, 1)
			vg.createLvThin('thin_pool', 'thin_lv', vg.getSize()/3)
			vg.close()
			vg = None

	def testPVresize(self):
		with lvm.listPvs() as pvs:
			pv = pvs[TestLvm.RESIZE_PV]
			curr_size = pv.getSize()
			dev_size = pv.getDevSize()
			self.assertTrue(curr_size == dev_size)
			pv.resize(curr_size/2)
		with lvm.listPvs() as pvs:
			pv = pvs[TestLvm.RESIZE_PV]
			resized_size = pv.getSize()
			self.assertTrue(resized_size != curr_size)
			pv.resize(dev_size)

	def testPVlifecycle(self):
		"""
		Test removing and re-creating a PV
		"""
		target = None

		with lvm.listPvs() as pvs:
			pv = pvs[TestLvm.RESIZE_PV]
			target = pv.getName()
			lvm.pvRemove(target)

		with lvm.listPvs() as pvs:
			for p in pvs:
				self.assertTrue(p.getName() != target)

		lvm.pvCreate(target, 0)

		with lvm.listPvs() as pvs:
			found = False
			for p in pvs:
				if p.getName() == target:
					found = True

		self.assertTrue(found)

	def testPvMethods(self):
		with lvm.listPvs() as pvs:
			for p in pvs:
				p.getName()
				p.getUuid()
				p.getMdaCount()
				p.getSize()
				p.getDevSize()
				p.getFree()
				p = None

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

	def _get_lv_test(self, lv_vol_type=None, lv_name=None):
		vg_name_list = lvm.listVgNames()
		for vgname in vg_name_list:
			vg = lvm.vgOpen(vgname, "w")
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

	def _get_pv_test(self):
		vg_name_list = lvm.listVgNames()
		for vgname in vg_name_list:
			vg = lvm.vgOpen(vgname, "w")
			pvs = vg.listPVs()
			if len(pvs):
				return pvs[0], vg
		return None, None

	def testPvGetters(self):
		pv, vg = self._get_pv_test()

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

		vg.close()

	def _test_prop(self, prop_obj, prop, var_type, settable):
		result = prop_obj.getProperty(prop)

		self.assertEqual(type(result[0]), var_type)
		self.assertEqual(type(result[1]), bool)
		self.assertTrue(result[1] == settable)

	def testPvSegs(self):
		pv, vg = self._get_pv_test()
		pv_segs = pv.listPVsegs()

		#LVsegs returns a tuple, (value, bool settable)

		#TODO: Test other properties of pv_seg
		for i in pv_segs:
			self._test_prop(i, 'pvseg_start', long, False)

		vg.close()

	def testPvProperty(self):
		pv, vg = self._get_pv_test()
		self._test_prop(pv, 'pv_mda_count', long, False)
		vg.close()

	def testLvProperty(self):
		lv, vg = self._get_lv_test()
		self._test_prop(lv, 'seg_count', long, False)
		vg.close()

	def testLvTags(self):
		lv, vg = self._get_lv_test()
		self._testTags(lv)
		vg.close()

	def testLvActiveInactive(self):
		lv, vg = self._get_lv_test()
		lv.deactivate()
		self.assertTrue(lv.isActive() is False)
		lv.activate()
		self.assertTrue(lv.isActive() is True)
		vg.close()

	def testLvRename(self):
		lv, vg = self._get_lv_test()

		current_name = lv.getName()
		new_name = rs()
		lv.rename(new_name)
		self.assertEqual(lv.getName(), new_name)
		lv.rename(current_name)
		vg.close()

	def testLvSnapshot(self):

		#Cleanup existing if already present
		to_remove = ['thick_lv_snapshot', 'thin_lv_snapshot']

		for ss in to_remove:
			snap, vg = self._get_lv_test(None, ss)
			if snap:
				snap.remove()
				vg.close()

		thick_lv, vg = self._get_lv_test(None, 'thick_lv')

		self.assertEqual('thick_lv', thick_lv.getName())

		thick_lv.snapshot('thick_lv_snapshot', 1024*1024)
		vg.close()

		thin_lv, vg = self._get_lv_test(None, 'thin_lv')
		thin_lv.snapshot('thin_lv_snapshot')

		vg.close()

		thin_ss, vg = self._get_lv_test(None, 'thin_lv_snapshot')
		self.assertTrue(thin_ss is not None)

		origin = thin_ss.getOrigin()
		self.assertTrue('thin_lv', origin)

		vg.close()

	def testLvSuspend(self):
		lv, vg = self._get_lv_test()

		result = lv.isSuspended()
		self.assertTrue(type(result) == bool)
		vg.close()

	def testLvSize(self):
		lv, vg = self._get_lv_test()
		result = lv.getSize()
		self.assertTrue(type(result) == int or type(result) == long)
		vg.close()

	def testLvResize(self):
		lv, vg = self._get_lv_test('V')
		curr_size = lv.getSize()
		lv.resize(curr_size+(1024*1024))
		latest = lv.getSize()
		self.assertTrue(curr_size != latest)

	def testLvSeg(self):
		lv, vg = self._get_lv_test()

		lv_segs = lv.listLVsegs()

		#LVsegs returns a tuple, (value, bool settable)

		#TODO: Test other properties of lv_seg
		for i in lv_segs:
			self._test_prop(i, 'seg_start_pe', long, False)

		vg.close()

	def testVGsetGetProp(self):
		vg_name = 'full_vg'
		vg = lvm.vgOpen(vg_name, 'w')

		self.assertTrue(vg is not None)
		if vg:
			vg_mda_copies = vg.getProperty('vg_mda_copies')
			vg.setProperty('vg_mda_copies', vg_mda_copies[0])
			vg.close()

	def testVGremoveRestore(self):

		#Store off the list of physical devices
		pe_devices = []
		vg = lvm.vgOpen('full_vg', 'w')

		pvs = vg.listPVs()
		for p in pvs:
			pe_devices.append(p.getName())
		vg.close()

		self._removeThick()
		self._createThick(pe_devices)

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

			#TODO Write/fix BUG, vg uuid don't match between
			#lvm.listVgUuids and vg.getUuid()
			vg_uuid_search = vg.getUuid().replace('-', '')

			self.assertTrue(vg_uuid_search in vgs_uuids)
			vgs_uuids.remove(vg_uuid_search)
			vg.close()

		self.assertTrue(len(vgs_uuids) == 0)

	def testPvLookupFromVG(self):
		vg_names = lvm.listVgNames()

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
		vg_name_list = lvm.listVgNames()

		for vg_name in vg_name_list:
			vg = lvm.vgOpen(vg_name, 'w')
			self._testTags(vg)
			vg.close()

if __name__ == "__main__":
	unittest.main()
