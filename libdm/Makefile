#
# Copyright (C) 2001 Sistina Software (UK) Limited.
#
# This file is released under the LGPL.
#

srcdir = .
top_srcdir = ..
VPATH = .
SHELL = /bin/sh

CC = gcc
RANLIB = ranlib
SHELL = /bin/sh
INSTALL = /usr/bin/install
LN_S = ln -s

prefix = /usr
libdir = ${prefix}/lib
incdir = ${prefix}/include
kernelsrcdir = /usr/src/linux

OWNER=root
GROUP=root

INCLUDES=-I${kernelsrcdir}/include
CFLAGS+=-Wall
#CFLAGS+=-O2
CFLAGS+=-g -fno-omit-frame-pointer
#CFLAGS+=-pg
#LD_FLAGS=-pg
STRIP=
#STRIP=-s

libdevmapper.so: libdm.o
	$(CC) -shared -o libdevmapper.so libdm.o

SUFFIXES=
SUFFIXES=.c .o .so

%.o: %.c
	$(CC) -c $(INCLUDES) $(CFLAGS) $< -o $@

install: libdevmapper.so
	$(INSTALL) -o $(OWNER) -g $(GROUP) -m 555 $(STRIP) libdevmapper.so \
		$(libdir)
	$(INSTALL) -D -o $(OWNER) -g $(GROUP) -m 444 libdevmapper.h \
		$(incdir)/libdevmapper.h

clean:
	$(RM) libdevmapper.so libdm.o

distclean: clean

.PHONY: install test clean distclean

