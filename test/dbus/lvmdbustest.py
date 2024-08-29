#!/usr/bin/python3

# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
import os
import signal
# noinspection PyUnresolvedReferences
import subprocess
import unittest
import tempfile
from glob import glob
from subprocess import Popen, PIPE

import dbus
import pyudev
# noinspection PyUnresolvedReferences
from dbus.mainloop.glib import DBusGMainLoop

import testlib
from testlib import *

g_tmo = 0

g_lvm_shell = False

# Approx. min size
VDO_MIN_SIZE = mib(8192)

VG_TEST_SUFFIX = "_vg_LvMdBuS_TEST"

EXE_NAME = "/lvmdbusd"

# Prefix on created objects to enable easier clean-up
g_prefix = os.getenv('PREFIX', '')

# Check dev dir prefix for test suite  (LVM_TEST_DEVDIR
dm_dev_dir = os.getenv('DM_DEV_DIR', '/dev')

# Use the session bus instead of the system bus
use_session = os.getenv('LVM_DBUSD_USE_SESSION', False)

# Only use the devices listed in the ENV variable
pv_device_list = os.getenv('LVM_DBUSD_PV_DEVICE_LIST', None)

# Default is to test all modes
# 0 == Only test fork & exec mode
# 1 == Only test lvm shell mode
# 2 == Test both fork & exec & lvm shell mode (default)
# Other == Test just lvm shell mode
test_shell = os.getenv('LVM_DBUSD_TEST_MODE', 2)

# LVM binary to use
LVM_EXECUTABLE = os.getenv('LVM_BINARY', '/usr/sbin/lvm')

# Empty options dictionary (EOD)
EOD = dbus.Dictionary({}, signature=dbus.Signature('sv'))
# Base interfaces on LV objects
LV_BASE_INT = (LV_COMMON_INT, LV_INT)

if use_session:
	bus = dbus.SessionBus(mainloop=DBusGMainLoop())
else:
	bus = dbus.SystemBus(mainloop=DBusGMainLoop())

# If we have multiple clients we will globally disable introspection
# validation to limit the massive amount of introspection calls we make as
# that method prevents things from executing concurrently
if pv_device_list:
	testlib.validate_introspection = False


def vg_n(prefix=None):
	name = rs(8, VG_TEST_SUFFIX)
	if prefix:
		name = prefix + name
	return g_prefix + name


def lv_n(suffix=None):
	if not suffix:
		s = '_lv'
	else:
		s = suffix
	return rs(8, s)


def _is_testsuite_pv(pv_name):
	return g_prefix != "" and pv_name[-1].isdigit() and \
			pv_name[:-1].endswith(g_prefix + "pv")


def is_nested_pv(pv_name):
	return pv_name.count('/') == 3 and not _is_testsuite_pv(pv_name)


def _root_pv_name(res, pv_name):
	if not is_nested_pv(pv_name):
		return pv_name
	vg_name = pv_name.split('/')[2]
	for v in res[VG_INT]:
		if v.Vg.Name == vg_name:
			for pv in res[PV_INT]:
				if pv.object_path in v.Vg.Pvs:
					return _root_pv_name(res, pv.Pv.Name)
			return None


def _prune_lvs(res, interface, vg_object_path):
	lvs = [lv for lv in res[interface] if lv.LvCommon.Vg == vg_object_path]
	res[interface] = lvs


def _prune(res, pv_filter):
	if pv_filter:
		pv_lookup = {}

		pv_list = []
		for p in res[PV_INT]:
			if _root_pv_name(res, p.Pv.Name) in pv_filter:
				pv_list.append(p)
				pv_lookup[p.object_path] = p

		res[PV_INT] = pv_list

		vg_list = []
		for v in res[VG_INT]:
			if v.Vg.Pvs[0] in pv_lookup:
				vg_list.append(v)

				for interface in \
					[LV_INT, THINPOOL_INT, LV_COMMON_INT,
						CACHE_POOL_INT, CACHE_LV_INT, VDOPOOL_INT]:
					_prune_lvs(res, interface, v.object_path)

		res[VG_INT] = vg_list

	return res


def get_objects():
	rc = {
		MANAGER_INT: [], PV_INT: [], VG_INT: [], LV_INT: [],
		THINPOOL_INT: [], JOB_INT: [], SNAPSHOT_INT: [], LV_COMMON_INT: [],
		CACHE_POOL_INT: [], CACHE_LV_INT: [], VG_VDO_INT: [], VDOPOOL_INT: []}

	object_manager_object = bus.get_object(
		BUS_NAME, "/com/redhat/lvmdbus1", introspect=False)

	manager_interface = dbus.Interface(
		object_manager_object, "org.freedesktop.DBus.ObjectManager")

	objects = manager_interface.GetManagedObjects()

	for object_path, v in objects.items():
		proxy = ClientProxy(bus, object_path, v)
		for interface in v.keys():
			rc[interface].append(proxy)

	# At this point we have a full population of everything, we now need to
	# prune the objects if we are filtering PVs with a sub selection.
	return _prune(rc, pv_device_list), bus


def set_exec_mode(lvmshell):
	lvm_manager = dbus.Interface(bus.get_object(
		BUS_NAME, "/com/redhat/lvmdbus1/Manager", introspect=False),
		"com.redhat.lvmdbus1.Manager")
	return lvm_manager.UseLvmShell(lvmshell)


def set_execution(lvmshell, test_result):
	global g_lvm_shell
	if lvmshell:
		m = 'lvm shell (non-fork)'
	else:
		m = "forking & exec'ing"

	rc = set_exec_mode(lvmshell)

	if rc:
		g_lvm_shell = lvmshell
		std_err_print('Successfully changed execution mode to "%s"' % m)
	else:
		std_err_print('ERROR: Failed to change execution mode to "%s"' % m)
		test_result.register_fail()
	return rc


def call_lvm(command):
	"""
	Call lvm executable and return a tuple of exitcode, stdout, stderr
	:param command:     Command to execute
	:type command: 		list
	:returns (exitcode, stdout, stderr)
	:rtype (int, str, str)
	"""

	# Prepend the full lvm executable so that we can run different versions
	# in different locations on the same box
	command.insert(0, LVM_EXECUTABLE)

	process = Popen(
		command, stdout=PIPE, stderr=PIPE, close_fds=True, env=os.environ)
	out = process.communicate()

	stdout_text = bytes(out[0]).decode("utf-8")
	stderr_text = bytes(out[1]).decode("utf-8")
	return process.returncode, stdout_text, stderr_text


def supports_vdo():
	cmd = ['segtypes']
	modprobe = Popen(["modprobe", "kvdo"], stdout=PIPE, stderr=PIPE, close_fds=True, env=os.environ)
	modprobe.communicate()
	if modprobe.returncode != 0:
		return False
	rc, out, err = call_lvm(cmd)
	if rc != 0 or "vdo" not in out:
		return False
	return True


def process_exists(name):
	# Walk the process table looking for executable 'name'
	for p in [pid for pid in os.listdir('/proc') if pid.isdigit()]:
		try:
			cmdline_args = read_file_split_nuls("/proc/%s/cmdline" % p)
		except OSError:
			continue
		for arg in cmdline_args:
			if name in arg:
				return int(p)
	return None


def read_file_split_nuls(fn):
	with open(fn, "rb") as fh:
		return [p.decode("utf-8") for p in fh.read().split(b'\x00') if len(p) > 0]


def read_file_build_hash(fn):
	rc = dict()
	lines = read_file_split_nuls(fn)
	for line in lines:
		if line.count("=") == 1:
			k, v = line.split("=")
			rc[k] = v
	return rc


def remove_lvm_debug():
	# If we are running the lvmdbusd daemon and collecting lvm debug data, check and
	# clean-up after the tests.
	tmpdir = tempfile.gettempdir()
	fp = os.path.join(tmpdir, "lvmdbusd.lvm.debug.*.log")
	for f in glob(fp):
		os.unlink(f)


class DaemonInfo(object):
	def __init__(self, pid):
		# The daemon is running, we have a pid, lets see how it's being run.
		# When running under systemd, fd 0 -> /dev/null, fd 1&2 -> socket
		# when ran manually it may have output re-directed to a file etc.
		# we need the following
		# command line arguments
		# cwd
		# where the output is going (in case it's directed to a file)
		# Which lvm binary is being used (check LVM_BINARY env. variable)
		# PYTHONPATH
		base = "/proc/%d" % pid
		self.cwd = os.readlink("%s/cwd" % base)
		self.cmdline = read_file_split_nuls("%s/cmdline" % (base))[1:]
		self.env = read_file_build_hash("%s/environ" % base)
		self.stdin = os.readlink("%s/fd/0" % base)
		self.stdout = os.readlink("%s/fd/1" % base)
		self.stderr = os.readlink("%s/fd/2" % base)

		if self.cwd == "/" and self.stdin == "/dev/null":
			self.systemd = True
		else:
			self.systemd = False

		self.process = None

	@classmethod
	def get(cls):
		pid = process_exists(EXE_NAME)
		if pid:
			return cls(pid)
		return None

	def start(self, expect_fail=False):
		if self.systemd:
			subprocess.run(["/usr/bin/systemctl", "start", "lvm2-lvmdbusd"], check=True)
		else:
			stdin_stream = None
			stdout_stream = None
			stderr_stream = None
			try:
				stdout_stream = open(self.stdout, "ab")
				stdin_stream = open(self.stdin, "rb")
				stderr_stream = open(self.stderr, "ab")

				self.process = Popen(self.cmdline, cwd=self.cwd, stdin=stdin_stream,
									stdout=stdout_stream, stderr=stderr_stream, env=self.env)

				if expect_fail:
					# Let's wait a bit to see if this process dies as expected and return the exit code
					try:
						self.process.wait(10)
						return self.process.returncode
					except subprocess.TimeoutExpired as e:
						# Process did not fail as expected, lets kill it
						os.kill(self.process.pid, signal.SIGKILL)
						self.process.wait(20)
						raise e
				else:
					# This is a hack to set the returncode.  When the Popen object goes out of scope during the unit test
					# the __del__ method gets called.  As we leave the daemon running the process.returncode
					# hasn't been set, so it incorrectly raises an exception that the process is still running
					# which in our case is correct and expected.
					self.process.returncode = 0
			finally:
				# Close these in the parent
				if stdin_stream:
					stdin_stream.close()
				if stderr_stream:
					stderr_stream.close()
				if stdout_stream:
					stdout_stream.close()

		# Make sure daemon is responding to dbus events before returning
		DaemonInfo._ensure_daemon("Daemon is not responding on dbus within 20 seconds of starting!")

		# During local testing it usually takes ~0.25 seconds for daemon to be ready
		return None

	@staticmethod
	def _ensure_no_daemon():
		start = time.time()
		pid = process_exists(EXE_NAME)
		while pid is not None and (time.time() - start) <= 20:
			time.sleep(0.1)
			pid = process_exists(EXE_NAME)

		if pid:
			raise Exception(
				"lsmd daemon did not exit within 20 seconds, pid = %s" % pid)

	@staticmethod
	def _ensure_daemon(msg):
		start = time.time()
		running = False
		while True and (time.time() - start) < 20:
			try:
				get_objects()
				running = True
				break
			except dbus.exceptions.DBusException:
				time.sleep(0.1)
				pass
		if not running:
			raise RuntimeError(msg)

	def term_signal(self, sig_number):
		# Used for signals that we expect with terminate the daemon, eg. SIGINT, SIGKILL
		if self.process:
			os.kill(self.process.pid, sig_number)
			# Note: The following should work, but doesn't!
			# self.process.send_signal(sig_number)
			try:
				self.process.wait(10)
			except subprocess.TimeoutExpired:
				std_err_print("Daemon hasn't exited within 10 seconds")
			if self.process.poll() is None:
				std_err_print("Daemon still running...")
			else:
				self.process = None
		else:
			pid = process_exists(EXE_NAME)
			os.kill(pid, sig_number)

		# Make sure there is no daemon present before we return for things to be "good"
		DaemonInfo._ensure_no_daemon()

	def non_term_signal(self, sig_number):
		if sig_number not in [signal.SIGUSR1, signal.SIGUSR2]:
			raise ValueError("Incorrect signal number! %d" % sig_number)
		if self.process:
			os.kill(self.process.pid, sig_number)
		else:
			pid = process_exists(EXE_NAME)
			os.kill(pid, sig_number)


