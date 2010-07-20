# Merely wraps the logger library with a bit of standard policy.
require 'logger'

module Log
  $log = Logger.new(STDERR)

  def init(io_)
    $log = Logger.new(io_)
  end
end

def fatal(*args)
  $log.fatal(*args)
end

def error(*args)
  $log.error(*args)
end

def info(*args)
  $log.info(*args)
end

def warning(*args)
  $log.warn(*args)
end

def debug(*args)
  $log.debug(*args)
end
