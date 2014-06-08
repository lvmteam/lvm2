/*
 * Copyright (C) 2010-2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <iostream>
#include <vector>
#include <deque>
#include <map>
#include <sstream>
#include <cassert>
#include <system_error>
#include <algorithm>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h> /* rusage */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>

#define SYSLOG_ACTION_READ_CLEAR     4
#define SYSLOG_ACTION_CLEAR          5

pid_t kill_pid = 0;
bool fatal_signal = false;

std::system_error syserr( std::string msg, std::string ctx = "" ) {
	return std::system_error( errno, std::generic_category(), msg + " " + ctx );
}

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

Listing listdir( std::string p, bool recurse = false, std::string prefix = "" ) {
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
				throw syserr( "stat error", s );
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

struct Options {
	bool verbose, quiet, interactive;
	std::string testdir, outdir;
};

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

struct Sink {
	int fd;

	typedef std::deque< char > Stream;
	typedef std::map< std::string, std::string > Subst;

	Stream stream;
	Subst subst;
	bool killed;

	virtual void outline( bool force )
	{
		Stream::iterator nl = std::find( stream.begin(), stream.end(), '\n' );
		if ( nl == stream.end() && !force )
			return;

		std::string line( stream.begin(), nl );
		stream.erase( stream.begin(), nl + 1 );

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
		}
	}

	void sync() {
		while ( !stream.empty() )
			outline( true );
	}

	void push( std::string x ) {
		if ( !killed )
			std::copy( x.begin(), x.end(), std::back_inserter( stream ) );
	}

	Sink( int fd ) : fd( fd ), killed( false ) {}
};

struct Observer : Sink {
	Observer() : Sink( -1 ) {}
};

struct IO {
	typedef std::vector< Sink* > Sinks;
	mutable Sinks sinks;
	Observer *_observer;

	int fd;
	char buf[ 4097 ];

	void sink( std::string x ) {
		for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
			(*i)->push( x );
	}

	void sync() {
		ssize_t sz;
		while ((sz = read(fd, buf, sizeof(buf) - 1)) > 0)
			sink( std::string( buf, sz ) );

		if ( sz < 0 && errno != EAGAIN )
			throw syserr( "reading pipe" );

		/* get the kernel ring buffer too */
		sz = klogctl( SYSLOG_ACTION_READ_CLEAR, buf, sizeof(buf) - 1 );
		if ( sz > 0 )
			sink( std::string( buf, sz ) );
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

	~IO() {
		for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
			delete *i;
	}

};

struct TestProcess
{
	int fd;
	bool interactive;
	std::string filename;

	void exec() {
		assert( fd >= 0 );
		if ( !interactive ) {
			close( STDIN_FILENO );
			dup2( fd, STDOUT_FILENO );
			dup2( fd, STDERR_FILENO );
		}

		environment();

		setpgid( 0, 0 );
		klogctl( SYSLOG_ACTION_CLEAR, 0, 0 );

		execlp( "bash", "bash", "-noprofile", "-norc", filename.c_str(), NULL );
		perror( "execlp" );
		_exit( 202 );
	}

	void environment() {
		/* if (strchr(f, ':')) {
			strcpy(flavour, f);
			*strchr(flavour, ':') = 0;
			setenv("LVM_TEST_FLAVOUR", flavour, 1);
			strcpy(script, strchr(f, ':') + 1);
		} else {
			strcpy(script, f);
		} */
	}

	TestProcess( std::string file )
		: filename( file ), interactive( false ), fd( -1 )
	{}
};

struct TestCase {
	TestProcess child;
	std::string name;
	IO io;

	struct rusage usage;
	int status;
	bool timeout;
	int silent_ctr;
	pid_t pid;

	time_t start, end;
	Options options;

	void pipe() {
		int fds[2];

		if (socketpair( PF_UNIX, SOCK_STREAM, 0, fds )) {
			perror("socketpair");
			exit(201);
		}

		if (fcntl( fds[0], F_SETFL, O_NONBLOCK ) == -1) {
			perror("fcntl on socket");
			exit(202);
		}

		io.fd = fds[0];
		child.fd = fds[1];
		child.interactive = options.interactive;
	}

	bool monitor() {
		end = time( 0 );
		if ( wait4(pid, &status, WNOHANG, &usage) != 0 )
			return false;

		/* kill off tests after a minute of silence */
		if ( silent_ctr > 2 * 60 ) {
			kill( pid, SIGINT );
			sleep( 5 ); /* wait a bit for a reaction */
			if ( waitpid( pid, &status, WNOHANG ) == 0 ) {
				system( "echo t > /proc/sysrq-trigger" );
				kill( -pid, SIGKILL );
				waitpid( pid, &status, 0 );
			}
			timeout = true;
			return false;
		}

		struct timeval wait;
		fd_set set;

		FD_ZERO( &set );
		FD_SET( io.fd, &set );
		wait.tv_sec = 0;
		wait.tv_usec = 500000; /* timeout 0.5s */

		if ( !options.verbose && !options.interactive )
			progress( Update ) << tag( "running" ) << name << " " << end - start << std::flush;

		if ( select( io.fd + 1, &set, NULL, NULL, &wait ) <= 0 )
		{
			silent_ctr++;
			return true;
		}

		io.sync();
		silent_ctr = 0;

		return true;
	}

	std::string tag( std::string n ) {
		int pad = (8 - n.length());
		return "### " + std::string( pad, ' ' ) + n + ": ";
	}

	std::string tag( Journal::R r ) {
		std::stringstream s;
		s << r;
		return tag( s.str() );
	}

	enum P { First, Update, Last };

