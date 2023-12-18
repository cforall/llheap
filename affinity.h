#include <stdio.h>										// perror
#include <pthread.h>

void affinity( unsigned int tid, cpu_set_t & mask ) {
	#if defined( plg2 )									// old AMD
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 16, HYPER = 1 };
	tid *= 8; // seperate caches
	#elif defined( algol )								// ARM
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 48, HYPER = 1 };
	#elif defined( nasus )								// AMD
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 64, HYPER = 1 };
	#elif defined( jax )								// Intel
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 4, CORES = 24, HYPER = 2 /* wrap on socket */ };
	#elif defined( pyke )								// Intel
	enum { OFFSETSOCK = 1 /* 0 origin */, SOCKETS = 2, CORES = 24, HYPER = 2 /* wrap on socket */ };
	#elif defined( cfapi1 )								// raspberrypi
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 4, HYPER = 1 };
	#elif defined( scspc482 )							// Mubeen PC
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 6, HYPER = 1 };
	#else // default
	enum { OFFSETSOCK = 0 /* 0 origin */, SOCKETS = 1, CORES = 8, HYPER = 1 };
	#endif // HOSTS

	int cpu = tid + ((tid < CORES) ? OFFSETSOCK * CORES : HYPER < 2 ? OFFSETSOCK * CORES : CORES * SOCKETS);
	//printf( "%d\n", cpu );

	CPU_ZERO( &mask );
	CPU_SET( cpu, &mask );
} // affinity
