// -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4 -*-

/*
 * This brick allows you to build a test runner for shell-based functional
 * tests. It comes with fairly elaborate features (although most are only
 * available on posix systems), geared toward difficult-to-test software.
 *
 * It provides a full-featured "main" function (brick::shelltest::run) that you
 * can use as a drop-in shell test runner.
 *
 * Features include:
 * - interactive and batch-mode execution
 * - collects test results and test logs in a simple text-based format
 * - measures resource use of individual tests
 * - rugged: suited for running in monitored virtual machines
 * - supports test flavouring
 */

/*
 * (c) 2014 Petr Rockai <me@mornfall.net>
 * (c) 2014 Red Hat, Inc.
 */

/* Redistribution and use in source and binary forms, with or without
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
 * POSSIBILITY OF SUCH DAMAGE. */

#include "configure.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <vector>
#include <map>
#include <deque>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cassert>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <ctime>

#include <dirent.h>

#ifdef __unix
#include <sys/stat.h>
#include <sys/resource.h> /* rusage */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/klog.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#endif

static const long TEST_SUITE_TIMEOUT = 4; // Timeout for the whole test suite in hours (4 hours)
static const long TEST_TIMEOUT = 10 * 60; // Timeout for a single test in seconds (10 minutes)

#ifndef BRICK_SHELLTEST_H
#define BRICK_SHELLTEST_H

namespace brick {
namespace shelltest {

/* TODO: remove this section in favour of brick-filesystem.h */

std::runtime_error syserr( const std::string &msg, std::string ctx = "" ) {
    return std::runtime_error( std::string( strerror( errno ) ) + " " + msg + " " + ctx );
}

// class to restore UI state
class IosFlagSaver {
public:
    explicit IosFlagSaver(std::ostream& _ios):
        ios(_ios),
        f(_ios.flags()) {
    }
    ~IosFlagSaver() {
        ios.flags(f);
    }

    //IosFlagSaver(const IosFlagSaver &rhs) = delete;
    //IosFlagSaver& operator= (const IosFlagSaver& rhs) = delete;

private:
    IosFlagSaver(const IosFlagSaver &rhs); // disable copy constructor
    IosFlagSaver& operator= (const IosFlagSaver& rhs); // old way

    std::ostream& ios;
    std::ios::fmtflags f;
};

class Timespec {
    struct timespec ts;
    static const long _NSEC_PER_SEC = 1000000000;
public:
    Timespec( time_t _sec = 0, long _nsec = 0 ) { ts = (struct timespec) { _sec, _nsec }; }
    Timespec( const struct timeval &tv ) { ts = (struct timespec) { tv.tv_sec, tv.tv_usec * 1000 }; }
    long sec() const { return (long)ts.tv_sec; }
    long nsec() const { return (long)ts.tv_nsec; }
    void gettime() {
#ifdef HAVE_REALTIME
        if ( !clock_gettime( CLOCK_MONOTONIC, &ts ) )
            return;
#endif
        ts.tv_sec = time(0);
        ts.tv_nsec = 0;
    }
    bool is_zero() const { return !ts.tv_sec && !ts.tv_nsec; }
    Timespec elapsed() const {
        Timespec now;
        now.gettime();
        now = now - *this;
        return now;
    }
    Timespec operator+( const Timespec& t ) const {
        Timespec r(ts.tv_sec + t.ts.tv_sec, ts.tv_nsec + t.ts.tv_nsec);
        if (r.ts.tv_nsec >= _NSEC_PER_SEC) {
            r.ts.tv_nsec -= _NSEC_PER_SEC;
            ++r.ts.tv_sec;
        }
        return r;
    }
    Timespec operator-( const Timespec& t ) const {
        Timespec r(ts.tv_sec - t.ts.tv_sec, ts.tv_nsec - t.ts.tv_nsec);
        if (r.ts.tv_nsec < 0) {
            r.ts.tv_nsec += _NSEC_PER_SEC;
            r.ts.tv_sec--;
        }
        return r;
    }
    friend bool operator==( const Timespec& a, const Timespec& b ) {
        return ( a.ts.tv_sec == b.ts.tv_sec ) && ( a.ts.tv_nsec == b.ts.tv_nsec );
    }
    friend bool operator>=( const Timespec& a, const Timespec& b ) {
        return ( a.ts.tv_sec > b.ts.tv_sec ) ||
            ( ( a.ts.tv_sec == b.ts.tv_sec ) && ( a.ts.tv_nsec >= b.ts.tv_nsec ) );
    }
    friend std::ostream& operator<<(std::ostream& os, const Timespec& t) {
        IosFlagSaver iosfs(os);

        os << std::right << std::setw( 2 ) << std::setfill( ' ' ) << t.ts.tv_sec / 60 << ":"
            << std::setw( 2 ) << std::setfill( '0' ) << t.ts.tv_sec % 60 << "."
            << std::setw( 3 ) << t.ts.tv_nsec / 1000000; // use milliseconds ATM
        return os;
    }
};

static void _fsync_name( std::string n )
{
    int fd = open( n.c_str(), O_WRONLY );
    if ( fd >= 0 ) {
        (void) fsync( fd );
        (void) close( fd );
    }
}

struct dir {
    DIR *d;
    dir( const std::string &p ) {
        d = opendir( p.c_str() );
        if ( !d )
            throw syserr( "error opening directory", p );
    }
    ~dir() { (void) closedir( d ); }
};

typedef std::vector< std::string > Listing;

Listing listdir( std::string p, bool recurse = false, std::string prefix = "" )
{
    Listing r;

    if ( ( p.size() > 0 ) && ( p[ p.size() - 1 ] == '/' ) )
        p.resize( p.size() - 1 ); // works with older g++

    dir d( p );
#if !defined(__GLIBC__) || (__GLIBC__ < 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ < 23))
    /* readdir_r is deprecated with newer GLIBC */
    struct dirent entry, *iter = 0;
    while ( (errno = readdir_r( d.d, &entry, &iter )) == 0 && iter ) {
        std::string ename( entry.d_name );
#else
    struct dirent *entry;
    errno = 0;
    while ( (entry = readdir( d.d )) ) {
        std::string ename( entry->d_name );
#endif

        if ( ename == "." || ename == ".." )
            continue;

        if ( recurse ) {
            struct stat64 stat;
            std::string s = p + "/" + ename;
            if ( ::stat64( s.c_str(), &stat ) == -1 ) {
                errno = 0;
                continue;
            }
            if ( S_ISDIR(stat.st_mode) ) {
                Listing sl = listdir( s, true, prefix + ename + "/" );
                for ( Listing::iterator i = sl.begin(); i != sl.end(); ++i )
                    r.push_back( prefix + *i );
            } else
                r.push_back( prefix + ename );
        } else
            r.push_back( ename );
    };

    if ( errno != 0 )
        throw syserr( "error reading directory", p );

    return r;
}

/* END remove this section */

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
        const char *t;
        switch ( r ) {
            case STARTED: t = "started"; break;
            case RETRIED: t = "retried"; break;
            case FAILED: t = "failed"; break;
            case INTERRUPTED: t = "interrupted"; break;
            case PASSED: t = "passed"; break;
            case SKIPPED: t = "skipped"; break;
            case TIMEOUT: t = "timeout"; break;
            case WARNED: t = "warnings"; break;
            default: t = "unknown";
        }
        return o << t;
    }

