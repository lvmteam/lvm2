# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
import errno
from subprocess import Popen, PIPE
import select
import time
import threading
from itertools import chain
import collections
import os

from lvmdbusd import cfg
from lvmdbusd.utils import pv_dest_ranges, log_debug, log_error, add_no_notify,\
			make_non_block, read_decoded, extract_stack_trace, LvmBug, add_config_option, get_error_msg
from lvmdbusd.lvm_shell_proxy import LVMShellProxy

try:
	import simplejson as json
except ImportError:
	import json


total_time = 0.0
total_count = 0

# We need to prevent different threads from using the same lvm shell
# at the same time.
cmd_lock = threading.RLock()


class LvmExecutionMeta(object):

	def __init__(self, start, ended, cmd, ec=-1000, stdout_txt=None, stderr_txt=None):
		self.lock = threading.RLock()
		self.start = start
		self.ended = ended
		self.cmd = cmd
		self.ec = ec
		self.stdout_txt = stdout_txt
		self.stderr_txt = stderr_txt

	def __str__(self):
		with self.lock:
			if self.ended == 0:
				ended_txt = "still running"
				self.ended = time.time()
			else:
				ended_txt = str(time.ctime(self.ended))

			return 'EC= %d for "%s"\n' \
				"STARTED: %s, ENDED: %s, DURATION: %f\n" \
				"STDOUT=%s\n" \
				"STDERR=%s\n" % \
				(self.ec, " ".join(self.cmd), time.ctime(self.start), ended_txt, float(self.ended) - self.start,
					self.stdout_txt,
					self.stderr_txt)

	def completed(self, end_time, ec, stdout_txt, stderr_txt):
		with self.lock:
			self.ended = end_time
			self.ec = ec
			self.stdout_txt = stdout_txt
			self.stderr_txt = stderr_txt


class LvmFlightRecorder(object):

	def __init__(self, size=16):
		self.queue = collections.deque(maxlen=size)
		self.lock = threading.RLock()

	def add(self, lvm_exec_meta):
		with self.lock:
			self.queue.append(lvm_exec_meta)

	def dump(self):
		with self.lock:
			if len(self.queue):
				log_error("LVM dbus flight recorder START (in order of newest to oldest)")
				for c in reversed(self.queue):
					log_error(str(c))
				log_error("LVM dbus flight recorder END")
				self.queue.clear()


cfg.flightrecorder = LvmFlightRecorder()


def _debug_c(cmd, exit_code, out):
	log_error('CMD= %s' % ' '.join(cmd))
	log_error(("EC= %d" % exit_code))
	log_error(("STDOUT=\n %s\n" % out[0]))
	log_error(("STDERR=\n %s\n" % out[1]))


def call_lvm(command, debug=False, line_cb=None,
			 cb_data=None):
	"""
	Call an executable and return a tuple of exitcode, stdout, stderr
	:param command: Command to execute
	:param debug:   Dump debug to stdout
	:param line_cb:	Call the supplied function for each line read from
					stdin, CALL MUST EXECUTE QUICKLY and not *block*
					otherwise call_lvm function will fail to read
					stdin/stdout.  Return value of call back is ignored
	:param cb_data: Supplied to call back to allow caller access to
								its own data

	# Callback signature
	def my_callback(my_context, line_read_stdin)
		pass
	"""
	# Prepend the full lvm executable so that we can run different versions
	# in different locations on the same box
	command.insert(0, cfg.LVM_CMD)
	command = add_no_notify(command)

	# Ensure we get an error message when we fork & exec the lvm command line
	command = add_config_option(command, "--config", 'log/command_log_selection="log_context!=''"')

	process = Popen(command, stdout=PIPE, stderr=PIPE, close_fds=True,
					env=os.environ)

	stdout_text = ""
	stderr_text = ""
	stdout_index = 0
	make_non_block(process.stdout)
	make_non_block(process.stderr)

	while True and cfg.run.value != 0:
		try:
			rd_fd = [process.stdout.fileno(), process.stderr.fileno()]
			ready = select.select(rd_fd, [], [], 2)

			for r in ready[0]:
				if r == process.stdout.fileno():
					stdout_text += read_decoded(process.stdout)
				elif r == process.stderr.fileno():
					stderr_text += read_decoded(process.stderr)

			if line_cb is not None:
				# Process the callback for each line read!
				while True:
					i = stdout_text.find("\n", stdout_index)
					if i != -1:
						try:
							line_cb(cb_data, stdout_text[stdout_index:i])
						except BaseException as be:
							st = extract_stack_trace(be)
							log_error("call_lvm: line_cb exception: \n %s" % st)
						stdout_index = i + 1
					else:
						break

			# Check to see if process has terminated, None when running
			if process.poll() is not None:
				break
		except IOError as ioe:
			log_debug("call_lvm:" + str(ioe))
			break

	if process.returncode is not None:
		cfg.lvmdebug.lvm_complete()
		if debug or (process.returncode != 0 and (process.returncode != 5 and "fullreport" in command)):
			_debug_c(command, process.returncode, (stdout_text, stderr_text))

		try:
			report_json = json.loads(stdout_text)
		except json.decoder.JSONDecodeError:
			# Some lvm commands don't return json even though we are asking for it to do so.
			return process.returncode, stdout_text, stderr_text

		error_msg = get_error_msg(report_json)
		if error_msg:
			stderr_text += error_msg

		return process.returncode, report_json, stderr_text
	else:
		if cfg.run.value == 0:
			raise SystemExit
		# We can bail out before the lvm command finished when we get a signal
		# which is requesting we exit
		return -errno.EINTR, "", "operation interrupted"


