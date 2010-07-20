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
