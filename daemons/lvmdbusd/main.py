# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from . import cfg
from . import objectmanager
from . import utils
from .cfg import BUS_NAME, BASE_INTERFACE, BASE_OBJ_PATH, MANAGER_OBJ_PATH
import threading
from . import cmdhandler
import time
import signal
import dbus
import dbus.mainloop.glib
from . import lvmdb
# noinspection PyUnresolvedReferences
from gi.repository import GLib
from .fetch import StateUpdate
from .manager import Manager
import queue
from . import udevwatch
from .utils import log_debug, log_error, log_msg, DebugMessages
import argparse
import os
import sys
from .cmdhandler import LvmFlightRecorder, supports_vdo, supports_json
from .request import RequestEntry


class Lvm(objectmanager.ObjectManager):
	def __init__(self, object_path):
		super(Lvm, self).__init__(object_path, BASE_INTERFACE)


def process_request():
	while cfg.run.value != 0:
		# noinspection PyBroadException
		try:
			req = cfg.worker_q.get(True, 5)
			log_debug(
				"Method start: %s with args %s (callback = %s)" %
				(str(req.method), str(req.arguments), str(req.cb)))
			req.run_cmd()
			log_debug("Method complete: %s" % str(req.method))
		except queue.Empty:
			pass
		except SystemExit:
			break
		except Exception as e:
			st = utils.extract_stack_trace(e)
			utils.log_error("process_request exception: \n%s" % st)
	log_debug("process_request thread exiting!")


def check_fr_size(value):
	v = int(value)
	if v < 0:
		raise argparse.ArgumentTypeError(
			"positive integers only ('%s' invalid)" % value)
	return v


def install_signal_handlers():
	# Because of the glib main loop stuff the python signal handler code is
	# apparently not usable and we need to use the glib calls instead
	signal_add = None

	if hasattr(GLib, 'unix_signal_add'):
		signal_add = GLib.unix_signal_add
	elif hasattr(GLib, 'unix_signal_add_full'):
		signal_add = GLib.unix_signal_add_full

	if signal_add:
		signal_add(GLib.PRIORITY_HIGH, signal.SIGHUP, utils.handler, signal.SIGHUP)
		signal_add(GLib.PRIORITY_HIGH, signal.SIGINT, utils.handler, signal.SIGINT)
		signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR1, utils.handler, signal.SIGUSR1)
		signal_add(GLib.PRIORITY_HIGH, signal.SIGUSR2, utils.handler, signal.SIGUSR2)
	else:
		log_error("GLib.unix_signal_[add|add_full] are NOT available!")


def process_args():
	parser = argparse.ArgumentParser()
	parser.add_argument(
		"--udev", action='store_true',
		help="Use udev for updating state",
		default=False,
		dest='use_udev')
	parser.add_argument(
		"--debug", action='store_true',
		help="Dump debug messages", default=False,
		dest='debug')
	parser.add_argument(
		"--nojson", action='store_false',
		help="Do not use LVM JSON output (disables lvmshell)", default=True,
		dest='use_json')
	parser.add_argument(
		"--lvmshell", action='store_true',
		help="Use the lvm shell, not fork & exec lvm",
		default=False,
		dest='use_lvm_shell')
	parser.add_argument(
		"--frsize",
		help="Size of the flight recorder (num. entries), 0 to disable (signal 12 to dump)",
		default=10,
		type=check_fr_size,
		dest='fr_size')

	args = parser.parse_args()

	if not args.use_json:
		log_error("Daemon no longer supports lvm without JSON support, exiting!")
		sys.exit(1)
	else:
		if not supports_json():
			log_error("Un-supported version of LVM, daemon requires JSON output, exiting!")
			sys.exit(1)

	# Add udev watching
	if args.use_udev:
		# Make sure this msg ends up in the journal, so we know
		log_msg('Utilizing udev to trigger updates')

	return args


def main():
	start = time.time()
	use_session = os.getenv('LVMDBUSD_USE_SESSION', False)

	# Ensure that we get consistent output for parsing stdout/stderr and that we
	# are using the lvmdbusd profile.
	os.environ["LC_ALL"] = "C"
	os.environ["LVM_COMMAND_PROFILE"] = "lvmdbusd"

	# Save off the debug data needed for lvm team to debug issues
	# only used for 'fullreport' at this time.
	cfg.lvmdebug = utils.LvmDebugData()

	# Add simple command line handling
	cfg.args = process_args()

	cfg.create_request_entry = RequestEntry

	# We create a flight recorder in cmdhandler too, but we replace it here
	# as the user may be specifying a different size.  The default one in
	# cmdhandler is for when we are running other code with a different main.
	cfg.flightrecorder = LvmFlightRecorder(cfg.args.fr_size)

	# Create a circular buffer for debug logs
	cfg.debug = DebugMessages()

	log_debug("Using lvm binary: %s" % cfg.LVM_CMD)

	# We will dynamically add interfaces which support vdo if it
	# exists.
	cfg.vdo_support = supports_vdo()

	# List of threads that we start up
	thread_list = []

	install_signal_handlers()

	with utils.LockFile(cfg.LOCK_FILE):
		dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
		dbus.mainloop.glib.threads_init()

		cmdhandler.set_execution(cfg.args.use_lvm_shell)

		if use_session:
			cfg.bus = dbus.SessionBus()
		else:
			cfg.bus = dbus.SystemBus()
		# The base name variable needs to exist for things to work.
		# noinspection PyUnusedLocal
		base_name = dbus.service.BusName(BUS_NAME, cfg.bus)
		cfg.om = Lvm(BASE_OBJ_PATH)
		cfg.om.register_object(Manager(MANAGER_OBJ_PATH))

		cfg.db = lvmdb.DataStore(vdo_support=cfg.vdo_support)

		# Using a thread to process requests, we cannot hang the dbus library
		# thread that is handling the dbus interface
		thread_list.append(
			threading.Thread(target=process_request, name='process_request'))

		# Have a single thread handling updating lvm and the dbus model so we
		# don't have multiple threads doing this as the same time
		updater = StateUpdate()
		thread_list.append(updater.thread)

		cfg.load = updater.load

		cfg.loop = GLib.MainLoop()

		for thread in thread_list:
			thread.damon = True
			thread.start()

		# In all cases we are going to monitor for udev until we get an
		# ExternalEvent.  In the case where we get an external event and the user
		# didn't specify --udev we will stop monitoring udev
		udevwatch.add()

		end = time.time()
		log_debug(
			'Service ready! total time= %.4f, lvm time= %.4f count= %d' %
			(end - start, cmdhandler.total_time, cmdhandler.total_count),
			'bg_black', 'fg_light_green')

		try:
			if cfg.run.value != 0:
				cfg.loop.run()
				udevwatch.remove()

				for thread in thread_list:
					thread.join()
		except KeyboardInterrupt:
			# If we are unable to register signal handler, we will end up here when
			# the service gets a ^C or a kill -2 <parent pid>
			utils.handler(signal.SIGINT)
	return 0
