# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

require 'test/unit'
require 'pathname'
require 'schedule_file'

class TestScheduleFile < Test::Unit::TestCase
  def test_reading
    p = Pathname.new("report-generators/test/example.schedule")
    p.open do |f|
      s = Schedule.read(p.dirname, f)

      assert_equal(3, s.schedules.size)
      assert_equal(s.schedules[2].desc, "this comment is prefixed with whitespace")
      assert_equal(s.schedules[0].command_line, "$TEST_TOOL ls")
    end
  end

  def test_running
    p = Pathname.new("report-generators/test/example.schedule")
    p.open do |f|
      s = Schedule.read(p.dirname, f)
      s.run

      s.schedules.each do |t|
        assert(t.status.success?)
      end
    end
  end
end