# The actual method which gets called to invoke the lvm command, can vary
# from forking a new process to using lvm shell
_t_call = call_lvm


def _shell_cfg():
	global _t_call
	# noinspection PyBroadException
	try:
		lvm_shell = LVMShellProxy()
		_t_call = lvm_shell.call_lvm
		cfg.SHELL_IN_USE = lvm_shell
		return True
	except Exception as e:
		_t_call = call_lvm
		cfg.SHELL_IN_USE = None
		log_error("Unable to utilize lvm shell, dropping "
				  "back to fork & exec\n%s" % extract_stack_trace(e))
		return False


def set_execution(shell):
	global _t_call
	with cmd_lock:
		# If the user requested lvm shell, and we are currently setup that
		# way, just return
		if cfg.SHELL_IN_USE and shell:
			return True
		else:
			if not shell and cfg.SHELL_IN_USE:
				cfg.SHELL_IN_USE.exit_shell()
				cfg.SHELL_IN_USE = None

		_t_call = call_lvm
		if shell:
			if cfg.args.use_json:
				return _shell_cfg()
			else:
				return False
		return True


def time_wrapper(command, debug=False):
	global total_time
	global total_count

	with cmd_lock:
		start = time.time()
		meta = LvmExecutionMeta(start, 0, command)
		# Add the partial metadata to flight recorder, so if the command hangs
		# we will see what it was.
		cfg.flightrecorder.add(meta)
		results = _t_call(command, debug)
		ended = time.time()
		total_time += (ended - start)
		total_count += 1
		meta.completed(ended, *results)
	return results


call = time_wrapper


# Default cmd
# Place default arguments for every command here.
def _dc(cmd, args):
	c = [cmd, '--nosuffix', '--unbuffered', '--units', 'b']
	c.extend(args)
	return c


def options_to_cli_args(options):
	rc = []
	for k, v in list(dict(options).items()):
		if k.startswith("-"):
			rc.append(k)
		else:
			rc.append("--%s" % k)
		if v != "":
			if isinstance(v, int):
				rc.append(str(int(v)))
			else:
				rc.append(str(v))
	return rc


def pv_remove(device, remove_options):
	cmd = ['pvremove']
	cmd.extend(options_to_cli_args(remove_options))
	cmd.append(device)
	return call(cmd)


def _qt(tag_name):
	return '@%s' % tag_name


def _tag(operation, what, add, rm, tag_options):
	cmd = [operation]
	cmd.extend(options_to_cli_args(tag_options))

	if isinstance(what, list):
		cmd.extend(what)
	else:
		cmd.append(what)

	if add:
		cmd.extend(list(chain.from_iterable(
			('--addtag', _qt(x)) for x in add)))
	if rm:
		cmd.extend(list(chain.from_iterable(
			('--deltag', _qt(x)) for x in rm)))

	return call(cmd, False)


