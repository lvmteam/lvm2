/*
 * Macro to change log messages into a format that xgettext can handle.
 *
 * Note that different PRI* definitions lead to different strings for
 * different architectures.
 */
#define print_log(level, file, line, format, args...) print_log(format, args)
