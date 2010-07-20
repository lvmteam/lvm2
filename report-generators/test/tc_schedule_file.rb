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