    friend std::istream &operator>>( std::istream &i, R &r ) {
        std::string x;
        i >> x;

        if ( x == "started" ) r = STARTED;
        else if ( x == "retried" ) r = RETRIED;
        else if ( x == "failed" ) r = FAILED;
        else if ( x == "interrupted" ) r = INTERRUPTED;
        else if ( x == "passed" ) r = PASSED;
        else if ( x == "skipped" ) r = SKIPPED;
        else if ( x == "timeout" ) r = TIMEOUT;
        else if ( x == "warnings" ) r = WARNED;
        else r = UNKNOWN;
        return i;
    }

    template< typename S, typename T >
    friend std::istream &operator>>( std::istream &i, std::pair< S, T > &r ) {
        return i >> r.first >> r.second;
    }

    typedef std::map< std::string, R > Status;
    Status status, written;
    typedef std::map< std::string, std::string > RUsage;
    RUsage rusage;

    std::string location, list;
    int timeouts;
    size_t size_max;

    void append( const std::string &path ) {
        std::ofstream of( path.c_str(), std::fstream::app );
        Status::iterator writ;
        for ( Status::const_iterator i = status.begin(); i != status.end(); ++i ) {
            writ = written.find( i->first );
            if ( writ == written.end() || writ->second != i->second ) {
                RUsage::const_iterator ru = rusage.find( i->first );
                of << std::left << std::setw( (int)size_max ) << std::setfill( ' ' )
                   << i->first << " " << std::setw( 12 ) << i->second;
                if (ru != rusage.end())
                    of << ru->second;
                else {
                    struct tm time_info;
                    char buf[64];
                    time_t t = time( 0 );
                    if (localtime_r(&t, &time_info)) {
                        strftime(buf, sizeof(buf), "%F %T", &time_info);
                        of << "--- " << buf << " ---";
                    }
                }
                of << std::endl;
            }
        }
        written = status;
        of.flush();
        of.close();
    }

    void write( const std::string &path ) {
        std::ofstream of( path.c_str() );
        for ( Status::const_iterator i = status.begin(); i != status.end(); ++i )
            of << i->first << " "  << i->second << std::endl;
        of.flush();
        of.close();
    }

    void sync() {
        append( location );
        _fsync_name( location );
        write ( list );
        _fsync_name( list );
    }

    void started( const std::string &n ) {
        if ( status.count( n ) && status[ n ] == STARTED )
            status[ n ] = RETRIED;
        else
            status[ n ] = STARTED;
        sync();
    }

    void done( const std::string &n, R r, const std::string &ru ) {
        status[ n ] = r;
        rusage[ n ] = ru;
        if ( r == TIMEOUT )
            ++ timeouts;
        else
            timeouts = 0;
        sync();
    }

    bool done( const std::string &n ) {
        if ( !status.count( n ) )
            return false;
        return status[ n ] != STARTED && status[ n ] != INTERRUPTED;
    }

    unsigned count( R r ) const {
        unsigned c = 0;
        for ( Status::const_iterator i = status.begin(); i != status.end(); ++i )
            if ( i->second == r )
                ++ c;
        return c;
    }

    void banner( const Timespec &start ) const {
        std::cout << std::endl << "### " << status.size() << " tests: "
                  << count( PASSED ) << " passed, "
                  << count( SKIPPED ) << " skipped, "
                  << count( TIMEOUT ) << " timed out, " << count( WARNED ) << " warned, "
                  << count( FAILED ) << " failed   in "
                  << start.elapsed() << std::endl;
    }

    void details() const {
        for ( Status::const_iterator i = status.begin(); i != status.end(); ++i )
            if ( i->second != PASSED )
                std::cout << i->second << ": " << i->first << std::endl;
    }