	std::ostream &progress( P p = Last )
	{
	        static struct : std::streambuf {} buf;
		static std::ostream null(&buf);

		if ( isatty( STDOUT_FILENO ) ) {
			if ( p != First )
				return std::cout << "\r";
			return std::cout;
		}

		if ( p == Last )
			return std::cout;
		return null;
	}

	void parent() {
		while ( monitor() );

		Journal::R r = Journal::UNKNOWN;

		if ( timeout ) {
			r = Journal::TIMEOUT;
		} else if ( WIFEXITED( status ) ) {
			if ( WEXITSTATUS( status ) == 0 )
				r = Journal::PASSED;
			else if ( WEXITSTATUS( status ) == 200 )
				r = Journal::SKIPPED;
			else
				r = Journal::FAILED;
		} else
			r = Journal::FAILED;

		::close( io.fd );

		/*
		if ((fd_debuglog = open(testdirdebug, O_RDONLY)) != -1) {
			drain(fd_debuglog, unlimited ? INT32_MAX : 4 * 1024 * 1024);
			close(fd_debuglog);
		} */

		progress( Last ) << tag( r ) << name << std::endl;
	}

	void run() {
		pipe();
		pid = kill_pid = fork();
		if (pid < 0) {
			perror("Fork failed.");
			exit(201);
		} else if (pid == 0) {
			io.close();
			child.exec();
		} else {
			progress( First ) << tag( "running" ) << name << std::flush;
			start = time( 0 );
			parent();
		}
	}

	TestCase( std::string path, std::string name )
		: timeout( false ), silent_ctr( 0 ), child( path ), name( name ) {}
};

struct Main {
	bool die;
	time_t start;

	typedef std::vector< TestCase > Cases;

	Journal journal;
	Options options;
	Cases cases;

	void setup() {
		Listing l = listdir( options.testdir, true );
		std::sort( l.begin(), l.end() );

		for ( Listing::iterator i = l.begin(); i != l.end(); ++i ) {
			if ( i->substr( i->length() - 3, i->length() ) != ".sh" )
				continue;
			if ( i->substr( 0, 4 ) == "lib/" )
				continue;
			cases.push_back( TestCase( options.testdir + *i, *i ) );
			cases.back().options = options;
		}

	}

	void run() {
		setup();
		start = time( 0 );
		for ( Cases::iterator i = cases.begin(); i != cases.end(); ++i ) {
			i->run();

			if ( time(0) - start > 3 * 3600 ) {
				printf("3 hours passed, giving up...\n");
				die = 1;
			}

			if ( die || fatal_signal )
				break;
		}

		journal.banner();
		if ( die || fatal_signal )
			exit( 1 );
	}

	Main( Options o ) : options( o ) {}
};

static void handler( int sig ) {
	signal( sig, SIG_DFL );
	if ( kill_pid > 0 )
		kill( -kill_pid, sig );
	fatal_signal = true;
}

void setup_handlers() {
	/* set up signal handlers */
	for ( int i = 0; i <= 32; ++i )
		switch (i) {
			case SIGCHLD: case SIGWINCH: case SIGURG:
			case SIGKILL: case SIGSTOP: break;
			default: signal(i, handler);
		}
}

static int64_t get_time_us(void)
{
       struct timeval tv;

       (void) gettimeofday(&tv, 0);
       return (int64_t) tv.tv_sec * 1000000 + (int64_t) tv.tv_usec;
}


static const char *duration(time_t start, const struct rusage *usage)
{
	static char buf[100];
	int t = (int)(time(NULL) - start);

	int p = sprintf(buf, "%2d:%02d walltime", t / 60, t % 60);

	if (usage)
		sprintf(buf + p, "   %2ld:%02ld.%03ld u, %ld:%02ld.%03ld s, %5ldk rss, %8ld/%ld IO",
			usage->ru_utime.tv_sec / 60, usage->ru_utime.tv_sec % 60,
			usage->ru_utime.tv_usec / 1000,
			usage->ru_stime.tv_sec / 60, usage->ru_stime.tv_sec % 60,
			usage->ru_stime.tv_usec / 1000,
			usage->ru_maxrss / 1024,
			usage->ru_inblock, usage->ru_oublock);

	return buf;
}

struct Args {
	typedef std::vector< std::string > V;
	V args;

	Args( int argc, char **argv ) {
		for ( int i = 1; i < argc; ++ i )
			args.push_back( argv[ i ] );
	}

	bool has( std::string fl ) {
		return std::find( args.begin(), args.end(), fl ) != args.end();
	}

	std::string opt( std::string fl ) {
		V::iterator i = std::find( args.begin(), args.end(), fl );
		if ( i == args.end() || i + 1 == args.end() )
			return "";
		return *(i + 1);
	}
};

int main(int argc, char **argv)
{
	Args args( argc, argv );
	Options opt;

	if ( args.has( "--quiet" ) || getenv( "QUIET" ) ) {
		opt.verbose = false;
		opt.quiet = true;
	}

	if ( args.has( "--verbose" ) || getenv( "VERBOSE" ) ) {
		opt.quiet = false;
		opt.verbose = true;
	}

	if ( args.has( "--interactive" ) || getenv( "INTERACTIVE" ) ) {
		opt.verbose = false;
		opt.quiet = false;
		opt.interactive = true;
	}

	opt.outdir = args.opt( "--outdir" );
	opt.testdir = args.opt( "--testdir" );

	if ( opt.testdir.empty() )
		opt.testdir = "/usr/share/lvm2-testsuite";

	opt.testdir += "/";

	setup_handlers();

	Main main( opt );
	main.run();

}

