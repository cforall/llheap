#include <string>										// stoi
#include <cstdarg>										// va_start, va_end
using namespace std;
// Use C I/O because cout does not a good mechanism for thread-safe I/O.
#include <string.h>										// strlen, strerror
#include <unistd.h>										// sysconf
#include "llheap.h"
#include "affinity.h"

enum { __ALIGN__ = 16 };								// minimum allocation alignment in bytes

static void abort( const char fmt[], ... ) __attribute__(( format(printf, 1, 2), __nothrow__, __noreturn__ ));
static void abort( const char fmt[], ... ) {			// overload real abort
	va_list args;
	va_start( args, fmt );
	vfprintf( stderr, fmt, args );
	if ( fmt[strlen( fmt ) - 1] != '\n' ) {				// add optional newline if missing at the end of the format text
		vfprintf( stderr, "\n", args );					// g++-10 does not allow nullptr for va_list
	} // if
	va_end( args );
	abort();											// call the real abort
	// CONTROL NEVER REACHES HERE!
} // abort

extern "C" size_t malloc_unfreed() { return 5979; }		// printf(1024) / setlocale(4043) / pthread(3*304)

timespec currTime() {
	timespec t;											// nanoseconds since UNIX epoch
	if ( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &t ) == -1 ) {
		fprintf( stderr, "internal error, clock failed %d %s\n", errno, strerror( errno ) );
	} // if
	return t;
} // currTime

inline double dur( timespec end, timespec start ) {
	long int sec = end.tv_sec - start.tv_sec, nsec = end.tv_nsec - start.tv_nsec;
	return sec + nsec * 1E-9;
} // dur

