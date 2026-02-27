#include <string>										// stoi
using namespace std;
// Use C I/O because cout does not provide a good mechanism for thread-safe I/O.
#include <cstdlib>										// abort, getenv, EXIT_FAILURE
#include <cerrno>										// errno
#include <cstring>										// strerror
#include <malloc.h>										// malloc_stats
#include <cmath>										// sqrt
#include <cassert>										// assert
#include <cstdint>										// uintptr_t
#include <ctime>										// clock
#include <sys/time.h>									// gettimeofday
#include <sys/resource.h>								// getrusage
#include <pthread.h>

#define str( s ) #s
#define xstr(s) str(s)

// HYPERAFF => use hyperthreads and fill in pairs of processors on a socket => 129,384, 129,385, ...
//#define HYPERAFF

// LINEARAFF => do not use hyperthreading and fill cores on a socket => 129, 130, 131, 132, ...
#define LINEARAFF
#include "affinity.h"

static timespec currTime() {
	timespec t;											// nanoseconds since UNIX epoch
	if ( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &t ) == -1 ) {
		fprintf( stderr, "internal error, clock failed %d %s\n", errno, strerror( errno ) );
	} // if
	return t;
} // currTime

static inline double dur( timespec end, timespec start ) {
	long int sec = end.tv_sec - start.tv_sec, nsec = end.tv_nsec - start.tv_nsec;
	return sec + nsec * 1E-9;
} // dur

static inline double dur( timeval end, timeval start ) {
	long int sec = end.tv_sec - start.tv_sec, msec = end.tv_usec - start.tv_usec;
	return sec + msec * 1E-6;
} // dur

