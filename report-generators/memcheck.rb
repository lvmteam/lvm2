# Reads the schedule files given on the command line.  Runs them and
# generates the reports.

# FIXME: a lot of duplication with unit_test.rb

require 'schedule_file'
require 'pathname'
require 'reports'
require 'erb'
require 'report_templates'

include ReportTemplates

schedules = ARGV.map do |f|
  p = Pathname.new(f)
  Schedule.read(p.dirname, p)
end

total_passed = 0
total_failed = 0

# We need to make sure the lvm shared libs are in the LD_LIBRARY_PATH
ENV['LD_LIBRARY_PATH'] = `pwd`.strip + "/libdm:" + (ENV['LD_LIBRARY_PATH'] || '')

ENV['TEST_TOOL'] = "valgrind --leak-check=full --show-reachable=yes"

schedules.each do |s|
  s.run

  s.schedules.each do |t|
    if t.status.success?
      total_passed += 1
    else
      total_failed += 1
    end
  end
end

def mangle(txt)
  txt.gsub(/\s+/, '_')
end

MemcheckStats = Struct.new(:definitely_lost, :indirectly_lost, :possibly_lost, :reachable)

def format(bytes, blocks)
  "#{bytes} bytes, #{blocks} blocks"
end

# Examines the output for details of leaks
def extract_stats(t)
  d = i = p = r = '-'

  t.output.split("\n").each do |l|
    case l
    when /==\d+==    definitely lost: ([0-9,]+) bytes in ([0-9,]+) blocks/
        d = format($1, $2)
    when /==\d+==    indirectly lost: ([0-9,]+) bytes in ([0-9,]+) blocks/
        i = format($1, $2)
    when /==\d+==    possibly lost: ([0-9,]+) bytes in ([0-9,]+) blocks/
        p = format($1, $2)
    when /==\d+==    still reachable: ([0-9,]+) bytes in ([0-9,]+) blocks/
        r = format($1, $2)
    end
  end

  MemcheckStats.new(d, i, p, r)
end

generate_report(:memcheck, binding)

# now we generate a detail report for each schedule
schedules.each do |s|
  s.schedules.each do |t|
    generate_report(:unit_detail, binding, Pathname.new("reports/memcheck_#{mangle(t.desc)}.html"))
  end
end
