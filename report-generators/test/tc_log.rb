# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

require 'test/unit'
require 'stringio'
require 'log'

class TestLog < Test::Unit::TestCase
  include Log

  private
  def remove_timestamps(l)
    l.gsub(/\[[^\]]*\]/, '')
  end

  public
  def test_log
    StringIO.open do |out|
      init(out)

      info("msg1")
      warning("msg2")
      debug("msg3")

      assert_equal("I,   INFO -- : msg1\nW,   WARN -- : msg2\nD,  DEBUG -- : msg3\n",
                   remove_timestamps(out.string))
    end
  end
end
