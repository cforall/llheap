#pragma once

#include <malloc.h>
#ifndef __cplusplus
#include <stdbool.h>									// bool
#endif

#ifdef __cplusplus
extern "C" {
#endif
	// New allocation operations
	void * aalloc( size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute__ ((malloc)) __attribute_alloc_size__ ((1, 2)); // calloc - zero-fill
	void * resize( void * oaddr, size_t size ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((2)); // realloc - data copy
	void * resizearray( void * oaddr, size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((2, 3)); // reallocarray - data copy
	int posix_realloc( void ** oaddrp, size_t size );
	int posix_reallocarray( void ** oaddrp, size_t dimension, size_t elemSize );
	void * amemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute__ ((malloc)) __attribute_alloc_size__ ((2, 3)); // memalign + array
	void * cmemalign( size_t alignment, size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute__ ((malloc)) __attribute_alloc_size__ ((2, 3)); // memalign + zero-fil
	void * aligned_resize( void * oaddr, size_t nalignment, size_t size ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((3)); // resize + alignment
	void * aligned_resizearray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((3, 4)); // resizearray + alignment
	void * aligned_realloc( void * oaddr, size_t nalignment, size_t size ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((3)); // realloc + alignment
	void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) __attribute_warn_unused_result__ __attribute_alloc_size__ ((3, 4)); // reallocarray + alignment
	int posix_aligned_realloc( void ** oaddrp, size_t nalignment, size_t size );
	int posix_aligned_reallocarray( void ** oaddrp, size_t nalignment, size_t dimension, size_t elemSize );

	// New control operations
	size_t malloc_extend( void );						// heap extend size (bytes)
	size_t malloc_mmap_start( void );					// crossover allocation size from sbrk to mmap
	size_t malloc_unfreed( void );						// amount subtracted to adjust for unfreed program storage (debug only)

	// Preserved properties
	size_t malloc_size( void * addr ) __attribute_warn_unused_result__;		 // object's request size, malloc_size <= malloc_usable_size
	size_t malloc_alignment( void * addr ) __attribute_warn_unused_result__; // object alignment
	bool malloc_zero_fill( void * addr ) __attribute_warn_unused_result__;	 // true if object is zero filled
	bool malloc_remote( void * addr ) __attribute_warn_unused_result__;		 // true if object is remote

	// Statistics
	int malloc_stats_fd( int fd );						// file descriptor global malloc_stats() writes (default stdout)
	void malloc_stats_clear( void );					// clear global heap statistics
	void heap_stats( void );							// print thread per heap statistics

	// If unsupport, create them, as supported in mallopt.
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
#ifdef __cplusplus
} // extern "C"
#endif

// Local Variables: //
// tab-width: 4 //
// End: //
