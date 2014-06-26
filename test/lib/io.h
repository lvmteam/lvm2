/* -*- C++ -*- copyright (c) 2014 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "util.h"

#include <deque>
#include <map>
#include <vector>
#include <string>
#include <cstdio>
#include <cassert>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/klog.h>

#include <iostream>

#ifndef RUNNER_IO_H
#define RUNNER_IO_H

#define SYSLOG_ACTION_READ_CLEAR     4
#define SYSLOG_ACTION_CLEAR          5

struct Sink {
	virtual void outline( bool ) {}
	virtual void push( std::string x ) = 0;
	virtual void sync() {}
	virtual ~Sink() {}
};

struct BufSink : Sink {
	std::vector< char > data;
	virtual void push( std::string x ) {
		std::copy( x.begin(), x.end(), std::back_inserter( data ) );
	}

	void dump( std::ostream &o ) {
		std::vector< char >::iterator b = data.begin(), e = data.begin();
		o << std::endl;
		while ( e != data.end() ) {
			e = std::find( b, data.end(), '\n' );
			o << "| " << std::string( b, e ) << std::endl;
			b = (e == data.end() ? e : e + 1);
		}
	}
};

struct FdSink : Sink {
	int fd;

	typedef std::deque< char > Stream;
	typedef std::map< std::string, std::string > Subst;

	Stream stream;
	Subst subst;
	bool killed;

	virtual void outline( bool force )
	{
		Stream::iterator nl = std::find( stream.begin(), stream.end(), '\n' );
		if ( nl == stream.end() ) {
			if ( !force )
				return;
		} else
			force = false;

		assert( nl != stream.end() || force );

		std::string line( stream.begin(), nl );
		stream.erase( stream.begin(), force ? nl : nl + 1 );

		if ( std::string( line, 0, 9 ) == "@TESTDIR=" )
			subst[ "@TESTDIR@" ] = std::string( line, 9, std::string::npos );
		else if ( std::string( line, 0, 8 ) == "@PREFIX=" )
			subst[ "@PREFIX@" ] = std::string( line, 8, std::string::npos );
		else {
			int off;
			for ( Subst::iterator s = subst.begin(); s != subst.end(); ++s )
				while ( (off = line.find( s->first )) != std::string::npos )
					line.replace( off, s->first.length(), s->second );
			write( fd, line.c_str(), line.length() );
			if ( !force )
				write( fd, "\n", 1 );
		}
	}

	virtual void sync() {
		if ( killed )
			return;
		while ( !stream.empty() )
			outline( true );
	}

	virtual void push( std::string x ) {
		if ( !killed )
			std::copy( x.begin(), x.end(), std::back_inserter( stream ) );
	}

	FdSink( int _fd ) : fd( _fd ), killed( false ) {}
};

struct FileSink : FdSink {
	std::string file;
	FileSink( std::string n ) : FdSink( -1 ), file( n ) {}

	void sync() {
		if ( fd < 0 && !killed ) {
			fd = open( file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 );
			if ( fd < 0 )
				killed = true;
		}
		FdSink::sync();
	}
	~FileSink() {
		if ( fd >= 0 ) {
			fsync( fd );
			close( fd );
		}
	}
};

struct Observer : Sink {
	Observer() {}
	void push( std::string ) {}
};

struct KMsg {
	int fd;

	bool dev_kmsg() {
		return fd >= 0;
	}

	void reset() {
		int sz;

		if ( dev_kmsg() ) {
			if ( (fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) < 0 ) {
				if (errno != ENOENT) /* Older kernels (<3.5) do not support /dev/kmsg */
					perror("opening /dev/kmsg");
			} else if (lseek(fd, 0L, SEEK_END) == (off_t) -1)
				perror("lseek /dev/kmsg");
		} else
			klogctl( SYSLOG_ACTION_CLEAR, 0, 0 );
	}

	void read( Sink *s ) {
		int sz;

		char buf[ 128 * 1024 ];

		if ( dev_kmsg() ) {
			while ( (sz = ::read(fd, buf, sizeof(buf) - 1)) > 0 )
				s->push( std::string( buf, sz ) );
			if ( sz < 0 ) {
				fd = -1;
				read( s );
			}
		} else {
			while ( (sz = klogctl( SYSLOG_ACTION_READ_CLEAR, buf, sizeof(buf) - 1 )) > 0 )
				s->push( std::string( buf, sz ) );
		}
	}

	KMsg() : fd( -1 ) {}
};

struct IO : Sink {
	typedef std::vector< Sink* > Sinks;
	mutable Sinks sinks;
	Observer *_observer;

	KMsg kmsg;
	int fd;

	virtual void push( std::string x ) {
		for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
			(*i)->push( x );
	}

	void sync() {
		ssize_t sz;
		char buf[ 128 * 1024 ];

		while ( (sz = read(fd, buf, sizeof(buf) - 1)) > 0 )
			push( std::string( buf, sz ) );

		if ( sz < 0 && errno != EAGAIN )
			throw syserr( "reading pipe" );

		kmsg.read( this );

		for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
			(*i)->sync();
	}

	void close() { ::close( fd ); }
	Observer &observer() { return *_observer; }

	IO() : fd( -1 ) {
		sinks.push_back( _observer = new Observer );
	}

	IO( const IO &io ) {
		fd = io.fd;
		sinks = io.sinks;
		io.sinks.clear();
	}

	IO &operator= ( const IO &io ) {
		fd = io.fd;
		sinks = io.sinks;
		io.sinks.clear();
		return *this;
	}

	void clear() {
		for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
			delete *i;
		sinks.clear();
	}

	~IO() { clear(); }

};

#endif
