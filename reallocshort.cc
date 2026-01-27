#include <malloc.h>

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	// no output => never reuse storage (always realloc)
	// 99% => always reuse storage (never realloc)
	// 50% => only realloc when storage size drops below 50% of original
	bool never = true;
	for ( size_t p = 0; p <= 100; p += 10 ) {
		if ( p == 0 ) p = 1;							// 99% shrinkage
		bool reuse = false;
		for ( size_t s = 64; s < 16 * 1024; s <<= 1 ) {
			void * prev = pass( malloc( s ) );
			void * curr = pass( realloc( prev, s * p / 100 ) ); // make smaller
			if ( prev == curr ) { printf( "%zd%% %zd %zd, ", 100 - p, s, s * p / 100 ); reuse = true; never = false; }
			free( curr );
		} // for
		if ( p == 1 ) {
			p = 0;
			if ( reuse ) { printf( "\nalways reuse (never realloc)\n" ); break; }
		} // if
		if ( reuse ) printf( "\n" );
	} // for
	if ( never ) printf( "never reuse (always realloc)\n" );
//	malloc_stats();
}

// Local Variables: //
// compile-command: "g++-14 -Wall -Wextra -g -O3 reallocshort.cc libllheap-stats-debug.o" //
// End: //
