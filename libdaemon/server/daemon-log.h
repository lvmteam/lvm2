enum { DAEMON_LOG_FATAL = 0 /* usually preceding daemon death */
     , DAEMON_LOG_ERROR = 1 /* something serious has happened */
     , DAEMON_LOG_WARN = 2 /* something unusual has happened */
     , DAEMON_LOG_INFO = 3 /* thought you might be interested */
     , DAEMON_LOG_WIRE = 4 /* dump traffic on client sockets */
     , DAEMON_LOG_DEBUG = 5 /* unsorted debug stuff */
     };

#define DEBUG(s, x...) daemon_logf((s)->log, DAEMON_LOG_DEBUG, x)
#define DEBUG_cft(s, i, n) daemon_log_cft((s)->log, DAEMON_LOG_DEBUG, i, n)
#define WARN(s, x...) daemon_logf((s)->log, DAEMON_LOG_WARN, x)
#define INFO(s, x...) daemon_logf((s)->log, DAEMON_LOG_INFO, x)
#define ERROR(s, x...) daemon_logf((s)->log, DAEMON_LOG_ERROR, x)
#define FATAL(s, x...) daemon_logf((s)->log, DAEMON_LOG_FATAL, x)