    void read( const std::string &n ) {
        std::ifstream ifs( n.c_str() );
        typedef std::istream_iterator< std::pair< std::string, R > > It;
        for ( std::string line; std::getline( ifs, line ); ) {
            std::istringstream iss( line );
            It i( iss );
            status[ i->first ] = i->second;
        }
    }

    void read() { read( location ); }

    void check_name_size( const std::string &n ) { if ( n.size() > size_max ) size_max = n.size(); }
    size_t name_size_max() const { return size_max; }

    Journal( const std::string &dir );
    ~Journal();
};

Journal::Journal( const std::string &dir ) :
    location( dir + "/journal" ),
    list( dir + "/list" ),
    timeouts( 0 ), size_max( 0 )
{
}

Journal::~Journal()
{
}

struct TimedBuffer {
    typedef std::pair< Timespec, std::string > Line;

    std::deque< Line > data;
    Line incomplete;
    bool stamp;

    Line shift( bool force = false ) {
        Line result = std::make_pair( 0, "" );
        if ( force && data.empty() )
            std::swap( result, incomplete );
        else {
            result = data.front();
            data.pop_front();
        }
        return result;
    }

    void push( const std::string &buf ) {
        Timespec now;
        if ( stamp )
            now.gettime();
        std::string::const_iterator b = buf.begin(), e = buf.begin();

        while ( e != buf.end() )
        {
            e = std::find( b, buf.end(), '\n' );
            incomplete.second += std::string( b, e );

            if ( incomplete.first.is_zero() )
                incomplete.first = now;

            if ( e != buf.end() ) {
                incomplete.second += "\n";
                data.push_back( incomplete );
                if (incomplete.second[0] == '#') {
                    /* Disable timing between '## 0 STACKTRACE' & '## teardown' keywords */
                    if (incomplete.second.find("# 0 STACKTRACE", 1) != std::string::npos ||
                        incomplete.second.find("# timing off", 1) != std::string::npos) {
                        stamp = false;
                        now = 0;
                    } else if (incomplete.second.find("# teardown", 1) != std::string::npos ||
                               incomplete.second.find("# timing on", 1) != std::string::npos) {
                        stamp = true;
                        now.gettime();
                    }
                }
                incomplete = std::make_pair( now, "" );
            }
            b = (e == buf.end() ? e : e + 1);
        }
    }

    bool empty( bool force = false ) {
        if ( force && !incomplete.second.empty() )
            return false;
        return data.empty();
    }

    TimedBuffer() : stamp(true) {}
};

struct Sink {
    virtual void outline( bool ) {}
    virtual void push( const std::string &x ) = 0;
    virtual void sync( bool ) {}
    virtual ~Sink() {}
};

struct Substitute {
    typedef std::map< std::string, std::string > Map;
    std::string testdir; // replace testdir first
    std::string prefix;

    std::string map( std::string line ) {
        return line;
    }
};

struct Format {
    Timespec start;
    Substitute subst;

    std::string format( const TimedBuffer::Line &l ) {
        std::stringstream result;
        if ( l.first >= start ) {
            Timespec rel = l.first - start;
            result << "[" << rel << "] ";
        }
        result << subst.map( l.second );
        return result.str();
    }

    Format() { start.gettime(); }
};

struct BufSink : Sink {
    TimedBuffer data;
    Format fmt;

    virtual void push( const std::string &x ) {
        data.push( x );
    }

    void dump( std::ostream &o ) {
        o << std::endl;
        while ( !data.empty( true ) )
            o << "| " << fmt.format( data.shift( true ) );
    }
};

struct FdSink : Sink {
    int fd;

    TimedBuffer stream;
    Format fmt;
    bool killed;

    virtual void outline( bool force )
    {
        TimedBuffer::Line line = stream.shift( force );
        std::string out = fmt.format( line );
        if ( write( fd, out.c_str(), out.length() ) < (int)out.length() )
            perror( "short write" );
    }

    virtual void sync( bool force ) {
        if ( killed )
            return;
        while ( !stream.empty( force ) )
            outline( force );
    }

    virtual void push( const std::string &x ) {
        if ( !killed )
            stream.push( x );
    }

    FdSink( int _fd ) : fd( _fd ), killed( false ) {}
    ~FdSink();
};

FdSink::~FdSink()
{ // no inline
}

struct FileSink : FdSink {
    std::string file;
    FileSink( const std::string &n ) : FdSink( -1 ), file( n ) {}

    void sync( bool force ) {
        if ( fd < 0 && !killed ) {
#ifdef O_CLOEXEC
            fd = open( file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 );
#else
            fd = open( file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
            if ( fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0 )
                perror("failed to set FD_CLOEXEC on file");
#endif
            if ( fd < 0 )
                killed = true;
        }
        FdSink::sync( force );
    }

    ~FileSink() {
        if ( fd >= 0 ) {
            (void) fsync( fd );
            (void) close( fd );
        }
    }
};

#define BRICK_SYSLOG_ACTION_READ           2
#define BRICK_SYSLOG_ACTION_READ_ALL       3
#define BRICK_SYSLOG_ACTION_READ_CLEAR     4
#define BRICK_SYSLOG_ACTION_CLEAR          5
#define BRICK_SYSLOG_ACTION_SIZE_UNREAD    9
#define BRICK_SYSLOG_ACTION_SIZE_BUFFER   10

struct Source {
    int fd;

