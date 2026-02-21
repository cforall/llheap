// 
// Check if an allocator supports sticky zero-fill and/or alignment properties.
// 

#include <stdio.h>										// I/O
#include <stdlib.h>										// abort
#include <malloc.h>										// memalign

template< typename T > static inline T pass( T v ) {	// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	struct S { int i, j; };
	enum { Small = 50, Big = 500 };						// array size

	// Test if allocator preserves zero fill across realloc
	for ( size_t t = 0; t < 100; t += 1 ) {
		S * arr = (S *)pass( calloc( Big, sizeof( S ) ) ); // zero fill
		if ( arr[Big - 1].i != 0 || arr[Big - 1].j != 0 ) {
			free( arr );								// free storage
			fprintf( stderr, "failed zero fill\n" );
			goto failed;
		} // if
		for ( int i = 0; i < Big; i += 1 ) arr[i] = (S){ 42, 42 };

		arr = (S *)pass( realloc( arr, sizeof( S ) * Small ) );	// make storage smaller
		if ( arr[Small - 1].i != 42 || arr[Small - 1].j != 42 ) {
			free( arr );								// free storage
			fprintf( stderr, "failed zero fill\n" );
			goto failed;
		} // if

		arr = (S *)pass( realloc( arr, sizeof( S ) * Big ) ); // make storage bigger
		if ( arr[Big - 1].i != 0 || arr[Big - 1].j != 0 ) {
			free( arr );								// free storage
			fprintf( stderr, "failed zero fill\n" );
			goto failed;
		} // if
		free( arr );
	}
	fprintf( stderr, "passed zero fill\n" );

  failed: ;
	// Test if allocator preserves alignment across realloc
	for ( size_t a = 16; a <= 32 * 1024; a += a ) {		// powers of 2
		void * area = pass( memalign( a, 27 ) );		// initial N byte allocation
		if ( area == NULL ) abort();					// no storage ?
		const size_t range = (32 * 1024);
		for ( size_t s = 27; s <= range; s += 1 ) {		// initial request
			size_t size = rand() % range + 1;			// [1,range], random
			area = (char *)pass( realloc( area, size ) ); // reuse storage ?
			if ( area == NULL ) abort();				// no storage ?
			if ( (size_t)area % a != 0 ) {				// check alignment
				fprintf( stderr, "failed alignment:%zd size:%zd\n", a, size );
				exit( 1 );
			} // if
		} // for
		free( area );									// free storage
	} // for
	fprintf( stderr, "passed alignment\n" );
	malloc_stats();
} // main