void * worker( void * ) {
	enum { NoOfAllocs = 10'000, NoOfMmaps = 10 };
	char * locns[NoOfAllocs];
	enum { limit = 64 * 1024 };							// check alignments up to here

	// check new/delete

	for ( int j = 0; j < 40; j += 1 ) {
		for ( int i = 0; i < NoOfAllocs; i += 1 ) {
			locns[i] = new char[i];
			//cout << setw(6) << (void *)locns[i] << endl;
			for ( int k = 0; k < i; k += 1 ) locns[i][k] = '\345';
		} // for
		//cout << (char *)sbrk(0) - start << " bytes" << endl;

		for ( int i = 0; i < NoOfAllocs; i += 1 ) {
			//cout << setw(6) << (void *)locns[i] << endl;
			for ( int k = 0; k < i; k += 1 ) if ( locns[i][k] != '\345' ) abort( "new/delete corrupt storage1" );
			delete [] locns[i];
		} // for
		//cout << (char *)sbrk(0) - start << " bytes" << endl;

		for ( int i = 0; i < NoOfAllocs; i += 1 ) {
			locns[i] = new char[i];
			//cout << setw(6) << (void *)locns[i] << endl;
			for ( int k = 0; k < i; k += 1 ) locns[i][k] = '\345';
		} // for
		for ( int i = NoOfAllocs - 1; i >=0 ; i -= 1 ) {
			//cout << setw(6) << (void *)locns[i] << endl;
			for ( int k = 0; k < i; k += 1 ) if ( locns[i][k] != '\345' ) abort( "new/delete corrupt storage2" );
			delete [] locns[i];
		} // for
	} // for

	// check malloc/free (sbrk)

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = (i + 1) * 20;
		char * area = (char *)malloc( s );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;								// +1 to make initialization simpler
		locns[i] = (char *)malloc( s );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "malloc/free corrupt storage" );
		free( locns[i] );
	} // for

	// check malloc/free (mmap)

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)malloc( s );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		locns[i] = (char *)malloc( s );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "malloc/free corrupt storage" );
		free( locns[i] );
	} // for

	// check aalloc/free (sbrk)

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = (i + 1) * 20;
		char * area = (char *)aalloc( s, 10 );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;								// +1 to make initialization simpler
		locns[i] = (char *)aalloc( s, 10 );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "aalloc/free corrupt storage" );
		free( locns[i] );
	} // for

	// check aalloc/free (mmap)

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)aalloc( s, 10 );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		locns[i] = (char *)aalloc( s, 10 );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "aalloc/free corrupt storage" );
		free( locns[i] );
	} // for

	// check calloc/free (sbrk)

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = (i + 1) * 20;
		char * area = (char *)calloc( 5, s );
		if ( area[0] != '\0' || area[s - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "calloc/free corrupt storage1" );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;
		locns[i] = (char *)calloc( 5, s );
		if ( locns[i][0] != '\0' || locns[i][s - 1] != '\0' ||
			 locns[i][malloc_request_size( locns[i] ) - 1] != '\0' ||
			 ! malloc_zero_fill( locns[i] ) ) abort( "calloc/free corrupt storage2" );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfAllocs; i += 1 ) {
		size_t s = i + 1;
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "calloc/free corrupt storage3" );
		free( locns[i] );
	} // for

	// check calloc/free (mmap)

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)calloc( 1, s );
		if ( area[0] != '\0' || area[s - 1] != '\0' ) abort( "calloc/free corrupt storage4.1" );
		if ( area[malloc_request_size( area ) - 1] != '\0' ) abort( "calloc/free corrupt storage4.2" );
		if ( ! malloc_zero_fill( area ) ) abort( "calloc/free corrupt storage4.3" );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		locns[i] = (char *)calloc( 1, s );
		if ( locns[i][0] != '\0' || locns[i][s - 1] != '\0' ||
			 locns[i][malloc_request_size( locns[i] ) - 1] != '\0' ||
			 ! malloc_zero_fill( locns[i] ) ) abort( "calloc/free corrupt storage5" );
		locns[i][0] = '\345'; locns[i][s - 1] = '\345';	// fill first/last
		locns[i][malloc_usable_size( locns[i] ) - 1] = '\345'; // fill ultimate byte
	} // for
	for ( int i = 0; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		if ( locns[i][0] != '\345' || locns[i][s - 1] != '\345' ||
			 locns[i][malloc_usable_size( locns[i] ) - 1] != '\345' ) abort( "calloc/free corrupt storage6" );
		free( locns[i] );
	} // for

	// check memalign/free (sbrk)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int s = 1; s < NoOfAllocs; s += 1 ) {		// allocation of size 0 can return null
			char * area = (char *)memalign( a, s );
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "memalign/free bad alignment : memalign( %d, %d ) = %p", (int)a, s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check memalign/free (mmap)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int i = 1; i < NoOfMmaps; i += 1 ) {
			size_t s = i + malloc_mmap_start();			// cross over point
			char * area = (char *)memalign( a, s );
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "memalign/free bad alignment : memalign( %d, %d ) = %p", (int)a, (int)s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check aligned_alloc/free (sbrk)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int s = 1; s < NoOfAllocs; s += 1 ) {		// allocation of size 0 can return null
			char * area = (char *)aligned_alloc( a, s );
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "aligned_alloc/free bad alignment : aligned_alloc( %d, %d ) = %p", (int)a, s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check aligned_alloc/free (mmap)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int i = 1; i < NoOfMmaps; i += 1 ) {
			size_t s = i + malloc_mmap_start();			// cross over point
			char * area = (char *)aligned_alloc( a, s );
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "aligned_alloc/free bad alignment : aligned_alloc( %d, %d ) = %p", (int)a, (int)s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check posix_memalign/free (sbrk)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int s = 1; s < NoOfAllocs; s += 1 ) {		// allocation of size 0 can return null
			char * area;
			int ret = posix_memalign( (void **)&area, a, s );
			if ( ret ) abort();
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "posix_memalign/free bad alignment : posix_memalign( %d, %d ) = %p", (int)a, s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check posix_memalign/free (mmap)

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int i = 1; i < NoOfMmaps; i += 1 ) {
			size_t s = i + malloc_mmap_start();			// cross over point
			char * area;
			int ret = posix_memalign( (void **)&area, a, s );
			if ( ret ) abort();
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "posix_memalign/free bad alignment : posix_memalign( %d, %d ) = %p", (int)a, (int)s, area );
			} // if
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			area[malloc_usable_size( area ) - 1] = '\345'; // fill ultimate byte
			free( area );
		} // for
	} // for

	// check valloc/free (sbrk)

	size_t pagesize = sysconf( _SC_PAGESIZE );
	for ( int s = 1; s < NoOfAllocs; s += 1 ) {			// allocation of size 0 can return null
		char * area = (char *)valloc( s );
		//cout << setw(6) << i << " " << area << endl;
		if ( (size_t)area % pagesize != 0 || malloc_alignment( area ) != pagesize ) { // check for initial alignment
			abort( "valloc/free bad alignment : valloc( %d ) = %p", s, area );
		} // if
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last byte
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	// check valloc/free (mmap)

	for ( int i = 1; i < NoOfMmaps; i += 1 ) {
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)valloc( s );
		//cout << setw(6) << i << " " << area << endl;
		if ( (size_t)area % pagesize != 0 || malloc_alignment( area ) != pagesize ) { // check for initial alignment
			abort( "valloc/free bad alignment : valloc( %d ) = %p", (int)s, area );
		} // if
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/last byte
		area[malloc_usable_size( area ) - 1] = '\345';	// fill ultimate byte
		free( area );
	} // for

	// check malloc/resize/free (sbrk)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		char * area = (char *)malloc( i );
		area[0] = '\345'; area[i - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because resize of 0 bytes frees the storage.
		int prev = i;
		for ( int s = i; s < 256 * 1024; s += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/resize/free corrupt storage" );
			area = (char *)resize( area, s );			// attempt to reuse storage
			area[0] = area[s - 1] = '\345';				// fill last byte
			prev = s;
		} // for
		free( area );
	} // for

	// check malloc/resize/free (mmap)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)malloc( s );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because resize of 0 bytes frees the storage.
		int prev = s;
		for ( int r = s; r < 256 * 1024; r += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/resize/free corrupt storage" );
			area = (char *)resize( area, s );			// attempt to reuse storage
			area[0] = area[r - 1] = '\345';				// fill last byte
			prev = r;
		} // for
		free( area );
	} // for

	// check malloc/realloc/free (sbrk)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		char * area = (char *)malloc( i );
		area[0] = '\345'; area[i - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		int prev = i;
		for ( int s = i; s < 256 * 1024; s += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/realloc/free corrupt storage" );
			area = (char *)realloc( area, s );			// attempt to reuse storage
			area[s - 1] = '\345';						// fill last byte
			prev = s;
		} // for
		free( area );
	} // for

	// check malloc/realloc/free (mmap)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)malloc( s );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		int prev = s;
		for ( int r = s; r < 256 * 1024; r += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/realloc/free corrupt storage" );
			area = (char *)realloc( area, s );			// attempt to reuse storage
			area[r - 1] = '\345';						// fill last byte
			prev = r;
		} // for
		free( area );
	} // for

	// check calloc/realloc/free (sbrk)

	for ( int i = 1; i < 10000; i += 12 ) {
		// initial N byte allocation
		char * area = (char *)calloc( 5, i );
		if ( area[0] != '\0' || area[i - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "calloc/realloc/free corrupt storage1" );

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = i; s < 256 * 1024; s += 26 ) {	// start at initial memory request
			area = (char *)realloc( area, s );			// attempt to reuse storage
			if ( area[0] != '\0' || area[s - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "calloc/realloc/free corrupt storage2" );
		} // for
		free( area );
	} // for

	// check calloc/realloc/free (mmap)

	for ( int i = 1; i < 1000; i += 12 ) {
		// initial N byte allocation
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)calloc( 1, s );
		if ( area[0] != '\0' || area[s - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "calloc/realloc/free corrupt storage3" );

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int r = i; r < 256 * 1024; r += 26 ) {	// start at initial memory request
			area = (char *)realloc( area, r );			// attempt to reuse storage
			if ( area[0] != '\0' || area[r - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "calloc/realloc/free corrupt storage4" );
		} // for
		free( area );
	} // for

	// check malloc/posix_realloc/free (sbrk)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		char * area = (char *)malloc( i );
		area[0] = '\345'; area[i - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because posix_realloc of 0 bytes frees the storage.
		int prev = i;
		for ( int s = i; s < 256 * 1024; s += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/posix_realloc/free corrupt storage" );
			posix_realloc( (void **)&area, s );			// attempt to reuse storage
			area[s - 1] = '\345';						// fill last byte
			prev = s;
		} // for
		free( area );
	} // for

	// check malloc/posix_realloc/free (mmap)

	for ( int i = 2; i < NoOfAllocs; i += 12 ) {
		// initial N byte allocation
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)malloc( s );
		area[0] = '\345'; area[s - 1] = '\345';			// fill first/penultimate byte

		// Do not start this loop index at 0 because posix_realloc of 0 bytes frees the storage.
		int prev = s;
		for ( int r = s; r < 256 * 1024; r += 26 ) {	// start at initial memory request
			if ( area[0] != '\345' || area[prev - 1] != '\345' ) abort( "malloc/posix_realloc/free corrupt storage" );
			posix_realloc( (void **)&area, s );			// attempt to reuse storage
			area[r - 1] = '\345';						// fill last byte
			prev = r;
		} // for
		free( area );
	} // for

	// check calloc/posix_realloc/free (sbrk)

	for ( int i = 1; i < 10000; i += 12 ) {
		// initial N byte allocation
		char * area = (char *)calloc( 5, i );
		if ( area[0] != '\0' || area[i - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "calloc/posix_realloc/free corrupt storage1" );

		// Do not start this loop index at 0 because posix_realloc of 0 bytes frees the storage.
		for ( int s = i; s < 256 * 1024; s += 26 ) {	// start at initial memory request
			posix_realloc( (void **)&area, s );			// attempt to reuse storage
			if ( area[0] != '\0' || area[s - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "calloc/posix_realloc/free corrupt storage2" );
		} // for
		free( area );
	} // for

	// check calloc/posix_realloc/free (mmap)

	for ( int i = 1; i < 1000; i += 12 ) {
		// initial N byte allocation
		size_t s = i + malloc_mmap_start();				// cross over point
		char * area = (char *)calloc( 1, s );
		if ( area[0] != '\0' || area[s - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "calloc/posix_realloc/free corrupt storage3" );

		// Do not start this loop index at 0 because posix_realloc of 0 bytes frees the storage.
		for ( int r = i; r < 256 * 1024; r += 26 ) {	// start at initial memory request
			posix_realloc( (void **)&area, r );			// attempt to reuse storage
			if ( area[0] != '\0' || area[r - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "calloc/posix_realloc/free corrupt storage4" );
		} // for
		free( area );
	} // for

	// check memalign/resize with align/free

	size_t amount;

	amount = 2;
	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		// initial N byte allocation
		char * area = (char *)memalign( a, amount );	// aligned N-byte allocation

		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "memalign/resize with align/free bad alignment : memalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because resize of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			area = (char *)aligned_resize( area, a * 2, s ); // attempt to reuse storage
			if ( (size_t)area % a * 2 != 0 ) {			// check for initial alignment
				abort( "memalign/resize with align/free bad alignment %p", area );
			} // if
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check memalign/realloc/free

	amount = 2;
	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		// initial N byte allocation
		char * area = (char *)memalign( a, amount );	// aligned N-byte allocation
		//cout << setw(6) << alignments[a] << " " << area << endl;
		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "memalign/realloc/free bad alignment : memalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			if ( area[0] != '\345' || area[s - 2] != '\345' ) abort( "memalign/realloc/free corrupt storage" );
			area = (char *)realloc( area, s );			// attempt to reuse storage
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 ) {				// check for initial alignment
				abort( "memalign/realloc/free bad alignment %p", area );
			} // if
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check amemalign/resize with align/free

	amount = 2;
	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		// initial N byte allocation
		char * area = (char *)amemalign( a, amount, 10 );	// aligned N-byte allocation

		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "amemalign/resize with align/free bad alignment : amemalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because resize of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			area = (char *)aligned_resize( area, a * 2, s ); // attempt to reuse storage
			if ( (size_t)area % a * 2 != 0 ) {			// check for initial alignment
				abort( "amemalign/resize with align/free bad alignment %p", area );
			} // if
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check amemalign/realloc/free

	amount = 2;
	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		// initial N byte allocation
		char * area = (char *)amemalign( a, amount, 10 );	// aligned N-byte allocation
		//cout << setw(6) << alignments[a] << " " << area << endl;
		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "amemalign/realloc/free bad alignment : amemalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			if ( area[0] != '\345' || area[s - 2] != '\345' ) abort( "amemalign/realloc/free corrupt storage" );
			area = (char *)realloc( area, s );			// attempt to reuse storage
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 ) {				// check for initial alignment
				abort( "amemalign/realloc/free bad alignment %p", area );
			} // if
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check cmemalign/free

	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		//cout << setw(6) << alignments[a] << endl;
		for ( int s = 1; s < limit; s += 1 ) {			// allocation of size 0 can return null
			char * area = (char *)cmemalign( a, 1, s );
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "cmemalign/free bad alignment : cmemalign( %d, %d ) = %p", (int)a, s, area );
			} // if
			if ( area[0] != '\0' || area[s - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "cmemalign/free corrupt storage" );
			area[0] = '\345'; area[s - 1] = '\345';		// fill first/last byte
			free( area );
		} // for
	} // for

	// check cmemalign/realloc/free

	amount = 2;
	for ( size_t a = __ALIGN__ + __ALIGN__; a <= limit; a += a ) { // generate powers of 2
		// initial N byte allocation
		char * area = (char *)cmemalign( a, 1, amount ); // aligned N-byte allocation
		//cout << setw(6) << alignments[a] << " " << area << endl;
		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "cmemalign/realloc/free bad alignment : cmemalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		if ( area[0] != '\0' || area[amount - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "cmemalign/realloc/free corrupt storage1" );
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			if ( area[0] != '\345' || area[s - 2] != '\345' ) abort( "cmemalign/realloc/free corrupt storage2" );
			area = (char *)realloc( area, s );			// attempt to reuse storage
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
				abort( "cmemalign/realloc/free bad alignment %p", area );
			} // if
			if ( area[0] != '\345' || area[s - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "cmemalign/realloc/free corrupt storage3" );
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check memalign/realloc with align/free

	amount = 2;
	for ( size_t a = __ALIGN__; a <= limit; a += a ) {	// generate powers of 2
		// initial N byte allocation
		char * area = (char *)memalign( a, amount );	// aligned N-byte allocation
		//cout << setw(6) << alignments[a] << " " << area << endl;
		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "memalign/realloc with align/free bad alignment : memalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			if ( area[0] != '\345' || area[s - 2] != '\345' ) abort( "memalign/realloc/free corrupt storage" );
			area = (char *)aligned_realloc( area, a * 2, s ); // attempt to reuse storage
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a * 2 != 0 ) {			// check for initial alignment
				abort( "memalign/realloc with align/free bad alignment %p", area );
			} // if
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	// check cmemalign/realloc with align/free

	amount = 2;
	for ( size_t a = __ALIGN__ + __ALIGN__; a <= limit; a += a ) { // generate powers of 2
		// initial N byte allocation
		char * area = (char *)cmemalign( a, 1, amount ); // aligned N-byte allocation
		//cout << setw(6) << alignments[a] << " " << area << endl;
		if ( (size_t)area % a != 0 || malloc_alignment( area ) != a ) { // check for initial alignment
			abort( "cmemalign/realloc with align/free bad alignment : cmemalign( %d, %d ) = %p", (int)a, (int)amount, area );
		} // if
		if ( area[0] != '\0' || area[amount - 1] != '\0' ||
			 area[malloc_request_size( area ) - 1] != '\0' ||
			 ! malloc_zero_fill( area ) ) abort( "cmemalign/realloc with align/free corrupt storage1" );
		area[0] = '\345'; area[amount - 2] = '\345';	// fill first/penultimate byte

		// Do not start this loop index at 0 because realloc of 0 bytes frees the storage.
		for ( int s = amount; s < 256 * 1024; s += 1 ) { // start at initial memory request
			if ( area[0] != '\345' || area[s - 2] != '\345' ) abort( "cmemalign/realloc with align/free corrupt storage2" );
			area = (char *)aligned_realloc( area, a * 2, s ); // attempt to reuse storage
			//cout << setw(6) << i << " " << area << endl;
			if ( (size_t)area % a * 2 != 0 || malloc_alignment( area ) != a * 2 ) { // check for initial alignment
				abort( "cmemalign/realloc with align/free bad alignment %p %zd %zd", area, malloc_alignment( area ), a * 2 );
			} // if
			if ( area[s - 1] != '\0' || area[s - 1] != '\0' ||
				 area[malloc_request_size( area ) - 1] != '\0' ||
				 ! malloc_zero_fill( area ) ) abort( "cmemalign/realloc/free corrupt storage3" );
			area[s - 1] = '\345';						// fill last byte
		} // for
		free( area );
	} // for

	printf( "worker %lu successful completion\n", pthread_self() );
	return nullptr;
} // worker