    virtual void sync( Sink *sink ) {
        ssize_t sz;
        /* coverity[stack_use_local_overflow] */
        char buf[ 128 * 1024 ];
        if ( (sz = read(fd, buf, sizeof(buf) - 1)) > 0 )
            sink->push( std::string( buf, sz ) );

        /*
         * On RHEL5 box this code busy-loops here, while
         * parent process no longer writes anything.
         *
         * Unclear why 'select()' is announcing available
         * data, while we read 0 bytes with errno == 0.
         *
         * Temporarily resolved with usleep() instead of loop.
         */
        if (!sz && (!errno || errno == EINTR))
            usleep(50000);

        if ( sz < 0 && errno != EAGAIN )
            throw syserr( "reading pipe" );
    }

    virtual void reset() {}

    virtual int fd_set_( fd_set *set ) {
        if ( fd >= 0 ) {
            FD_SET( fd, set );
            return fd;
        } else
            return -1;
    }

    Source( int _fd = -1 ) : fd( _fd ) {}
    virtual ~Source() {
        if ( fd >= 0 )
            (void) ::close( fd );
    }
};

struct FileSource : Source {
    std::string file;
    FileSource( const std::string &n ) : Source( -1 ), file( n ) {}

    int fd_set_( ::fd_set * ) { return -1; } /* reading a file is always non-blocking */
    void sync( Sink *s ) {
        if ( fd < 0 ) {
#ifdef O_CLOEXEC
            fd = open( file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK );
#else
            fd = open( file.c_str(), O_RDONLY | O_NONBLOCK );
            if ( fcntl( fd, F_SETFD, FD_CLOEXEC ) < 0 )
                perror("failed to set FD_CLOEXEC on file");
#endif
            if ( fd >= 0 )
                lseek( fd, 0, SEEK_END );
        }
        if ( fd >= 0 )
            try {
                Source::sync( s );
            } catch (...) {
                perror("failed to sync");
            }
    }
};

struct KMsg : Source {
    bool can_clear;
    ssize_t buffer_size;

    KMsg() : can_clear( strcmp(getenv("LVM_TEST_CAN_CLOBBER_DMESG") ? : "0", "0") ),
        buffer_size(128 * 1024)
    {
#ifdef __unix
        struct utsname uts;
        unsigned kmaj, kmin, krel;
        const char *read_msg = "/dev/kmsg";

        // Can't use kmsg on kernels pre 3.5, read /var/log/messages
        if ( ( ::uname(&uts) == 0 ) &&
            ( ::sscanf( uts.release, "%u.%u.%u", &kmaj, &kmin, &krel ) == 3 ) &&
            ( ( kmaj < 3 ) || ( ( kmaj == 3 ) && ( kmin < 5 ) ) ) )
            can_clear = false, read_msg = "/var/log/messages";

        if ( ( fd = open(read_msg, O_RDONLY | O_NONBLOCK)) < 0 ) {
            if ( errno != ENOENT ) /* Older kernels (<3.5) do not support /dev/kmsg */
                fprintf( stderr, "open log %s %s\n", read_msg, strerror( errno ) );
            if ( can_clear && ( klogctl( BRICK_SYSLOG_ACTION_CLEAR, 0, 0 ) < 0 ) )
                can_clear = false;
        } else if ( lseek( fd, 0L, SEEK_END ) == (off_t) -1 ) {
            fprintf( stderr, "lseek log %s %s\n", read_msg, strerror( errno ) );
            (void) close(fd);
            fd = -1;
        }
#endif
    }

    bool dev_kmsg() {
        return fd >= 0;
    }

    void transform( char *buf, ssize_t *sz ) {
        char newbuf[ buffer_size ];
        struct tm time_info;
        unsigned level, num, pos;
        unsigned long t;
        time_t tt;
        size_t len;
        const char *delimiter;

        buf[ *sz ] = 0;
        delimiter = strchr(buf, ';');

        if ( sscanf( buf, "%u,%u,%lu,-%n", &level, &num, &t, &pos) == 3 ) {
            if ( delimiter++ && ( delimiter - buf ) > pos )
                pos = delimiter - buf;
            memcpy( newbuf, buf, *sz );
            tt = time( 0 );
            len = snprintf( buf, 64, "[%lu.%06lu] <%u> ", t / 1000000, t % 1000000, level );
            if ( localtime_r( &tt, &time_info ) )
                len += strftime( buf + len, 64, "%F %T  ", &time_info );
            memcpy( buf + len, newbuf + pos, *sz - pos );
            *sz = *sz - pos + len;
        }
    }

    void sync( Sink *s ) {
#ifdef __unix
        ssize_t sz;
        char buf[ buffer_size ];

        if ( dev_kmsg() ) {
            while ( (sz = ::read( fd, buf, buffer_size - 129 ) ) > 0 ) {
                transform( buf, &sz );
                s->push( std::string( buf, sz ) );
            }
        } else if ( can_clear ) {
            while ( ( sz = klogctl( BRICK_SYSLOG_ACTION_READ_CLEAR, buf,
                                   ( int) buffer_size ) ) > 0 )
                s->push( std::string( buf, sz ) );
            if ( sz < 0 && errno == EPERM )
                can_clear = false;
        }
#endif
    }
};

struct Observer : Sink {
    TimedBuffer stream;

    bool warnings;
    Observer() : warnings( false ) {}

    void push( const std::string &s ) {
        stream.push( s );
    }

    void sync( bool force ) {
        while ( !stream.empty( force ) ) {
            TimedBuffer::Line line = stream.shift( force );
            if ( line.second.find( "TEST WARNING" ) != std::string::npos )
                warnings = true;
        }
    }
};

struct IO : Sink {
    typedef std::vector< Sink* > Sinks;
    typedef std::vector< Source* > Sources;

