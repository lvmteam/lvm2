# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Policy for the location of report templates
require 'string-store'

class TemplateStringStore < StringStore
  def initialize()
    super(['report-generators/templates'])
  end
end

module ReportTemplates
  def generate_report(report, bs, dest_path = nil)
    include Reports
    reports = ReportRegister.new
    template_store = TemplateStringStore.new
    report = reports.get_report(report)
    erb = ERB.new(template_store.lookup(report.template))
    body = erb.result(bs)
    title = report.short_desc

    erb = ERB.new(template_store.lookup("boiler_plate.rhtml"))
    txt = erb.result(binding)

    dest_path = dest_path.nil? ? report.path : dest_path
    dest_path.open("w") do |out|
      out.puts txt
    end
  end
end
