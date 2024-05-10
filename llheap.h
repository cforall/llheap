#pragma once

#include <malloc.h>

// supported mallopt options
#ifndef M_MMAP_THRESHOLD
#define M_MMAP_THRESHOLD (-1)
#endif // M_MMAP_THRESHOLD

#ifndef M_TOP_PAD
#define M_TOP_PAD (-2)
#endif // M_TOP_PAD

extern "C" {
	// New allocation operations.
	void * aalloc( size_t dim, size_t elemSize ) __attribute__ ((malloc));
	void * resize( void * oaddr, size_t size ) __attribute__ ((malloc));
	void * amemalign( size_t align, size_t dim, size_t elemSize ) __attribute__ ((malloc));
	void * cmemalign( size_t align, size_t dim, size_t elemSize ) __attribute__ ((malloc));
	size_t malloc_alignment( void * addr );
	bool malloc_zero_fill( void * addr );
	size_t malloc_size( void * addr );
	int malloc_stats_fd( int fd );
	size_t malloc_expansion();							// heap expansion size (bytes)
	size_t malloc_mmap_start();							// crossover allocation size from sbrk to mmap
	size_t malloc_unfreed();							// heap unfreed size (bytes)
	void malloc_stats_clear();							// clear heap statistics
	void heap_stats();									// print thread-heap statistics
} // extern "C"

// New allocation operations.
void * resize( void * oaddr, size_t nalign, size_t size ) __THROW;
void * realloc( void * oaddr, size_t nalign, size_t size ) __THROW;
void * reallocarray( void * oaddr, size_t nalign, size_t dim, size_t elemSize ) __THROW;

// Local Variables: //
// tab-width: 4 //
// End: //
