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
