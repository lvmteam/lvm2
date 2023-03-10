#!/usr/bin/python3

# Simulate errors by doing the following for both lvm and lvm shell:
# Randomly return
#  - Bad exit code
#  - Exit code 5 (exported VG)
#  - Truncated JSON
#  - Missing key in JSON
#
# This is done by sitting between lvm dbusd and lvm.  If running via systemd, add the following to the service file
# Environment="LVM_BINARY=/path/to/this file/lvm2/test/dbus/lvm_error_inject.py"
# systemctl daemon-reload
# systemctl restart lvm2-lvmdbusd
import copy
import json
import multiprocessing
import os
import pty
import random
import select
import signal
import string
import subprocess
import sys
import tempfile
import traceback
from collections import deque
from fcntl import fcntl, F_GETFL, F_SETFL
from subprocess import Popen


CS = string.ascii_letters + "\n\t " + string.digits

run = multiprocessing.Value('i', 1)

SH = None


def rs(length, character_set=CS):
    return ''.join(random.choice(character_set) for _ in range(length))


RS = rs(512)

d_out = open(os.path.join(tempfile.gettempdir(), "mitm_lvm.txt"), "a")


def debug(msg):
    m = str(msg)
    d_out.write(m)
    if m[-1] != "\n":
        d_out.write("\n")
    d_out.flush()


# Make stream non-blocking
def make_non_block(stream):
    flags = fcntl(stream, F_GETFL)
    fcntl(stream, F_SETFL, flags | os.O_NONBLOCK)


def read_decoded(stream):
    tmp = stream.read()
    if tmp:
        return tmp.decode("utf-8")
    return ''


def write_some(q, stream, remaining=False, binary=False):
    if len(q) > 0:
        if remaining:
            to_send = len(q)
        else:
            to_send = random.randint(1, len(q))

        for _ in range(0, to_send):
            c = q.popleft()
            if binary:
                stream.write(bytes(c, "utf-8"))
            else:
                stream.write(c)
        stream.flush()


def del_random_key(src_dict):
    keys = list(src_dict.keys())
    pick = random.randint(0, len(keys) - 1)
    debug("%s will be deleted" % keys[pick])
    del src_dict[keys[pick]]


def inject_key_error(output_json):
    debug("Deleting a key")
    for r in output_json['report']:
        if 'lv' in r:
            for i in r['lv']:
                debug("deleting a lv key")
                del_random_key(i)
                return
        if 'vg' in r:
            for i in r["vg"]:
                debug("deleting a vg key")
                del_random_key(i)
                return
        elif 'pv' in r:
            for i in r["pv"]:
                debug("deleting a pv key")
                del_random_key(i)
                return


def inject_exit_error(output_json, val):
    if 'log' in output_json and len(output_json['log']) > 0:
        debug("Returning bad exit code")
        # Change the exit code to failure
        output_json['log'][-1:][0]['log_ret_code'] = "%d" % val
    else:
        # We are in fork & exec mode, just exit.
        if val == 0:
            sys.exit(1)
        sys.exit(val)


