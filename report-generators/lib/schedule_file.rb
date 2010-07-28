# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Parses the simple colon delimited test schedule files.

ScheduledTest = Struct.new(:desc, :command_line, :status, :output)

class Schedule
  attr_reader :dir, :schedules

  def initialize(dir, ss)
    @dir = dir
    @schedules = ss
  end

  def run
    Dir::chdir(@dir.to_s) do
      @schedules.each do |s|
        reader, writer = IO.pipe
        print "#{s.desc} ... "
        pid = spawn(s.command_line, [ STDERR, STDOUT ] => writer)
        writer.close
        _, s.status = Process::waitpid2(pid)
        puts (s.status.success? ? "pass" : "fail")
        s.output = reader.read
      end
    end
  end

  def self.read(dir, io)
    ss = Array.new

    io.readlines.each do |line|
      case line.strip
      when /^\#.*/
        next

      when /([^:]+):(.*)/
        ss << ScheduledTest.new($1.strip, $2.strip)

      else
        raise RuntimeError, "badly formatted schedule line"
      end  
    end

    Schedule.new(dir, ss)
  end
end

