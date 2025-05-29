#include <malloc.h>
#include <pthread.h>
#include <sys/time.h>									// gettimeofday
#include <sys/resource.h>								// getrusage
#include <string.h>										// strerror

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

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

enum { ASIZE = 50 }; // 5

void * worker( void * arg ) {
	volatile size_t * arr = (size_t *)arg;
	for ( size_t t = 0; t < 10'000'000'000 / ASIZE; t += 1 ) {
		for ( size_t r = 0; r < ASIZE; r += 1 ) {
			arr[r] += r;								// reads and writes
		} // for
	} // for
	free( (void *)arr );
	return nullptr;
} // worker

int main() {
	#if defined( HYPERAFF )
	printf( "HYPERAFF affinity\n" );
	#elif defined( LINEARAFF )
	printf( "LINEARAFF affinity\n" );
	#else
		#error no affinity specified
	#endif
	#if defined( plg2 )
	unsigned int THREADS[] = { 4 };
	#else
	unsigned int THREADS[] = { 4, 8, 16, 32 };
	#endif // plg2
	enum { threads = sizeof( THREADS ) / sizeof( THREADS[0] ) };

	affinity( pthread_self(), 0 );
	
	struct rusage rnow;
	struct timeval tbegin, tnow;						// there is no real time in getrusage
	timespec start;
	timeval puser = { 0, 0 }, psys = { 0, 0 };
	gettimeofday( &tbegin, 0 );

	for ( unsigned int t = 0; t < threads; t += 1 ) {
		printf( "Number of threads: %d\n", THREADS[t] );

		gettimeofday( &tnow, 0 );
		getrusage( RUSAGE_SELF, &rnow );
		start = currTime();

		pthread_t thread[THREADS[t]];					// thread[0] unused
		for ( ssize_t tid = 1; tid < THREADS[t]; tid += 1 ) { // N - 1 thread
			if ( pthread_create( &thread[tid], NULL, worker, pass( malloc( sizeof(size_t) * ASIZE ) ) ) < 0 ) abort();
			affinity( thread[tid], tid );
		} // for
	
		worker( pass( malloc( sizeof(size_t) * ASIZE ) ) );	// initialize thread runs one test

		for ( unsigned int tid = 1; tid < THREADS[t]; tid += 1 ) {
			if ( pthread_join( thread[tid], NULL ) < 0 ) abort();
		} // for

		gettimeofday( &tnow, 0 );
		getrusage( RUSAGE_SELF, &rnow );
		printf( "%.2fu %.2fs %.2fr %ldkb\n",
				dur( rnow.ru_utime, puser ), dur( rnow.ru_stime, psys ), dur( currTime(), start ), rnow.ru_maxrss );
		puser = rnow.ru_utime;  psys = rnow.ru_stime;	// update
	} // for
} // main

// Local Variables: //
// compile-command: "g++-11 -Wall -Wextra -g -O3 cache.cc -pthread" //
// End: //
