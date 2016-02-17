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
from .cfg import BASE_INTERFACE, BASE_OBJ_PATH, MANAGER_OBJ_PATH
import threading
from . import cmdhandler
import time
import signal
import dbus
from . import lvmdb
# noinspection PyUnresolvedReferences
from gi.repository import GObject
from .fetch import load
from .manager import Manager
from .background import background_reaper
import traceback
import queue
import sys
from . import udevwatch
from .utils import log_debug
import argparse


class Lvm(objectmanager.ObjectManager):
	def __init__(self, object_path):
		super(Lvm, self).__init__(object_path, BASE_INTERFACE)


def process_request():
	while cfg.run.value != 0:
		try:
			req = cfg.worker_q.get(True, 5)

			start = cfg.db.num_refreshes

			log_debug(
				"Running method: %s with args %s" %
				(str(req.method), str(req.arguments)))
			req.run_cmd()

			end = cfg.db.num_refreshes

			if end - start > 1:
				log_debug(
					"Inspect method %s for too many refreshes" %
					(str(req.method)))
			log_debug("Complete ")
		except queue.Empty:
			pass
		except Exception:
			traceback.print_exc(file=sys.stdout)
			pass


def main():
	# Add simple command line handling
	parser = argparse.ArgumentParser()
	parser.add_argument("--udev", action='store_true',
						help="Use udev for updating state", default=False,
						dest='use_udev')
	parser.add_argument("--debug", action='store_true',
						help="Dump debug messages", default=False,
						dest='debug')

	args = parser.parse_args()

	cfg.DEBUG = args.debug

	# List of threads that we start up
	thread_list = []

	start = time.time()

	# Install signal handlers
	for s in [signal.SIGHUP, signal.SIGINT]:
		try:
			signal.signal(s, utils.handler)
		except RuntimeError:
			pass

	dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
	GObject.threads_init()
	dbus.mainloop.glib.threads_init()
	cfg.bus = dbus.SystemBus()
	# The base name variable needs to exist for things to work.
	# noinspection PyUnusedLocal
	base_name = dbus.service.BusName(BASE_INTERFACE, cfg.bus)
	cfg.om = Lvm(BASE_OBJ_PATH)
	cfg.om.register_object(Manager(MANAGER_OBJ_PATH))

	cfg.load = load

	cfg.db = lvmdb.DataStore()

	# Start up thread to monitor pv moves
	thread_list.append(
		threading.Thread(target=background_reaper, name="pv_move_reaper"))

	# Using a thread to process requests.
	thread_list.append(threading.Thread(target=process_request))

	cfg.load(refresh=False, emit_signal=False)
	cfg.loop = GObject.MainLoop()

	for process in thread_list:
		process.damon = True
		process.start()

	end = time.time()
	log_debug(
		'Service ready! total time= %.2f, lvm time= %.2f count= %d' %
		(end - start, cmdhandler.total_time, cmdhandler.total_count),
		'bg_black', 'fg_light_green')

	# Add udev watching
	if args.use_udev:
		log_debug('Utilizing udev to trigger updates')
		udevwatch.add()

	try:
		if cfg.run.value != 0:
			cfg.loop.run()

			if args.use_udev:
				udevwatch.remove()

			for process in thread_list:
				process.join()
	except KeyboardInterrupt:
		utils.handler(signal.SIGINT, None)
	return 0