def pv_tag(pv_devices, add, rm, tag_options):
	return _tag('pvchange', pv_devices, add, rm, tag_options)


def vg_tag(vg_name, add, rm, tag_options):
	return _tag('vgchange', vg_name, add, rm, tag_options)


def lv_tag(lv_name, add, rm, tag_options):
	return _tag('lvchange', lv_name, add, rm, tag_options)


def vg_rename(vg_uuid, new_name, rename_options):
	cmd = ['vgrename']
	cmd.extend(options_to_cli_args(rename_options))
	cmd.extend([vg_uuid, new_name])
	return call(cmd)


def vg_remove(vg_id, remove_options):
	cmd = ['vgremove']
	cmd.extend(options_to_cli_args(remove_options))
	cmd.extend(['-f', vg_id])
	# https://bugzilla.redhat.com/show_bug.cgi?id=2175220 is preventing us from doing the following
	# cmd.extend(['-f', "--select", "vg_uuid=%s" % vg_id])
	return call(cmd)


def vg_lv_create(vg_name, create_options, name, size_bytes, pv_dests):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['--size', '%dB' % size_bytes])
	cmd.extend(['--name', name, vg_name, '--yes'])
	pv_dest_ranges(cmd, pv_dests)
	return call(cmd)


def vg_lv_snapshot(vg_name, snapshot_options, name, size_bytes):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(snapshot_options))
	cmd.extend(["-s"])

	if size_bytes != 0:
		cmd.extend(['--size', '%dB' % size_bytes])

	cmd.extend(['--name', name, vg_name])
	return call(cmd)


def _vg_lv_create_common_cmd(create_options, size_bytes, thin_pool):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))

	if not thin_pool:
		cmd.extend(['--size', '%dB' % size_bytes])
	else:
		cmd.extend(['--thin', '--size', '%dB' % size_bytes])

	cmd.extend(['--yes'])
	return cmd


def vg_lv_create_linear(vg_name, create_options, name, size_bytes, thin_pool):
	cmd = _vg_lv_create_common_cmd(create_options, size_bytes, thin_pool)
	cmd.extend(['--name', name, vg_name])
	return call(cmd)


def vg_lv_create_striped(vg_name, create_options, name, size_bytes,
							num_stripes, stripe_size_kb, thin_pool):
	cmd = _vg_lv_create_common_cmd(create_options, size_bytes, thin_pool)
	cmd.extend(['--stripes', str(int(num_stripes))])

	if stripe_size_kb != 0:
		cmd.extend(['--stripesize', str(int(stripe_size_kb))])

	cmd.extend(['--name', name, vg_name])
	return call(cmd)


def _vg_lv_create_raid(vg_name, create_options, name, raid_type, size_bytes,
						num_stripes, stripe_size_kb):
	cmd = ['lvcreate']

	cmd.extend(options_to_cli_args(create_options))

	cmd.extend(['--type', raid_type])
	cmd.extend(['--size', '%dB' % size_bytes])

	if num_stripes != 0:
		cmd.extend(['--stripes', str(int(num_stripes))])

	if stripe_size_kb != 0:
		cmd.extend(['--stripesize', str(int(stripe_size_kb))])

	cmd.extend(['--name', name, vg_name, '--yes'])
	return call(cmd)


def vg_lv_create_raid(vg_name, create_options, name, raid_type, size_bytes,
						num_stripes, stripe_size_kb):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))

	return _vg_lv_create_raid(vg_name, create_options, name, raid_type,
								size_bytes, num_stripes, stripe_size_kb)


def vg_lv_create_mirror(
		vg_name, create_options, name, size_bytes, num_copies):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))

	cmd.extend(['--type', 'mirror'])
	cmd.extend(['--mirrors', str(int(num_copies))])
	cmd.extend(['--size', '%dB' % size_bytes])
	cmd.extend(['--name', name, vg_name, '--yes'])
	return call(cmd)


def vg_create_cache_pool(md_full_name, data_full_name, create_options):
	cmd = ['lvconvert']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['--type', 'cache-pool', '--force', '-y',
				'--poolmetadata', md_full_name, data_full_name])
	return call(cmd)


