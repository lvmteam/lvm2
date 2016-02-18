#!/usr/bin/env python3

# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
# Copyright 2015-2016, Vratislav Podzimek <vpodzime@redhat.com>

import subprocess
import shlex
from fcntl import fcntl, F_GETFL, F_SETFL
from os import O_NONBLOCK
import traceback
import sys
import re

try:
	from .cfg import LVM_CMD
	from .utils import log_debug, log_error
except:
	from cfg import LVM_CMD
	from utils import log_debug, log_error

SHELL_PROMPT = "lvm> "


def _quote_arg(arg):
	if len(shlex.split(arg)) > 1:
		return '"%s"' % arg
	else:
		return arg


class LVMShellProxy(object):
	def _read_until_prompt(self):
		prev_ec = None
		stdout = ""
		while not stdout.endswith(SHELL_PROMPT):
			try:
				tmp = self.lvm_shell.stdout.read()
				if tmp:
					stdout += tmp.decode("utf-8")
			except IOError:
				# nothing written yet
				pass

		# strip the prompt from the STDOUT before returning and grab the exit
		# code if it's available
		m = self.re.match(stdout)
		if m:
			prev_ec = int(m.group(2))
			strip_idx = -1 * len(m.group(1))
		else:
			strip_idx = -1 * len(SHELL_PROMPT)

		return stdout[:strip_idx], prev_ec

	def _read_line(self):
		while True:
			try:
				tmp = self.lvm_shell.stdout.readline()
				if tmp:
					return tmp.decode("utf-8")
			except IOError:
				pass

	def _discard_echo(self, expected):
		line = ""
		while line != expected:
			# GNU readline inserts some interesting characters at times...
			line += self._read_line().replace(' \r', '')

	def _write_cmd(self, cmd):
		cmd_bytes = bytes(cmd, "utf-8")
		num_written = self.lvm_shell.stdin.write(cmd_bytes)
		assert (num_written == len(cmd_bytes))
		self.lvm_shell.stdin.flush()

	def _lvm_echos(self):
		echo = False
		cmd = "version\n"
		self._write_cmd(cmd)
		line = self._read_line()

		if line == cmd:
			echo = True

		self._read_until_prompt()

		return echo

	def __init__(self):
		self.re = re.compile(".*(\[(-?[0-9]+)\] lvm> $)", re.DOTALL)

		# run the lvm shell
		self.lvm_shell = subprocess.Popen(
			[LVM_CMD], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
			stderr=subprocess.PIPE, close_fds=True)
		flags = fcntl(self.lvm_shell.stdout, F_GETFL)
		fcntl(self.lvm_shell.stdout, F_SETFL, flags | O_NONBLOCK)
		flags = fcntl(self.lvm_shell.stderr, F_GETFL)
		fcntl(self.lvm_shell.stderr, F_SETFL, flags | O_NONBLOCK)

		# wait for the first prompt
		self._read_until_prompt()

		# Check to see if the version of LVM we are using is running with
		# gnu readline which will echo our writes from stdin to stdout
		self.echo = self._lvm_echos()

	def call_lvm(self, argv, debug=False):
		# create the command string
		cmd = " ".join(_quote_arg(arg) for arg in argv)
		cmd += "\n"

		# run the command by writing it to the shell's STDIN
		self._write_cmd(cmd)

		# If lvm is utilizing gnu readline, it echos stdin to stdout
		if self.echo:
			self._discard_echo(cmd)

		# read everything from the STDOUT to the next prompt
		stdout, exit_code = self._read_until_prompt()

		# read everything from STDERR if there's something (we waited for the
		# prompt on STDOUT so there should be all or nothing at this point on
		# STDERR)
		stderr = None
		try:
			t_error = self.lvm_shell.stderr.read()
			if t_error:
				stderr = t_error.decode("utf-8")
		except IOError:
			# nothing on STDERR
			pass

		if exit_code is not None:
			rc = exit_code
		else:
			# LVM does write to stderr even when it did complete successfully,
			# so without having the exit code in the prompt we can never be
			# sure.
			if stderr:
				rc = 1
			else:
				rc = 0

		if debug or rc != 0:
			log_error(('CMD: %s' % cmd))
			log_error(("EC = %d" % rc))
			log_error(("STDOUT=\n %s\n" % stdout))
			log_error(("STDERR=\n %s\n" % stderr))

		return (rc, stdout, stderr)

	def __del__(self):
		self.lvm_shell.terminate()


if __name__ == "__main__":
	shell = LVMShellProxy()
	in_line = "start"
	try:
		while in_line:
			in_line = input("lvm> ")
			if in_line:
				ret, out, err, = shell.call_lvm(in_line.split())
				print(("RET: %d" % ret))
				print(("OUT:\n%s" % out))
				print(("ERR:\n%s" % err))
	except KeyboardInterrupt:
		pass
	except EOFError:
		pass
	except Exception:
		traceback.print_exc(file=sys.stdout)
	finally:
		print()