def inject_error(output_str, output_json=None):
    try:
        if random.randint(0, 9) == 1:
            error_case = random.randint(0, 3)
            if error_case == 0:
                debug("Truncating JSON")
                # Create bad JSON by truncating it
                str_rep = output_str[:-len(output_str) // 2]
                rc = str_rep
            else:
                if output_json is None:
                    output_json = json.loads(output_str)
                if error_case == 1:
                    inject_key_error(output_json)
                elif error_case == 2:
                    debug("Returning bad exit code")
                    inject_exit_error(output_json, 0)
                else:
                    debug("Returning exit code 5")
                    inject_exit_error(output_json, 5)

                rc = json.dumps(output_json) + "\n"
        else:
            rc = output_str
    except Exception as e:
        debug("Exception %s occurred: JSON = \n%s\n" % (str(e), output_str))
        sys.exit(100)

    return rc


def run_one(cmd):
    debug("run_one(%s)" % str(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)

    if "fullreport" in cmd:
        sys.stdout.write(inject_error(result.stdout))
    else:
        sys.stdout.write(result.stdout)
    sys.stdout.flush()
    sys.stderr.write(result.stderr)
    sys.stderr.flush()
    return result.returncode


class LvmShellHandler:

    def __init__(self, cmd):
        debug(os.environ)

        self.d_stdout = deque()
        self.d_stderr = deque()
        self.d_report = deque()

        tmp_dir = tempfile.mkdtemp(prefix="pipeinmiddle_")
        self.tmp_file = "%s/middle_man_report" % tmp_dir

        # Let's create a fifo for the report output
        os.mkfifo(self.tmp_file, 0o600)

        self.child_report_fd = os.open(self.tmp_file, os.O_NONBLOCK)
        self.child_report_stream = os.fdopen(self.child_report_fd, 'rb', 0)
        passed_report_fd = os.open(self.tmp_file, os.O_WRONLY)

        debug("passed_report_fd = %d" % passed_report_fd)

        # The report FD from who executed us.
        self.daemon_report_fd = int(os.environ["LVM_REPORT_FD"])
        self.daemon_report_stream = os.fdopen(self.daemon_report_fd, "wb", 0)

        env = copy.deepcopy(os.environ)
        env["LVM_REPORT_FD"] = "%s" % str(passed_report_fd)
        env["LC_ALL"] = "C"
        env["LVM_COMMAND_PROFILE"] = "lvmdbusd"

        self.parent_stdin_fd, child_stdin_fd = pty.openpty()
        self.parent_stdout_fd, child_stdout_fd = pty.openpty()
        self.parent_stderr_fd, child_stderr_fd = pty.openpty()
        self.parent_stdin = os.fdopen(self.parent_stdin_fd, "w")
        self.parent_stdout = os.fdopen(self.parent_stdout_fd, "r")
        self.parent_stderr = os.fdopen(self.parent_stderr_fd, "r")

        debug("exec'ing %s" % cmd)
        self.process = Popen(cmd,
                        stdin=child_stdin_fd,
                        stdout=child_stdout_fd,
                        stderr=child_stderr_fd,
                        close_fds=True,
                        env=env,
                        pass_fds=[passed_report_fd, ],
                        shell=False)

        os.close(passed_report_fd)
        os.close(child_stdin_fd)
        os.close(child_stdout_fd)
        os.close(child_stderr_fd)

        make_non_block(self.parent_stdout_fd)
        make_non_block(self.parent_stderr_fd)
        make_non_block(sys.stdin)

        self.report_text_in_progress = ""
        self.last_request = ""

        os.unlink(self.tmp_file)
        os.rmdir(tmp_dir)

    def _complete_response(self):
        try:
            _complete_json = json.loads(self.report_text_in_progress)
            return _complete_json
        except ValueError:
            return None

    def _write_all(self):
        write_some(self.d_stderr, sys.stderr, remaining=True)
        write_some(self.d_report, self.daemon_report_stream, remaining=True, binary=True)
        write_some(self.d_stdout, sys.stdout, remaining=True)

    def _handle_report(self):
        # Read from child report stream, write to parent report stream
        report_text = read_decoded(self.child_report_stream)
        self.report_text_in_progress += report_text
        report_json = self._complete_response()

        # Always wait until we have a full response before we do anything with the output
        if report_json is not None:
            # Only add data to d_report after we have the entire JSON and have injected
            # an error into it if we so wish, usually only for 'fullreport'
            if "fullreport" in self.last_request:
                self.d_report.extend(inject_error(self.report_text_in_progress, report_json))
            else:
                debug("Not the cmd we are looking for ...")
                self.d_report.extend(self.report_text_in_progress)

            self.report_text_in_progress = ""

    def _handle_command(self):
        global run
        stdin_text = sys.stdin.readline()
        self.last_request = stdin_text

        debug("stdin: %s..." % stdin_text[:min(10, len(stdin_text) - 1)])

        if "exit\n" in stdin_text:
            debug("asking to exit ...")
            run.value = 0

        self.parent_stdin.writelines(stdin_text)
        self.parent_stdin.flush()

    @staticmethod
    def _empty_stream_to_queue(stream, queue):
        read_text = stream.readlines()
        for line in read_text:
            queue.extend(line)

    def run(self):
        global run
        select_tmo = 0.2
        while run.value == 1:
            try:
                rd_fd = [sys.stdin.fileno(), self.parent_stdout_fd, self.parent_stderr_fd, self.child_report_fd]
                ready = select.select(rd_fd, [], [], select_tmo)

                if len(ready[0]) == 0:
                    write_some(self.d_stderr, sys.stderr)
                    write_some(self.d_report, self.daemon_report_stream, binary=True)

                for r in ready[0]:
                    if r == self.parent_stdout_fd:
                        LvmShellHandler._empty_stream_to_queue(self.parent_stdout, self.d_stdout)
                    elif r == self.parent_stderr_fd:
                        LvmShellHandler._empty_stream_to_queue(self.parent_stderr, self.d_stderr)
                    elif r == self.child_report_fd:
                        self._handle_report()
                    elif r == sys.stdin.fileno():
                        # Read from parent stdin write to child stdin, this is a command getting issued.
                        self._handle_command()
                    else:
                        debug("FD %d not handled!" % r)
                        sys.exit(10)

                # We have handled all the FDs that were ready, write some output
                if len(self.d_stdout) > 0:
                    self._write_all()
                else:
                    write_some(self.d_stderr, sys.stderr)
                    write_some(self.d_report, self.daemon_report_stream, binary=True)

                # Check to see if child process has terminated, None when running
                if self.process.poll() is not None:
                    self._write_all()
                    debug("child process %s exited %d" % (cmd, self.process.returncode))
                    break
            except IOError as ioe:
                debug("run_cmd:" + str(ioe))

        if self.process.poll() is not None:
            debug("exiting %d " % self.process.returncode)
        else:
            debug("lvm process still running, be we are exiting ...")
        return self.process.returncode


if __name__ == "__main__":
    try:
        args = sys.argv[1:]

        exe = os.getenv("LVM_MAN_IN_MIDDLE", "/usr/sbin/lvm")
        cmdline = [exe, ]
        if args:
            cmdline.extend(args)
            ec = run_one(cmdline)
        else:
            if "LVM_REPORT_FD" in os.environ:
                SH = LvmShellHandler(cmdline)
                ec = SH.run()
            else:
                debug('running as lvm shell requires: LVM_REPORT_FD to be set')
                ec = 1
        sys.exit(ec)
    except Exception:
        traceback.print_exc(file=d_out)
        sys.exit(1)
