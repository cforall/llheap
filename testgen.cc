#include <string>										// stoi
using namespace std;
// Use C I/O because cout does not provide a good mechanism for thread-safe I/O.
#include <stdlib.h>										// abort, getenv, EXIT_FAILURE
#include <errno.h>										// errno
#include <string.h>										// strerror
#include <malloc.h>										// malloc_stats
#include <locale.h>										// setlocale
#include <unistd.h>										// sleep
#include <pthread.h>
#include "affinity.h"

// llheap only
extern "C" size_t malloc_unfreed() { return 5979; }		// printf(1024)/setlocale(4043)/pthread(3*304)

timespec currTime() {
	timespec t;											// nanoseconds since UNIX epoch
	if ( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &t ) == -1 ) {
		fprintf( stderr, "internal error, clock failed %d %s\n", errno, strerror( errno ) );
	} // if
	return t;
} // currTime

inline double dur( timespec end, timespec start ) {
	long int sec = end.tv_sec - start.tv_sec, nsec = end.tv_nsec - start.tv_nsec;
	return sec + nsec * 1E-9;
} // dur

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

pthread_barrier_t barrier;


void * worker( void * ) {
	enum { TIMES = 500'000'000, TIMES2 = TIMES / 1000, FIXED = 42, FIXED2 = 1 * 1024 * 1024, GROUP1 = 100, GROUP2 = 1000 };
	timespec start;
	int * ip;
	int * ips1[GROUP1], * ips2[GROUP2];

	// sbrk storage
	printf( "sbrk area %'d times\n", TIMES );

#if 1
	// malloc/free 0/null pointer
	start = currTime();
	for ( int i = 0; i < TIMES; i += 1 ) {
		void * vp = pass( malloc( 0 ) );				// warning: insufficient size '0' for type 'int'
		free( vp );
	} // for
	printf( "x = malloc( 0 )/free( x )\t\t\t\t%7.3f seconds\n", dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );

	// free null pointer (CANNOT BE FIRST TEST BECAUSE HEAP IS NOT INITIALIZED => HIGH COST)
	ip = nullptr;
	start = currTime();
	for ( int i = 0; i < TIMES; i += 1 ) {
		free( pass( ip ) );
	} // for
	printf( "free( nullptr )\t\t\t\t\t\t%7.3f seconds\n", dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );

	// alternating malloc/free FIXED bytes
	start = currTime();
	for ( int i = 0; i < TIMES; i += 1 ) {
		ip = (int *)pass( malloc( FIXED ) );
		free( ip );
	} // for
	printf( "alternating malloc/free %'d bytes\t\t\t%7.3f seconds\n", FIXED, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED bytes

	start = currTime();
	for ( int i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( FIXED ) );
		} // for
		for ( int g = 0; g < GROUP1; g += 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "group %'d malloc/free %'d bytes\t\t\t\t%7.3f seconds\n", GROUP1, FIXED, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED bytes

	start = currTime();
	for ( int i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( FIXED ) );
		} // for
		for ( int g = 0; g < GROUP2; g += 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "group %'d malloc/free %'d bytes\t\t\t%7.3f seconds\n", GROUP2, FIXED, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( FIXED ) );
		} // for
		for ( int g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "group %'d malloc/reverse-free %'d bytes\t\t\t%7.3f seconds\n", GROUP1, FIXED, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( FIXED ) );
		} // for
		for ( int g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "group %'d malloc/reverse-free %'d bytes\t\t%7.3f seconds\n", GROUP2, FIXED, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// alternating malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ip = (int *)pass( malloc( g ) );
			free( ip );
		} // for
	} // for
	printf( "alternating malloc/free 1-%'d bytes\t\t\t%7.3f seconds\n", GROUP1, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP1 bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( g ) );
		} // for
		for ( int g = 0; g < GROUP1; g += 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "group %'d malloc/free 1-%'d bytes\t\t\t%7.3f seconds\n", GROUP1, GROUP1, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free 1-GROUP2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( g ) );
		} // for
		for ( int g = 0; g < GROUP2; g += 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "group %'d malloc/free 1-%'d bytes\t\t\t%7.3f seconds\n", GROUP2, GROUP2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP1 bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( g ) );
		} // for
		for ( int g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "group %'d malloc/reverse-free 1-%'d bytes\t\t%7.3f seconds\n", GROUP1, GROUP1, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free 1-GROUP2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( g ) );
		} // for
		for ( int g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "group %'d malloc/reverse-free 1-%'d bytes\t\t%7.3f seconds\n", GROUP2, GROUP2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );
