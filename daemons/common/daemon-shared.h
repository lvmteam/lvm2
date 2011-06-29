#include <stdarg.h>
#include <libdevmapper.h>

int read_buffer(int fd, char **buffer);
int write_buffer(int fd, char *buffer, int length);
char *format_buffer(const char *what, const char *id, va_list ap);
