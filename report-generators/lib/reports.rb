# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Data about the various reports we support
require 'log'
require 'pathname'

module Reports
  Report = Struct.new(:short_desc, :desc, :path, :template)

  class ReportRegister
    attr_reader :reports

    private
    def add_report(sym, *args)
      @reports[sym] = Report.new(*args)
    end

    public
    def initialize()
      @reports = Hash.new

      add_report(:unit_test,
                 "Unit Tests",
                 "unit tests",
                 Pathname.new("reports/unit.html"),
                 Pathname.new("unit_test.rhtml"))

      add_report(:memcheck,
                 "Memory Tests",
                 "unit tests with valgrind memory checking",
                 Pathname.new("reports/memcheck.html"),
                 Pathname.new("memcheck.rhtml"))

      add_report(:unit_detail,
                 "Unit Test Detail",
                 "unit test detail",
                 Pathname.new("reports/unit_detail.html"), # FIXME replace this with a lambda
                 Pathname.new("unit_detail.rhtml"))
    end

    def get_report(sym)
      raise RuntimeError, "unknown report '#{sym}'" unless @reports.member?(sym)
      @reports[sym]
    end

    def each(&block)
      @reports.each(&block)
    end
  end
end
