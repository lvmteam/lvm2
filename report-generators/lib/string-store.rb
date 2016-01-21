# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Provides a simple way of accessing the contents of files by a symbol
# name.  Useful for erb templates.

require 'pathname'

class StringStore
  attr_accessor :path

  def initialize(p)
    @paths = p.nil? ? Array.new : p # FIXME: do we need to copy p ?
  end

  def lookup(sym)
    files = expansions(sym)

    @paths.each do |p|
      files.each do |f|
        pn = Pathname.new("#{p}/#{f}")
        if pn.file?
          return pn.read
        end
      end
    end

    raise RuntimeError, "unknown string entry: #{sym}"
  end

  private
  def expansions(sym)
    ["#{sym}", "#{sym}.txt"]
  end
end
