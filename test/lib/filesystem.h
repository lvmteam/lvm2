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

#include <vector>
#include <string>

#include <dirent.h>
#include <sys/stat.h>

#ifndef RUNNER_FILESYSTEM_H
#define RUNNER_FILESYSTEM_H

struct dir {
	DIR *d;
	dir( std::string p ) {
		d = opendir( p.c_str() );
		if ( !d )
			throw syserr( "error opening directory", p );
	}
	~dir() { closedir( d ); }
};

typedef std::vector< std::string > Listing;

inline Listing listdir( std::string p, bool recurse = false, std::string prefix = "" )
{
	Listing r;

	dir d( p );
	struct dirent entry, *iter = 0;
	int readerr;

	while ( (readerr = readdir_r( d.d, &entry, &iter )) == 0 && iter ) {
		std::string ename( entry.d_name );

		if ( ename == "." || ename == ".." )
			continue;

		if ( recurse ) {
			struct stat64 stat;
			std::string s = p + "/" + ename;
			if ( ::stat64( s.c_str(), &stat ) == -1 )
				continue;
			if ( S_ISDIR(stat.st_mode) ) {
				Listing sl = listdir( s, true, prefix + ename + "/" );
				for ( Listing::iterator i = sl.begin(); i != sl.end(); ++i )
					r.push_back( prefix + *i );
			} else
				r.push_back( prefix + ename );
		} else
			r.push_back( ename );
	};

	if ( readerr != 0 )
		throw syserr( "error reading directory", p );

	return r;
}

#endif
