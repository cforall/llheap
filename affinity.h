#include <stdio.h>										// perror
#include <pthread.h>
#include <stdlib.h>										// exit, EXIT_FAILURE
#include <errno.h>										// errno

void affinity( pthread_t pthreadid, unsigned int tid ) {
// There are many ways to assign threads to processors: cores, chips, etc.  Below are some alternative approaches.

//#define LINEARAFF
#if ! defined( HYPERAFF ) && ! defined( LINEARAFF )		// default affinity
#define HYPERAFF
#endif // HYPERAFF

#if defined( swift )
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 128, HYPER = 1 };
	#if defined( LINEARAFF )
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	#endif // LINEARAFF
	#if defined( HYPERAFF )
	int cpu = OFFSETSOCK * CORES + (tid / 2) + ((tid % 2 == 0) ? 0 : CORES * SOCKETS);
	#endif // HYPERAFF
#elif defined( java )
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 2, CORES = 32, HYPER = 1 /* wrap on socket */ };
	#if defined( LINEARAFF )
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	#endif // LINEARAFF
	#if defined( HYPERAFF )
	int cpu = OFFSETSOCK * CORES + (tid / 2) + ((tid % 2 == 0) ? 0 : CORES * SOCKETS );
	#endif // HYPERAFF
#elif defined( nasus )
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 64, HYPER = 1 };
	#if defined( LINEARAFF )
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	#endif // LINEARAFF
	#if defined( HYPERAFF )
	int cpu = OFFSETSOCK * CORES + (tid / 2) + ((tid % 2 == 0) ? 0 : CORES * SOCKETS);
	#endif // HYPERAFF
#elif defined( pyke )
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 2, CORES = 24, HYPER = 1 /* wrap on socket */ };
	#if defined( LINEARAFF )
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	#endif // LINEARAFF
	#if defined( HYPERAFF )
	int cpu = OFFSETSOCK * CORES + (tid / 2) + ((tid % 2 == 0) ? 0 : CORES * SOCKETS );
	#endif // HYPERAFF
#elif defined( jax )
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 4, CORES = 24, HYPER = 2 /* wrap on socket */ };
	#if defined( LINEARAFF )
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	#endif // LINEARAFF
	#if defined( HYPERAFF )
	int cpu = OFFSETSOCK * CORES + (tid / 2) + ((tid % 2 == 0) ? 0 : CORES * SOCKETS );
	#endif // HYPERAFF
#else
	// HYPERAFF unsupported for these architectures.
	#define LINEARAFF

#if defined( plg2 )									// old AMD
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 16, HYPER = 1 };
	tid *= 8; // seperate caches
#elif defined( algol )								// ARM
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 2, CORES = 48, HYPER = 1 };
#elif defined( prolog )								// ARM
	// enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 64, HYPER = 1 }; // pretend 2 sockets
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 128, HYPER = 1 };
#elif defined( cfapi1 )								// raspberrypi
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 4, HYPER = 1 };
#else // default
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 8, HYPER = 1 };
#endif // HOSTS
	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
#endif // computer

#if 0
	// 4x8x2 : 4 sockets, 8 cores per socket, 2 hyperthreads per core
	int cpu = (tid & 0x30) | ((tid & 1) << 3) | ((tid & 0xE) >> 1) + 32;
#endif // 0
	// printf( "%d ", cpu );

	cpu_set_t mask;
	CPU_ZERO( &mask );
	CPU_SET( cpu, &mask );
	int rc = pthread_setaffinity_np( pthreadid, sizeof(cpu_set_t), &mask );
	if ( rc != 0 ) {
		errno = rc;
		char buf[64];
		snprintf( buf, 64, "***ERROR*** setaffinity failure for CPU %d", cpu );
		perror( buf );
		abort();
	} // if
} // affinity
