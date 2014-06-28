// -*- C++ -*-

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
