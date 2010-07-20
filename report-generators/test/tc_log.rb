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
