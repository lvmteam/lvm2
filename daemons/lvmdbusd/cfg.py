# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import os
import multiprocessing
import queue
import itertools
try:
	from . import path
except SystemError:
	import path

LVM_CMD = os.getenv('LVM_BINARY', path.LVM_BINARY)

# This is the global object manager
om = None

# This is the global bus connection
bus = None

# Shared state variable across all processes
run = multiprocessing.Value('i', 1)

# Debug
DEBUG = True

# Use lvm shell
USE_SHELL = False

# Lock used by pprint
stdout_lock = multiprocessing.Lock()

kick_q = multiprocessing.Queue()
worker_q = queue.Queue()

# Main event loop
loop = None

BASE_INTERFACE = 'com.redhat.lvmdbus1'
PV_INTERFACE = BASE_INTERFACE + '.Pv'
VG_INTERFACE = BASE_INTERFACE + '.Vg'
LV_INTERFACE = BASE_INTERFACE + '.Lv'
LV_COMMON_INTERFACE = BASE_INTERFACE + '.LvCommon'
THIN_POOL_INTERFACE = BASE_INTERFACE + '.ThinPool'
CACHE_POOL_INTERFACE = BASE_INTERFACE + '.CachePool'
LV_CACHED = BASE_INTERFACE + '.CachedLv'
SNAPSHOT_INTERFACE = BASE_INTERFACE + '.Snapshot'
MANAGER_INTERFACE = BASE_INTERFACE + '.Manager'
JOB_INTERFACE = BASE_INTERFACE + '.Job'

BASE_OBJ_PATH = '/' + BASE_INTERFACE.replace('.', '/')
PV_OBJ_PATH = BASE_OBJ_PATH + '/Pv'
VG_OBJ_PATH = BASE_OBJ_PATH + '/Vg'
LV_OBJ_PATH = BASE_OBJ_PATH + '/Lv'
THIN_POOL_PATH = BASE_OBJ_PATH + "/ThinPool"
CACHE_POOL_PATH = BASE_OBJ_PATH + "/CachePool"
HIDDEN_LV_PATH = BASE_OBJ_PATH + "/HiddenLv"
MANAGER_OBJ_PATH = BASE_OBJ_PATH + '/Manager'
JOB_OBJ_PATH = BASE_OBJ_PATH + '/Job'

# Counters for object path generation
pv_id = itertools.count()
vg_id = itertools.count()
lv_id = itertools.count()
thin_id = itertools.count()
cache_pool_id = itertools.count()
job_id = itertools.count()
hidden_lv = itertools.count()

# Used to prevent circular imports...
load = None

# Global cached state
db = None
