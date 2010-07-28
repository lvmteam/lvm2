# Copyright (C) 2010 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

require 'string-store'
require 'test/unit'

class TestStringStore < Test::Unit::TestCase
  def setup
    @ss = StringStore.new(['report-generators/test/strings',
                           'report-generators/test/strings/more_strings'])
  end

  def test_lookup
    assert_equal("Hello, world!\n", @ss.lookup(:test1))
    assert_equal("one\ntwo\nthree", @ss.lookup(:test2))
    assert_equal("lorem\n", @ss.lookup(:test3))

    assert_raises(RuntimeError) do
      @ss.lookup(:unlikely_name)
    end
  end
end
