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

#include "filesystem.h"

#include <map>
#include <string>
#include <iostream>
#include <fstream>
#include <iterator>

#ifndef RUNNER_JOURNAL_H
#define RUNNER_JOURNAL_H

struct Journal {
	enum R {
		STARTED,
		RETRIED,
		UNKNOWN,
		FAILED,
		INTERRUPTED,
		KNOWNFAIL,
		PASSED,
		SKIPPED,
		TIMEOUT,
		WARNED,
	};

	friend std::ostream &operator<<( std::ostream &o, R r ) {
		switch ( r ) {
			case STARTED: return o << "started";
			case RETRIED: return o << "retried";
			case FAILED: return o << "failed";
			case INTERRUPTED: return o << "interrupted";
			case PASSED: return o << "passed";
			case SKIPPED: return o << "skipped";
			case TIMEOUT: return o << "timeout";
			case WARNED: return o << "warnings";
			default: return o << "unknown";
		}
	}

	friend std::istream &operator>>( std::istream &i, R &r ) {
		std::string x;
		i >> x;

		r = UNKNOWN;
		if ( x == "started" ) r = STARTED;
		if ( x == "retried" ) r = RETRIED;
		if ( x == "failed" ) r = FAILED;
		if ( x == "interrupted" ) r = INTERRUPTED;
		if ( x == "passed" ) r = PASSED;
		if ( x == "skipped" ) r = SKIPPED;
		if ( x == "timeout" ) r = TIMEOUT;
		if ( x == "warnings" ) r = WARNED;
		return i;
	}

	template< typename S, typename T >
	friend std::istream &operator>>( std::istream &i, std::pair< S, T > &r ) {
		return i >> r.first >> r.second;
	}

	typedef std::map< std::string, R > Status;
	Status status, written;

	std::string location, list;

	void append( std::string path ) {
		std::ofstream of( path.c_str(), std::fstream::app );
		Status::iterator writ;
		for ( Status::iterator i = status.begin(); i != status.end(); ++i ) {
			writ = written.find( i->first );
			if ( writ == written.end() || writ->second != i->second )
				of << i->first << " " << i->second << std::endl;
		}
		written = status;
		of.close();
	}

	void write( std::string path ) {
		std::ofstream of( path.c_str() );
		for ( Status::iterator i = status.begin(); i != status.end(); ++i )
			of << i->first << " " << i->second << std::endl;
		of.close();
	}

	void sync() {
		append( location );
		fsync_name( location );
		write ( list );
		fsync_name( list );
	}

	void started( std::string n ) {
		if ( status.count( n ) && status[ n ] == STARTED )
			status[ n ] = RETRIED;
		else
			status[ n ] = STARTED;
		sync();
	}

	void done( std::string n, R r ) {
		status[ n ] = r;
		sync();
	}

	bool done( std::string n ) {
		if ( !status.count( n ) )
			return false;
		return status[ n ] != STARTED && status[ n ] != INTERRUPTED;
	}

	int count( R r ) {
		int c = 0;
		for ( Status::iterator i = status.begin(); i != status.end(); ++i )
			if ( i->second == r )
				++ c;
		return c;
	}

	void banner() {
		std::cout << std::endl << "### " << status.size() << " tests: "
			  << count( PASSED ) << " passed" << std::endl;
	}

	void details() {
		for ( Status::iterator i = status.begin(); i != status.end(); ++i )
			if ( i->second != PASSED )
				std::cout << i->second << ": " << i->first << std::endl;
	}

	void read( std::string n ) {
		std::ifstream ifs( n.c_str() );
		typedef std::istream_iterator< std::pair< std::string, R > > It;
		std::copy( It( ifs ), It(), std::inserter( status, status.begin() ) );
	}

	void read() { read( location ); }

	Journal( std::string dir )
		: location( dir + "/journal" ),
		  list( dir + "/list" )
	{}
};

#endif
