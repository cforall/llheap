# llheap -- Memory Allocator

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

* `__FASTLOOKUP__` (default) use O(1) table lookup from allocation size to bucket size for small allocations, but more storage.
* `__OWNERSHIP__` (default) return freed memory to owner thread.
* `__RETURNSPIN__` (not default) use spinlock for mutual exclusion versus lockfree stack.

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

For all routines, a zero-sized allocation returns a unique memory location that must be freed.
If an allocation cannot be fulfilled because memory is full, null is returned and `errno` is set to `ENOMEM`.

		int mallopt( int option, int value );
			option M_TOP_PAD sets the amount to extend the heap size once all the current storage in the heap is
				allocated.
			option M_MMAP_THRESHOLD sets the division point after which allocation requests are separately
				memory mapped rather than being allocated from the heap area.
		size_t malloc_usable_size( void * addr );
		void malloc_stats( void );
		int malloc_info( int options, FILE * fp );

Unsupported routines.

		struct mallinfo mallinfo( void );
		int malloc_trim( size_t );
		void * malloc_get_state( void );
		int malloc_set_state( void * );

## Objectives

* low-latency => no delays in the allocator, only delays accessing the operating system to acquire storage.
* thread-safe,
* fast concurrent allocation/deallocation with or without statistics/debugging,
* making zero-fill and alignment sticky properties preserved by realloc,
* extend semantics of existing allocator operations and provide new operations to simplify allocation and increase safety,
* achieve performance comparable to the best allocators in common use.

## Extended Features

* `malloc` remembers the original allocation size separate from the actual allocation size.
* `calloc` sets the sticky zero-fill property.
* `memalign`, `aligned_alloc`, `posix_memalign`, `valloc` and `pvalloc` set the sticky alignment property, remembering the specified alignment size.
* `realloc` and `reallocarray` preserve sticky properties across copying.
* `malloc_stats` prints detailed statistics of allocation/free operations when linked with a statistic version.
* Existence of shell variable `MALLOC_STATS` implicitly calls `malloc_stats` at program termination. If `MALLOC_STATS=1`, allocation-bucket information is printed.

## Added Features

New POSIX-style `realloc` routines are available to handle the failure case `p = realloc( p, size )`, when there is insufficient memory.
Here, the original storage is leaked when pointer `p` is overwritten with null, negating the benefit of not freeing the storage on failure for recovery purposes.

### New allocation operations

The following allocation features require including `llheap.h`.

#### `void * resize( void * oaddr, size_t size )`
equivalent to `realloc( oaddr, size )` to repurpose a prior allocation for a new type *without* copying the previous data into the new allocation or preserving zero fill or alignment (faster than `realloc`).
If a new allocation fails, the original storage is always freed (unlike `realloc`).

**Parameters:**

* `oaddr`: address to be resized
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the old or new storage with the specified new size.

#### `void * resizearray( void * oaddr, size_t dimension, size_t elemSize )`
equivalent to `resize( oaddr, dimension * elemSize )` for a new array-type allocation *without* copying previous data into the new allocation or preserving zero fill or alignment (faster than `reallocarray`).
If a new allocation fails, the original storage is always freed (unlike `realloc`).

**Parameters:**

* `oaddr`: address to be resized
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the old or new storage with the specified new size.

#### `int posix_realloc( void ** oaddrp, size_t size )`
equivalent to `oaddrp = realloc( *oaddrp, size )`.
Sticky properties are preserved.
Requires an ugly cast: `int ret = posix_realloc( (void **)&p, size )`.

**Parameters:**

* `oaddrp`: address of the address to be realloced
* `size`: new allocation size (smaller or larger than previous)

**Return:** On success, directly returns 0 and indirectly the address of the new storage though output parameter `oaddrp`.
On failure, directly returns 0 or `ENOMEM`, and `oaddrp` and `errno` are not set.

#### `int posix_reallocarray( void ** oaddrp, size_t dimension, size_t elemSize )`
equivalent to `posix_realloc( oaddrp, dimension * elemSize )` for an array allocation.
Sticky properties are preserved.

**Parameters:**

* `oaddrp`: address of the address to be realloced
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** On success, directly returns 0 and indirectly the address of the new storage though output parameter `oaddrp`.
On failure, directly returns 0 or `ENOMEM`, and `oaddrp` and `errno` are not set.

#### `void * aalloc( size_t dimension, size_t elemSize )`
equivalent to `malloc( dimension * elemSize )` *without* zero filling the memory (faster than `calloc`).
No sticky properties are set.

**Parameters:**

* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the dynamic array.

#### `void * amemalign( size_t alignment, size_t dimension, size_t elemSize )`
equivalent to `memalign( alignment, dimension * elemSize )` for array alignment.
Sets sticky alignment property.

**Parameters:**

* `alignment`: alignment
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the aligned dynamic array.

