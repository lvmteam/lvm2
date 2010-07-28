# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# This generates the index for the reports, including generation
# times.

require 'log'
require 'string-store'
require 'reports'
require 'erb'
require 'report_templates'

include Reports

reports = ReportRegister.new

def safe_mtime(r)
  r.path.file? ? r.path.mtime.to_s : "not generated"
end

template_store = TemplateStringStore.new

# FIXME: use generate_report() method
erb = ERB.new(template_store.lookup("index.rhtml"))
body = erb.result(binding)
title = "Generation times"

erb = ERB.new(template_store.lookup("boiler_plate.rhtml"))
txt = erb.result(binding)

Pathname.new("reports/index.html").open("w") do |f|
  f.puts txt
end