def vg_create_thin_pool(md_full_name, data_full_name, create_options):
	cmd = ['lvconvert']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['--type', 'thin-pool', '--force', '-y',
				'--poolmetadata', md_full_name, data_full_name])
	return call(cmd)


def vg_create_vdo_pool_lv_and_lv(vg_name, pool_name, lv_name, data_size,
									virtual_size, create_options):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['-y', '--type', 'vdo', '-n', lv_name,
				'-L', '%dB' % data_size, '-V', '%dB' % virtual_size,
				"%s/%s" % (vg_name, pool_name)])
	return call(cmd)


def vg_create_vdo_pool(pool_full_name, lv_name, virtual_size, create_options):
	cmd = ['lvconvert']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['--type', 'vdo-pool', '-n', lv_name, '--force', '-y',
				'-V', '%dB' % virtual_size, pool_full_name])
	return call(cmd)


def lv_remove(lv_path, remove_options):
	cmd = ['lvremove']
	cmd.extend(options_to_cli_args(remove_options))
	cmd.extend(['-f', lv_path])
	return call(cmd)


def lv_rename(lv_path, new_name, rename_options):
	cmd = ['lvrename']
	cmd.extend(options_to_cli_args(rename_options))
	cmd.extend([lv_path, new_name])
	return call(cmd)


def lv_resize(lv_full_name, size_change, pv_dests,
				resize_options):
	cmd = ['lvresize', '--force']

	cmd.extend(options_to_cli_args(resize_options))

	if size_change < 0:
		cmd.append("-L-%dB" % (-size_change))
	else:
		cmd.append("-L+%dB" % (size_change))

	cmd.append(lv_full_name)
	pv_dest_ranges(cmd, pv_dests)
	return call(cmd)


def lv_lv_create(lv_full_name, create_options, name, size_bytes):
	cmd = ['lvcreate']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(['--virtualsize', '%dB' % size_bytes, '-T'])
	cmd.extend(['--name', name, lv_full_name, '--yes'])
	return call(cmd)


def lv_cache_lv(cache_pool_full_name, lv_full_name, cache_options):
	# lvconvert --type cache --cachepool VG/CachePoolLV VG/OriginLV
	cmd = ['lvconvert']
	cmd.extend(options_to_cli_args(cache_options))
	cmd.extend(['-y', '--type', 'cache', '--cachepool',
				cache_pool_full_name, lv_full_name])
	return call(cmd)


def lv_writecache_lv(cache_lv_full_name, lv_full_name, cache_options):
	# lvconvert --type writecache --cachevol VG/CacheLV VG/OriginLV
	cmd = ['lvconvert']
	cmd.extend(options_to_cli_args(cache_options))
	cmd.extend(['-y', '--type', 'writecache', '--cachevol',
				cache_lv_full_name, lv_full_name])
	return call(cmd)


def lv_detach_cache(lv_full_name, detach_options, destroy_cache):
	cmd = ['lvconvert']
	if destroy_cache:
		option = '--uncache'
	else:
		# Currently fairly dangerous
		# see: https://bugzilla.redhat.com/show_bug.cgi?id=1248972
		option = '--splitcache'
	cmd.extend(options_to_cli_args(detach_options))
	# needed to prevent interactive questions
	cmd.extend(["--yes", "--force"])
	cmd.extend([option, lv_full_name])
	return call(cmd)


def lv_vdo_compression(lv_path, enable, comp_options):
	cmd = ['lvchange', '--compression']
	if enable:
		cmd.append('y')
	else:
		cmd.append('n')
	cmd.extend(options_to_cli_args(comp_options))
	cmd.append(lv_path)
	return call(cmd)


def lv_vdo_deduplication(lv_path, enable, dedup_options):
	cmd = ['lvchange', '--deduplication']
	if enable:
		cmd.append('y')
	else:
		cmd.append('n')
	cmd.extend(options_to_cli_args(dedup_options))
	cmd.append(lv_path)
	return call(cmd)


def supports_json():
	cmd = ['help']
	rc, out, err = call(cmd)
	if rc == 0:
		if cfg.SHELL_IN_USE:
			return True
		else:
			if 'fullreport' in err:
				return True
	return False


