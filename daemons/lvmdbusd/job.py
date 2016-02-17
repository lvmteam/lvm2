# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

from .automatedproperties import AutomatedProperties
from .utils import job_obj_path_generate
from . import cfg
from .cfg import JOB_INTERFACE
import dbus
import threading


# noinspection PyPep8Naming
class JobState(object):
	def __init__(self, request):
		self.rlock = threading.RLock()

		self._percent = 0
		self._complete = False
		self._request = request
		self._cond = threading.Condition(self.rlock)
		self._ec = 0
		self._stderr = ''

		# This is an lvm command that is just taking too long and doesn't
		# support background operation
		if self._request:
			# Faking the percentage when we don't have one
			self._percent = 1

	@property
	def Percent(self):
		with self.rlock:
			return self._percent

	@Percent.setter
	def Percent(self, value):
		with self.rlock:
			self._percent = value

	@property
	def Complete(self):
		with self.rlock:
			if self._request:
				self._complete = self._request.is_done()
				if self._complete:
					self._percent = 100

			return self._complete

	@Complete.setter
	def Complete(self, value):
		with self.rlock:
			self._complete = value
			self._cond.notify_all()

	@property
	def GetError(self):
		with self.rlock:
			if self.Complete:
				if self._request:
					(rc, error) = self._request.get_errors()
					return (rc, str(error))
				else:
					return (self._ec, self._stderr)
			else:
				return (-1, 'Job is not complete!')

	def set_result(self, ec, msg):
		with self.rlock:
			self.Complete = True
			self._ec = ec
			self._stderr = msg

	def dtor(self):
		with self.rlock:
			self._request = None

	def Wait(self, timeout):
		try:
			with self._cond:
				# Check to see if we are done, before we wait
				if not self.Complete:
					if timeout != -1:
						self._cond.wait(timeout)
					else:
						self._cond.wait()
				return self.Complete
		except RuntimeError:
			return False

	@property
	def Result(self):
		with self.rlock:
			if self._request:
				return self._request.result()
			return '/'


# noinspection PyPep8Naming
class Job(AutomatedProperties):
	_Percent_meta = ('y', JOB_INTERFACE)
	_Complete_meta = ('b', JOB_INTERFACE)
	_Result_meta = ('o', JOB_INTERFACE)
	_GetError_meta = ('(is)', JOB_INTERFACE)

	def __init__(self, request, job_state=None):
		super(Job, self).__init__(job_obj_path_generate())
		self.set_interface(JOB_INTERFACE)

		if job_state:
			self.state = job_state
		else:
			self.state = JobState(request)

	@property
	def Percent(self):
		return self.state.Percent

	@Percent.setter
	def Percent(self, value):
		self.state.Percent = value

	@property
	def Complete(self):
		return self.state.Complete

	@Complete.setter
	def Complete(self, value):
		self.state.Complete = value

	@property
	def GetError(self):
		return self.state.GetError

	def set_result(self, ec, msg):
		self.state.set_result(ec, msg)

	@dbus.service.method(dbus_interface=JOB_INTERFACE)
	def Remove(self):
		if self.state.Complete:
			cfg.om.remove_object(self, True)
			self.state.dtor()
		else:
			raise dbus.exceptions.DBusException(
				JOB_INTERFACE, 'Job is not complete!')

	@dbus.service.method(dbus_interface=JOB_INTERFACE,
							in_signature='i',
							out_signature='b')
	def Wait(self, timeout):
		return self.state.Wait(timeout)

	@property
	def Result(self):
		return self.state.Result

	@property
	def lvm_id(self):
		return str(id(self))

	@property
	def Uuid(self):
		import uuid
		return uuid.uuid1()