    mutable Sinks sinks;
    mutable Sources sources;

    Observer *_observer;

    virtual void push( const std::string &x ) {
        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            (*i)->push( x );
    }

    void sync( bool force ) {
        for ( Sources::iterator i = sources.begin(); i != sources.end(); ++i )
            (*i)->sync( this );

        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            (*i)->sync( force );
    }

    void close() {
        for ( Sources::iterator i = sources.begin(); i != sources.end(); ++i )
            delete *i;
        sources.clear();
    }

    int fd_set_( fd_set *set ) {
        int max = -1;

        for ( Sources::iterator i = sources.begin(); i != sources.end(); ++i )
            max = std::max( (*i)->fd_set_( set ), max );
        return max + 1;
    }

    Observer &observer() { return *_observer; }

    IO() {
        clear();
    }

    /* a stealing copy constructor */
    IO( const IO &io ) : sinks( io.sinks ), sources( io.sources ), _observer( io._observer )
    {
        io.sinks.clear();
        io.sources.clear();
    }

    IO &operator= ( const IO &io ) {
        this->~IO();
        return *new (this) IO( io );
    }

    void clear( int to_push = 1 ) {
        for ( Sinks::iterator i = sinks.begin(); i != sinks.end(); ++i )
            delete *i;
        sinks.clear();
        if ( to_push )
            sinks.push_back( _observer = new Observer );
    }

    ~IO() { close(); clear(0); }

};

namespace {
pid_t kill_pid = 0;
bool fatal_signal = false;
bool interrupt = false;
}

struct Options {
    bool verbose, batch, interactive, cont, fatal_timeouts, kmsg;
    std::string testdir, outdir, workdir, heartbeat;
    std::vector< std::string > flavours, filter, skip, watch;
    std::string flavour_envvar;
    int timeout;
    Options();
    Options(const Options &o); // copy
    ~Options();
};

Options::Options() :
    verbose( false ), batch( false ), interactive( false ),
    cont( false ), fatal_timeouts( false ), kmsg( true ),
    timeout( TEST_TIMEOUT )
{ // no inline
}

Options::Options(const Options &o) :
    verbose( o.verbose ), batch( o.batch ), interactive( o.interactive ),
    cont( o.cont ), fatal_timeouts( o.fatal_timeouts ), kmsg( o.kmsg ),
    testdir( o.testdir ), outdir( o.outdir ), workdir( o.workdir ), heartbeat( o.heartbeat ),
    flavours( o.flavours ), filter( o.filter ), skip( o.skip ), watch( o.watch ),
    flavour_envvar( o.flavour_envvar ),
    timeout( o.timeout )
{ // no inline
}

Options::~Options()
{ // no inline
}

struct TestProcess
{
    std::string filename;
    bool interactive;
    int fd;

    void exec() __attribute__ ((noreturn)) {
        assert( fd >= 0 );
        if ( !interactive ) {
            int devnull = ::open( "/dev/null", O_RDONLY );
            if ( devnull >= 0 ) { /* gcc really doesn't like to not have stdin */
                (void) dup2( devnull, STDIN_FILENO );
                (void) close( devnull );
            } else
                (void) close( STDIN_FILENO );
            (void) dup2( fd, STDOUT_FILENO );
            (void) dup2( fd, STDERR_FILENO );
            (void) close( fd );
        }

        setpgid( 0, 0 );

        execlp( "bash", "bash", "-noprofile", "-norc", filename.c_str(), NULL );
        perror( "execlp" );
        _exit( 202 );
    }

    TestProcess( const std::string &file ) :
        filename( file ), interactive( false ), fd( -1 )
    {
    }
};

struct TestCase {
    TestProcess child;
    std::string name, flavour;
    IO io;
    BufSink *iobuf;

    struct rusage usage;
    int status;
    bool timeout;
    pid_t pid;

    Timespec start, silent_start, last_update, last_heartbeat;
    Options options;

    Journal *journal;

    TestCase(const TestCase &t); // copy

    std::string pretty() {
        if ( options.batch )
            return flavour + ": " + name;
        return "[" + flavour + "] " + name;
    }

    std::string id() {
        return flavour + ":" + name;
    }

    void pipe() {
        int fds[2] = { 0 };

        if (socketpair( PF_UNIX, SOCK_STREAM, 0, fds )) {
            perror("socketpair");
            exit(201);
        }

#if 0
        if (fcntl( fds[0], F_SETFL, O_NONBLOCK ) == -1) {
            perror("fcntl on socket");
            exit(202);
        }
#endif

        io.sources.push_back( new Source( fds[0] ) );
        child.fd = fds[1];
        child.interactive = options.interactive;
    }

    void show_progress() {
        progress( Update ) << tag( "running" )
            << pretty() << " " << start.elapsed() << std::flush;
    }