def supports_vdo():
	cmd = ['segtypes']
	rc, out, err = call(cmd)
	if rc == 0:
		if "vdo" in out:
			log_debug("We have VDO support")
			return True
	return False


def lvm_full_report_json():
	pv_columns = ['pv_name', 'pv_uuid', 'pv_fmt', 'pv_size', 'pv_free',
					'pv_used', 'dev_size', 'pv_mda_size', 'pv_mda_free',
					'pv_ba_start', 'pv_ba_size', 'pe_start', 'pv_pe_count',
					'pv_pe_alloc_count', 'pv_attr', 'pv_tags', 'vg_name',
					'vg_uuid', 'pv_missing']

	pv_seg_columns = ['pvseg_start', 'pvseg_size', 'segtype',
						'pv_uuid', 'lv_uuid', 'pv_name']

	vg_columns = ['vg_name', 'vg_uuid', 'vg_fmt', 'vg_size', 'vg_free',
					'vg_sysid', 'vg_extent_size', 'vg_extent_count',
					'vg_free_count', 'vg_profile', 'max_lv', 'max_pv',
					'pv_count', 'lv_count', 'snap_count', 'vg_seqno',
					'vg_mda_count', 'vg_mda_free', 'vg_mda_size',
					'vg_mda_used_count', 'vg_attr', 'vg_tags']

	lv_columns = ['lv_uuid', 'lv_name', 'lv_path', 'lv_size',
				'vg_name', 'pool_lv_uuid', 'pool_lv', 'origin_uuid',
				'origin', 'data_percent',
				'lv_attr', 'lv_tags', 'vg_uuid', 'lv_active', 'data_lv',
				'metadata_lv', 'lv_parent', 'lv_role', 'lv_layout',
				'snap_percent', 'metadata_percent', 'copy_percent',
				'sync_percent', 'lv_metadata_size', 'move_pv', 'move_pv_uuid']

	lv_seg_columns = ['seg_pe_ranges', 'segtype', 'lv_uuid']

	if cfg.vdo_support:
		lv_columns.extend(
			['vdo_operating_mode', 'vdo_compression_state', 'vdo_index_state',
				'vdo_used_size', 'vdo_saving_percent']
		)

		lv_seg_columns.extend(
			['vdo_compression', 'vdo_deduplication',
				'vdo_use_metadata_hints', 'vdo_minimum_io_size',
				'vdo_block_map_cache_size', 'vdo_block_map_era_length',
				'vdo_use_sparse_index', 'vdo_index_memory_size',
				'vdo_slab_size', 'vdo_ack_threads', 'vdo_bio_threads',
				'vdo_bio_rotation', 'vdo_cpu_threads', 'vdo_hash_zone_threads',
				'vdo_logical_threads', 'vdo_physical_threads',
				'vdo_max_discard', 'vdo_write_policy', 'vdo_header_size'])

	cmd = _dc('fullreport', [
		'-a',		# Need hidden too
		'--configreport', 'pv', '-o', ','.join(pv_columns),
		'--configreport', 'vg', '-o', ','.join(vg_columns),
		'--configreport', 'lv', '-o', ','.join(lv_columns),
		'--configreport', 'seg', '-o', ','.join(lv_seg_columns),
		'--configreport', 'pvseg', '-o', ','.join(pv_seg_columns)
	])

	# We are running the fullreport command, we will ask lvm to output the debug
	# data, so we can have the required information for lvm to debug the fullreport failures.
	# Note: this is disabled by default and can be enabled with env. var.
	# LVM_DBUSD_COLLECT_LVM_DEBUG=True
	fn = cfg.lvmdebug.setup()
	if fn is not None:
		add_config_option(cmd, "--config", "log {level=7 file=%s syslog=0}" % fn)

	rc, out, err = call(cmd)
	# When we have an exported vg the exit code of lvs or fullreport will be 5
	if rc == 0 or rc == 5:
		if type(out) != dict:
			raise LvmBug("lvm likely returned invalid JSON, lvm exit code = %d, output = %s, err= %s" %
						 (rc, str(out), str(err)))
		return out
	raise LvmBug("'fullreport' exited with code '%d'" % rc)


