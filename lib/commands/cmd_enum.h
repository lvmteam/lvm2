#ifndef _CMD_ENUM_H
#define _CMD_ENUM_H

/*
 * include/cmds.h is generated by the Makefile.  For each command definition
 * in command-lines.in, cmds.h contains:
 * cmd(foo_CMD, foo)
 *
 * This header adds each of the foo_CMD's into an enum, so there's
 * a unique integer identifier for each command definition.
 */

enum {
#define cmd(a, b) a ,
#include "include/cmds.h"
#undef cmd
};

#endif