template< typename T > static inline T pass( T v ) {	// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

static pthread_barrier_t barrier;

enum : uint64_t { TIMES = 1'000'000'000, TIMES2 = TIMES / 5'000 };
#define FIXED 42
#define FIXED2 1'048'576
#define GROUP1 100
#define GROUP2 1000

static double ** eresults;								// store experimental results

//#define RANDOM
#ifdef RANDOM
int rgroup1[GROUP1], rgroup2[GROUP2];
#endif // RANDOM

#define PRINT_DOTS
#ifdef PRINT_DOTS
#define DOTS() if ( tid == 0 ) { printf( "." ); fflush( stdout ); }
#else
#define DOTS()
#endif // PRINT


#define MALLOC
//#define MMAP

static const char * titles[] = {
	#ifdef MALLOC
	"x = malloc( 0 )/free( x )\t\t\t\t",
	"free( nullptr )\t\t\t\t\t\t",
	"alternate malloc/free " xstr(FIXED) " bytes\t\t\t\t",
	"group " xstr(GROUP1) " malloc/free " xstr(FIXED) " bytes\t\t\t\t",
	"group " xstr(GROUP2) " malloc/free " xstr(FIXED) " bytes\t\t\t\t",
	"group " xstr(GROUP1) " malloc/reverse-free " xstr(FIXED) " bytes\t\t\t",
	"group " xstr(GROUP2) " malloc/reverse-free " xstr(FIXED) " bytes\t\t\t",
	"alternate malloc/free 1-" xstr(GROUP1) " bytes\t\t\t",
	"group " xstr(GROUP1) " malloc/free 1-"  xstr(GROUP1) " bytes\t\t\t",
	"group " xstr(GROUP2) " malloc/free 1-"  xstr(GROUP2) " bytes\t\t\t",
	"group " xstr(GROUP1) " malloc/reverse-free 1-"  xstr(GROUP1) " bytes\t\t",
	"group " xstr(GROUP2) " malloc/reverse-free 1-"  xstr(GROUP2) " bytes\t\t",
	#ifdef RANDOM
	"ralternate malloc/free 1-" xstr(GROUP1) " bytes\t\t\t",
	"rgroup " xstr(GROUP1) " malloc/free 1-"  xstr(GROUP1) " bytes\t\t\t",
	"rgroup " xstr(GROUP2) " malloc/free 1-"  xstr(GROUP2) " bytes\t\t\t",
	"rgroup " xstr(GROUP1) " malloc/reverse-free 1-"  xstr(GROUP1) " bytes\t\t",
	"rgroup " xstr(GROUP2) " malloc/reverse-free 1-"  xstr(GROUP2) " bytes\t\t",
	#endif // RANDOM
	#endif // MALLOC
	#ifdef MMAP
	"mmap alternate malloc/free " xstr(FIXED2) " bytes\t\t",
	"mmap group " xstr(GROUP1) " malloc/free " xstr(FIXED2) " bytes\t\t",
	"mmap group " xstr(GROUP2) " malloc/free " xstr(FIXED2) " bytes\t\t",
	"mmap group " xstr(GROUP1) " malloc/reverse-free " xstr(FIXED2) " bytes\t",
	"mmap group " xstr(GROUP2) " malloc/reverse-free " xstr(FIXED2) " bytes\t",
	#endif // MMAP
};
enum { EXPERIMENTS = sizeof( titles ) / sizeof( titles[0] ) };


timeval puser = { 0, 0 }, psys = { 0, 0 };


static void * worker( void * arg ) {
	uintptr_t tid = (uintptr_t)arg;						// thread id
	timespec start;
	double etime;
	char * cp;
	char * cps1[GROUP1], * cps2[GROUP2];
	unsigned int exp = 0;
	struct rusage rnow;
	struct timeval tbegin, tnow;						// there is no real time in getrusage

	// sbrk storage

#ifdef MALLOC
	gettimeofday( &tbegin, 0 );

	// malloc/free 0/null pointer
	start = currTime();
	for ( uint64_t i = 0; i < TIMES; i += 1 ) {
		char * cp = (char *)pass( malloc( 0 ) );
		assert( cp );
		free( cp );
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );

	// free null pointer (CANNOT BE FIRST TEST BECAUSE HEAP IS NOT INITIALIZED => HIGH COST)
	cp = nullptr;
	start = currTime();
	for ( uint64_t i = 0; i < TIMES; i += 1 ) {
		free( pass( cp ) );
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );

	// alternate malloc/free FIXED bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES; i += 1 ) {
		cp = (char *)pass( malloc( FIXED ) );
		assert( cp );
		cp[0] = cp[FIXED - 1] = 'a';					// touch ends
		free( cp );
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( FIXED ) );
			assert( cps1[g] );
			cps1[g][0] = cps1[g][FIXED - 1] = 'a';		// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( FIXED ) );
			assert( cps2[g] );
			cps2[g][0] = cps2[g][FIXED - 1] = 'a';		// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( FIXED ) );
			assert( cps1[g] );
			cps1[g][0] = cps1[g][FIXED - 1] = 'a';		// touch ends
		} // for
		for ( int64_t g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( FIXED ) );
			assert( cps2[g] );
			cps2[g][0] = cps2[g][FIXED - 1] = 'a';		// touch ends
		} // for
		for ( int64_t g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// alternate malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cp = (char *)pass( malloc( g ) );
			assert( cp );
			if ( g ) cp[0] = cp[g - 1] = 'a';			// touch ends
			free( cp );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( g ) );
			assert( cps1[g] );
			if ( g ) cps1[g][0] = cps1[g][g - 1] = 'a';	// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( g ) );
			assert( cps2[g] );
			if ( g ) cps2[g][0] = cps2[g][g - 1] = 'a';	// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( g ) );
			assert( cps1[g] );
			if ( g ) cps1[g][0] = cps1[g][g - 1] = 'a';	// touch ends
		} // for
		for ( int64_t g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( g ) );
			assert( cps2[g] );
			if ( g )cps2[g][0] = cps2[g][g - 1] = 'a';	// touch ends
		} // for
		for ( int64_t g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	#ifdef RANDOM
	// alternate malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cp = (char *)pass( malloc( rgroup1[g] ) );
			assert( cp );
			if ( rgroup1[g] ) cp[0] = cp[rgroup1[g] - 1] = 'a';	// touch ends
			free( cp );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( rgroup1[g] ) );
			assert( cps1[g] );
			if ( rgroup1[g] ) cps1[g][0] = cps1[g][rgroup1[g] - 1] = 'a'; // touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( rgroup2[g] ) );
			assert( cps2[g] );
			if ( rgroup1[g] ) cps2[g][0] = cps2[g][rgroup2[g] - 1] = 'a'; // touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP1 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( rgroup1[g] ) );
			assert( cps1[g] );
			if ( rgroup1[g] ) cps1[g][0] = cps1[g][rgroup1[g] - 1] = 'a'; // touch ends
		} // for
		for ( uint64_t g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( rgroup2[g] ) );
			assert( cps2[g] );
			if ( rgroup1[g] ) cps2[g][0] = cps2[g][rgroup2[g] - 1] = 'a'; // touch ends
		} // for
		for ( uint64_t g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );
	#endif // RANDOM

	gettimeofday( &tnow, 0 );
	getrusage( RUSAGE_SELF, &rnow );
	if ( tid == 0 ) {
		printf( "\n%.2fu %.2fs %.2fr %ldkb\n",
				dur( rnow.ru_utime, puser ), dur( rnow.ru_stime, psys ), dur( tnow, tbegin ), rnow.ru_maxrss );
		puser = rnow.ru_utime;  psys = rnow.ru_stime;	// update
	} // if
