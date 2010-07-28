# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Merely wraps the logger library with a bit of standard policy.
require 'logger'

module Log
  $log = Logger.new(STDERR)

  def init(io_)
    $log = Logger.new(io_)
  end
end

def fatal(*args)
  $log.fatal(*args)
end

def error(*args)
  $log.error(*args)
end

def info(*args)
  $log.info(*args)
end

def warning(*args)
  $log.warn(*args)
end

def debug(*args)
  $log.debug(*args)
end