int main( int argc, char *argv[] ) {
	setlocale( LC_NUMERIC, getenv( "LANG" ) );

#if 1
	int Threads = 4;

	try {
		switch( argc ) {
		  case 2:
			Threads = stoi( argv[1] ); if ( Threads <= 0 ) throw 1;
		} // switch
	} catch( ... ) {
		printf( "Usage: %s [ threads (>0) ]\n", argv[0] );
		exit( EXIT_FAILURE );
	} // try
	printf("Number of Threads: %d\n\n", Threads);

	pthread_t thread[Threads];							// thread[0] unused

	affinity( pthread_self(), 0 );
	for ( int tid = 1; tid < Threads; tid += 1 ) {		// N - 1 thread
		if ( pthread_create( &thread[tid], nullptr, worker, nullptr) < 0 ) abort();
		affinity( thread[tid], tid );
	} // for
	
	worker( nullptr );									// initialize thread runs one test

	for ( int tid = 1; tid < Threads; tid += 1 ) {
		if ( pthread_join( thread[tid], nullptr ) < 0 ) abort();
	} // for
#else
	worker( nullptr );
#endif // 0

	malloc_stats();
} // main

// /usr/bin/time -f %Uu %Ss %er %Mkb ./a.out

// Local Variables: //
// tab-width: 4 //
// compile-command: "g++-14 -Wall -Wextra -g -O3 -D`hostname` testllheap.cc libllheap.o -lpthread -Wl,-rpath=/u/pabuhr/software/llheap" //
// End: //
