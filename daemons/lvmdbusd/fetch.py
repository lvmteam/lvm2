# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from .pv import load_pvs
from .vg import load_vgs
from .lv import load_lvs
from . import cfg
from .utils import MThreadRunner, log_debug, log_error, LvmBug, extract_stack_trace
import threading
import queue
import time


def _main_thread_load(refresh=True, emit_signal=True):
	num_total_changes = 0
	to_remove = []

	(changes, remove) = load_pvs(
		refresh=refresh,
		emit_signal=emit_signal,
		cache_refresh=False)[1:]
	num_total_changes += changes
	to_remove.extend(remove)

	(changes, remove) = load_vgs(
		refresh=refresh,
		emit_signal=emit_signal,
		cache_refresh=False)[1:]

	num_total_changes += changes
	to_remove.extend(remove)

	(lv_changes, remove) = load_lvs(
		refresh=refresh,
		emit_signal=emit_signal,
		cache_refresh=False)[1:]

	num_total_changes += lv_changes
	to_remove.extend(remove)

	# When the LVs change it can cause another change in the VGs which is
	# missed if we don't scan through the VGs again.  We could achieve this
	# the other way and re-scan the LVs, but in general there are more LVs than
	# VGs, thus this should be more efficient.  This happens when a LV interface
	# changes causing the dbus object representing it to be removed and
	# recreated.
	if refresh and lv_changes > 0:
		(changes, remove) = load_vgs(
			refresh=refresh,
			emit_signal=emit_signal,
			cache_refresh=False)[1:]

	num_total_changes += changes
	to_remove.extend(remove)

	# Remove any objects that are no longer needed.  We do this after we process
	# all the objects to ensure that references still exist for objects that
	# are processed after them.
	to_remove.reverse()
	for i in to_remove:
		dbus_obj = cfg.om.get_object_by_path(i)
		if dbus_obj:
			cfg.om.remove_object(dbus_obj, True)
			num_total_changes += 1

	return num_total_changes


def load(refresh=True, emit_signal=True, cache_refresh=True, log=True,
			need_main_thread=True):
	# Go through and load all the PVs, VGs and LVs
	if cache_refresh:
		cfg.db.refresh(log)

	if need_main_thread:
		rc = MThreadRunner(_main_thread_load, refresh, emit_signal).done()
	else:
		rc = _main_thread_load(refresh, emit_signal)

	return rc


# Even though lvm can handle multiple changes concurrently it really doesn't
# make sense to make a 1-1 fetch of data for each change of lvm because when
# we fetch the data once all previous changes are reflected.
class StateUpdate(object):

	class UpdateRequest(object):

		def __init__(self, refresh, emit_signal, cache_refresh, log,
						need_main_thread):
			self.is_done = False
			self.refresh = refresh
			self.emit_signal = emit_signal
			self.cache_refresh = cache_refresh
			self.log = log
			self.need_main_thread = need_main_thread
			self.result = None
			self.cond = threading.Condition(threading.Lock())

		def done(self):
			with self.cond:
				if not self.is_done:
					self.cond.wait()
			return self.result

		def set_result(self, result):
			with self.cond:
				self.result = result
				self.is_done = True
				self.cond.notify_all()

	@staticmethod
	def update_thread(obj):
		exception_count = 0
		queued_requests = []

		def set_results(val):
			nonlocal queued_requests
			for idx in queued_requests:
				idx.set_result(val)
				# Only clear out the requests after we have given them a result
				# otherwise we can orphan the waiting threads, and they never
				# wake up if we get an exception
				queued_requests = []

		def bailing(rv):
			set_results(rv)
			try:
				while True:
					item = obj.queue.get(False)
					item.set_result(rv)
			except queue.Empty:
				pass

		def _load_args(requests):
			"""
			If we have multiple requests in the queue, they might not all have the same options.  If any of the requests
			have an option set we need to honor it.
			"""
			refresh = any([r.refresh for r in requests])
			emit_signal = any([r.emit_signal for r in requests])
			cache_refresh = any([r.cache_refresh for r in requests])
			log = any([r.log for r in requests])
			need_main_thread = any([r.need_main_thread for r in requests])

			return refresh, emit_signal, cache_refresh, log, need_main_thread

		def _drain_queue(queued, incoming):
			try:
				while True:
					queued.append(incoming.get(block=False))
			except queue.Empty:
				pass

		def _handle_error():
			nonlocal exception_count
			exception_count += 1

			if exception_count >= 5:
				log_error("Too many errors in update_thread, exiting daemon")
				cfg.debug.dump()
				cfg.flightrecorder.dump()
				bailing(e)
				cfg.exit_daemon()
			else:
				# Slow things down when encountering errors
				time.sleep(1)

		while cfg.run.value != 0:
			# noinspection PyBroadException
			try:
				with obj.lock:
					wait = not obj.deferred
					obj.deferred = False

				if len(queued_requests) == 0 and wait:
					# Note: If we don't have anything for 2 seconds we will
					# get a queue.Empty exception raised here
					queued_requests.append(obj.queue.get(block=True, timeout=2))

				# Ok we have one or the deferred queue has some,
				# check if any others and grab them too
				_drain_queue(queued_requests, obj.queue)

				if len(queued_requests) > 1:
					log_debug("Processing %d updates!" % len(queued_requests),
							'bg_black', 'fg_light_green')

				num_changes = load(*_load_args(queued_requests))
				# Update is done, let everyone know!
				set_results(num_changes)

				# We retrieved OK, clear exception count
				exception_count = 0

			except queue.Empty:
				pass
			except SystemExit:
				break
			except LvmBug as bug:
				log_error(str(bug))
				_handle_error()
			except Exception as e:
				log_error("update_thread: \n%s" % extract_stack_trace(e))
				_handle_error()

		# Make sure to unblock any that may be waiting before we exit this thread
		# otherwise they hang forever ...
		bailing(Exception("update thread exiting"))
		log_debug("update thread exiting!")

	def __init__(self):
		self.lock = threading.RLock()
		self.queue = queue.Queue()
		self.deferred = False

		# Do initial load
		load(refresh=False, emit_signal=False, need_main_thread=False)

		self.thread = threading.Thread(target=StateUpdate.update_thread,
										args=(self,),
										name="StateUpdate.update_thread")

	def load(self, refresh=True, emit_signal=True, cache_refresh=True,
					log=True, need_main_thread=True):
		# Place this request on the queue and wait for it to be completed
		req = StateUpdate.UpdateRequest(refresh, emit_signal, cache_refresh,
										log, need_main_thread)
		self.queue.put(req)
		return req.done()

	def event(self):
		with self.lock:
			self.deferred = True
