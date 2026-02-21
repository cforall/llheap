#include <iostream>
using namespace std;
#include <cstdio>
#include <cstdlib>
#include <cstdint>										// intmax_t
#include <cstring>
#include <stdexcept>									// out_of_range
#include <malloc.h>										// malloc_usable_size

template< typename T > static inline T pass( T v ) {	// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

static intmax_t convert( const char * str ) {			// convert C string to integer
	char * endptr;
	errno = 0;											// reset
	intmax_t val = strtoll( str, &endptr, 10 );			// attempt conversion
	if ( errno == ERANGE ) throw std::out_of_range( "" );
	if ( endptr == str ||								// conversion failed, no characters generated
		 *endptr != '\0' ) throw std::invalid_argument( "" ); // not at end of str ?
	return val;
} // convert

int main( int argc, char * argv[] ) {
	ssize_t Blocks = 10000, MinBlkSize = 16, MaxBlkSize = 1024, Step = 8, Times = 1000; // defaults

	switch ( argc ) {
	  case 6:
		if ( strcmp( argv[5], "d" ) != 0 && (Times = convert( argv[5] )) < 0 ) goto usage;
		// FALL THROUGH
	  case 5:
		if ( strcmp( argv[4], "d" ) != 0 && (Step = convert( argv[4] )) <= 0 ) goto usage;
		// FALL THROUGH
	  case 4:
		if ( strcmp( argv[3], "d" ) != 0 && (MaxBlkSize = convert( argv[3] )) <= 0 ) goto usage;
		// FALL THROUGH
	  case 3:
		if ( strcmp( argv[2], "d" ) != 0 && (MinBlkSize = convert( argv[2] )) <= 0 && MinBlkSize <= MaxBlkSize ) goto usage;
		// FALL THROUGH
	  case 2:
		if ( strcmp( argv[1], "d" ) != 0 && (Blocks = convert( argv[1] )) < 0 ) goto usage;
		// FALL THROUGH
		break;
	  usage:
	  default:
		cerr << "Usage: " << argv[0] << " [ Blocks (>= 0) | d [ MinBlkSize (> 0) | d [ MaxBlkSize (> 0) | d [ Step (> 0) | d [ Times (> 0) | d ] ] ] ] ]" << endl;
		exit( EXIT_FAILURE );							// TERMINATE
	} // switch

	struct Block { Block * link ; int touch; };

//#define CONTIG
#ifdef CONTIG
	double sum = 0.0;
#endif // CONTIG
	size_t nbs = 0;
	for ( size_t bs = MinBlkSize; bs <= (size_t)MaxBlkSize; bs += Step, nbs += 1 ) { // different object sizes
		Block * list = nullptr, * b;					// stack head pointer
#ifdef CONTIG
		size_t contig = 0;
#endif // CONTIG
		for ( size_t nb = 0; nb < (size_t)Blocks; nb += 1, b->link = list, list = b ) { 
			// Use intrusive stack to avoid confounding interspersed allocation arising from array.
			b = (Block *)pass( malloc( bs ) );
			// printf( "contig %p %p %zd\n", b, list, (char *)b - ((char *)list + malloc_usable_size( list )) );
			// if ( b == nullptr ) { printf( "malloc %zu/%zd failed\n", nb, Blocks ); return 1; } // if
#ifdef CONTIG
			// Check if previous and current allocation are contiguous.
			if ( (char *)b - ((char *)list + malloc_usable_size( list )) <= 16 ) contig += 1;
#endif // CONTIG
		}
#ifdef CONTIG
		sum += contig;
#endif // CONTIG
//		int x;
		for ( size_t t = 0; t < (size_t)Times; t += 1 ) { // repeat
			for ( Block * b = list; b; b = b->link ) {
				// pass( x = b->touch );					// read only
				pass( b->touch += 1 );					// read/write only
			}
		}
//#define FREE
#ifdef FREE
		int cnt1 = 1;
		for ( Block * b = list, * next; b; b = next, cnt1 += 1 ) { // delete every Nth block
			next = b->link;
			if ( cnt1 % 2 == 0 ) free( b );
		}
#endif // FREE
	}
#ifdef CONTIG
	printf( "avg %.0f\n", sum / nbs );
#else
	printf( "avg %.0f\n", 0.0 );						// dummy
#endif
} // main