#### `void * cmemalign( size_t alignment, size_t dimension, size_t elemSize )`
equivalent to `memset( amemalign( alignment, dimension, elemSize ), '\0', dimension * elemsize )` for array with zero fill.
Sets sticky zero-fill and alignment property.

**Parameters:**

* `alignment`: alignment
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the aligned, zero-filled dynamic array.

#### `void * aligned_resize( void * oaddr, size_t nalignment, size_t size )`
equivalent to `resize( oaddr, size )` with an alignment.
No sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size.

#### `void * aligned_resizearray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize )`
equivalent to `resizearray( oaddr, dimension, elemSize )` with an alignment.
No sticky properties are preserved.

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `dimension`: number of array objects
* `elemSize`: new size of array object (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size.

#### `void * aligned_realloc( void * oaddr, size_t nalignment, size_t size )`
equivalent to `realloc( oaddr, size )` with an alignment.
Sticky properties are preserved.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size.

#### `void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize )`
equivalent to `reallocarray( oaddr, dimension, elemSize )` with an alignment.
Sticky properties are preserved.

* `oaddr`: address to be resized
* `nalignment`: new alignment
* `dimension`: number of array objects
* `elemSize`: new size of array object (smaller or larger than previous)

**Return:** address of the aligned old or new storage with the specified new size.

#### `int aligned_posix_realloc( void ** oaddrp, size_t nalignment, size_t size )`
equivalent to `posix_resizearray( oaddrp, dimension, elemSize )` with an alignment.
Sticky properties are preserved.

**Parameters:**

* `oaddrp`: address of the address to be realloced
* `nalignment`: new alignment
* `size`: new allocation size (smaller or larger than previous)

**Return:** On success, directly returns 0 and indirectly the address of the new storage though output parameter `oaddrp`.
On failure, directly returns 0 or `ENOMEM`, and `oaddrp` and `errno` are not set.

#### `void * aligned_posix_reallocarray( void * oaddrp, size_t nalignment, size_t dimension, size_t elemSize )`
equivalent to `posix_reallocarray( oaddr, dimension, elemSize )` with an alignment.
Sticky properties are preserved.

**Parameters:**

* `oaddrp`: address of the address to be realloced
* `nalignment`: new alignment
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** On success, directly returns 0 and indirectly the address of the new storage though output parameter `oaddrp`.
On failure, directly returns 0 or `ENOMEM`, and `oaddrp` and `errno` are not set.

### New object preserved-properties

#### `size_t malloc_size( void * addr )`
returns the requested size of a dynamic object, which is updated when an object is resized. See also `malloc_usable_size`.

**Parameters:**

* `addr`: address of allocated object.

**Return:** request size or zero if `addr` is null.

#### `size_t malloc_alignment( void * addr )`
returns the object alignment, where the minimal alignment is 16 bytes, e.g., by `memalign`/`cmemalign`/etc.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** alignment of the given object, where objects not allocated with alignment return the minimal allocation alignment.

#### `bool malloc_zero_fill( void * addr )`
returns if the object is zero filled, e.g., by `calloc`/`cmemalign`.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** true if the zero-fill sticky property is set and false otherwise.

#### `bool malloc_remote( void * addr )`
returns if the object is from a remote heap (`OWNERSHIP` only).

**Parameters:**

* `addr`: address of an allocated object.

**Return:** true if the object belongs to another heap and false otherwise.

### New statistics control

#### `bool malloc_stats_all( bool state )`
set state true means print information about heap buckets when `malloc_stats` is called.
Default is false: do not print bucket information.

**Return:** previous statistics all state.

#### `int malloc_stats_fd( int fd )`
set the file descriptor for `malloc_stats` writes (default `stdout`).

**Return:** previous file descriptor

#### `void malloc_stats_clear( void )`
clear the statistics counters for all thread heaps.

#### `void heap_stats( void )`
extends `malloc_stats` to only print statistics for the heap associated with the executing thread.

### New control operations

These routines are called *once* during llheap startup to set specific limits *before* an application starts.
Setting these value early is essential because allocations can occur from the dynamic loader and other libraries before application code executes.
To set a value, define a specific routine in an application and return the desired value, e.g.:

		size_t malloc_extend( void ) { return 16 * 1024 * 1024; }  // bytes

#### `size_t malloc_extend( void )`
return the number of bytes to extend the `sbrk` area when there is insufficient free storage to service an allocation request.

**Return:** heap extension size used throughout a program.

#### `size_t malloc_mmap_start( void )`
return the crossover allocation size from the `sbrk` area to separate mapped areas.
Can be changed dynamically with `mallopt` and `M_MMAP_THRESHOLD`.

**Return:** crossover point used throughout a program.

#### `size_t malloc_unfreed( void )`
return the amount subtracted from the global unfreed program storage to adjust for unreleased storage from routines like `printf` (debug only).

**Return:** new subtraction amount and called by `malloc_stats`.

### New backtrace

When an application fails, a stack backtrace is printed for debug.
Use compilation flag `-rdynamic` to get symbolic names printed.