# noinspection PyUnresolvedReferences
class TestDbusService(unittest.TestCase):
	def setUp(self):
		self.pvs = []

		# Because of the sensitive nature of running LVM tests we will only
		# run if we have PVs and nothing else, so that we can be confident that
		# we are not mucking with someone's data on their system
		self.objs, self.bus = get_objects()
		if len(self.objs[PV_INT]) == 0:
			std_err_print('No PVs present exiting!')
			sys.exit(1)

		for p in self.objs[PV_INT]:
			self.pvs.append(p.Pv.Name)

		if len(self.objs[MANAGER_INT]) != 1:
			std_err_print('Expecting a manager object!')
			sys.exit(1)

		if len(self.objs[VG_INT]) != 0:
			std_err_print('Expecting no VGs to exist!')
			sys.exit(1)

		self.addCleanup(self.clean_up)

		self.vdo = supports_vdo()
		remove_lvm_debug()

	def _recurse_vg_delete(self, vg_proxy, pv_proxy, nested_pv_hash):
		vg_name = str(vg_proxy.Vg.Name)

		if not vg_name.endswith(VG_TEST_SUFFIX):
			std_err_print("Refusing to remove VG: %s" % vg_name)
			return

		for pv_device_name, t in nested_pv_hash.items():
			if vg_name in pv_device_name:
				self._recurse_vg_delete(t[0], t[1], nested_pv_hash)
				break

		vg_proxy.update()

		self.handle_return(vg_proxy.Vg.Remove(dbus.Int32(g_tmo), EOD))
		if is_nested_pv(pv_proxy.Pv.Name):
			rc = self._pv_remove(pv_proxy)
			self.assertTrue(rc == '/', "We expected a '/', but got %s when removing a PV" % str(rc))

	def clean_up(self):
		self.objs, self.bus = get_objects()

		# The self.objs[PV_INT] list only contains those which we should be
		# mucking with, lets remove any embedded/nested PVs first, then proceed
		# to walk the base PVs and remove the VGs
		nested_pvs = {}
		non_nested = []

		for p in self.objs[PV_INT]:
			if is_nested_pv(p.Pv.Name):
				if p.Pv.Vg != '/':
					v = ClientProxy(self.bus, p.Pv.Vg, interfaces=(VG_INT,))
					nested_pvs[p.Pv.Name] = (v, p)
				else:
					# Nested PV with no VG, so just simply remove it!
					self._pv_remove(p)
			else:
				non_nested.append(p)

		for p in non_nested:
			# When we remove a VG for a PV it could ripple across multiple
			# PVs, so update each PV while removing each VG, to ensure
			# the properties are current and correct.
			p.update()
			if p.Pv.Vg != '/':
				v = ClientProxy(self.bus, p.Pv.Vg, interfaces=(VG_INT,))
				self._recurse_vg_delete(v, p, nested_pvs)

		# Check to make sure the PVs we had to start exist, else re-create
		# them
		self.objs, self.bus = get_objects()
		if len(self.pvs) != len(self.objs[PV_INT]):
			for p in self.pvs:
				found = False
				for pc in self.objs[PV_INT]:
					if pc.Pv.Name == p:
						found = True
						break

				if not found:
					# print('Re-creating PV=', p)
					self._pv_create(p)

		remove_lvm_debug()

	def _check_consistency(self):
		# Only do consistency checks if we aren't running the unit tests
		# concurrently
		if pv_device_list is None:
			self.assertEqual(self._refresh(), 0)

	def handle_return(self, rc):
		if isinstance(rc, (tuple, list)):
			# We have a tuple returned
			if rc[0] != '/':
				return rc[0]
			else:
				return self._wait_for_job(rc[1])
		else:
			if rc == '/':
				return rc
			else:
				return self._wait_for_job(rc)

	def _pv_create(self, device):

		pv_path = self.handle_return(
			self.objs[MANAGER_INT][0].Manager.PvCreate(
				dbus.String(device), dbus.Int32(g_tmo), EOD)
		)

		self._validate_lookup(device, pv_path)

		self.assertTrue(pv_path is not None and len(pv_path) > 0,
						"When creating a PV we expected the returned path to be valid")
		return pv_path

	def _manager(self):
		return self.objs[MANAGER_INT][0]

	def _refresh(self):
		return self._manager().Manager.Refresh()

	def test_refresh(self):
		self._check_consistency()

	def test_version(self):
		rc = self.objs[MANAGER_INT][0].Manager.Version
		self.assertTrue(rc is not None and len(rc) > 0, "Manager.Version is invalid")
		self._check_consistency()

	def _vg_create(self, pv_paths=None, vg_prefix=None, options=None):

		if not pv_paths:
			pv_paths = self._all_pv_object_paths()

		if options is None:
			options = EOD

		vg_name = vg_n(prefix=vg_prefix)

		vg_path = self.handle_return(
			self.objs[MANAGER_INT][0].Manager.VgCreate(
				dbus.String(vg_name),
				dbus.Array(pv_paths, signature=dbus.Signature('o')),
				dbus.Int32(g_tmo),
				options))

		self._validate_lookup(vg_name, vg_path)
		self.assertTrue(vg_path is not None and len(vg_path) > 0, "During VG creation, returned path is empty")

		intf = [VG_INT, ]
		if self.vdo:
			intf.append(VG_VDO_INT)

		return ClientProxy(self.bus, vg_path, interfaces=intf)

	def test_vg_create(self):
		self._vg_create()
		self._check_consistency()

	def test_vg_delete(self):
		vg = self._vg_create().Vg

		self.handle_return(
			vg.Remove(dbus.Int32(g_tmo), EOD))
		self._check_consistency()

	def _pv_remove(self, pv):
		rc = self.handle_return(
			pv.Pv.Remove(dbus.Int32(g_tmo), EOD))
		return rc

	def test_pv_remove_add(self):
		target = self.objs[PV_INT][0]

		# Remove the PV
		rc = self._pv_remove(target)
		self.assertTrue(rc == '/')
		self._check_consistency()

		# Add it back
		rc = self._pv_create(target.Pv.Name)[0]
		self.assertTrue(rc == '/')
		self._check_consistency()

	def _create_raid5_thin_pool(self, vg=None):

		meta_name = "meta_r5"
		data_name = "data_r5"

		if not vg:
			vg = self._vg_create(self._all_pv_object_paths()).Vg

		lv_meta_path = self.handle_return(
			vg.LvCreateRaid(
				dbus.String(meta_name),
				dbus.String("raid5"),
				dbus.UInt64(mib(4)),
				dbus.UInt32(0),
				dbus.UInt32(0),
				dbus.Int32(g_tmo),
				EOD)
		)
		self._validate_lookup("%s/%s" % (vg.Name, meta_name), lv_meta_path)

		lv_data_path = self.handle_return(
			vg.LvCreateRaid(
				dbus.String(data_name),
				dbus.String("raid5"),
				dbus.UInt64(mib(16)),
				dbus.UInt32(0),
				dbus.UInt32(0),
				dbus.Int32(g_tmo),
				EOD)
		)

		self._validate_lookup("%s/%s" % (vg.Name, data_name), lv_data_path)

		thin_pool_path = self.handle_return(
			vg.CreateThinPool(
				dbus.ObjectPath(lv_meta_path),
				dbus.ObjectPath(lv_data_path),
				dbus.Int32(g_tmo), EOD)
		)

		# Get thin pool client proxy
		intf = (LV_COMMON_INT, LV_INT, THINPOOL_INT)
		thin_pool = ClientProxy(self.bus, thin_pool_path, interfaces=intf)

		return vg, thin_pool

	def test_meta_lv_data_lv_props(self):
		# Ensure that metadata lv and data lv for thin pools and cache pools
		# point to a valid LV
		(vg, thin_pool) = self._create_raid5_thin_pool()

		# Check properties on thin pool
		self.assertTrue(thin_pool.ThinPool.DataLv != '/')
		self.assertTrue(thin_pool.ThinPool.MetaDataLv != '/')

		(vg, cache_pool) = self._create_cache_pool(vg)

		self.assertTrue(cache_pool.CachePool.DataLv != '/')
		self.assertTrue(cache_pool.CachePool.MetaDataLv != '/')

		# Cache the thin pool
		cached_thin_pool_path = self.handle_return(
			cache_pool.CachePool.CacheLv(
				dbus.ObjectPath(thin_pool.object_path),
				dbus.Int32(g_tmo), EOD)
		)

		# Get object proxy for cached thin pool
		intf = (LV_COMMON_INT, LV_INT, THINPOOL_INT)
		cached_thin_pool_object = ClientProxy(
			self.bus, cached_thin_pool_path, interfaces=intf)

		# Check properties on cache pool
		self.assertTrue(cached_thin_pool_object.ThinPool.DataLv != '/')
		self.assertTrue(cached_thin_pool_object.ThinPool.MetaDataLv != '/')

	def _lookup(self, lvm_id):
		return self.objs[MANAGER_INT][0].\
			Manager.LookUpByLvmId(dbus.String(lvm_id))

	def _validate_lookup(self, lvm_name, object_path):
		t = self._lookup(lvm_name)
		self.assertTrue(
			object_path == t, "%s != %s for %s" % (object_path, t, lvm_name))

	def test_lookup_by_lvm_id(self):
		# For the moment lets just lookup what we know about which is PVs
		# When we start testing VGs and LVs we will test lookups for those
		# during those unit tests
		for p in self.objs[PV_INT]:
			rc = self._lookup(p.Pv.Name)
			self.assertTrue(rc is not None and rc != '/')

		# Search for something which doesn't exist
		rc = self._lookup('/dev/null')
		self.assertTrue(rc == '/')

	def test_vg_extend(self):
		# Create a VG
		self.assertTrue(len(self.objs[PV_INT]) >= 2)

		if len(self.objs[PV_INT]) >= 2:
			pv_initial = self.objs[PV_INT][0]
			pv_next = self.objs[PV_INT][1]

			vg = self._vg_create([pv_initial.object_path]).Vg

			path = self.handle_return(
				vg.Extend(
					dbus.Array([pv_next.object_path], signature="o"),
					dbus.Int32(g_tmo), EOD)
			)
			self.assertTrue(path == '/')
			self._check_consistency()

	# noinspection PyUnresolvedReferences
	def test_vg_reduce(self):
		self.assertTrue(len(self.objs[PV_INT]) >= 2)

		if len(self.objs[PV_INT]) >= 2:
			vg = self._vg_create(
				[self.objs[PV_INT][0].object_path,
					self.objs[PV_INT][1].object_path]).Vg

			path = self.handle_return(
				vg.Reduce(
					dbus.Boolean(False), dbus.Array([vg.Pvs[0]], signature='o'),
					dbus.Int32(g_tmo), EOD)
			)
			self.assertTrue(path == '/')
			self._check_consistency()

	def _verify_lv_paths(self, vg, new_name):
		"""
		# Go through each LV and make sure it has the correct path back to the
		# VG
		:return:
		"""
		lv_paths = vg.Lvs

		for l in lv_paths:
			lv_proxy = ClientProxy(
				self.bus, l, interfaces=(LV_COMMON_INT,)).LvCommon
			self.assertTrue(
				lv_proxy.Vg == vg.object_path, "%s != %s" %
				(lv_proxy.Vg, vg.object_path))
			full_name = "%s/%s" % (new_name, lv_proxy.Name)
			lv_path = self._lookup(full_name)
			self.assertTrue(
				lv_path == lv_proxy.object_path, "%s != %s" %
				(lv_path, lv_proxy.object_path))

	# noinspection PyUnresolvedReferences
	def test_vg_rename(self):
		vg = self._vg_create().Vg

		# Do a vg lookup
		path = self._lookup(vg.Name)

		vg_name_start = vg.Name

		prev_path = path
		self.assertTrue(path != '/', "%s" % (path))

		# Create some LVs in the VG
		for i in range(0, 5):
			lv_t = self._create_lv(size=mib(4), vg=vg)
			full_name = "%s/%s" % (vg_name_start, lv_t.LvCommon.Name)
			lv_path = self._lookup(full_name)
			self.assertTrue(lv_path == lv_t.object_path)

		new_name = 'renamed_' + vg.Name

		path = self.handle_return(
			vg.Rename(dbus.String(new_name), dbus.Int32(g_tmo), EOD))
		self.assertTrue(path == '/')
		self._check_consistency()

		# Do a vg lookup
		path = self._lookup(new_name)
		self.assertTrue(path != '/', "%s" % (path))
		self.assertTrue(prev_path == path, "%s != %s" % (prev_path, path))

		# Go through each LV and make sure it has the correct path back to the
		# VG
		vg.update()

		self.assertTrue(len(vg.Lvs) == 5)
		self._verify_lv_paths(vg, new_name)

	def _verify_hidden_lookups(self, lv_common_object, vgname):
		hidden_lv_paths = lv_common_object.HiddenLvs

		for h in hidden_lv_paths:
			h_lv = ClientProxy(
				self.bus, h, interfaces=(LV_COMMON_INT,)).LvCommon

			if len(h_lv.HiddenLvs) > 0:
				self._verify_hidden_lookups(h_lv, vgname)

			full_name = "%s/%s" % (vgname, h_lv.Name)
			# print("Hidden check %s" % (full_name))
			lookup_path = self._lookup(full_name)
			self.assertTrue(lookup_path != '/')
			self.assertTrue(lookup_path == h_lv.object_path)

			# Lets's strip off the '[ ]' and make sure we can find
			full_name = "%s/%s" % (vgname, h_lv.Name[1:-1])
			# print("Hidden check %s" % (full_name))

			lookup_path = self._lookup(full_name)
			self.assertTrue(lookup_path != '/')
			self.assertTrue(lookup_path == h_lv.object_path)

	def test_vg_rename_with_thin_pool(self):

		(vg, thin_pool) = self._create_raid5_thin_pool()

		vg_name_start = vg.Name

		# noinspection PyTypeChecker
		self._verify_hidden_lookups(thin_pool.LvCommon, vg_name_start)

		for i in range(0, 5):
			lv_name = lv_n()

			thin_lv_path = self.handle_return(
				thin_pool.ThinPool.LvCreate(
					dbus.String(lv_name),
					dbus.UInt64(mib(16)),
					dbus.Int32(g_tmo),
					EOD))

			self._validate_lookup(
				"%s/%s" % (vg_name_start, lv_name), thin_lv_path)

			self.assertTrue(thin_lv_path != '/')

			full_name = "%s/%s" % (vg_name_start, lv_name)

			lookup_lv_path = self._lookup(full_name)
			self.assertTrue(
				thin_lv_path == lookup_lv_path,
				"%s != %s" % (thin_lv_path, lookup_lv_path))

		# Rename the VG
		new_name = 'renamed_' + vg.Name
		path = self.handle_return(
			vg.Rename(dbus.String(new_name), dbus.Int32(g_tmo), EOD))

		self.assertTrue(path == '/')
		self._check_consistency()

		vg.update()
		thin_pool.update()
		self._verify_lv_paths(vg, new_name)
		# noinspection PyTypeChecker
		self._verify_hidden_lookups(thin_pool.LvCommon, new_name)

	def _test_lv_create(self, method, params, vg, proxy_interfaces=None):
		lv = None

		path = self.handle_return(method(*params))
		self.assertTrue(vg)

		if path:
			lv = ClientProxy(self.bus, path, interfaces=proxy_interfaces)

		# We are quick enough now that we can get VolumeType changes from
		# 'I' to 'i' between the time it takes to create a RAID and it returns
		# and when we refresh state here.  Not sure how we can handle this as
		# we cannot just sit and poll all the time for changes...
		# self._check_consistency()
		return lv

	def test_lv_create(self):
		lv_name = lv_n()
		vg = self._vg_create().Vg
		lv = self._test_lv_create(
			vg.LvCreate,
			(dbus.String(lv_name), dbus.UInt64(mib(4)),
				dbus.Array([], signature='(ott)'), dbus.Int32(g_tmo),
				EOD), vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def test_prop_get(self):
		lv_name = lv_n()
		vg = self._vg_create().Vg
		lv = self._test_lv_create(
			vg.LvCreate,
				(dbus.String(lv_name), dbus.UInt64(mib(4)),
				dbus.Array([], signature='(ott)'), dbus.Int32(g_tmo),
				EOD), vg, LV_BASE_INT)
		ri = RemoteInterface(lv.dbus_object, interface=LV_COMMON_INT, introspect=False)

		ri.update()
		for prop_name in ri.get_property_names():
			self.assertEqual(ri.get_property_value(prop_name), getattr(ri, prop_name))

	def test_lv_create_job(self):
		lv_name = lv_n()
		vg = self._vg_create().Vg
		(object_path, job_path) = vg.LvCreate(
			dbus.String(lv_name), dbus.UInt64(mib(4)),
			dbus.Array([], signature='(ott)'), dbus.Int32(0),
			EOD)

		self.assertTrue(object_path == '/')
		self.assertTrue(job_path != '/')
		object_path = self._wait_for_job(job_path)

		self._validate_lookup("%s/%s" % (vg.Name, lv_name), object_path)
		self.assertTrue(object_path != '/')

	def test_lv_create_linear(self):

		lv_name = lv_n()
		vg = self._vg_create().Vg
		lv = self._test_lv_create(
			vg.LvCreateLinear,
			(dbus.String(lv_name), dbus.UInt64(mib(4)), dbus.Boolean(False),
				dbus.Int32(g_tmo), EOD), vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def _all_pv_object_paths(self):
		return [pp.object_path for pp in self.objs[PV_INT]]

	def test_lv_create_striped(self):
		lv_name = lv_n()
		vg = self._vg_create(self._all_pv_object_paths()).Vg
		lv = self._test_lv_create(
			vg.LvCreateStriped,
			(dbus.String(lv_name), dbus.UInt64(mib(4)),
				dbus.UInt32(2), dbus.UInt32(8), dbus.Boolean(False),
				dbus.Int32(g_tmo), EOD), vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def test_lv_create_mirror(self):
		lv_name = lv_n()
		vg = self._vg_create(self._all_pv_object_paths()).Vg
		lv = self._test_lv_create(
			vg.LvCreateMirror,
			(dbus.String(lv_name), dbus.UInt64(mib(4)), dbus.UInt32(2),
				dbus.Int32(g_tmo), EOD), vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def test_lv_create_raid(self):
		lv_name = lv_n()
		vg = self._vg_create(self._all_pv_object_paths()).Vg
		lv = self._test_lv_create(
			vg.LvCreateRaid,
			(dbus.String(lv_name), dbus.String('raid5'), dbus.UInt64(mib(16)),
				dbus.UInt32(2), dbus.UInt32(8), dbus.Int32(g_tmo), EOD),
			vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def _create_lv(self, thinpool=False, size=None, vg=None, suffix=None):

		lv_name = lv_n(suffix=suffix)
		interfaces = list(LV_BASE_INT)

		if thinpool:
			interfaces.append(THINPOOL_INT)

		if not vg:
			vg = self._vg_create(self._all_pv_object_paths()).Vg

		if size is None:
			size = mib(8)

		lv = self._test_lv_create(
			vg.LvCreateLinear,
			(dbus.String(lv_name), dbus.UInt64(size),
				dbus.Boolean(thinpool), dbus.Int32(g_tmo), EOD),
			vg, interfaces)

		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)
		return lv

	def _create_thin_pool_lv(self):
		return self._create_lv(True)

	def test_lv_create_rounding(self):
		self._create_lv(size=(mib(2) + 13))

	def test_lv_create_thin_pool(self):
		self._create_thin_pool_lv()

	def _rename_lv_test(self, lv):
		path = self._lookup(lv.LvCommon.Name)
		prev_path = path

		new_name = 'renamed_' + lv.LvCommon.Name

		self.handle_return(
			lv.Lv.Rename(dbus.String(new_name), dbus.Int32(g_tmo), EOD))

		path = self._lookup(new_name)

		self._check_consistency()
		self.assertTrue(prev_path == path, "%s != %s" % (prev_path, path))

		lv.update()
		self.assertTrue(
			lv.LvCommon.Name == new_name,
			"%s != %s" % (lv.LvCommon.Name, new_name))

	def test_lv_rename(self):
		# Rename a regular LV
		lv = self._create_lv()
		self._rename_lv_test(lv)

	def test_lv_thinpool_rename(self):
		# Rename a thin pool
		tp = self._create_lv(True)
		self.assertTrue(
			THINPOOL_LV_PATH in tp.object_path,
			"%s" % (tp.object_path))

		new_name = 'renamed_' + tp.LvCommon.Name
		self.handle_return(tp.Lv.Rename(
			dbus.String(new_name), dbus.Int32(g_tmo), EOD))
		tp.update()
		self._check_consistency()
		self.assertEqual(new_name, tp.LvCommon.Name)

	def _create_thin_lv(self):
		vg = self._vg_create().Vg
		tp = self._create_lv(thinpool=True, vg=vg)

		lv_name = lv_n('_thin_lv')

		thin_path = self.handle_return(
			tp.ThinPool.LvCreate(
				dbus.String(lv_name),
				dbus.UInt64(mib(10)),
				dbus.Int32(g_tmo),
				EOD)
		)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), thin_path)

		lv = ClientProxy(
			self.bus, thin_path, interfaces=(LV_COMMON_INT, LV_INT))
		return vg, thin_path, lv

	# noinspection PyUnresolvedReferences
	def test_lv_on_thin_pool_rename(self):
		# Rename a LV on a thin Pool
		vg, thin_path, lv = self._create_thin_lv()
		re_named = 'rename_test' + lv.LvCommon.Name
		rc = self.handle_return(
			lv.Lv.Rename(
				dbus.String(re_named),
				dbus.Int32(g_tmo),
				EOD)
		)

		self._validate_lookup("%s/%s" % (vg.Name, re_named), thin_path)
		self.assertTrue(rc == '/')
		self._check_consistency()

	def _lv_remove(self, lv):
		rc = self.handle_return(
			lv.Lv.Remove(
				dbus.Int32(g_tmo),
				EOD))
		self.assertTrue(rc == '/')
		self._check_consistency()

	def test_lv_remove(self):
		lv = self._create_lv()
		self._lv_remove(lv)

	def _take_lv_snapshot(self, lv_p):
		ss_name = 'ss_' + lv_p.LvCommon.Name

		ss_obj_path = self.handle_return(lv_p.Lv.Snapshot(
			dbus.String(ss_name),
			dbus.UInt64(0),
			dbus.Int32(g_tmo),
			EOD))

		self.assertTrue(ss_obj_path != '/')
		return ClientProxy(
			self.bus, ss_obj_path, interfaces=(LV_COMMON_INT, LV_INT))

	def test_lv_snapshot(self):
		lv_p = self._create_lv()
		self._take_lv_snapshot(lv_p)

	# noinspection PyUnresolvedReferences,PyUnusedLocal
	def _wait_for_job(self, j_path):
		rc = None
		j = ClientProxy(self.bus, j_path, interfaces=(JOB_INT, )).Job

		while True:
			j.update()
			if j.Complete:
				(ec, error_msg) = j.GetError
				self.assertTrue(ec == 0, "%d :%s" % (ec, error_msg))

				if ec == 0:
					self.assertTrue(j.Percent == 100, "P= %f" % j.Percent)

				rc = j.Result
				j.Remove()

				break

			if j.Wait(1):
				self.assertTrue(j.Wait(0))
				j.update()
				self.assertTrue(j.Complete)

		return rc

	def test_lv_create_pv_specific(self):
		vg = self._vg_create().Vg
		lv_name = lv_n()
		pv = vg.Pvs
		pvp = ClientProxy(self.bus, pv[0], interfaces=(PV_INT,))

		lv = self._test_lv_create(
			vg.LvCreate, (
				dbus.String(lv_name),
				dbus.UInt64(mib(4)),
				dbus.Array(
					[[pvp.object_path, 0, (pvp.Pv.PeCount - 1)]],
					signature='(ott)'),
				dbus.Int32(g_tmo), EOD), vg, LV_BASE_INT)
		self._validate_lookup("%s/%s" % (vg.Name, lv_name), lv.object_path)

	def _test_lv_resize(self, lv):
		# Can't resize cache or thin pool volumes or vdo pool lv
		if lv.LvCommon.Attr[0] == 'C' or lv.LvCommon.Attr[0] == 't' or \
				lv.LvCommon.Attr[0] == 'd':
			return

		vg = ClientProxy(self.bus, lv.LvCommon.Vg, interfaces=(VG_INT,)).Vg

		start_size = lv.LvCommon.SizeBytes

		# Vdo are fairly big and need large re-size amounts.
		if start_size > mib(4) * 3:
			delta = mib(4)
		else:
			delta = 16384

		for size in [start_size + delta, start_size - delta]:
			# Select a PV in the VG that isn't in use
			pv_empty = []
			for p in vg.Pvs:
				pobj = ClientProxy(self.bus, p, interfaces=(PV_INT,))
				if len(pobj.Pv.Lv) == 0:
					pv_empty.append(p)

			prev = lv.LvCommon.SizeBytes

			if len(pv_empty):
				p = ClientProxy(self.bus, pv_empty[0], interfaces=(PV_INT,))

				rc = self.handle_return(
					lv.Lv.Resize(
						dbus.UInt64(size),
						dbus.Array(
							[[p.object_path, 0, p.Pv.PeCount - 1]], '(oii)'),
						dbus.Int32(g_tmo), EOD))
			else:
				rc = self.handle_return(
					lv.Lv.Resize(
						dbus.UInt64(size),
						dbus.Array([], '(oii)'),
						dbus.Int32(g_tmo), EOD))

			self.assertEqual(rc, '/')
			self._check_consistency()

			lv.update()

			if prev < size:
				self.assertTrue(lv.LvCommon.SizeBytes > prev)
			else:
				# We are testing re-sizing to same size too...
				self.assertTrue(lv.LvCommon.SizeBytes <= prev)

	def test_lv_resize(self):

		pv_paths = [
			self.objs[PV_INT][0].object_path, self.objs[PV_INT][1].object_path]

		vg = self._vg_create(pv_paths).Vg
		lv = self._create_lv(vg=vg, size=mib(16))

		self._test_lv_resize(lv)

	def test_lv_resize_same(self):
		vg = self._vg_create(self._all_pv_object_paths()).Vg
		lv = self._create_lv(vg=vg)

		with self.assertRaises(dbus.exceptions.DBusException):
			lv.Lv.Resize(
				dbus.UInt64(lv.LvCommon.SizeBytes),
				dbus.Array([], '(oii)'),
				dbus.Int32(-1), EOD)

	def test_lv_move(self):
		lv = self._create_lv()

		pv_path_move = str(lv.LvCommon.Devices[0][0])

		# Test moving a specific LV
		rc = self.handle_return(
			lv.Lv.Move(
				dbus.ObjectPath(pv_path_move),
				dbus.Struct((0, 0), signature='(tt)'),
				dbus.Array([], '(ott)'), dbus.Int32(g_tmo),
				EOD))
		self.assertTrue(rc == '/')
		self._check_consistency()

		lv.update()
		new_pv = str(lv.LvCommon.Devices[0][0])
		self.assertTrue(
			pv_path_move != new_pv, "%s == %s" % (pv_path_move, new_pv))

	def _test_activate_deactivate(self, lv_p):
		self.handle_return(lv_p.Lv.Deactivate(
			dbus.UInt64(0), dbus.Int32(g_tmo), EOD))
		lv_p.update()
		self.assertFalse(lv_p.LvCommon.Active)
		self._check_consistency()

		self.handle_return(lv_p.Lv.Activate(
			dbus.UInt64(0), dbus.Int32(g_tmo), EOD))

		lv_p.update()
		self.assertTrue(lv_p.LvCommon.Active)

		# Vdo property "IndexState" when getting activated goes from
		# "opening" -> "online" after we have returned from the activate call
		# thus when we try to check the consistency we fail as the property
		# is changing on it's own and not because the lvmdbusd failed to
		# refresh it's own state.  One solution is to not expose IndexState as
		# a property.
		# TODO Expose method to determine if Lv is partaking in VDO.
		vg = ClientProxy(self.bus, lv_p.LvCommon.Vg, interfaces=(VG_INT,))
		if "vdo" not in vg.Vg.Name:
			self._check_consistency()

		# Try control flags
		for i in range(0, 6):

			self.handle_return(lv_p.Lv.Activate(
				dbus.UInt64(1 << i),
				dbus.Int32(g_tmo),
				EOD))

			self.assertTrue(lv_p.LvCommon.Active)
			self._check_consistency()

	def test_lv_activate_deactivate(self):
		lv_p = self._create_lv()
		self._test_activate_deactivate(lv_p)

	def test_move(self):
		lv = self._create_lv()

		# Test moving without being LV specific
		vg = ClientProxy(self.bus, lv.LvCommon.Vg, interfaces=(VG_INT, )).Vg
		pv_to_move = str(lv.LvCommon.Devices[0][0])

		rc = self.handle_return(
			vg.Move(
				dbus.ObjectPath(pv_to_move),
				dbus.Struct((0, 0), signature='tt'),
				dbus.Array([], '(ott)'),
				dbus.Int32(0),
				EOD))
		self.assertEqual(rc, '/')
		self._check_consistency()

		vg.update()
		lv.update()

		location = lv.LvCommon.Devices[0][0]

		dst = None
		for p in vg.Pvs:
			if p != location:
				dst = p

		# Fetch the destination
		pv = ClientProxy(self.bus, dst, interfaces=(PV_INT, )).Pv

		# Test range, move it to the middle of the new destination
		job = self.handle_return(
			vg.Move(
				dbus.ObjectPath(location),
				dbus.Struct((0, 0), signature='tt'),
				dbus.Array([(dst, pv.PeCount // 2, 0), ], '(ott)'),
				dbus.Int32(g_tmo),
				EOD))
		self.assertEqual(job, '/')
		self._check_consistency()

	def test_job_handling(self):
		pv_paths = self._all_pv_object_paths()
		vg_name = vg_n()

		# Test getting a job right away
		vg_path, vg_job = self.objs[MANAGER_INT][0].Manager.VgCreate(
			dbus.String(vg_name),
			dbus.Array(pv_paths, 'o'),
			dbus.Int32(0),
			EOD)

		self.assertTrue(vg_path == '/')
		self.assertTrue(vg_job and len(vg_job) > 0)

		vg_path = self._wait_for_job(vg_job)
		self._validate_lookup(vg_name, vg_path)

	def _create_num_lvs(self, num_lvs, no_wait=False):
		vg_proxy = self._vg_create(self._all_pv_object_paths())
		if no_wait:
			tmo = 0
		else:
			tmo = g_tmo

		for i in range(0, num_lvs):
			lv_name = lv_n()
			vg_proxy.update()
			if vg_proxy.Vg.FreeCount > 0:
				create_result = vg_proxy.Vg.LvCreateLinear(
						dbus.String(lv_name),
						dbus.UInt64(mib(4)),
						dbus.Boolean(False),
						dbus.Int32(tmo),
						EOD)

				if not no_wait:
					lv_path = self.handle_return(create_result)
					self.assertTrue(lv_path != '/')
					self._validate_lookup("%s/%s" % (vg_proxy.Vg.Name, lv_name), lv_path)
			else:
				# We ran out of space, test(s) may fail
				break
		return vg_proxy

	def _test_expired_timer(self, num_lvs):
		rc = False

		# In small configurations lvm is pretty snappy, so let's create a VG
		# add a number of LVs and then remove the VG and all the contained
		# LVs which appears to consistently run a little slow.

		vg_proxy = self._create_num_lvs(num_lvs)

		# Make sure that we are honoring the timeout
		start = time.time()

		remove_job = vg_proxy.Vg.Remove(dbus.Int32(1), EOD)

		end = time.time()

		tt_remove = float(end) - float(start)

		self.assertTrue(tt_remove < 2.0, "remove time %s" % (str(tt_remove)))

		# Depending on how long it took we could finish either way
		if remove_job != '/':
			# We got a job
			result = self._wait_for_job(remove_job)
			self.assertTrue(result == '/')
			rc = True
		else:
			# It completed before timer popped
			pass

		return rc

	# noinspection PyUnusedLocal
	def test_job_handling_timer(self):

		yes = False

		for pp in self.objs[PV_INT]:
			if '/dev/sd' not in pp.Pv.Name:
				std_err_print("Skipping test_job_handling_timer on loopback")
				return

		# This may not pass
		for i in [128, 256]:
			yes = self._test_expired_timer(i)
			if yes:
				break
			std_err_print('Attempt (%d) failed, trying again...' % (i))

		self.assertTrue(yes)

	def test_pv_tags(self):
		pvs = []
		vg = self._vg_create(self._all_pv_object_paths()).Vg

		# Get the PVs
		for p in vg.Pvs:
			pvs.append(ClientProxy(self.bus, p, interfaces=(PV_INT, )).Pv)

		for tags_value in [['hello'], ['foo', 'bar']]:

			rc = self.handle_return(
				vg.PvTagsAdd(
					dbus.Array(vg.Pvs, 'o'),
					dbus.Array(tags_value, 's'),
					dbus.Int32(g_tmo),
					EOD))
			self.assertTrue(rc == '/')

			for p in pvs:
				p.update()
				self.assertTrue(sorted(tags_value) == p.Tags)

			rc = self.handle_return(
				vg.PvTagsDel(
					dbus.Array(vg.Pvs, 'o'),
					dbus.Array(tags_value, 's'),
					dbus.Int32(g_tmo),
					EOD))
			self.assertEqual(rc, '/')

			for p in pvs:
				p.update()
				self.assertTrue([] == p.Tags)

	def test_vg_tags(self):
		vg = self._vg_create().Vg

		t = ['Testing', 'tags']

		self.handle_return(
			vg.TagsAdd(
				dbus.Array(t, 's'),
				dbus.Int32(g_tmo),
				EOD))

		vg.update()
		self.assertTrue(t == vg.Tags)

		self.handle_return(
			vg.TagsDel(
				dbus.Array(t, 's'),
				dbus.Int32(g_tmo),
				EOD))
		vg.update()
		self.assertTrue([] == vg.Tags)

	def _test_lv_tags(self, lv):
		t = ['Testing', 'tags']

		self.handle_return(
			lv.Lv.TagsAdd(
				dbus.Array(t, 's'), dbus.Int32(g_tmo), EOD))
		self._check_consistency()
		lv.update()
		self.assertTrue(t == lv.LvCommon.Tags)

		self.handle_return(
			lv.Lv.TagsDel(
				dbus.Array(t, 's'),
				dbus.Int32(g_tmo),
				EOD))
		self._check_consistency()
		lv.update()
		self.assertTrue([] == lv.LvCommon.Tags)

	def test_lv_tags(self):
		vg = self._vg_create().Vg
		lv = self._create_lv(vg=vg)
		self._test_lv_tags(lv)

	def test_vg_allocation_policy_set(self):
		vg = self._vg_create().Vg

		for p in ['anywhere', 'contiguous', 'cling', 'normal']:
			rc = self.handle_return(
				vg.AllocationPolicySet(
					dbus.String(p), dbus.Int32(g_tmo), EOD))

			self.assertEqual(rc, '/')
			vg.update()

			prop = getattr(vg, 'Alloc' + p.title())
			self.assertTrue(prop)

	def test_vg_max_pv(self):
		vg = self._vg_create([self.objs[PV_INT][0].object_path]).Vg
		for p in [0, 1, 10, 100, 100, 1024, 2 ** 32 - 1]:
			rc = self.handle_return(
				vg.MaxPvSet(
					dbus.UInt64(p), dbus.Int32(g_tmo), EOD))
			self.assertEqual(rc, '/')
			vg.update()
			self.assertTrue(
				vg.MaxPv == p,
				"Expected %s != Actual %s" % (str(p), str(vg.MaxPv)))

	def test_vg_max_lv(self):
		vg = self._vg_create().Vg
		for p in [0, 1, 10, 100, 100, 1024, 2 ** 32 - 1]:
			rc = self.handle_return(
				vg.MaxLvSet(
					dbus.UInt64(p), dbus.Int32(g_tmo), EOD))
			self.assertEqual(rc, '/')
			vg.update()
			self.assertTrue(
				vg.MaxLv == p,
				"Expected %s != Actual %s" % (str(p), str(vg.MaxLv)))

	def test_vg_uuid_gen(self):
		vg = self._vg_create().Vg
		prev_uuid = vg.Uuid
		rc = self.handle_return(
			vg.UuidGenerate(
				dbus.Int32(g_tmo),
				EOD))
		self.assertEqual(rc, '/')
		vg.update()
		self.assertTrue(
			vg.Uuid != prev_uuid,
			"Expected %s != Actual %s" % (vg.Uuid, prev_uuid))

	def test_vg_activate_deactivate(self):
		vg = self._vg_create().Vg
		self._create_lv(vg=vg)
		vg.update()

		rc = self.handle_return(
			vg.Deactivate(
				dbus.UInt64(0), dbus.Int32(g_tmo), EOD))
		self.assertEqual(rc, '/')
		self._check_consistency()

		rc = self.handle_return(
			vg.Activate(
				dbus.UInt64(0), dbus.Int32(g_tmo), EOD))

		self.assertEqual(rc, '/')
		self._check_consistency()

		# Try control flags
		for i in range(0, 5):
			self.handle_return(
				vg.Activate(
					dbus.UInt64(1 << i),
					dbus.Int32(g_tmo),
					EOD))

	def test_pv_resize(self):

		self.assertTrue(len(self.objs[PV_INT]) > 0)

		if len(self.objs[PV_INT]) > 0:
			pv = ClientProxy(
				self.bus, self.objs[PV_INT][0].object_path,
				interfaces=(PV_INT, )).Pv

			original_size = pv.SizeBytes

			new_size = original_size // 2

			self.handle_return(
				pv.ReSize(
					dbus.UInt64(new_size),
					dbus.Int32(g_tmo),
					EOD))

			self._check_consistency()
			pv.update()

			self.assertTrue(pv.SizeBytes != original_size)
			self.handle_return(
				pv.ReSize(
					dbus.UInt64(0),
					dbus.Int32(g_tmo),
					EOD))
			self._check_consistency()
			pv.update()
			self.assertTrue(pv.SizeBytes == original_size)

	def test_pv_allocation(self):
		vg = self._vg_create(self._all_pv_object_paths()).Vg

		pv = ClientProxy(self.bus, vg.Pvs[0], interfaces=(PV_INT, )).Pv

		self.handle_return(
			pv.AllocationEnabled(
				dbus.Boolean(False),
				dbus.Int32(g_tmo),
				EOD))

		pv.update()
		self.assertFalse(pv.Allocatable)

		self.handle_return(
			pv.AllocationEnabled(
				dbus.Boolean(True),
				dbus.Int32(g_tmo),
				EOD))

		self.handle_return(
			pv.AllocationEnabled(
				dbus.Boolean(True),
				dbus.Int32(g_tmo),
				EOD))
		pv.update()
		self.assertTrue(pv.Allocatable)

		self._check_consistency()

	@staticmethod
	def _get_devices():
		context = pyudev.Context()
		bd = context.list_devices(subsystem='block')
		# Handle block extended major too (259)
		return [b for b in bd if b.properties.get('MAJOR') == '8' or
				b.properties.get('MAJOR') == '259']

	def _pv_scan(self, activate, cache, device_paths, major_minors):
		mgr = self._manager().Manager
		return self.handle_return(
			mgr.PvScan(
				dbus.Boolean(activate),
				dbus.Boolean(cache),
				dbus.Array(device_paths, 's'),
				dbus.Array(major_minors, '(ii)'),
				dbus.Int32(g_tmo),
				EOD))

	def test_pv_scan(self):

		def major_minor(d):
			return (int(d.properties.get('MAJOR')), int(d.properties.get('MINOR')))

		devices = TestDbusService._get_devices()

		self.assertEqual(self._pv_scan(False, True, [], []), '/')
		self._check_consistency()
		self.assertEqual(self._pv_scan(False, False, [], []), '/')
		self._check_consistency()

		block_path = [d.properties.get('DEVNAME') for d in devices]
		self.assertEqual(self._pv_scan(False, True, block_path, []), '/')
		self._check_consistency()

		mm = [major_minor(d) for d in devices]

		self.assertEqual(self._pv_scan(False, True, block_path, mm), '/')
		self._check_consistency()

		self.assertEqual(self._pv_scan(False, True, [], mm), '/')
		self._check_consistency()

	@staticmethod
	def _write_some_data(device_path, size):
		blocks = int(size // 512)
		block = bytearray(512)
		for i in range(0, 512):
			block[i] = i % 255

		with open(device_path, mode='wb') as lv:
			for i in range(0, blocks):
				lv.write(block)

	def test_snapshot_merge(self):
		# Create a non-thin LV and merge it
		ss_size = mib(8)

		lv_p = self._create_lv(size=mib(16))
		ss_name = lv_p.LvCommon.Name + '_snap'

		snapshot_path = self.handle_return(
			lv_p.Lv.Snapshot(
				dbus.String(ss_name),
				dbus.UInt64(ss_size),
				dbus.Int32(g_tmo),
				EOD))

		intf = (LV_COMMON_INT, LV_INT, SNAPSHOT_INT, )
		ss = ClientProxy(self.bus, snapshot_path, interfaces=intf)

		# Write some data to snapshot so merge takes some time
		TestDbusService._write_some_data(ss.LvCommon.Path, ss_size // 2)

		job_path = self.handle_return(
			ss.Snapshot.Merge(
				dbus.Int32(g_tmo),
				EOD))
		self.assertEqual(job_path, '/')

	def test_snapshot_merge_thin(self):
		# Create a thin LV, snapshot it and merge it
		_vg, _thin_path, lv_p = self._create_thin_lv()

		ss_name = lv_p.LvCommon.Name + '_snap'
		snapshot_path = self.handle_return(
			lv_p.Lv.Snapshot(
				dbus.String(ss_name),
				dbus.UInt64(0),
				dbus.Int32(g_tmo),
				EOD))

		intf = (LV_INT, LV_COMMON_INT, SNAPSHOT_INT)
		ss = ClientProxy(self.bus, snapshot_path, interfaces=intf)

		job_path = self.handle_return(
			ss.Snapshot.Merge(
				dbus.Int32(g_tmo), EOD)
		)
		self.assertTrue(job_path == '/')

	def _create_cache_pool(self, vg=None):

		if not vg:
			vg = self._vg_create().Vg

		md = self._create_lv(size=(mib(8)), vg=vg)
		data = self._create_lv(size=(mib(8)), vg=vg)

		cache_pool_path = self.handle_return(
			vg.CreateCachePool(
				dbus.ObjectPath(md.object_path),
				dbus.ObjectPath(data.object_path),
				dbus.Int32(g_tmo),
				EOD))

		intf = (CACHE_POOL_INT, )
		cp = ClientProxy(self.bus, cache_pool_path, interfaces=intf)

		return vg, cp

	def test_cache_pool_create(self):

		vg, cache_pool = self._create_cache_pool()

		self.assertTrue(
			'/com/redhat/lvmdbus1/CachePool' in cache_pool.object_path)

	def _create_cache_lv(self, return_all=False):
		vg, cache_pool = self._create_cache_pool()

		lv_to_cache = self._create_lv(size=mib(32), vg=vg)

		c_lv_path = self.handle_return(
			cache_pool.CachePool.CacheLv(
				dbus.ObjectPath(lv_to_cache.object_path),
				dbus.Int32(g_tmo),
				EOD))

		intf = (LV_COMMON_INT, LV_INT, CACHE_LV_INT)
		cached_lv = ClientProxy(self.bus, c_lv_path, interfaces=intf)

		if return_all:
			return vg, cache_pool, cached_lv
		return cached_lv

	def test_cache_lv_create(self):

		for destroy_cache in [True, False]:
			vg, _, cached_lv = self._create_cache_lv(True)
			uncached_lv_path = self.handle_return(
				cached_lv.CachedLv.DetachCachePool(
					dbus.Boolean(destroy_cache),
					dbus.Int32(g_tmo),
					EOD))

			self.assertTrue(
				'/com/redhat/lvmdbus1/Lv' in uncached_lv_path)

			rc = self.handle_return(
				vg.Remove(dbus.Int32(g_tmo), EOD))
			self.assertTrue(rc == '/')

	def test_cache_lv_rename(self):
		"""
		Make sure that if we rename a cache lv that we correctly handle the
		internal state update.
		:return:
		"""
		def verify_cache_lv_count():
			cur_objs, _ = get_objects()
			self.assertEqual(len(cur_objs[CACHE_LV_INT]), 2)
			self._check_consistency()

		cached_lv = self._create_cache_lv()

		verify_cache_lv_count()
		new_name = 'renamed_' + cached_lv.LvCommon.Name
		self.handle_return(
			cached_lv.Lv.Rename(dbus.String(new_name), dbus.Int32(g_tmo), EOD))
		verify_cache_lv_count()

	def test_writecache_lv(self):
		vg = self._vg_create().Vg
		data_lv = self._create_lv(size=mib(16), vg=vg)
		cache_lv = self._create_lv(size=mib(16), vg=vg)

		# both LVs need to be inactive
		self.handle_return(data_lv.Lv.Deactivate(
			dbus.UInt64(0), dbus.Int32(g_tmo), EOD))
		data_lv.update()
		self.handle_return(cache_lv.Lv.Deactivate(
			dbus.UInt64(0), dbus.Int32(g_tmo), EOD))
		cache_lv.update()

		cached_lv_path = self.handle_return(
			cache_lv.Lv.WriteCacheLv(
				dbus.ObjectPath(data_lv.object_path),
				dbus.Int32(g_tmo),
				EOD))

		intf = (LV_COMMON_INT, LV_INT, CACHE_LV_INT)
		cached_lv = ClientProxy(self.bus, cached_lv_path, interfaces=intf)
		self.assertEqual(cached_lv.LvCommon.SegType, ["writecache"])

		uncached_lv_path = self.handle_return(
				cached_lv.CachedLv.DetachCachePool(
					dbus.Boolean(True),
					dbus.Int32(g_tmo),
					EOD))
		self.assertTrue('/com/redhat/lvmdbus1/Lv' in uncached_lv_path)

	def test_vg_change(self):
		vg_proxy = self._vg_create()

		result = self.handle_return(vg_proxy.Vg.Change(
			dbus.Int32(g_tmo),
			dbus.Dictionary({'-a': 'ay'}, 'sv')))
		self.assertTrue(result == '/')

		result = self.handle_return(
			vg_proxy.Vg.Change(
				dbus.Int32(g_tmo),
				dbus.Dictionary({'-a': 'n'}, 'sv')))
		self.assertTrue(result == '/')

	@staticmethod
	def _invalid_vg_lv_name_characters():
		bad_vg_lv_set = set(string.printable) - \
			set(string.ascii_letters + string.digits + '.-_+')
		return ''.join(bad_vg_lv_set)

	def test_invalid_names(self):
		mgr = self.objs[MANAGER_INT][0].Manager

		# Pv device path
		with self.assertRaises(dbus.exceptions.DBusException):
			self.handle_return(
				mgr.PvCreate(
					dbus.String("/dev/space in name"),
					dbus.Int32(g_tmo),
					EOD))

		# VG Name testing...
		# Go through all bad characters
		pv_paths = [self.objs[PV_INT][0].object_path]
		bad_chars = TestDbusService._invalid_vg_lv_name_characters()
		for c in bad_chars:
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					mgr.VgCreate(
						dbus.String("name%s" % (c)),
						dbus.Array(pv_paths, 'o'),
						dbus.Int32(g_tmo),
						EOD))

		# Bad names
		for bad in [".", ".."]:
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					mgr.VgCreate(
						dbus.String(bad),
						dbus.Array(pv_paths, 'o'),
						dbus.Int32(g_tmo),
						EOD))

		# Exceed name length
		for i in [128, 1024, 4096]:
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					mgr.VgCreate(
						dbus.String('T' * i),
						dbus.Array(pv_paths, 'o'),
						dbus.Int32(g_tmo),
						EOD))

		# Create a VG and try to create LVs with different bad names
		vg_name = vg_n()
		vg_path = self.handle_return(
			mgr.VgCreate(
				dbus.String(vg_name),
				dbus.Array(pv_paths, 'o'),
				dbus.Int32(g_tmo),
				EOD))
		self._validate_lookup(vg_name, vg_path)

		vg_proxy = ClientProxy(self.bus, vg_path, interfaces=(VG_INT, ))

		for c in bad_chars:
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					vg_proxy.Vg.LvCreateLinear(
						dbus.String(lv_n() + c),
						dbus.UInt64(mib(4)),
						dbus.Boolean(False),
						dbus.Int32(g_tmo),
						EOD))

		for reserved in (
				"_cdata", "_cmeta", "_corig", "_mimage", "_mlog",
				"_pmspare", "_rimage", "_rmeta", "_tdata", "_tmeta",
				"_vorigin", "_vdata"):
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					vg_proxy.Vg.LvCreateLinear(
						dbus.String(lv_n() + reserved),
						dbus.UInt64(mib(4)),
						dbus.Boolean(False),
						dbus.Int32(g_tmo),
						EOD))

		for reserved in ("snapshot", "pvmove"):
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					vg_proxy.Vg.LvCreateLinear(
						dbus.String(reserved + lv_n()),
						dbus.UInt64(mib(4)),
						dbus.Boolean(False),
						dbus.Int32(g_tmo),
						EOD))

	_ALLOWABLE_TAG_CH = string.ascii_letters + string.digits + "._-+/=!:&#"

	def _invalid_tag_characters(self):
		bad_tag_ch_set = set(string.printable) - set(self._ALLOWABLE_TAG_CH)
		return ''.join(bad_tag_ch_set)

	def test_invalid_tags(self):
		vg_proxy = self._vg_create()

		for c in self._invalid_tag_characters():
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					vg_proxy.Vg.TagsAdd(
						dbus.Array([c], 's'),
						dbus.Int32(g_tmo),
						EOD))

		for c in self._invalid_tag_characters():
			with self.assertRaises(dbus.exceptions.DBusException):
				self.handle_return(
					vg_proxy.Vg.TagsAdd(
						dbus.Array(["a%sb" % (c)], 's'),
						dbus.Int32(g_tmo),
						EOD))

	def _tag_add_common(self, vg_proxy, tag):
		tmp = self.handle_return(
			vg_proxy.Vg.TagsAdd(
				dbus.Array([tag], 's'),
				dbus.Int32(g_tmo),
				EOD))
		self.assertTrue(tmp == '/')
		vg_proxy.update()

		self.assertTrue(
			tag in vg_proxy.Vg.Tags,
			"%s not in %s" % (tag, str(vg_proxy.Vg.Tags)))

	def test_tag_names(self):
		vg_proxy = self._vg_create()

		for i in range(1, 64):
			tag = rs(i, "", self._ALLOWABLE_TAG_CH)
			self._tag_add_common(vg_proxy, tag)

			self.assertEqual(
				i, len(vg_proxy.Vg.Tags),
				"%d != %d" % (i, len(vg_proxy.Vg.Tags)))

	def test_tag_regression(self):
		vg_proxy = self._vg_create()
		tag = '--h/K.6g0A4FOEatf3+k_nI/Yp&L_u2oy-=j649x:+dUcYWPEo6.IWT0c'
		self._tag_add_common(vg_proxy, tag)

	def _verify_existence(self, cmd, operation, resource_name):
		ec, stdout, stderr = call_lvm(cmd)
		if ec == 0:
			path = self._lookup(resource_name)
			self.assertTrue(path != '/')
		else:
			std_err_print(
				"%s failed with stdout= %s, stderr= %s" %
				(operation, stdout, stderr))
			self.assertTrue(ec == 0, "%s exit code = %d" % (operation, ec))

	def test_external_vg_create(self):
		# We need to ensure that if a user creates something outside lvm
		# dbus service that things are sequenced correctly so that if a dbus
		# user calls into the service they will find the same information.
		vg_name = vg_n()

		# Get all the PV device paths
		pv_device_paths = [p.Pv.Name for p in self.objs[PV_INT]]

		cmd = ['vgcreate', vg_name]
		cmd.extend(pv_device_paths)
		self._verify_existence(cmd, cmd[0], vg_name)

	def test_external_lv_create(self):
		# Let's create a LV outside of service and see if we correctly handle
		# its inclusion
		vg = self._vg_create().Vg
		lv_name = lv_n()
		full_name = "%s/%s" % (vg.Name, lv_name)

		cmd = ['lvcreate', '-L4M', '-n', lv_name, vg.Name]
		self._verify_existence(cmd, cmd[0], full_name)

	def test_external_pv_create(self):
		# Let's create a PV outside of service and see if we correctly handle
		# its inclusion
		target = self.objs[PV_INT][0]

		# Remove the PV
		rc = self._pv_remove(target)
		self.assertTrue(rc == '/')
		self._check_consistency()

		# Make sure the PV we removed no longer exists
		self.assertTrue(self._lookup(target.Pv.Name) == '/')

		# Add it back with external command line
		cmd = ['pvcreate', target.Pv.Name]
		self._verify_existence(cmd, cmd[0], target.Pv.Name)

	def _create_nested(self, pv_object_path, vg_suffix):
		vg = self._vg_create([pv_object_path], vg_suffix)
		pv = ClientProxy(self.bus, pv_object_path, interfaces=(PV_INT,))

		self.assertEqual(pv.Pv.Vg, vg.object_path)
		self.assertIn(
			pv_object_path, vg.Vg.Pvs, "Expecting PV object path in Vg.Pvs")

		lv = self._create_lv(
			vg=vg.Vg, size=vg.Vg.FreeBytes, suffix="_pv0")
		device_path = '/dev/%s/%s' % (vg.Vg.Name, lv.LvCommon.Name)
		dev_info = os.stat(device_path)
		major = os.major(dev_info.st_rdev)
		minor = os.minor(dev_info.st_rdev)
		sysfs = "/sys/dev/block/%d:%d" % (major, minor)
		self.assertTrue(os.path.exists(sysfs))
		new_pv_object_path = self._pv_create(device_path)
		vg.update()

		self.assertEqual(lv.LvCommon.Vg, vg.object_path)
		self.assertIn(
			lv.object_path, vg.Vg.Lvs, "Expecting LV object path in Vg.Lvs")

		new_pv_proxy = ClientProxy(
			self.bus, new_pv_object_path, interfaces=(PV_INT, ))
		self.assertEqual(new_pv_proxy.Pv.Name, device_path)

		return new_pv_object_path

	@staticmethod
	def _scan_lvs_enabled():
		cmd = ['lvmconfig',  '--typeconfig', 'full', 'devices/scan_lvs']
		config = Popen(cmd, stdout=PIPE, stderr=PIPE, close_fds=True, env=os.environ)
		out = config.communicate()
		if config.returncode != 0:
			return False
		if "scan_lvs=1" == out[0].decode("utf-8").strip():
			return True
		return False

	def test_nesting(self):
		# check to see if we handle an LV becoming a PV which has it's own
		# LV
		#
		# NOTE: This needs an equivalent of aux extend_filter_LVMTEST
		# when run from lvm2 testsuite. See dbustest.sh.
		# Also, if developing locally with actual devices one can achieve this
		# by editing lvm.conf with "devices/scan_lvs = 1"  As testing
		# typically utilizes loopback, this test is skipped in
		# those environments.

		if dm_dev_dir != '/dev':
			raise unittest.SkipTest('test not running in real /dev')
		if not TestDbusService._scan_lvs_enabled():
			raise unittest.SkipTest('scan_lvs=0 in config, unit test requires scan_lvs=1')
		pv_object_path = self.objs[PV_INT][0].object_path
		if not self.objs[PV_INT][0].Pv.Name.startswith("/dev"):
			raise unittest.SkipTest('test not running in /dev')

		for i in range(0, 5):
			pv_object_path = self._create_nested(pv_object_path, "nest_%d_" % i)

	def test_pv_symlinks(self):
		# Let's take one of our test PVs, pvremove it, find a symlink to it
		# and re-create using the symlink to ensure we return an object
		# path to it.  Additionally, we will take the symlink and do a lookup
		# (Manager.LookUpByLvmId) using it and the original device path to
		# ensure that we can find the PV.
		symlink = None

		pv = self.objs[PV_INT][0]
		pv_device_path = pv.Pv.Name

		if dm_dev_dir != '/dev':
			raise unittest.SkipTest('test not running in real /dev')

		if not pv_device_path.startswith("/dev"):
			raise unittest.SkipTest('test not running in /dev')

		self._pv_remove(pv)

		# Make sure we no longer find the pv
		rc = self._lookup(pv_device_path)
		self.assertEqual(rc, '/')

		# Let's locate a symlink for it
		devices = glob('/dev/disk/*/*')
		rp_pv_device_path = os.path.realpath(pv_device_path)
		for d in devices:
			if rp_pv_device_path == os.path.realpath(d):
				symlink = d
				break

		self.assertIsNotNone(symlink, "We expected to find at least 1 symlink!")

		# Make sure symlink look up fails too
		rc = self._lookup(symlink)
		self.assertEqual(rc, '/')

		### pv_object_path = self._pv_create(symlink)
		### Test is limited by filter rules and must use  /dev/mapper/LVMTEST path
		pv_object_path = self._pv_create(pv_device_path)

		self.assertNotEqual(pv_object_path, '/')

		pv_proxy = ClientProxy(self.bus, pv_object_path, interfaces=(PV_INT, ))
		self.assertEqual(pv_proxy.Pv.Name, pv_device_path)

		# Lets check symlink lookup
		self.assertEqual(pv_object_path, self._lookup(pv_device_path))

	def _create_vdo_pool_and_lv(self, vg_prefix="vdo_"):
		pool_name = lv_n("_vdo_pool")
		lv_name = lv_n()

		vg_proxy = self._vg_create(vg_prefix=vg_prefix)
		vdo_pool_object_path = self.handle_return(
			vg_proxy.VgVdo.CreateVdoPoolandLv(
				pool_name, lv_name,
				dbus.UInt64(VDO_MIN_SIZE),
				dbus.UInt64(VDO_MIN_SIZE * 2),
				dbus.Int32(g_tmo),
				EOD))

		self.assertNotEqual(vdo_pool_object_path, "/")
		self.assertEqual(
			vdo_pool_object_path,
			self._lookup("%s/%s" % (vg_proxy.Vg.Name, pool_name)))

		vdo_pool_path = self._lookup("%s/%s" % (vg_proxy.Vg.Name, pool_name))
		self.assertNotEqual(vdo_pool_path, "/")
		intf = [LV_COMMON_INT, LV_INT]
		vdo_lv_obj_path = self._lookup("%s/%s" % (vg_proxy.Vg.Name, lv_name))
		vdo_lv = ClientProxy(self.bus, vdo_lv_obj_path, interfaces=intf)
		intf.append(VDOPOOL_INT)
		vdo_pool_lv = ClientProxy(self.bus, vdo_pool_path, interfaces=intf)
		return vg_proxy, vdo_pool_lv, vdo_lv

	def _create_vdo_lv(self):
		return self._create_vdo_pool_and_lv()[2]

	def _vdo_pool_lv(self):
		return self._create_vdo_pool_and_lv()[1]

	def test_vdo_pool_create(self):
		# Basic vdo sanity testing
		if not self.vdo:
			raise unittest.SkipTest('vdo not supported')

		# Do this twice to ensure we are providing the correct flags to force
		# the operation when it finds an existing vdo signature, which likely
		# shouldn't exist.
		for _ in range(0, 2):
			vg, _, _ = self._create_vdo_pool_and_lv()
			self.handle_return(vg.Vg.Remove(dbus.Int32(g_tmo), EOD))

	def _create_vdo_pool(self):
		pool_name = lv_n('_vdo_pool')
		lv_name = lv_n('_vdo_data')
		vg_proxy = self._vg_create(vg_prefix="vdo_conv_")
		lv = self._test_lv_create(
			vg_proxy.Vg.LvCreate,
			(dbus.String(pool_name), dbus.UInt64(VDO_MIN_SIZE),
				dbus.Array([], signature='(ott)'), dbus.Int32(g_tmo),
				EOD), vg_proxy.Vg, LV_BASE_INT)
		lv_obj_path = self._lookup("%s/%s" % (vg_proxy.Vg.Name, pool_name))
		self.assertNotEqual(lv_obj_path, "/")

		vdo_pool_path = self.handle_return(
			vg_proxy.VgVdo.CreateVdoPool(
				dbus.ObjectPath(lv.object_path), lv_name,
				dbus.UInt64(VDO_MIN_SIZE),
				dbus.Int32(g_tmo),
				EOD))

		self.assertNotEqual(vdo_pool_path, "/")
		self.assertEqual(
			vdo_pool_path,
			self._lookup("%s/%s" % (vg_proxy.Vg.Name, pool_name)))
		intf = [LV_COMMON_INT, LV_INT]
		vdo_lv_obj_path = self._lookup("%s/%s" % (vg_proxy.Vg.Name, lv_name))
		vdo_lv = ClientProxy(self.bus, vdo_lv_obj_path, interfaces=intf)
		intf.append(VDOPOOL_INT)
		vdo_pool_lv = ClientProxy(self.bus, vdo_pool_path, interfaces=intf)
		return vg_proxy, vdo_pool_lv, vdo_lv

	def test_vdo_pool_convert(self):
		# Basic vdo sanity testing
		if not self.vdo:
			raise unittest.SkipTest('vdo not supported')

		vg, _pool, _lv = self._create_vdo_pool()
		self.handle_return(vg.Vg.Remove(dbus.Int32(g_tmo), EOD))

	def test_vdo_pool_compression_deduplication(self):
		if not self.vdo:
			raise unittest.SkipTest('vdo not supported')

		vg, pool, _lv = self._create_vdo_pool_and_lv(vg_prefix="vdo2_")

		# compression and deduplication should be enabled by default
		self.assertEqual(pool.VdoPool.Compression, "enabled")
		self.assertEqual(pool.VdoPool.Deduplication, "enabled")

		self.handle_return(
			pool.VdoPool.DisableCompression(dbus.Int32(g_tmo), EOD))
		self.handle_return(
			pool.VdoPool.DisableDeduplication(dbus.Int32(g_tmo), EOD))
		pool.update()
		self.assertEqual(pool.VdoPool.Compression, "")
		self.assertEqual(pool.VdoPool.Deduplication, "")

		self.handle_return(
			pool.VdoPool.EnableCompression(dbus.Int32(g_tmo), EOD))
		self.handle_return(
			pool.VdoPool.EnableDeduplication(dbus.Int32(g_tmo), EOD))
		pool.update()
		self.assertEqual(pool.VdoPool.Compression, "enabled")
		self.assertEqual(pool.VdoPool.Deduplication, "enabled")

		self.handle_return(vg.Vg.Remove(dbus.Int32(g_tmo), EOD))

	def _test_lv_method_interface(self, lv):
		self._rename_lv_test(lv)
		self._test_activate_deactivate(lv)
		self._test_lv_tags(lv)
		self._test_lv_resize(lv)

	def _test_lv_method_interface_sequence(
			self, lv, test_ss=True, remove_lv=True):
		self._test_lv_method_interface(lv)

		# We can't take a snapshot of a pool lv (not yet).
		if test_ss:
			ss_lv = self._take_lv_snapshot(lv)
			self._test_lv_method_interface(ss_lv)
			self._lv_remove(ss_lv)

		if remove_lv:
			self._lv_remove(lv)

	def test_lv_interface_plain_lv(self):
		self._test_lv_method_interface_sequence(self._create_lv())

	def test_lv_interface_vdo_lv(self):
		if not self.vdo:
			raise unittest.SkipTest('vdo not supported')
		self._test_lv_method_interface_sequence(self._create_vdo_lv())

	def test_lv_interface_cache_lv(self):
		self._test_lv_method_interface_sequence(
			self._create_cache_lv(), remove_lv=False)

	def test_lv_interface_thin_pool_lv(self):
		self._test_lv_method_interface_sequence(
			self._create_thin_pool_lv(), test_ss=False)

	def test_lv_interface_vdo_pool_lv(self):
		if not self.vdo:
			raise unittest.SkipTest('vdo not supported')
		self._test_lv_method_interface_sequence(
			self._vdo_pool_lv(), test_ss=False)

	def _log_file_option(self):
		fn = os.path.join(tempfile.gettempdir(), rs(8, "_lvm.log"))
		try:
			options = dbus.Dictionary({}, signature=dbus.Signature('sv'))
			option_str = "log { level=7 file=%s syslog=0 }" % fn
			options["config"] = dbus.String(option_str)
			self._vg_create(None, None, options)
			self.assertTrue(os.path.exists(fn),
							"We passed the following options %s to lvm while creating a VG and the "
							"log file we expected to exist (%s) was not found" % (option_str, fn))
		finally:
			if os.path.exists(fn):
				os.unlink(fn)

	def test_log_file_option(self):
		self._log_file_option()

	def test_external_event(self):
		# Call into the service to register an external event, so that we can test sending the path
		# where we don't send notifications on the command line in addition to the logging
		lvm_manager = dbus.Interface(bus.get_object(
			BUS_NAME, "/com/redhat/lvmdbus1/Manager", introspect=False),
			"com.redhat.lvmdbus1.Manager")
		rc = lvm_manager.ExternalEvent("unit_test")
		self.assertTrue(rc == 0)
		self._log_file_option()

	def test_delete_non_complete_job(self):
		# Let's create a vg with some number of lvs and then delete it all
		# to hopefully create a long-running job.
		vg_proxy = self._create_num_lvs(4)
		job_path = vg_proxy.Vg.Remove(dbus.Int32(0), EOD)
		self.assertNotEqual(job_path, "/")

		# Try to delete the job expecting an exception
		job_proxy = ClientProxy(self.bus, job_path, interfaces=(JOB_INT,)).Job
		with self.assertRaises(dbus.exceptions.DBusException):
			try:
				job_proxy.Remove()
			except dbus.exceptions.DBusException as e:
				# Verify we got the expected text in exception
				self.assertTrue('Job is not complete!' in str(e))
				raise e

	def test_z_sigint(self):

		number_of_intervals = 3
		number_of_lvs = 10

		# Issue SIGINT while daemon is processing work to ensure we shut down.
		if bool(int(os.getenv("LVM_DBUSD_TEST_SKIP_SIGNAL", "0"))):
			raise unittest.SkipTest("Skipping as env. LVM_DBUSD_TEST_SKIP_SIGNAL is '1'")

		if g_tmo != 0:
			raise unittest.SkipTest("Skipping for g_tmo != 0")

		di = DaemonInfo.get()
		self.assertTrue(di is not None)
		if di:
			# Find out how long it takes to create a VG and a number of LVs
			# we will then issue the creation of the LVs async., wait, then issue a signal
			# and repeat stepping through the entire time range.
			start = time.time()
			vg_proxy = self._create_num_lvs(number_of_lvs)
			end = time.time()

			self.handle_return(vg_proxy.Vg.Remove(dbus.Int32(g_tmo), EOD))
			total = end - start

			for i in range(number_of_intervals):
				sleep_amt = i * (total/float(number_of_intervals))
				self._create_num_lvs(number_of_lvs, True)
				time.sleep(sleep_amt)

				exited = False
				try:
					di.term_signal(signal.SIGINT)
					exited = True
				except Exception:
					std_err_print("Failed to exit on SIGINT, sending SIGKILL...")
					di.term_signal(signal.SIGKILL)
				finally:
					di.start()
					self.clean_up()

				self.assertTrue(exited,
								"Failed to exit after sending signal %f seconds after "
								"queuing up work for signal %d" % (sleep_amt, signal.SIGINT))
		set_exec_mode(g_lvm_shell)

	def test_z_singleton_daemon(self):
		# Ensure we can only have 1 daemon running at a time, daemon should exit with 114 if already running
		di = DaemonInfo.get()
		self.assertTrue(di is not None)
		if di.systemd:
			raise unittest.SkipTest('existing daemon running via systemd')
		if di:
			ec = di.start(True)
			self.assertEqual(ec, 114)

	def test_z_switching(self):
		# Ensure we can switch from forking to shell repeatedly
		try:
			t_mode = True
			for _ in range(50):
				t_mode = not t_mode
				set_exec_mode(t_mode)
		finally:
			set_exec_mode(g_lvm_shell)

	@staticmethod
	def _wipe_it(block_device):
		cmd = ["/usr/sbin/wipefs", '-a', block_device]
		config = Popen(cmd, stdout=PIPE, stderr=PIPE, close_fds=True, env=os.environ)
		config.communicate()
		if config.returncode != 0:
			return False
		return True

	def _block_present_absent(self, block_device, present=False):
		start = time.time()
		keep_looping = True
		max_wait = 5
		while keep_looping and time.time() < start + max_wait:
			time.sleep(0.2)
			if present:
				if (self._lookup(block_device) != "/"):
					keep_looping = False
			else:
				if (self._lookup(block_device) == "/"):
					keep_looping = False

		if keep_looping:
			print("Daemon failed to update within %d seconds!" % max_wait)
		else:
			print("Note: Time for udev update = %f" % (time.time() - start))
		if present:
			rc = self._lookup(block_device)
			self.assertNotEqual(rc, '/', "Daemon failed to update, missing udev change event?")
			return True
		else:
			rc = self._lookup(block_device)
			self.assertEqual(rc, '/', "Daemon failed to update, missing udev change event?")
			return True

	def test_wipefs(self):
		# Ensure we update the status of the daemon if an external process clears a PV
		pv = self.objs[PV_INT][0]
		pv_device_path = pv.Pv.Name

		wipe_result = TestDbusService._wipe_it(pv_device_path)
		self.assertTrue(wipe_result)

		if wipe_result:
			# Need to wait a bit before the daemon will reflect the change
			self._block_present_absent(pv_device_path, False)

			# Put it back
			pv_object_path = self._pv_create(pv_device_path)
			self.assertNotEqual(pv_object_path, '/')

	@staticmethod
	def _write_signature(device, data=None):
		fd = os.open(device, os.O_RDWR|os.O_EXCL|os.O_NONBLOCK)
		existing = os.read(fd, 1024)
		os.lseek(fd, 0, os.SEEK_SET)

		if data is None:
			data_copy = bytearray(existing)
			# Clear lvm signature
			data_copy[536:536+9] = bytearray(8)
			os.write(fd, data_copy)
		else:
			os.write(fd, data)
		os.sync()
		os.close(fd)
		return existing

	def test_copy_signature(self):
		# Ensure we update the state of the daemon if an external process copies
		# a pv signature onto a block device
		pv = self.objs[PV_INT][0]
		pv_device_path = pv.Pv.Name

		try:
			existing = TestDbusService._write_signature(pv_device_path, None)
			if self._block_present_absent(pv_device_path, False):
				TestDbusService._write_signature(pv_device_path, existing)
				self._block_present_absent(pv_device_path, True)
		finally:
			# Ensure we put the PV back for sure.
			rc = self._lookup(pv_device_path)
			if rc == "/":
				self._pv_create(pv_device_path)

	def test_stderr_collection(self):
		lv_name = lv_n()
		vg = self._vg_create().Vg
		(object_path, job_path) = vg.LvCreate(
			dbus.String(lv_name), dbus.UInt64(vg.SizeBytes * 2),
			dbus.Array([], signature='(ott)'), dbus.Int32(0),
			EOD)

		self.assertTrue(object_path == '/')
		self.assertTrue(job_path != '/')

		j = ClientProxy(self.bus, job_path, interfaces=(JOB_INT,)).Job
		while True:
			j.update()
			if j.Complete:
				(ec, error_msg) = j.GetError
				self.assertTrue("insufficient free space" in error_msg,
								"We're expecting 'insufficient free space' in \n\"%s\"\n, stderr missing?" % error_msg)
				break
			else:
				time.sleep(0.1)

	@staticmethod
	def _is_vg_devices_supported():
		rc, stdout_txt, stderr_txt = call_lvm(["vgcreate", "--help"])
		if rc == 0:
			for line in stdout_txt.split("\n"):
				if "--devices " in line:
					return True
		return False

	@staticmethod
	def _vg_create_specify_devices(name, device):
		cmd = [LVM_EXECUTABLE, "vgcreate", "--devices", device, name, device]
		outcome = Popen(cmd, stdout=PIPE, stderr=PIPE, close_fds=True, env=os.environ)
		outcome.communicate()
		if outcome.returncode == 0:
			return True
		else:
			print("Failed to create vg %s, stdout= %s, stderr= %s" % (name, outcome.stdout, outcome.stderr))
			return False

	def test_duplicate_vg_name(self):
		# LVM allows duplicate VG names, test handling renames for now
		if not TestDbusService._is_vg_devices_supported():
			raise unittest.SkipTest("lvm does not support vgcreate with --device syntax")

		if len(self.objs[PV_INT]) < 2:
			raise unittest.SkipTest("we need at least 2 PVs to run test")

		vg_name = vg_n()
		if TestDbusService._vg_create_specify_devices(vg_name, self.objs[PV_INT][0].Pv.Name) and \
				TestDbusService._vg_create_specify_devices(vg_name, self.objs[PV_INT][1].Pv.Name):
			objects, _ = get_objects()
			self.assertEqual(len(objects[VG_INT]), 2)

			if len(objects[VG_INT]) == 2:
				for vg in objects[VG_INT]:
					new_name = vg_n()
					vg.Vg.Rename(dbus.String(new_name), dbus.Int32(g_tmo), EOD)
					# Ensure we find the renamed VG
					self.assertNotEqual("/", self._lookup(new_name), "Expecting to find VG='%s'" % new_name)
		else:
			self.assertFalse(True, "We failed to create 2 VGs with same name!")


