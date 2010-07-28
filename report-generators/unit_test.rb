# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

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
