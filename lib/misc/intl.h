/*
 * Copyright (C) 2004 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#ifndef _LVM_INTL_H
#define _LVM_INTL_H

#ifdef INTL_PACKAGE
#  include <libintl.h>
#  define _(String) dgettext(INTL_PACKAGE, (String))
#else
#  define _(String) (String)
#endif

#endif
