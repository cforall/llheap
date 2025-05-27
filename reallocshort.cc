#include <malloc.h>

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	for ( size_t p = 10; p <= 100; p += 10 ) {
		for ( size_t s = 64; s < 16 * 1024; s <<= 1 ) {
			bool reuse = false;
			void * prev = pass( malloc( s ) );
			void * curr = pass( realloc( prev, s * p / 100 ) );
			if ( prev == curr ) { reuse = true; }
			free( curr );
			printf( "%zd %zd %zd %d ", 100 - p, s, s * p / 100, reuse );
		} // for
		printf( "\n" );
	} // for
//	malloc_stats();
}

// Local Variables: //
// compile-command: "g++-14 -Wall -Wextra -g -O3 reallocshort.cc libllheap-stats-debug.o" //
// End: //
