# Install

**Requires >= g++-9**

$ make all

Creates 8 libraries that can be linked to a program to replace the default memory allocator.

* `libllheap.o` statically-linkable allocator with optimal performance without statistics or debugging.
* `libllheap-debug.o` statically-linkable allocator with debugging.
* `libllheap-stats.o` statically-linkable allocator with statistics.
* `libllheap-stats-debug.o` statically-linkable allocator with debugging and statistics.
* `libllheap.so` dynamically-linkable allocator with optimal performance without statistics or debugging.
* `libllheap-debug.so` dynamically-linkable allocator with debugging.
* `libllheap-stats.so` dynamically-linkable allocator with statistics.
* `libllheap-stats-debug.so` dynamically-linkable allocator with debugging and statistics.

The Makefile has building options.

* `__FASTLOOKUP__` (default) use O(1) table lookup from allocation size to bucket size for small allocations.
* `__OWNERSHIP__` (default) return freed memory to owner thread.
* `__RETURNSPIN__` (not default) use spinlock for mutual exclusion versus lockfree stack.
* `__NULL_0_ALLOC__` (default) return an allocation addresses for a 0-sized allocation rather than a null pointer.

# Memory Allocator Design

llheap is a fast concurrent (heap-per-thread) memory allocator for managed programing languages with low latency at the cost of a slightly larger memory footprints.
The implementation provides the following GNUC library routines.

		void * malloc( size_t size );
		void * calloc( size_t dimension, size_t size );
		void * realloc( void * oaddr, size_t size );
		void * reallocarray( void * oaddr, size_t dimension, size_t size );
		void free( void * addr );
		void * memalign( size_t alignment, size_t size );
		void * aligned_alloc( size_t alignment, size_t size );
		int posix_memalign( void ** memptr, size_t alignment, size_t size );
		void * valloc( size_t size );
		void * pvalloc( size_t size );
		int mallopt( int option, int value );
		size_t malloc_usable_size( void * addr );
		void malloc_stats( void );
		int malloc_info( int options, FILE * fp );

Unsupported routines.

		struct mallinfo mallinfo( void );
		int malloc_trim( size_t );
		void * malloc_get_state( void );
		int malloc_set_state( void * );

## Objective

The llheap objectives are:

* thread-safe,
* fast concurrent allocation/free with or without statistics/debugging,
* making zero-fill and alignment sticky properties preserved by realloc,
* extend semantics of existing allocator operations and provide new operations to simplify allocation and increase safety,
* achieve performance comparable to the best allocators in common use.

## Extended Features

* `malloc` remembers the original allocation size separate from the actual allocation size.
* `calloc` sets the sticky zero-fill property.
* `memalign`, `aligned_alloc`, `posix_memalign`, `valloc` and `pvalloc` set the sticky alignment property, remembering the specified alignment size.
* `realloc` and `reallocarray` preserve sticky properties across copying.
* `malloc_stats` prints detailed statistics of allocation/free operations when linked with a statistic version.
* Existence of shell variable `MALLOC_STATS` implicitly calls `malloc_stats` at program termination.

## Added Features

The following allocation features are available with llheap.

### New allocation operations

### `void * aalloc( size_t dimension, size_t elemSize )`
extends `calloc` for dynamic array allocation *without* zero-filling the memory (faster than `calloc`).

**Parameters:**

* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the dynamic array or NULL if allocation fails.

### `void * resize( void * oaddr, size_t size )`
extends `realloc` for resizing an allocation *without* copying previous data into the new allocation (faster than `realloc`).
No sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the old or new storage with the specified new size or NULL if size is zero.

### `void * resizearray( void * oaddr, size_t dimension, size_t elemSize )`
extends `resize` for an array allocation (faster than `reallocarray`).
No sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the old or new storage with the specified new size or NULL if size is zero.

### `void * amemalign( size_t alignment, size_t dimension, size_t elemSize )`
extends `aalloc` with an alignment.
Sets sticky alignment property.

**Parameters:**

* `alignment`: alignment
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the aligned dynamic array or NULL if either `dimension` or `elemSize` are zero.

### `void * cmemalign( size_t alignment, size_t dimension, size_t elemSize )`
extends `amemalign` with zero fill.
Sets sticky zero-fill and alignment property.

**Parameters:**

* `alignment`: alignment
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the aligned, zero-filled dynamic array or NULL if either dimension or `elemSize` are zero.

### `void * aligned_resize( void * oaddr, size_t nalignment, size_t size )`
extends `resize` with an alignment.
No sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size or NULL if size is zero.

### `void * aligned_resizearray( void * oaddr, size_t nalignment, size_t `dimension`, size_t elemSize )`
extends `resizearray` with an alignment.
No sticky properties are preserved.

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `dimension`: number of array objects
* `elemSize`: new size of array object (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size or NULL if the resize fails.

### `void * aligned_realloc( void * oaddr, size_t nalignment, size_t size )`
extends `realloc` with an alignment.
All sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size or NULL if size is zero.

### `void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize )`
extends `resizearray` with an alignment.
All sticky properties are preserved.

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `dimension`: number of array objects
* `elemSize`: new size of array object (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size or NULL if the resize fails.

### New control operations

These routines are called *once* during llheap startup to set specific limits *before* an application starts.
Setting these value early is essential because allocations can occur from the dynamic loader and other libraries before application code executes.
To set a value, define a specific routine in an application and return the desired value, e.g.:

		size_t malloc_extend() { return 16 * 1024 * 1024; }

### `size_t malloc_extend( void )`
return the number of bytes to extend the `sbrk` area when there is insufficient free storage to service an allocation request.

**Return:** heap extension size used throughout a program.

### `size_t malloc_mmap_start( void )`
return the crossover allocation size from the `sbrk` area to separate mapped areas.
Can be changed dynamically with `mallopt` and `M_MMAP_THRESHOLD`.

**Return:** crossover point used throughout a program.

### `size_t malloc_unfreed( void )`
return the amount subtracted from the global unfreed program storage to adjust for unreleased storage from routines like `printf` (debug only).

**Return:** new subtraction amount and called by `malloc_stats`.

### New object preserved-properties

### `size_t malloc_size( void * addr )`
returns the requested size of a dynamic object, which is updated when an object is resized. See also `malloc_usable_size`.

**Parameters:**

* `addr`: address of allocated object.

**Return:** request size or zero if `addr` is NULL.

### `size_t malloc_alignment( void * addr )`
returns the object alignment, where the minimal alignment is 16 bytes.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** alignment of the given object, where objects not allocated with alignment return the minimal allocation alignment.

### `bool malloc_zero_fill( void * addr )`
returns if the object is zero filled.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** true if the zero-fill sticky property is set and false otherwise.

### `bool malloc_remote( void * addr )`
returns if the object is from a remote heap (`OWNERSHIP` only).

**Parameters:**

* `addr`: address of an allocated object.

**Return:** true if the object belongs to another heap and false otherwise.

### New statistics control

### `int malloc_stats_fd( int fd )`
set the file descriptor for `malloc_stats` writes (default `stdout`).

**Return:** previous file descriptor

### `void malloc_stats_clear()`
clear the statistics counters for all thread heaps.

### `void heap_stats()`
extends `malloc_stats` to only print statistics for the heap associated with the executing thread.
