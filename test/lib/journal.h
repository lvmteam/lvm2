// -*- C++ -*-

#include <map>
#include <iostream>

#ifndef RUNNER_JOURNAL_H
#define RUNNER_JOURNAL_H

struct Journal {
	enum R {
		STARTED,
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
			case FAILED: return o << "failed";
			case INTERRUPTED: return o << "interrupted";
			case PASSED: return o << "passed";
			case SKIPPED: return o << "skipped";
			case TIMEOUT: return o << "timeout";
			case WARNED: return o << "warnings";
			default: return o << "unknown";
		}
	}

	typedef std::map< std::string, R > Status;
	Status status;

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
};

#endif