def pv_resize(device, size_bytes, create_options):
	cmd = ['pvresize']

	cmd.extend(options_to_cli_args(create_options))

	if size_bytes != 0:
		cmd.extend(['--yes', '--setphysicalvolumesize', '%dB' % size_bytes])

	cmd.extend([device])
	return call(cmd)


def pv_create(create_options, devices):
	cmd = ['pvcreate', '-ff']
	cmd.extend(options_to_cli_args(create_options))
	cmd.extend(devices)
	return call(cmd)


def pv_allocatable(device, yes, allocation_options):
	yn = 'n'

	if yes:
		yn = 'y'

	cmd = ['pvchange']
	cmd.extend(options_to_cli_args(allocation_options))
	cmd.extend(['-x', yn, device])
	return call(cmd)


def pv_scan(activate, cache, device_paths, major_minors, scan_options):
	cmd = ['pvscan']
	cmd.extend(options_to_cli_args(scan_options))

	if activate:
		cmd.extend(['--activate', "ay"])

	if cache:
		cmd.append('--cache')

		if len(device_paths) > 0:
			for d in device_paths:
				cmd.append(d)

		if len(major_minors) > 0:
			for mm in major_minors:
				cmd.append("%s:%s" % (mm))

	return call(cmd)


def vg_create(create_options, pv_devices, name):
	cmd = ['vgcreate']
	cmd.extend(options_to_cli_args(create_options))
	cmd.append(name)
	cmd.extend(pv_devices)
	return call(cmd)


def vg_change(change_options, name):
	cmd = ['vgchange']
	cmd.extend(options_to_cli_args(change_options))
	cmd.append(name)
	return call(cmd)


def vg_reduce(vg_name, missing, pv_devices, reduce_options):
	cmd = ['vgreduce']
	cmd.extend(options_to_cli_args(reduce_options))

	if missing:
		cmd.append('--removemissing')
	elif len(pv_devices) == 0:
		cmd.append('--all')

	cmd.append(vg_name)
	cmd.extend(pv_devices)
	return call(cmd)


def vg_extend(vg_name, extend_devices, extend_options):
	cmd = ['vgextend']
	cmd.extend(options_to_cli_args(extend_options))
	cmd.append(vg_name)
	cmd.extend(extend_devices)
	return call(cmd)


def _vg_value_set(name, arguments, options):
	cmd = ['vgchange']
	cmd.extend(options_to_cli_args(options))
	cmd.append(name)
	cmd.extend(arguments)
	return call(cmd)


def vg_allocation_policy(vg_name, policy, policy_options):
	return _vg_value_set(vg_name, ['--alloc', policy], policy_options)


def vg_max_pv(vg_name, number, max_options):
	return _vg_value_set(vg_name, ['--maxphysicalvolumes', str(int(number))],
							max_options)


def vg_max_lv(vg_name, number, max_options):
	return _vg_value_set(vg_name, ['-l', str(int(number))], max_options)


def vg_uuid_gen(vg_name, ignore, options):
	assert ignore is None
	return _vg_value_set(vg_name, ['--uuid'], options)


def activate_deactivate(op, name, activate, control_flags, options):
	cmd = [op]
	cmd.extend(options_to_cli_args(options))

	op = '-a'

	if control_flags:
		# Autoactivation
		if (1 << 0) & control_flags:
			op += 'a'
		# Exclusive locking (Cluster)
		if (1 << 1) & control_flags:
			op += 'e'

		# Local node activation
		if (1 << 2) & control_flags:
			op += 'l'

		# Activation modes
		if (1 << 3) & control_flags:
			cmd.extend(['--activationmode', 'complete'])
		elif (1 << 4) & control_flags:
			cmd.extend(['--activationmode', 'partial'])

		# Ignore activation skip
		if (1 << 5) & control_flags:
			cmd.append('--ignoreactivationskip')

		# Shared locking (Cluster)
		if (1 << 6) & control_flags:
			op += 's'

	if activate:
		op += 'y'
	else:
		op += 'n'

	cmd.append(op)
	cmd.append("-y")
	cmd.append(name)
	return call(cmd)


if __name__ == '__main__':
	# Leave this for future debug as needed
	pass
