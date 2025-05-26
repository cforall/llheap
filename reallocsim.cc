#include <malloc.h>
#include <string.h>										// memcpy

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	struct S { size_t ca[DIM]; };
	enum { SSize = sizeof( S ) };

	printf( "rallocsim dimension %d size %d\n", DIM, SSize );

	for ( size_t t = 0; t < 100; t += 1 ) { // not 100'000
		S * sa = nullptr, * so = (S *)malloc( SSize );
		for ( size_t i = 0, s = SSize; i < 10'000; i += 1, s += SSize ) {
			sa = (S *)pass( malloc( s ) );				// simulate realloc
			memcpy( sa, so, s - SSize );				// so one smaller
			sa[i].ca[0] = i;
			free( so );
			so = sa;
		} // for
		free( sa );
	} // for
//	malloc_stats();
}

// Local Variables: //
// compile-command: "g++-11 -Wall -Wextra -g -O3 -DDIM=4 reallocsim.cc -c" //
// End: //