    bool monitor() {
        /* heartbeat */
        if ( last_heartbeat.elapsed().sec() >= 20 && !options.heartbeat.empty() ) {
            std::ofstream hb( options.heartbeat.c_str(), std::fstream::app );
            hb << ".";
            hb.flush();
            hb.close();
            _fsync_name( options.heartbeat );
            last_heartbeat.gettime();
        }

        if ( wait4(pid, &status, WNOHANG, &usage) != 0 ) {
            io.sync( true );
            show_progress();
            return false;
        }

        /* kill off tests after a timeout silence */
        if ( !options.interactive )
            if ( silent_start.elapsed().sec() > options.timeout ) {
                pid_t p;
                for ( int i = 0; i < 5; ++i ) {
                    kill( pid, (i > 2) ? SIGTERM : SIGINT );
                    if ( (p = waitpid( pid, &status, WNOHANG ) ) != 0 )
                        break;
                    sleep( 1 ); /* wait a bit for a reaction */
                }
                if ( !p ) {
                    std::ofstream tr( DEFAULT_PROC_DIR "/sysrq-trigger" );
                    tr << "t";
                    tr.close();
                    kill( -pid, SIGKILL );
                    (void) waitpid( pid, &status, 0 );
                }
                timeout = true;
                io.sync( true );
                return false;
            }

        struct timeval wait = (struct timeval) { 0, 500000 /* timeout 0.5s */ };
        fd_set set;

        FD_ZERO( &set );
        int nfds = io.fd_set_( &set );

        if ( !options.verbose && !options.interactive && !options.batch ) {
            if ( last_update.elapsed().sec() ) {
                show_progress();
                last_update.gettime();
            }
        }
        if ( select( nfds, &set, NULL, NULL, &wait ) > 0 ) {
            io.sync( false );
            silent_start.gettime(); /* something happened */
        }

        return true;
    }

    std::string rusage()
    {
        std::stringstream ss;
        Timespec wall(start.elapsed()), user(usage.ru_utime), system(usage.ru_stime);
        size_t rss = (usage.ru_maxrss + 512) / 1024,
            inb = (usage.ru_inblock + 1024) / 2048,  // to MiB
            outb = (usage.ru_oublock + 1024) / 2048; // to MiB

        ss << wall << " wall " << user << " user " << system << " sys "
            << std::setw( 4 ) << std::setfill( ' ' ) << rss << " M mem "
            << std::setw( 5 ) << inb << " M in "
            << std::setw( 5 ) << outb << " M out";

        return ss.str();
    }

    std::string tag( const std::string &n ) {
        if ( options.batch )
            return "## ";
        size_t pad = n.length();
        pad = (pad < 12) ? 12 - pad : 0;
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

        if ( options.batch && p == First )
            return std::cout;

        if ( isatty( STDOUT_FILENO ) && !options.batch ) {
            if ( p != First )
                return std::cout << "\r";
            return std::cout;
        }

        if ( p == Last )
            return std::cout;

        return null;
    }

    void parent()
    {
        (void) ::close( child.fd );
        setupIO();

        journal->started( id() );
        start.gettime();
        silent_start = start;

        progress( First ) << tag( "running" ) << pretty() << std::flush;
        if ( options.verbose || options.interactive )
            progress() << std::endl;

        while ( monitor() )
            /* empty */ ;

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
        } else if ( interrupt && WIFSIGNALED( status ) && WTERMSIG( status ) == SIGINT )
            r = Journal::INTERRUPTED;
        else
            r = Journal::FAILED;

        if ( r == Journal::PASSED && io.observer().warnings )
            r = Journal::WARNED;

        io.close();

        if ( iobuf && ( r == Journal::FAILED || r == Journal::TIMEOUT ) )
            iobuf->dump( std::cout );

        std::string ru = rusage();
        journal->done( id(), r, ru );

        if ( options.batch ) {
            int spaces = std::max( 4 + int(journal->name_size_max()) - int(pretty().length()), 0 );
            std::string sp;
            sp.reserve( spaces );
            if ( spaces % 2 )
                sp += " ";
            for ( int i = 0; i < spaces / 2; ++i )
                sp += " .";
            progress( Last ) << sp << " "
                << std::left << std::setw( 8 ) << std::setfill( ' ' ) << r;
            if ( r != Journal::SKIPPED )
                progress( First ) << "   " << ru;
            progress( Last ) << std::endl;
        } else
            progress( Last ) << tag( r ) << pretty() << std::endl;

        io.clear();
    }

    void run() {
        pipe();
        pid = kill_pid = fork();
        if (pid < 0) {
            perror("Fork failed.");
            exit(201);
        } else if (pid == 0) {
            io.close();
            if ( chdir( options.workdir.c_str() ) )
                perror( "chdir failed." );

            if ( !options.flavour_envvar.empty() )
                (void) setenv( options.flavour_envvar.c_str(), flavour.c_str(), 1 );
            child.exec();
        } else {
            parent();
        }
    }

    void setupIO() {
        iobuf = 0;
        if ( options.verbose || options.interactive )
            io.sinks.push_back( new FdSink( 1 ) );
        else if ( !options.batch )
            io.sinks.push_back( iobuf = new BufSink() );

        std::string n = id();
        std::replace( n.begin(), n.end(), '/', '_' );
        std::string fn = options.outdir + "/" + n + ".txt";
        io.sinks.push_back( new FileSink( fn ) );

        for ( std::vector< std::string >::iterator i = options.watch.begin();
              i != options.watch.end(); ++i )
            io.sources.push_back( new FileSource( *i ) );
        if ( options.kmsg )
            io.sources.push_back( new KMsg );
    }

    TestCase( Journal &j, const Options &opt, const std::string &path, const std::string &_name, const std::string &_flavour );
    ~TestCase();
};

TestCase::TestCase( Journal &j, const Options &opt, const std::string &path, const std::string &_name, const std::string &_flavour ) :
    child( path ), name( _name ), flavour( _flavour ),
    iobuf( NULL ), usage( ( struct rusage ) { { 0 } } ), status( 0 ), timeout( false ),
    pid( 0 ), options( opt ), journal( &j )
{ // no inline
}

TestCase::TestCase( const TestCase &t ) :
    child( t.child ), name( t.name ), flavour( t.flavour),
    io( t.io ), iobuf( t.iobuf ), usage( t.usage ), status( t.status ), timeout( t.timeout ),
    pid( t.pid ), start( t.start), silent_start( t.silent_start ),
    last_update( t.last_update ), last_heartbeat( t.last_heartbeat ),
    options( t.options ), journal( t.journal )
{ // no inline
}

TestCase::~TestCase()
{ // no inline
}

struct Main {
    bool die;
    Timespec start;

