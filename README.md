# Install

**Requires >= g++-9**

```
$ make all
```

creates 4 shared libraries that can be linked to a program to replace the default memory allocator.

* **`libhThread.o`** statically-linkable allocator optimized for performance without statistics or debugging.
* **`libhThread-stats.o`** statically-linkable allocator optimized for debugging with statistics and debugging.
* **`libhThread.so`** dynamically-linkable allocator optimized for performance without statistics or debugging.
* **`libhThread-stats.o`** dynamically-linkable allocator optimized for debugging with statistics and debugging.

By changing the Makefile (toggle `-D__STATISTICS__` and `-D__DEBUG__`) , it is possible to independently turn on statistics, debugging or both.
By adding `-D__NONNULL_0_ALLOC__`, it is possible to get allocation addresses for a 0-sized allocation rather than a null pointer. 

# Memory Allocator Design

llheap is designed as a fast concurrent allocator with very low latency at the cost of a slightly larger memory footprints. The implementation fulfills GNUC library requirements while adding significant extensions and safety.

## Objective

The objectives of the llheap design are:

* thread-safe,
* fast concurrent allocation/free with or without statistics/debugging,
* making zero-fill and alignment sticky properties preserved by realloc,
* extend semantics of existing allocator operations and provide new operations to simplify allocation and increase safety,
* achieve performance comparable to the best allocators in common use.

## Extended Features

* `malloc` remembers the original allocation size separate from the actual allocation size
* `malloc` of zero bytes or errors (e.g., out of memory) return NULL (nullptr), rather than raising an exception (C++)
* `calloc` sets the sticky zero-fill property
* `memalign` sets the alignment sticky property, remembering the specified alignment size
* `realloc` preserved all sticky properties when moving and increasing space
* `malloc_stats` prints (default standard error) detailed statistics of all allocation/free operations. llheap must be compiled with statistic flag. Existence of shell variable LLHEAP_MALLOC_STATS implicitly calls malloc_stats at program termination.
* `malloc_stats_clear` clears all thread statistic couters. llheap must be compiled with statistic flag.

**Return:** side-effect of writing out statistics.

## Added Features

The following new allocation operations are available with llheap:

### `void * aalloc( size_t dimension, size_t elemSize )`
extends calloc for allocating a dynamic array of objects but *without* zero-filling the memory. aalloc is significantly faster than calloc.

**Parameters:**

* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the dynamic array or NULL if allocation fails.

### `void * resize( void * oaddr, size_t size )`
extends realloc for resizing an existing allocation *without* copying previous data into the new allocation or preserving sticky properties. resize is significantly faster than realloc.

**Parameters:**

* `oaddr`: address to be resized
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the old or new storage with the specified new size or NULL if size is zero.

### `void * resize( void * oaddr, size_t nalignment, size_t size )`
extends resize with an alignment requirement.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment requirement
* `size`: new allocation size (smaller or larger than previous)

**Return:** address of the old or new storage with the specified new size or NULL if the resize fails.

### `void * amemalign( size_t alignment, size_t dimension, size_t elemSize )`
extends aalloc and memalign for allocating an aligned dynamic array of objects. Sets sticky alignment property

**Parameters:**

* `alignment`: alignment requirement
* `dimension`: number of array objects
* `elemSize`: size of array object

**Return:** address of the aligned dynamic array or NULL if either dimension or elemSize are zero.

### `void * cmemalign( size_t alignment, size_t dimension, size_t elemSize )`
extends amemalign with zero fill and has the same parameters as amemalign.
Sets sticky zero-fill and alignment property.

**Return:** address of the aligned, zero-filled dynamic-array or NULL if either dimension or elemSize are zero.

### `void * aligned_resize( void * oaddr, size_t nalignment, size_t size )`
extends resize by realigning the old object to a new alignment requirement. Sets the sticky alignment property.

### `void * aligned_realloc( void * oaddr, size_t nalignment, size_t size )`
extends realloc by realigning the old object to a new alignment requirement. Sets the sticky alignment property.

**Parameters:**

* `oaddr`: address to be resized
* `nalignment`: new alignment requirement
* `size`: new allocation size (smaller or larger than previous)

### `void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize )`
extends reallocarray by realigning the old object to a new alignment requirement. Sets the sticky alignment property.

* `oaddr`: address to be resized
* `nalignment`: new alignment requirement
* `dimension`: number of array objects
* `elemSize`: new size of array object (smaller or larger than previous)

**Return:** address of the old or new storage with the specified new size or NULL if the resize fails. All sticky properties are preserved

### `size_t malloc_alignment( void * addr )`
returns the alignment of the dynamic object.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** alignment of the given object, where objects not allocated with alignment return the minimal allocation alignment.

### `bool malloc_zero_fill( void * addr )`
returns if the dynamic object is zero filled.

**Parameters:**

* `addr`: address of an allocated object.

**Return:** true if the zero-fill sticky property is set and false otherwise.

### `size_t malloc_size( void * addr )`
returns the requested size of a dynamic object, which is updated when an object is resized. See also malloc_usable_size.

**Parameters:**

* `addr`: address of allocated object.

**Return:** request size or zero if `addr` is NULL.

### `int malloc_stats_fd( int fd )`
set the file descriptor for malloc_stats() writes (default stdout).

**Return:** previous file descriptor

### `size_t malloc_expansion()`
set the amount (bytes) to extend the heap when there is insufficient free storage to service an allocation request.

**Return:** heap extension size used throughout a program, i.e., called once at heap initialization.

### `size_t malloc_mmap_start()`
set the crossover between allocations occuring in the sbrk area or separately mmapped.

**Return:** crossover point used throughout a program, i.e., called once at heap initialization.

### `size_t malloc_unfreed()`
amount subtracted to adjust for unfreed program storage (debug only).

**Return:** new subtraction amount and called by `malloc_stats`.

### `void malloc_stats_clear()`
clear the statistics counters for the master heap and all thread heaps.