#endif // 0

#if 1
	// mmap storage

	printf( "mmap area %'d time\n", TIMES2 );

	// alternating malloc/free FIXED2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES2; i += 1 ) {
		ip = (int *)pass( malloc( FIXED2 ) );
		free( ip );
	} // for
	printf( "mmap alternating malloc/free %'d bytes\t\t%7.3f seconds\n", FIXED2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES2 / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( FIXED2 ) );
		} // for
		for ( int g = 0; g < GROUP1; g += 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "mmap group %'d malloc/free %'d bytes\t\t%7.3f seconds\n", GROUP1, FIXED2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/free FIXED2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES2 / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( FIXED2 ) );
		} // for
		for ( int g = 0; g < GROUP2; g += 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "mmap group %'d malloc/free %'d bytes\t\t%7.3f seconds\n", GROUP2, FIXED2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES2 / GROUP1; i += 1 ) {
		for ( int g = 0; g < GROUP1; g += 1 ) {
			ips1[g] = (int *)pass( malloc( FIXED2 ) );
		} // for
		for ( int g = GROUP1 - 1; g >= 0; g -= 1 ) {
			free( ips1[g] );
		} // for
	} // for
	printf( "mmap group %'d malloc/reverse-free %'d bytes \t%7.3f seconds\n", GROUP1, FIXED2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );


	// group malloc/reverse-free FIXED2 bytes
	start = currTime();
	for ( int i = 0; i < TIMES2 / GROUP2; i += 1 ) {
		for ( int g = 0; g < GROUP2; g += 1 ) {
			ips2[g] = (int *)pass( malloc( FIXED2 ) );
		} // for
		for ( int g = GROUP2 - 1; g >= 0; g -= 1 ) {
			free( ips2[g] );
		} // for
	} // for
	printf( "mmap group %'d malloc/reverse-free %'d bytes \t%7.3f seconds\n", GROUP2, FIXED2, dur( currTime(), start ) );
	pthread_barrier_wait( &barrier );
#endif // 0
	return nullptr;
} // worker

int main( int argc, char * argv[] ) {
	setlocale( LC_NUMERIC, getenv( "LANG" ) );			// separators in numbers
	if ( mallopt( M_MMAP_THRESHOLD, 512 * 1024 ) == 0 ) { // smaller mmap crossover
		printf( "M_MMAP_THRESHOLD unsupported\n" );
	} // if

#if 1
	int Threads = 4;

	try {
		switch( argc ) {
		  case 2:
			Threads = stoi( argv[1] ); if ( Threads <= 0 ) throw 1;
		} // switch
	} catch( ... ) {
		printf( "Usage: %s [ threads (>0) ]\n", argv[0] );
		exit( EXIT_FAILURE );
	} // try
	printf( "Number of Threads: %d\n\n", Threads );

	cpu_set_t mask;
	affinity( 0, mask );
	if ( pthread_setaffinity_np( pthread_self(), sizeof(cpu_set_t), &mask ) ) abort();

	if ( pthread_barrier_init( &barrier, nullptr, Threads ) ) abort();

	pthread_t thread[Threads];							// thread[0] unused
	for ( int tid = 1; tid < Threads; tid += 1 ) {		// N - 1 thread
		if ( pthread_create( &thread[tid], NULL, worker, NULL) < 0 ) abort();
		affinity( tid, mask );
		if ( pthread_setaffinity_np( thread[tid], sizeof(cpu_set_t), &mask ) ) abort();
	} // for
	
	worker( nullptr );									// initialize thread runs one test

	for ( int tid = 1; tid < Threads; tid += 1 ) {
		if ( pthread_join( thread[tid], NULL ) < 0 ) abort();
	} // for

	if ( pthread_barrier_destroy( &barrier ) ) abort();
#else
	worker( nullptr );
#endif // 0

	// malloc_stats();
} // main

// repeat 3 \time -f "%Uu %Ss %Er %Mkb" a.out

// g++-10 -Wall -Wextra -g -O3 -D`hostname` testgen.cc libllheap.so -lpthread -Wl,-rpath=/u/pabuhr/software/llheap -L/u/pabuhr/software/llheap

// Local Variables: //
// compile-command: "g++-10 -Wall -Wextra -g -O3 -D`hostname` testgen.cc libllheap.o -lpthread" //
// End: //