    typedef std::vector< TestCase > Cases;
    typedef std::vector< std::string > Flavours;

    Journal journal;
    Options options;
    Cases cases;

    void setup() {
        bool filter;
        Listing l = listdir( options.testdir, true );
        std::sort( l.begin(), l.end() );

        for ( Flavours::iterator flav = options.flavours.begin();
              flav != options.flavours.end(); ++flav ) {

            for ( Listing::iterator i = l.begin(); i != l.end(); ++i ) {
                if ( ( i->length() < 3 ) || ( i->substr( i->length() - 3, i->length() ) != ".sh" ) )
                    continue;
                if ( i->substr( 0, 4 ) == "lib/" )
                    continue;

                if (!options.filter.empty()) {
                    filter = true;
                    for ( std::vector< std::string >::iterator filt = options.filter.begin();
                          filt != options.filter.end(); ++filt ) {
                        if ( i->find( *filt ) != std::string::npos ) {
                            filter = false;
                            break;
                        }
                    }
                    if ( filter )
                        continue;
                }

                if (!options.skip.empty()) {
                    filter = false;
                    for ( std::vector< std::string >::iterator filt = options.skip.begin();
                          filt != options.skip.end(); ++filt ) {
                        if ( i->find( *filt ) != std::string::npos ) {
                            filter = true;
                            break;
                        }
                    }
                    if ( filter )
                        continue;
                }

                cases.push_back( TestCase( journal, options, options.testdir + *i, *i, *flav ) );
                cases.back().options = options;
                journal.check_name_size( cases.back().id() );
            }
        }

        if ( options.cont )
            journal.read();
        else
            (void) ::unlink( journal.location.c_str() );
    }

    int run() {
        setup();

        std::cerr << "running " << cases.size() << " tests" << std::endl;

        for ( Cases::iterator i = cases.begin(); i != cases.end(); ++i ) {

            if ( options.cont && journal.done( i->id() ) )
                continue;

            i->run();

            if ( options.fatal_timeouts && journal.timeouts >= 2 ) {
                journal.started( i->id() ); // retry the test on --continue
                std::cerr << "E: Hit 2 timeouts in a row with --fatal-timeouts" << std::endl;
                std::cerr << "Suspending (please restart the VM)." << std::endl;
                sleep( 3600 );
                die = 1;
            }

            if ( start.elapsed().sec() > ( TEST_SUITE_TIMEOUT * 3600 ) ) {
                std::cerr << TEST_SUITE_TIMEOUT << " hours passed, giving up..." << std::endl;
                die = 1;
            }

            if ( die || fatal_signal )
                break;
        }

        journal.banner( start );
        if ( die || fatal_signal )
            return 1;

        return journal.count( Journal::FAILED ) || journal.count( Journal::TIMEOUT ) ? 1 : 0;
    }

    Main( const Options &o );
    ~Main();
};

Main::Main( const Options &o ) :
    die( false ), journal( o.outdir ), options( o )
{
    start.gettime();
}

Main::~Main()
{ // no inline
}

namespace {

void handler( int sig ) {
    signal( sig, SIG_DFL ); /* die right away next time */
    if ( kill_pid > 0 )
        kill( -kill_pid, sig );
    fatal_signal = true;
    if ( sig == SIGINT )
        interrupt = true;
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

}

/* TODO remove in favour of brick-commandline.h */
struct Args {
    typedef std::vector< std::string > V;
    V args;

    Args( int argc, const char **argv ) {
        for ( int i = 1; i < argc; ++ i )
            args.push_back( argv[ i ] );
    }

    bool has( std::string fl ) {
        return std::find( args.begin(), args.end(), fl ) != args.end();
    }