#endif // MALLOC

#ifdef MMAP
	// mmap storage

	gettimeofday( &tbegin, 0 );

	// alternate malloc/free FIXED2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES2; i += 1 ) {
		cp = (char *)pass( malloc( FIXED2 ) );
		assert( cp );
		cp[0] = cp[FIXED2 - 1] = 'a';					// touch ends
		free( cp );
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES2 / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( FIXED2 ) );
			assert( cps1[g] );
			cps1[g][0] = cps1[g][FIXED2 - 1] = 'a';		// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES2 / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( FIXED2 ) );
			assert( cps2[g] );
			cps2[g][0] = cps2[g][FIXED2 - 1] = 'a';		// touch ends
		} // for
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES2 / GROUP1; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP1; g += 1 ) {
			cps1[g] = (char *)pass( malloc( FIXED2 ) );
			assert( cps1[g] );
			cps1[g][0] = cps1[g][FIXED2 - 1] = 'a';		// touch ends
		} // for
		for ( int64_t g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( cps1[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED2 bytes
	start = currTime();
	for ( uint64_t i = 0; i < TIMES2 / GROUP2; i += 1 ) {
		for ( uint64_t g = 0; g < GROUP2; g += 1 ) {
			cps2[g] = (char *)pass( malloc( FIXED2 ) );
			assert( cps2[g] );
			cps2[g][0] = cps2[g][FIXED2 - 1] = 'a';		// touch ends
		} // for
		for ( int64_t g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( cps2[g] );
		} // for
	} // for
	etime = dur( currTime(), start );
	eresults[exp++][tid] = etime;
	DOTS();
	pthread_barrier_wait( &barrier );

	gettimeofday( &tnow, 0 );
	getrusage( RUSAGE_SELF, &rnow );
	if ( tid == 0 ) {
		printf( "\n%.2fu %.2fs %.2fr %ldkb\n",
				dur( rnow.ru_utime, puser ), dur( rnow.ru_stime, psys ), dur( tnow, tbegin ), rnow.ru_maxrss );
		puser = rnow.ru_utime;  psys = rnow.ru_stime;	// update
	} // if
#endif // MMAP
	return nullptr;
} // worker


static void statistics( size_t N, double values[], double * avg, double * std, double * rstd ) {
	double sum = 0.;
	for ( size_t r = 0; r < N; r += 1 ) {
		sum += values[r];
	} // for
	*avg = sum / N;										// average
	sum = 0.;
	for ( size_t r = 0; r < N; r += 1 ) {				// sum squared differences from average
		double diff = values[r] - *avg;
		sum += diff * diff;
	} // for
	*std = sqrt( sum / N );
	*rstd = *avg == 0.0 ? 0.0 : *std / *avg * 100;
} // statisitics


// llheap only
extern "C" size_t malloc_unfreed() { return 6324; }		// printf(1024)/setlocale(4000)/pthread(1300)

int main() {
	setlocale( LC_NUMERIC, getenv( "LANG" ) );			// separators in numbers
	#ifdef MMAP
	if ( mallopt( M_MMAP_THRESHOLD, 512 * 1024 ) == 0 ) { // smaller mmap crossover
		printf( "M_MMAP_THRESHOLD unsupported\n" );
	} // if
	#endif //  MMAP

	#ifdef RANDOM
	for ( uint64_t i = 0; i < GROUP1; i += 1 ) {
		rgroup1[i] = i;
	} // for
	for ( uint64_t i = 0; i < GROUP1; i += 1 ) {
		swap( rgroup1[0], rgroup1[rand() % GROUP1] );	// randomize
	} // for
	for ( uint64_t i = 0; i < GROUP2; i += 1 ) {
		rgroup2[i] = i;
	} // for
	for ( uint64_t i = 0; i < GROUP2; i += 1 ) {
		swap( rgroup2[0], rgroup2[rand() % GROUP1] );	// randomize
	} // for
	#endif // RANDOM

	#if defined( plg2 )
	unsigned int THREADS[] = { 4 };
	#else
	unsigned int THREADS[] = { 4, 8, 16, 32 };
	#endif // plg2
	enum { threads = sizeof( THREADS ) / sizeof( THREADS[0] ) };

	eresults = (double **)malloc( sizeof( double *[EXPERIMENTS] ) );
	for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
		eresults[e] = (double *)malloc( sizeof( double[THREADS[threads - 1]] ) );
	} // for

	// Allocate largest case thread table.
	double *** tresults = (double ***)malloc( sizeof( double **[THREADS[threads - 1]] ) );
	for ( unsigned int t = 0; t < THREADS[threads - 1]; t += 1 ) {
		tresults[t] = (double **)malloc( sizeof( double *[EXPERIMENTS] ) );
		for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
			tresults[t][e] = (double *)malloc( sizeof( double[3] ) );
		} // for
	} // for

//	#if defined( HYPERAFF )
//	printf( "HYPERAFF affinity\n" );
//	#elif defined( LINEARAFF )
//	printf( "LINEARAFF affinity\n" );
//	#else
//		#error no affinity specified
//	#endif
	printf( "sbrk area %lu times\n", TIMES );
	#ifdef MMAP
	printf( "mmap area %lu time\n\n", TIMES2 );
	#endif // MMAP

//	affinity( pthread_self(), 0 );
	
	for ( unsigned int t = 0; t < threads; t += 1 ) {
		printf( "Number of threads: %d\n", THREADS[t] );

		if ( pthread_barrier_init( &barrier, nullptr, THREADS[t] ) ) abort();

		pthread_t thread[THREADS[t]];					// thread[0] unused
		for ( uintptr_t tid = 1; tid < THREADS[t]; tid += 1 ) { // N - 1 thread
			if ( pthread_create( &thread[tid], nullptr, worker, (void *)tid ) < 0 ) abort();
//			affinity( thread[tid], tid );
		} // for
	
		worker( 0 );									// initialize thread runs one test

		for ( unsigned int tid = 1; tid < THREADS[t]; tid += 1 ) {
			if ( pthread_join( thread[tid], nullptr ) < 0 ) abort();
		} // for

		if ( pthread_barrier_destroy( &barrier ) ) abort();

		double avg = 0.0, std = 0.0, rstd;
		//#define PRINT_THREAD
		#ifdef PRINT_THREAD
		printf( "\t\t\t\t\t\t\t " );
		char buf[64];
		for ( unsigned int tid = 0; tid < THREADS[t]; tid += 1 ) {
			sprintf( buf, "T%d", tid );
			printf( "%7s ", buf );
		} // for
		printf( " :     avg   std rstd\n" );
		#endif // PRINT_THREAD

		for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
			#ifdef PRINT_THREAD
			printf( "%s ", titles[e] );
			for ( unsigned int tid = 0; tid < THREADS[t]; tid += 1 ) {
				printf( "%7.2f ", eresults[e][tid] );
			} // for
			printf( " : " );
			#endif // PRINT_THREAD

			statistics( THREADS[t], eresults[e], &avg, &std, &rstd );
			tresults[t][e][0] = avg; tresults[t][e][1] = std; tresults[t][e][2] = rstd;

			#ifdef PRINT_THREAD
			printf( "%7.2f %5.2f %3.f%%\n", avg, std, rstd );
			#endif // PRINT_THREAD
		} // for
	} // for

	printf( "\t\t\t\t\t\t\t " );
	for ( unsigned int t = 0; t < threads; t += 1 ) {
		printf( "      %5u      ", THREADS[t] );
	} // for
	printf( "\n\t\t\t\t\t\t\t " );
	for ( unsigned int t = 0; t < threads; t += 1 ) {
		printf( "    avg   std rstd" );
	} // for
	printf( "\n" );
	for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
		printf( "%s ", titles[e] );
		for ( unsigned int t = 0; t < threads; t += 1 ) {
				printf( "%7.2f %5.2f %3.f%%", tresults[t][e][0], tresults[t][e][1], tresults[t][e][2] );
		} // for
		printf( "\n" );
	} // for

	for ( unsigned int t = 0; t < THREADS[threads - 1]; t += 1 ) {
		for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
			free( tresults[t][e] );
		} // for
		free( tresults[t] );
	} // for
	free( tresults );

	for ( unsigned int e = 0; e < EXPERIMENTS; e += 1 ) {
		free( eresults[e] );
	} // for
	free( eresults );
	// malloc_stats();
} // main

// repeat 3 \time -f "%Uu %Ss %er %Mkb" a.out

// g++-14 -Wall -Wextra -g -O3 -D`hostname` latency.cc -lpthread ${HOME}/software/allocators/libllheap.so -Wl,-rpath=${HOME}/software/allocators

// Local Variables: //
// compile-command: "g++-14 -Wall -Wextra -g -O3 -D`hostname` latency.cc -lpthread libllheap.o" //
// End: //
