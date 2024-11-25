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
	void * aalloc( size_t dimension, size_t elemSize ) __attribute__ ((malloc));
	void * resize( void * oaddr, size_t size ) __attribute__ ((malloc));
	void * amemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc));
	void * cmemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc));
	void * aligned_resize( void * oaddr, size_t nalignment, size_t size ) __attribute__ ((malloc));
	void * aligned_realloc( void * oaddr, size_t nalignment, size_t size ) __attribute__ ((malloc));
	void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc));

	size_t malloc_alignment( void * addr );		// object alignment
	bool malloc_zero_fill( void * addr );		// true if object is zero filled
	size_t malloc_size( void * addr );			// object's request size
	int malloc_stats_fd( int fd );				// file descriptor for malloc_stats() writes (default stdout)
	size_t malloc_expansion();					// heap expansion size (bytes)
	size_t malloc_mmap_start();					// crossover allocation size from sbrk to mmap
	size_t malloc_unfreed();					// amount subtracted to adjust for unfreed program storage (debug only)
	void malloc_stats_clear();					// clear heap statistics
	void heap_stats();							// print thread-heap statistics
} // extern "C"

// Local Variables: //
// tab-width: 4 //
// End: //