    // TODO: This does not handle `--option=VALUE`:
    std::string opt( std::string fl ) {
        V::iterator i = std::find( args.begin(), args.end(), fl );
        if ( i == args.end() || i + 1 == args.end() )
            return "";
        return *(i + 1);
    }
};

namespace {

const char* hasenv( const char *name ) {
    const char *v = getenv( name );
    if ( !v )
        return NULL;
    if ( strlen( v ) == 0 || !strcmp( v, "0" ) )
        return NULL;
    return v;
}

template< typename C >
void split( std::string s, C &c ) {
    std::stringstream ss( s );
    std::string item;
    while ( std::getline( ss, item, ',' ) )
        c.push_back( item );
}

}

const char *DEF_FLAVOURS="ndev-vanilla";

std::string resolve_path(std::string a_path, const char *default_path=".")
{
    char temp[PATH_MAX];
    const char *p;
    p = a_path.empty() ? default_path : a_path.c_str();
    if ( !realpath( p, temp ) )
        throw syserr( "Failed to resolve path", p );
    return temp;
}

static int run( int argc, const char **argv, std::string fl_envvar = "TEST_FLAVOUR" )
{
    Args args( argc, argv );
    Options opt;
    const char *env;

    if ( args.has( "--help" ) ) {
        std::cout <<
            "  lvm2-testsuite - Run a lvm2 testsuite.\n\n"
            "lvm2-testsuite"
            "\n\t"
            " [--flavours FLAVOURS]"
            " [--only TESTS]"
            "\n\t"
            " [--outdir OUTDIR]"
            " [--testdir TESTDIR]"
            " [--workdir WORKDIR]"
            "\n\t"
            " [--batch|--verbose|--interactive]"
            "\n\t"
            " [--fatal-timeouts]"
            " [--continue]"
            " [--heartbeat]"
            " [--watch WATCH]"
            " [--timeout TIMEOUT]"
            " [--nokmsg]\n\n"
            /* TODO: list of flavours:
            "lvm2-testsuite"
            "\n\t"
            " --list-flavours [--testdir TESTDIR]"
            */
            "\n\n"
            "OPTIONS:\n\n"
            // TODO: looks like this could be worth a man page...
            "Filters:\n"
            "  --flavours FLAVOURS\n\t\t- comma separated list of flavours to run.\n\t\t  For the list of flavours see `$TESTDIR/lib/flavour-*`.\n\t\t  Default: \"" << DEF_FLAVOURS << "\".\n"
            "  --only TESTS\t- comma separated list of tests to run. Default: All tests.\n"
            "\n"
            "Directories:\n"
            "  --testdir TESTDIR\n\t\t- directory where tests reside. Default: \"" TESTSUITE_DATA "\".\n"
            "  --workdir WORKDIR\n\t\t- directory to change to when running tests.\n\t\t  This is directory containing testing libs. Default: TESTDIR.\n"
            "  --outdir OUTDIR\n\t\t- directory where all the output files should go. Default: \".\".\n"
            "\n"
            "Formatting:\n"
            "  --batch\t- Brief format for automated runs.\n"
            "  --verbose\t- More verbose format for automated runs displaying progress on stdout.\n"
            "  --interactive\t- Verbose format for interactive runs.\n"
            "\n"
            "Other:\n"
            "  --fatal-timeouts\n\t\t- exit after encountering 2 timeouts in a row.\n"
            "  --continue\t- If set append to journal. Otherwise it will be overwritten.\n"
            "  --heartbeat HEARTBEAT\n\t\t- Name of file to update periodically while running.\n"
            "  --watch WATCH\t- Comma separated list of files to watch and print.\n"
            "  --timeout TIMEOUT\n\t\t- Period of silence in seconds considered a timeout. Default: 180.\n"
            "  --nokmsg\t- Do not try to read kernel messages.\n"
            "\n\n"
            "ENV.VARIABLES:\n\n"
            "  T\t\t- see --only\n"
            "  INTERACTIVE\t- see --interactive\n"
            "  VERBOSE\t- see --verbose\n"
            "  BATCH\t\t- see --batch\n"
            "  LVM_TEST_CAN_CLOBBER_DMESG\n\t\t- when set and non-empty tests are allowed to flush\n\t\t  kmsg in an attempt to read it."
            "\n\n"
            "FORMATS:\n\n"
            "When multiple formats are specified interactive overrides verbose\n"
            "which overrides batch. Command line options override environment\n"
            "variables.\n\n"
            ;
        return 0;
    }

    opt.flavour_envvar = fl_envvar;

    if ( args.has( "--continue" ) )
        opt.cont = true;

    if ( args.has( "--only" ) )
        split( args.opt( "--only" ), opt.filter );
    else if ( ( env = hasenv( "T" ) ) )
        split( env, opt.filter );

    if ( args.has( "--skip" ) )
        split( args.opt( "--skip" ), opt.skip );
    else if ( ( env = hasenv( "S" ) ) )
        split( env, opt.skip );

    if ( args.has( "--fatal-timeouts" ) )
        opt.fatal_timeouts = true;

    if ( args.has( "--heartbeat" ) )
        opt.heartbeat = args.opt( "--heartbeat" );

    if ( args.has( "--batch" ) || args.has( "--verbose" ) || args.has( "--interactive" ) ) {
        if ( args.has( "--batch" ) ) {
            opt.verbose = false;
            opt.batch = true;
        }

        if ( args.has( "--verbose" ) ) {
            opt.batch = false;
            opt.verbose = true;
        }

        if ( args.has( "--interactive" ) ) {
            opt.verbose = false;
            opt.batch = false;
            opt.interactive = true;
        }
    } else {
        if ( hasenv( "BATCH" ) ) {
            opt.verbose = false;
            opt.batch = true;
        }

        if ( hasenv( "VERBOSE" ) ) {
            opt.batch = false;
            opt.verbose = true;
        }

        if ( hasenv( "INTERACTIVE" ) ) {
            opt.verbose = false;
            opt.batch = false;
            opt.interactive = true;
        }
    }

    if ( args.has( "--flavours" ) )
        split( args.opt( "--flavours" ), opt.flavours );
    else
        split( DEF_FLAVOURS, opt.flavours );

    if ( args.has( "--watch" ) )
        split( args.opt( "--watch" ), opt.watch );

    if ( args.has( "--timeout" ) )
        opt.timeout = atoi( args.opt( "--timeout" ).c_str() );

    if ( args.has( "--nokmsg" ) )
        opt.kmsg = false;

    opt.testdir = resolve_path( args.opt( "--testdir" ), TESTSUITE_DATA ) + "/";
    opt.workdir = resolve_path( args.opt( "--workdir" ), opt.testdir.c_str() );
    opt.outdir = resolve_path( args.opt( "--outdir" ), "." );

    setup_handlers();

    Main main( opt );
    return main.run();
}

}
}

#endif

#ifdef BRICK_DEMO

int main( int argc, const char **argv ) {
    return brick::shelltest::run( argc, argv );
}

#endif

// vim: syntax=cpp tabstop=4 shiftwidth=4 expandtab
