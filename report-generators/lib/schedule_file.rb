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