class AggregateResults(object):

	def __init__(self):
		self.no_errors = True

	def register_result(self, result):
		if not result.result.wasSuccessful():
			self.no_errors = False

	def register_fail(self):
		self.no_errors = False

	def exit_run(self):
		if self.no_errors:
			sys.exit(0)
		sys.exit(1)


if __name__ == '__main__':

	r = AggregateResults()
	mode = int(test_shell)

	# To test with error injection, simply set the env. variable LVM_BINARY to the error inject script
	# and the LVM_MAN_IN_MIDDLE variable to the lvm binary to test which defaults to "/usr/sbin/lvm"
	# An example
	# export LVM_BINARY=/home/tasleson/projects/lvm2/test/dbus/lvm_error_inject.py
	# export LVM_MAN_IN_MIDDLE=/home/tasleson/projects/lvm2/tools/lvm

	if mode == 0:
		std_err_print('\n*** Testing only lvm fork & exec test mode ***\n')
	elif mode == 1:
		std_err_print('\n*** Testing only lvm shell mode ***\n')
	elif mode == 2:
		std_err_print('\n*** Testing fork & exec & lvm shell mode ***\n')
	else:
		std_err_print("Unsupported \"LVM_DBUSD_TEST_MODE\"=%d, [0-2] valid" % mode)
		sys.exit(1)

	for g_tmo in [0, 15]:
		std_err_print('Testing TMO=%d\n' % g_tmo)
		if mode == 0:
			if set_execution(False, r):
				r.register_result(unittest.main(exit=False))
		elif mode == 1:
			if set_execution(True, r):
				r.register_result(unittest.main(exit=False))
		else:
			if set_execution(False, r):
				r.register_result(unittest.main(exit=False))
			# Test lvm shell
			if set_execution(True, r):
				r.register_result(unittest.main(exit=False))

		if not r.no_errors:
			break

	r.exit_run()
