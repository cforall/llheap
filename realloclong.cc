#include <malloc.h>

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

int main() {
	struct S { size_t ca[DIM]; };
	enum { Ssize = sizeof( S ) };

	printf( "realloc dimension %d size %d ", DIM, Ssize );

	size_t copies = 0;
	void * prev = nullptr;
	for ( size_t t = 0; t < 10'000; t += 1 ) {
		S * sa = nullptr, * perturb = nullptr;
		for ( size_t i = 0, s = Ssize; i < 10'000; i += 1, s += Ssize ) {
			sa = (S *)pass( realloc( sa, s ) );
			sa[i].ca[0] = i;
			if ( prev != sa ) { prev = sa; copies += 1; }
			if ( i % 1024 == 0 ) perturb = (S *)pass( realloc( perturb, s ) );
		} // for
		free( sa );
		free( perturb );
	} // for
	printf( "copies %zd\n", copies );
//	malloc_stats();
}

// Local Variables: //
// compile-command: "g++-11 -Wall -Wextra -g -O3 -DDIM=4 realloc.cc -c" //
// End: //
