#pragma once

#include <malloc.h>

extern "C" {
	// New allocation operations
	void * aalloc( size_t dimension, size_t elemSize ) __attribute__ ((malloc)); // calloc - zero-fill
	void * resize( void * oaddr, size_t size ) __attribute__ ((malloc)); // realloc - data copy
	void * resizearray( void * oaddr, size_t dimension, size_t elemSize ); // reallocarray - data copy
	void * amemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc)); // memalign + array
	void * cmemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc)); // memalign + zero-fil
	void * aligned_resize( void * oaddr, size_t nalignment, size_t size ) __attribute__ ((malloc)); // resize + alignment
	void * aligned_resizearray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc)); // resizearray + alignment
	void * aligned_realloc( void * oaddr, size_t nalignment, size_t size ) __attribute__ ((malloc)); // realloc + alignment
	void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) __attribute__ ((malloc)); // reallocarray + alignment

	// New control operations
	size_t malloc_extend( void );						// heap expansion size (bytes)
	size_t malloc_mmap_start( void );					// crossover allocation size from sbrk to mmap
	size_t malloc_unfreed( void );						// amount subtracted to adjust for unfreed program storage (debug only)

	// Preserved properties
	size_t malloc_size( void * addr );					// object's request size, malloc_size <= malloc_usable_size
	size_t malloc_alignment( void * addr );				// object alignment
	bool malloc_zero_fill( void * addr );				// true if object is zero filled
	bool malloc_remote( void * addr );					// true if object is remote

	// Statistics
	int malloc_stats_fd( int fd );						// file descriptor global malloc_stats() writes (default stdout)
	void malloc_stats_clear();							// clear global heap statistics
	void heap_stats();									// print thread per heap statistics

	// If unsupport, create them, as llheap supports them in mallopt.
	#ifndef M_MMAP_THRESHOLD
	#define M_MMAP_THRESHOLD (-1)
	#endif // M_MMAP_THRESHOLD

	#ifndef M_TOP_PAD
	#define M_TOP_PAD (-2)
	#endif // M_TOP_PAD

	// Unsupported
	int malloc_trim( size_t );
	void * malloc_get_state( void );
	int malloc_set_state( void * );
} // extern "C"

// Local Variables: //
// tab-width: 4 //
// End: //
