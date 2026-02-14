#include <malloc.h>
#include <string.h>										// memcpy

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	struct S { size_t ca[DIM]; };
	enum { Ssize = sizeof( S ) };

	printf( "reallocsim dimension %d size %d\n", DIM, Ssize );

	for ( size_t t = 0; t < 100; t += 1 ) { // not 100'000
		S * sa = nullptr, * so = (S *)malloc( Ssize );
		for ( size_t i = 0, s = Ssize; i < 10'000; i += 1, s += Ssize ) {
			sa = (S *)pass( malloc( s ) );				// simulate realloc
			memcpy( sa, so, s - Ssize );				// so one smaller
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
