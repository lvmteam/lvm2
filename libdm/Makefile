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
INSTALL = /usr/bin/install -c
LN_S = ln -s

prefix = /usr
libdir = ${prefix}/lib
incdir = ${prefix}/include
kernelsrcdir = /scratch/alphalinux/linux

OWNER=root
GROUP=root

INCLUDES=-I. -I${kernelsrcdir}/include
CFLAGS+=-Wall
#CFLAGS+=-O2
CFLAGS+=-g -fno-omit-frame-pointer
#CFLAGS+=-pg
#LD_FLAGS=-pg

libdevmapper.so: libdm.o
	$(CC) -shared -o libdevmapper.so libdm.o

SUFFIXES=
SUFFIXES=.c .o .so

%.o: %.c
	$(CC) -c $(INCLUDES) $(CFLAGS) $< -o $@

install: libdevmapper.so
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 555 -s libdevmapper.so \
		$(libdir)
	$(INSTALL) -D -c -o $(OWNER) -g $(GROUP) -m 444 libdevmapper.h \
		$(incdir)/devmapper/libdevmapper.h

clean:
	$(RM) libdevmapper.so libdm.o

distclean: clean

.PHONY: install test clean distclean

