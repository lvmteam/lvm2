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
