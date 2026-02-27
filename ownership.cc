#include <iostream>
#include <iomanip>
#include <cmath>
#include <locale>
using namespace std;

#include "llheap.h"
#include <stdint.h>										// uintptr_t, UINTPTR_MAX
#include <unistd.h>										// sleep
#include <string.h>										// strcmp
#include <pthread.h>

#define str( s ) #s
#define xstr(s) str(s)

// HYPERAFF => use hyperthreads and fill in pairs of processors on a socket => 129,384, 129,385, ...
#define HYPERAFF

// LINEARAFF => do not use hyperthreading and fill cores on a socket => 129, 130, 131, 132, ...
//#define LINEARAFF
#include "affinity.h"

typedef size_t TYPE;									// unsigned word-size

#define CACHE_ALIGN 128									// Intel recommendation
#define CALIGN __attribute__(( aligned(CACHE_ALIGN) ))

#define Fas( change, assn ) __atomic_exchange_n( (&(change)), (assn), __ATOMIC_SEQ_CST )
static __attribute__(( unused )) inline size_t cycleUp( size_t v, size_t n ) { return ( ((v) >= (n - 1)) ? 0 : (v + 1) ); }
static __attribute__(( unused )) inline size_t cycleDown( size_t v, size_t n ) { return ( ((v) <= 0) ? (n - 1) : (v - 1) ); }


template<typename T> T statistics( size_t N, T values[], double & avg, double & std, double & rstd ) {
	T sum = 0;
	for ( size_t r = 0; r < N; r += 1 ) {
		sum += values[r];
	} // for
	avg = sum / N;										// average
	double sum2 = 0.0;									// sum squared
	for ( size_t r = 0; r < N; r += 1 ) {				// sum squared differences from average
		double diff = values[r] - avg;
		sum2 += diff * diff;
	} // for
	std = sqrt( sum2 / N );
	rstd = avg == 0.0 ? 0.0 : std / avg * 100;
	return sum;
} // statisitics


enum { MaxThread = 256, MaxBatch = 500 };				// thread global
void * batches[MaxThread][MaxBatch];					// set to nullptr
struct Aligned { CALIGN void * * col; };
volatile Aligned allocations[MaxThread];				// set to nullptr
size_t times[MaxThread];								// set to zero
size_t Threads, Batch;									// set in program main
volatile bool stop = false;

void * worker( void * arg ) {
	size_t id = (size_t)arg;
	Aligned batch = { batches[id] };					// thread local
	size_t cnt = 0, a = 0;

	for ( ; ! stop; ) {									// loop for T second
		for ( ssize_t i = Batch - 1; i >= 0; i -= 1 ) { // allocations, oppose order from frees
			batch.col[i] = malloc( i & 1 ? 42 : 192 );	// two allocation sizes
			*(int *)batch.col[i] = 42;					// write storage
		} // for

		Aligned obatch = batch;
		while ( ! stop && ( (batch.col = Fas( allocations[a].col, batch.col )) == obatch.col || batch.col == nullptr ) ) { // Fas => atomic exchange
			a = cycleUp( a, Threads );					// try another batch
		} // while

	  if ( batch.col == nullptr ) break;

		for ( size_t i = 0; i < Batch; i += 1 ) {		// deallocations
			if ( *(int *)batch.col[i] != 42 ) abort();	// read storage check
			free( batch.col[i] );						// remote free
		} // for
		cnt += Batch;									// sum allocations/frees
		a = cycleUp( a, Threads );						// try another batchmodulo N increment
	} // for
	times[id] = cnt;									// return throughput
	return nullptr;
}; // worker


extern "C" size_t malloc_unfreed() {					// llheap only
	return Threads * 320/* pthreads */ + 16400/*locale*/ + 840/*printing floating-point*/;
}

int main( int argc, char * argv[] ) {
	// const char * lang = getenv( "LANG" );				// may cause memory leak
	// try {
	// 	locale loc( lang );
	// 	cout.imbue( loc );								// print numbers with separators (',')
	// } catch( runtime_error & ) {
	// 	cerr << "Invalid locale language name \"" << lang << "\"" << endl;
	// 	exit( EXIT_FAILURE );
	// } // try

	enum {
		Dduration = 30,									// default duration (seconds)
		Dthreads = 8,									// default threads
		Dbatch = 100,									// default batch size
	};
	size_t Duration = Dduration;
	Threads = Dthreads;
	Batch = Dbatch;

	switch ( argc ) {
	  case 4:
		if ( strcmp( argv[3], "d" ) != 0 ) {			// default ?
			Batch = atoi( argv[3] );					// experiment duration
			if ( (ssize_t)Batch < 1 || Batch > MaxBatch ) goto USAGE;
		} // if
		[[fallthrough]];
	  case 3:
		if ( strcmp( argv[2], "d" ) != 0 ) {			// default ?
			Threads = atoi( argv[2] );					// experiment duration
			if ( (ssize_t)Threads < 1 || Threads > MaxThread ) goto USAGE;
		} // if
		[[fallthrough]];
	  case 2:
		if ( strcmp( argv[1], "d" ) != 0 ) {			// default ?
			Duration = atoi( argv[1] );					// experiment duration
			if ( (ssize_t)Duration < 1 ) goto USAGE;
		} // if
		[[fallthrough]];
	  case 1:											// defaults
		break;
	  USAGE:
	  default:
		cout << "Usage: " << argv[0] << " [ duration (> 0, seconds) | 'd' (default) " << Dduration
			 << " [ threads (> 0 && <= " << MaxThread << ") | 'd' (default) " << Dthreads
			 << " [ batches (> 0 && <= " << MaxBatch << ") | 'd' (default) " << Dbatch << "] ] ]"
			 << endl;
		exit( EXIT_FAILURE );
	} // switch

	cout << fixed << Duration << ' ' << Threads << ' ' << Batch << ' ' << flush;

	pthread_t workers[Threads];
	for ( size_t i = 0; i < Threads; i += 1 ) {
		if ( pthread_create( &workers[i], nullptr, worker, (void *)i ) < 0 ) abort();
//		affinity( workers[i], i );
	} // for

	sleep( Duration );
	stop = true;

	for ( unsigned int i = 0; i < Threads; i += 1 ) {
		if ( pthread_join( workers[i], nullptr ) < 0 ) abort();
	} // for

	for ( unsigned int i = 0; i < Threads; i += 1 ) { // free any outstanding allocations
		if ( allocations[i].col != nullptr ) {
			for ( unsigned int j = 0; j < Batch; j += 1 ) { // free any outstanding allocations
				free( allocations[i].col[j] );
			} // for
		} // if
	} // for

	double avg, std, rstd;
	decltype( +times[0] ) total = statistics( Threads, times, avg, std, rstd );
	cout << fixed << total << setprecision(0) << ' ' << avg << ' ' << std << ' ' << setprecision(1) << rstd << "% ";

//	#if defined( HYPERAFF )
//	cout << "HYPERAFF affinity" << endl;
//	#elif defined( LINEARAFF )
//	cout << "LINEARAFF affinity" << endl;
//	#else
//		#error no affinity specified
//	#endif
} // main

// g++-10 -Wall -Wextra -g -O3 -D`hostname` ownership.cc libllheap.so -lpthread -Wl,-rpath=/u/pabuhr/heap -L/u/pabuhr/heap

// Local Variables: //
// compile-command: "g++-14 -Wall -Wextra -g -O3 ownership.cc libllheap-stats-debug.o -lpthread -D`hostname`" //
// End: //
