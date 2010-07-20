# Reads the schedule files given on the command line.  Runs them and
# generates the reports.

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

generate_report(:unit_test, binding)

# now we generate a detail report for each schedule
schedules.each do |s|
  s.schedules.each do |t|
    generate_report(:unit_detail, binding, Pathname.new("reports/detail_#{mangle(t.desc)}.html"))
  end
end
