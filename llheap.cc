#include "llheap.h"

#include <new>											// set_new_handler
#include <cstdlib>										// abort
#include <cstring>										// strlen, memset, memcpy
#include <climits>										// ULONG_MAX
#include <cstdarg>										// va_start, va_end
#include <cerrno>										// errno, ENOMEM, EINVAL
#include <cassert>
#include <cstdint>										// uintptr_t, uint64_t, uint32_t
#include <unistd.h>										// STDERR_FILENO, sbrk, sysconf, write
#include <sys/mman.h>									// mmap, munmap
#include <sys/sysinfo.h>								// get_nprocs

#define FASTLOOKUP										// use O(1) table lookup from allocation size to bucket size
#define OWNERSHIP										// return freed memory to owner thread
#define RETURNSPIN										// toggle spinlock / lockfree queue
#if ! defined( OWNERSHIP ) && defined( RETURNSPIN )
#warning "RETURNSPIN is ignored without OWNERSHIP; suggest commenting out RETURNSPIN"
#endif // ! OWNERSHIP && RETURNSPIN

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define str(s) #s
#define xstr(s) str(s)
#define WARNING( s ) xstr( GCC diagnostic ignored str( -W ## s ) )
#define NOWARNING( statement, warning ) \
	_Pragma( "GCC diagnostic push" ) \
	_Pragma( WARNING( warning ) ) \
	statement ;	\
	_Pragma ( "GCC diagnostic pop" )

#define CACHE_ALIGN 64
#define CALIGN __attribute__(( aligned(CACHE_ALIGN) ))

#ifdef TLS
#define TLSMODEL __attribute__(( tls_model("initial-exec") ))
#else
#define TLSMODEL
#endif // TLS

enum {
	// The minimum allocation alignment in bytes.
	__ALIGN__ = __BIGGEST_ALIGNMENT__,

	// The default extension heap amount in units of bytes. When the current heap reaches the brk address, the brk
	// address is extended by the extension amount.
	__DEFAULT_HEAP_EXPANSION__ = 10 * 1024 * 1024,

	// The mmap crossover point during allocation. Allocations less than this amount are allocated from buckets; values
	// greater than or equal to this value are mmap from the operating system.
	__DEFAULT_MMAP_START__ = 512 * 1024 + 1,

	// The default unfreed storage amount in units of bytes. When the uC++ program ends it subtracts this amount from
	// the malloc/free counter to adjust for storage the program does not free.
	__DEFAULT_HEAP_UNFREED__ = 0
}; // enum


//######################### Helpers #########################


// Called by macro assert in assert.h. Replace to prevent recursive call to malloc.
void __assert_fail( const char assertion[], const char file[], unsigned int line, const char function[] ) {
	extern const char * __progname;						// global name of running executable (argv[0])
	char helpText[1024];
	int len = snprintf( helpText, sizeof(helpText), "Internal assertion error \"%s\" from program \"%s\" in \"%s\" at line %d in file \"%s.\n",
						assertion, __progname, function, line, file );
	if ( write( STDERR_FILENO, helpText, len ) == -1 ) {} // fallthrough
	abort();
	// CONTROL NEVER REACHES HERE!
} // __assert_fail

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

//#define __DEBUG_PRT__
static void debug( const char fmt[], ... ) __attribute__(( format(printf, 1, 2), unused ));
static void debug( const char fmt[] __attribute__(( unused )), ... ) {
#ifdef __DEBUG_PRT__
	va_list args;
	va_start( args, fmt );
	enum { BufSize = 128 };
	char buf[BufSize];
	int len = vsnprintf( buf, BufSize, fmt, args );
	if ( write( 2, buf, len ) == -1 ) abort( "**** Error **** write failed" );
	va_end( args );
#else
#endif // __DEBUG_PRT__
} // debug

static inline __attribute__((always_inline)) bool Pow2( unsigned long int value ) {
	// clears all bits below value, rounding value down to the next lower multiple of value
	return (value & (value - 1)) == 0;
} // Pow2

static inline __attribute__((always_inline)) unsigned long int Floor( unsigned long int value, unsigned long int align ) {
	assert( Pow2( align ) );
	// clears all bits above or equal to align, getting (value % align), the phase of value with regards to align
	return value & -align;
} // Floor

static inline __attribute__((always_inline)) unsigned long int Ceiling( unsigned long int value, unsigned long int align ) {
	assert( Pow2( align ) );
	// "negate, round down, negate" is the same as round up
	return -Floor( -value, align );
} // Ceiling

template< typename T > static inline __attribute__((always_inline)) T AtomicFetchAdd( volatile T & counter, int increment ) {
	return __atomic_fetch_add( &counter, increment, __ATOMIC_SEQ_CST );
} // AtomicFetchAdd


// std::min and std::lower_bound do not always inline, so substitute hand-coded versions.

#define Min( x, y ) (x < y ? x : y)

inline __attribute__((always_inline))
static size_t Bsearchl( unsigned int key, const unsigned int vals[], size_t dim ) {
	size_t l = 0, m, h = dim;
	while ( l < h ) {
		m = (l + h) / 2;
		if ( (unsigned int &)(vals[m]) < key ) {		// cast away const
			l = m + 1;
		} else {
			h = m;
		} // if
	} // while
	return l;
} // Bsearchl


// pause to prevent excess processor bus usage
#if defined( __i386 ) || defined( __x86_64 )
	#define Pause() __asm__ __volatile__ ( "pause" : : : )
#elif defined(__ARM_ARCH)
	#define Pause() __asm__ __volatile__ ( "YIELD" : : : )
#else
	#error unsupported architecture
#endif

typedef volatile uintptr_t SpinLock_t;

static inline __attribute__((always_inline)) void spin_acquire( volatile SpinLock_t * lock ) {
	enum { SPIN_START = 4, SPIN_END = 64 * 1024, };
	unsigned int spin = SPIN_START;

	for ( unsigned int i = 1;; i += 1 ) {
	  if ( *lock == 0 && __atomic_test_and_set( lock, __ATOMIC_ACQUIRE ) == 0 ) break; // Fence
		for ( volatile unsigned int s = 0; s < spin; s += 1 ) Pause(); // exponential spin
		spin += spin;									// powers of 2
		//if ( i % 64 == 0 ) spin += spin;				// slowly increase by powers of 2
		if ( spin > SPIN_END ) spin = SPIN_END;			// cap spinning
	} // for
} // spin_lock

static inline __attribute__((always_inline)) void spin_release( volatile SpinLock_t * lock ) {
	__atomic_clear( lock, __ATOMIC_RELEASE );			// Fence
} // spin_unlock


//####################### Heap Statistics ####################


#ifdef __STATISTICS__
enum { CntTriples = 12 };								// number of counter triples
struct HeapStatistics {
	enum { MALLOC, AALLOC, CALLOC, MEMALIGN, AMEMALIGN, CMEMALIGN, RESIZE, REALLOC, FREE };
	union {
		struct {										// minimum qualification
			unsigned int malloc_calls, malloc_0_calls;
			unsigned long long int malloc_storage_request, malloc_storage_alloc;
			unsigned int aalloc_calls, aalloc_0_calls;
			unsigned long long int aalloc_storage_request, aalloc_storage_alloc;
			unsigned int calloc_calls, calloc_0_calls;
			unsigned long long int calloc_storage_request, calloc_storage_alloc;
			unsigned int memalign_calls, memalign_0_calls;
			unsigned long long int memalign_storage_request, memalign_storage_alloc;
			unsigned int amemalign_calls, amemalign_0_calls;
			unsigned long long int amemalign_storage_request, amemalign_storage_alloc;
			unsigned int cmemalign_calls, cmemalign_0_calls;
			unsigned long long int cmemalign_storage_request, cmemalign_storage_alloc;
			unsigned int resize_calls, resize_0_calls;
			unsigned long long int resize_storage_request, resize_storage_alloc;
			unsigned int realloc_calls, realloc_0_calls;
			unsigned long long int realloc_storage_request, realloc_storage_alloc;
			unsigned int free_calls, free_null_0_calls;
			unsigned long long int free_storage_request, free_storage_alloc;
			unsigned int return_pulls, return_pushes;
			unsigned long long int return_storage_request, return_storage_alloc;
			unsigned int mmap_calls, mmap_0_calls;		// no zero calls
			unsigned long long int mmap_storage_request, mmap_storage_alloc;
			unsigned int munmap_calls, munmap_0_calls;	// no zero calls
			unsigned long long int munmap_storage_request, munmap_storage_alloc;
		};
		struct {										// overlay for iteration
			unsigned int calls, calls_0;
			unsigned long long int request, alloc;
		} counters[CntTriples];
	};
}; // HeapStatistics

static_assert( sizeof(HeapStatistics) == CntTriples * sizeof(HeapStatistics::counters[0]),
			   "Heap statistics counter-triplets does not match with array size" );

static void HeapStatisticsCtor( HeapStatistics & stats ) {
	memset( &stats, '\0', sizeof(stats) );				// very fast
	// for ( unsigned int i = 0; i < CntTriples; i += 1 ) {
	// 	stats.counters[i].calls = stats.counters[i].calls_0 = stats.counters[i].request = stats.counters[i].alloc = 0;
	// } // for
} // HeapStatisticsCtor

static HeapStatistics & operator+=( HeapStatistics & lhs, const HeapStatistics & rhs ) {
	for ( unsigned int i = 0; i < CntTriples; i += 1 ) {
		lhs.counters[i].calls += rhs.counters[i].calls;
		lhs.counters[i].calls_0 += rhs.counters[i].calls_0;
		lhs.counters[i].request += rhs.counters[i].request;
		lhs.counters[i].alloc += rhs.counters[i].alloc;
	} // for
	return lhs;
} // ::operator+=
#endif // __STATISTICS__


//####################### Heap Structure ####################


struct Heap {
	struct FreeHeader;									// forward declaration

	struct Storage {
		struct Header {									// header
			union Kind {
				struct RealHeader {						// 4-byte word => 8-byte header, 8-byte word => 16-byte header
					union {
						// 2nd low-order bit => zero filled, 3rd low-order bit => mmapped
						FreeHeader * home;				// allocated block points back to home locations (must overlay alignment)
						size_t blockSize;				// size for munmap (must overlay alignment)
						Storage * next;					// freed block points to next freed block of same size
					};
					size_t size;						// allocation size in bytes
				} real; // RealHeader

				struct FakeHeader {
					uintptr_t alignment;				// 1st low-order bit => fake header & alignment
					uintptr_t offset;
				} fake; // FakeHeader
			} kind; // Kind
		} header; // Header

		char pad[__ALIGN__ - sizeof( Header )];
		char data[0];									// storage
	}; // Storage

	static_assert( __ALIGN__ >= sizeof( Storage ), "minimum alignment < sizeof( Storage )" );

	struct CALIGN FreeHeader {
		#ifdef OWNERSHIP
		#ifdef RETURNSPIN
		SpinLock_t returnLock;
		#endif // RETURNSPIN
		Storage * returnList;							// other thread return list
		#endif // OWNERSHIP

		Storage * freeList;								// thread free list
		Heap * homeManager;								// heap owner (free storage to bucket, from bucket to heap)
		size_t blockSize;								// size of allocations on this list
	}; // FreeHeader

	// Recursive definitions: HeapManager needs size of bucket array and bucket area needs sizeof HeapManager storage.
	// Break recursion by hardcoding number of buckets and statically checking number is correct after bucket array defined.
	enum { NoBucketSizes = 91 };						// number of bucket sizes

	FreeHeader freeLists[NoBucketSizes];				// buckets for different allocation sizes
	void * heapBuffer;									// start of free storage in buffer
	size_t heapReserve;									// amount of remaining free storage in buffer

	#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
	Heap * nextHeapManager;								// intrusive link of existing heaps; traversed to collect statistics or check unfreed storage
	#endif // __STATISTICS__ || __DEBUG__
	Heap * nextFreeHeapManager;							// intrusive link of free heaps from terminated threads; reused by new threads

	#ifdef __DEBUG__
	ptrdiff_t allocUnfreed;								// running total of allocations minus frees; can be negative
	#endif // __DEBUG__

	#ifdef __STATISTICS__
	HeapStatistics stats;								// local statistic table for this heap
	#endif // __STATISTICS__
}; // Heap


static void heapManagerCtor();
static void heapManagerDtor();


namespace {												// hide static members
	struct ThreadManager {
		volatile bool pgm_thread;						// used to trigger allocation of storage and indicate program thread
		~ThreadManager() { if ( pgm_thread ) heapManagerDtor(); } // called automagically when thread terminates
	}; // ThreadManager

	struct HeapMaster {
		SpinLock_t extLock;								// protects allocation-buffer extension
		SpinLock_t mgrLock;								// protects freeHeapManagersList, heapManagersList, heapManagersStorage, heapManagersStorageEnd

		void * heapBegin;								// start of heap
		void * heapEnd;									// logical end of heap
		size_t heapRemaining;							// amount of storage not allocated in the current chunk
		size_t pageSize;								// architecture pagesize
		size_t heapExpand;								// sbrk advance
		size_t mmapStart;								// cross over point for mmap
		unsigned int maxBucketsUsed;					// maximum number of buckets in use

		Heap * heapManagersList;						// heap-list head
		Heap * freeHeapManagersList;					// free-list head

		// Heap superblocks are not linked; heaps in superblocks are linked via intrusive links.
		Heap * heapManagersStorage;						// next heap to use in heap superblock
		Heap * heapManagersStorageEnd;					// logical heap outside of superblock's end

		#ifdef __DEBUG__
		ptrdiff_t allocUnfreed;							// running total of allocations minus frees; can be negative
		#endif // __DEBUG__

		#ifdef __STATISTICS__
		HeapStatistics stats;							// global stats for thread-local heaps to add there counters when exiting
		unsigned long int threads_started, threads_exited; // counts threads that have started and exited
		unsigned long int reused_heap, new_heap;		// counts reusability of heaps
		unsigned int sbrk_calls;
		unsigned long long int sbrk_storage;
		int stats_fd;
		#endif // __STATISTICS__

		static void heapMasterCtor();
		static Heap * getHeap();
	}; // HeapMaster
} // namespace


#ifdef FASTLOOKUP
enum { LookupSizes = 65'536 + sizeof(Heap::Storage) };	// number of fast lookup sizes
static unsigned char lookup[LookupSizes];				// O(1) lookup for small sizes
#endif // FASTLOOKUP

static volatile bool heapMasterBootFlag = false;		// trigger for first heap
static HeapMaster heapMaster;							// program global


// Size of array must harmonize with NoBucketSizes and individual bucket sizes must be multiple of 16.
// Smaller multiples of 16 and powers of 2 are common allocation sizes, so make them generate the minimum required bucket size.
// malloc(0) returns nullptr, so no bucket is necessary for 0 bytes returning an address that can be freed.
static const unsigned int bucketSizes[] = {				// different bucket sizes
	16 + sizeof(Heap::Storage), 32 + sizeof(Heap::Storage), 48 + sizeof(Heap::Storage), 64 + sizeof(Heap::Storage), // 4
	96 + sizeof(Heap::Storage), 112 + sizeof(Heap::Storage), 128 + sizeof(Heap::Storage), // 3
	160, 192, 224, 256 + sizeof(Heap::Storage), // 4
	320, 384, 448, 512 + sizeof(Heap::Storage), // 4
	640, 768, 896, 1'024 + sizeof(Heap::Storage), // 4
	1'536, 2'048 + sizeof(Heap::Storage), // 2
	2'560, 3'072, 3'584, 4'096 + sizeof(Heap::Storage), // 4
	6'144, 8'192 + sizeof(Heap::Storage), // 2
	9'216, 10'240, 11'264, 12'288, 13'312, 14'336, 15'360, 16'384 + sizeof(Heap::Storage), // 8
	18'432, 20'480, 22'528, 24'576, 26'624, 28'672, 30'720, 32'768 + sizeof(Heap::Storage), // 8
	36'864, 40'960, 45'056, 49'152, 53'248, 57'344, 61'440, 65'536 + sizeof(Heap::Storage), // 8
	73'728, 81'920, 90'112, 98'304, 106'496, 114'688, 122'880, 131'072 + sizeof(Heap::Storage), // 8
	147'456, 163'840, 180'224, 196'608, 212'992, 229'376, 245'760, 262'144 + sizeof(Heap::Storage), // 8
	294'912, 327'680, 360'448, 393'216, 425'984, 458'752, 491'520, 524'288 + sizeof(Heap::Storage), // 8
	655'360, 786'432, 917'504, 1'048'576 + sizeof(Heap::Storage), // 4
	1'179'648, 1'310'720, 1'441'792, 1'572'864, 1'703'936, 1'835'008, 1'966'080, 2'097'152 + sizeof(Heap::Storage), // 8
	2'621'440, 3'145'728, 3'670'016, 4'194'304 + sizeof(Heap::Storage), // 4
};

static_assert( Heap::NoBucketSizes == sizeof(bucketSizes) / sizeof(bucketSizes[0] ), "size of bucket array wrong" );


// Thread-local storage is allocated lazily when the storage is accessed.
static __thread size_t PAD1 CALIGN TLSMODEL __attribute__(( unused )); // protect false sharing
static thread_local ThreadManager threadManager CALIGN TLSMODEL;
static __thread Heap * heapManager CALIGN TLSMODEL;
#ifndef OWNERSHIP
static __thread Heap * shadow_heap CALIGN TLSMODEL;
#endif // ! OWNERSHIP
static __thread bool heapManagerBootFlag CALIGN TLSMODEL = false;
static __thread size_t PAD2 CALIGN TLSMODEL __attribute__(( unused )); // protect further false sharing


// declare helper functions for HeapMaster
void noMemory();										// forward, called by "builtin_new" when malloc returns 0


void HeapMaster::heapMasterCtor() {
	// Singleton pattern to initialize heap master

	assert( bucketSizes[0] == (16 + sizeof(Heap::Storage)) );

	heapMaster.pageSize = sysconf( _SC_PAGESIZE );

	heapMaster.extLock = 0;
	heapMaster.mgrLock = 0;

	char * end = (char *)sbrk( 0 );
	heapMaster.heapBegin = heapMaster.heapEnd = sbrk( (char *)Ceiling( (long unsigned int)end, __ALIGN__ ) - end ); // move start of heap to multiple of alignment
	heapMaster.heapRemaining = 0;
	heapMaster.heapExpand = malloc_expansion();
	heapMaster.mmapStart = malloc_mmap_start();

	// find the closest bucket size less than or equal to the mmapStart size
	heapMaster.maxBucketsUsed = Bsearchl( heapMaster.mmapStart, bucketSizes, Heap::NoBucketSizes ); // binary search

	assert( (heapMaster.mmapStart >= heapMaster.pageSize) && (bucketSizes[Heap::NoBucketSizes - 1] >= heapMaster.mmapStart) );
	assert( heapMaster.maxBucketsUsed < Heap::NoBucketSizes ); // subscript failure ?
	assert( heapMaster.mmapStart <= bucketSizes[heapMaster.maxBucketsUsed] ); // search failure ?

	heapMaster.heapManagersList = nullptr;
	heapMaster.freeHeapManagersList = nullptr;

	heapMaster.heapManagersStorage = nullptr;
	heapMaster.heapManagersStorageEnd = nullptr;

	#ifdef __STATISTICS__
	HeapStatisticsCtor( heapMaster.stats );				// clear statistic counters
	heapMaster.threads_started = heapMaster.threads_exited = 0;
	heapMaster.reused_heap = heapMaster.new_heap = 0;
	heapMaster.sbrk_calls = heapMaster.sbrk_storage = 0;
	heapMaster.stats_fd = STDERR_FILENO;
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	debug( "MCtor %jd set to zero\n", heapMaster.allocUnfreed );
	heapMaster.allocUnfreed = 0;
	#endif // __DEBUG__

	#ifdef FASTLOOKUP
	for ( unsigned int i = 0, idx = 0; i < LookupSizes; i += 1 ) {
		if ( i > bucketSizes[idx] ) idx += 1;
		lookup[i] = idx;
		assert( i <= bucketSizes[idx] );
		assert( (i <= 32 && idx == 0) || (i > bucketSizes[idx - 1]) );
	} // for
	#endif // FASTLOOKUP

	std::set_new_handler( noMemory );					// do not throw exception as the default

	heapMasterBootFlag = true;
} // HeapMaster::heapMasterCtor


#define NO_MEMORY_MSG "**** Error **** insufficient heap memory available to allocate %zd new bytes."

Heap * HeapMaster::getHeap() {
	Heap * heap;
	if ( heapMaster.freeHeapManagersList ) {			// free heap for reused ?
		heap = heapMaster.freeHeapManagersList;
		heapMaster.freeHeapManagersList = heap->nextFreeHeapManager;

		#ifdef __STATISTICS__
		heapMaster.reused_heap += 1;
		#endif // __STATISTICS__
	} else {											// free heap not found, create new
		// Heap size is about 12K, FreeHeader (128 bytes because of cache alignment) * NoBucketSizes (91) => 128 heaps *
		// 12K ~= 120K byte superblock.  Where 128-heap superblock handles a medium sized multi-processor server.
		size_t remaining = heapMaster.heapManagersStorageEnd - heapMaster.heapManagersStorage; // remaining free heaps in superblock
		if ( ! heapMaster.heapManagersStorage || remaining == 0 ) {
			// Each block of heaps is a multiple of the number of cores on the computer.
			int HeapDim = get_nprocs();					// get_nprocs_conf does not work
			size_t size = HeapDim * sizeof( Heap );

			heapMaster.heapManagersStorage = (Heap *)mmap( 0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
			if ( UNLIKELY( heapMaster.heapManagersStorage == MAP_FAILED ) ) { // failed ?
				if ( errno == ENOMEM ) abort( NO_MEMORY_MSG, size ); // no memory
				// Do not call strerror( errno ) as it may call malloc.
				abort( "**** Error **** attempt to allocate block of heaps of size %zu bytes and mmap failed with errno %d.", size, errno );
			} // if
			heapMaster.heapManagersStorageEnd = &heapMaster.heapManagersStorage[HeapDim]; // outside array
		} // if

		heap = heapMaster.heapManagersStorage;
		heapMaster.heapManagersStorage = heapMaster.heapManagersStorage + 1; // bump next heap

		#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
		heap->nextHeapManager = heapMaster.heapManagersList;
		#endif // __STATISTICS__ || __DEBUG__
		heapMaster.heapManagersList = heap;

		#ifdef __STATISTICS__
		heapMaster.new_heap += 1;
		#endif // __STATISTICS__

		for ( unsigned int j = 0; j < Heap::NoBucketSizes; j += 1 ) { // initialize free lists
			heap->freeLists[j] = (Heap::FreeHeader){
				#ifdef OWNERSHIP
				#ifdef RETURNSPIN
				.returnLock = 0,
				.returnList = nullptr,
				#endif // RETURNSPIN
				#endif // OWNERSHIP

				.freeList = nullptr,
				.homeManager = heap,
				.blockSize = bucketSizes[j],
			};
		} // for

		heap->heapBuffer = nullptr;
		heap->heapReserve = 0;
		heap->nextFreeHeapManager = nullptr;
		#ifdef __DEBUG__
		heap->allocUnfreed = 0;
		#endif // __DEBUG__
		heapManagerBootFlag = true;
	} // if

	return heap;
} // HeapMaster::getHeap


static void heapManagerCtor() {
	// Trigger thread_local storage implicit allocation (causes recursive call) and identify program thread because
	// heapMasterBootFlag is false.
	threadManager.pgm_thread = heapMasterBootFlag;

	if ( UNLIKELY( ! heapMasterBootFlag ) ) HeapMaster::heapMasterCtor();

	spin_acquire( &heapMaster.mgrLock );				// protect heapMaster counters

  if ( heapManagerBootFlag ) {							// singleton
		spin_release( &heapMaster.mgrLock );
		return;											// always return on recursive initiation
	} // if

	assert( ! heapManagerBootFlag );

	// get storage for heap manager

	heapManager = HeapMaster::getHeap();

	#ifdef __STATISTICS__
	HeapStatisticsCtor( heapManager->stats );			// heap local
	heapMaster.threads_started += 1;
	#endif // __STATISTICS__

	spin_release( &heapMaster.mgrLock );
} // heapManagerCtor


static void heapManagerDtor() {
  if ( UNLIKELY( ! heapManagerBootFlag ) ) return;		// thread never used ?

	spin_acquire( &heapMaster.mgrLock );

	// place heap on list of free heaps for reusability
	heapManager->nextFreeHeapManager = heapMaster.freeHeapManagersList;
	heapMaster.freeHeapManagersList = heapManager;

	#ifdef __DEBUG__
	debug( "HDtor %p %jd %jd\n", heapManager, heapManager->allocUnfreed, heapMaster.allocUnfreed );
	#endif // __DEBUG__

	#ifdef __STATISTICS__
	heapMaster.stats += heapManager->stats;				// retain this heap's statistics
	heapMaster.threads_exited += 1;
	#endif // __STATISTICS__

	// SKULLDUGGERY: The thread heap ends BEFORE the last free(s) occurs from the thread-local storage allocations for
	// the thread. This final allocation must be handled in doFree for this thread and its terminated heap. However,
	// this heap has just been put on the heap freelist, and hence there is a race returning the thread-local storage
	// and a new thread using this heap. The current thread detects it is executing its last free in doFree via
	// heapManager being null. The trick is for this thread to place the last free onto the current heap's return-list as
	// the free-storage header points are this heap. Now, even if other threads are pushing to the return list, it is safe
	// because of the locking.
	#ifndef OWNERSHIP
	shadow_heap = heapManager;
	#endif // ! OWNERSHIP
	heapManager = nullptr;

	heapManagerBootFlag = false;
	spin_release( &heapMaster.mgrLock );
} // heapManagerDtor


//####################### Memory Allocation Routines Helpers ####################


NOWARNING( __attribute__(( constructor( 100 ) )) static void startup( void ) {, prio-ctor-dtor )
	if ( ! heapMasterBootFlag ) heapManagerCtor();		// sanity check
	#ifdef __DEBUG__
	heapManager->allocUnfreed = 0;						// clear prior allocation counts
	#endif // __DEBUG__
} // startup

NOWARNING( __attribute__(( destructor( 100 ) )) static void shutdown( void ) {, prio-ctor-dtor )
	#ifdef __STATISTICS__
	if ( getenv( "LLHEAP_MALLOC_STATS" ) ) {			// check for external printing
		malloc_stats();
	} // if
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	// allocUnfreed is set to 0 when a heap is created and it accumulates any unfreed storage during its multiple thread
	// usages.  At the end, add up each heap allocUnfreed value across all heaps to get the total unfreed storage.
	ptrdiff_t allocUnfreed = heapMaster.allocUnfreed;
	debug( "shutdown1 %jd\n", heapMaster.allocUnfreed );
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		debug( "shutdown2 %p %jd\n", heap, heap->allocUnfreed );
		allocUnfreed += heap->allocUnfreed;
	} // for

	allocUnfreed -= malloc_unfreed();					// subtract any user specified unfreed storage
	debug( "shutdown3 %td %zd\n", allocUnfreed, malloc_unfreed() );
	if ( allocUnfreed > 0 ) {
		// DO NOT USE STREAMS AS THEY MAY BE UNAVAILABLE AT THIS POINT.
		char helpText[512];
		int len = snprintf( helpText, sizeof(helpText), "**** Warning **** (UNIX pid:%ld) : program terminating with %td(%#tx) bytes of storage allocated but not freed.\n"
							"Possible cause is unfreed storage allocated by the program or system/library routines called from the program.\n",
							(long int)getpid(), allocUnfreed, allocUnfreed ); // always print the UNIX pid
		if ( write( STDERR_FILENO, helpText, len ) == -1 ) abort( "**** Error **** write error in shutdown" );
	} // if
	#endif // __DEBUG__
} // shutdown


#ifdef __STATISTICS__
#define prtFmt \
	"\nHeap statistics: (storage request / allocation)\n" \
	"  malloc    >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  aalloc    >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  calloc    >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  memalign  >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  amemalign >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  cmemalign >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  resize    >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  realloc   >0 calls %'u; 0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  free      !null calls %'u; null/0 calls %'u; storage %'llu / %'llu bytes\n" \
	"  return    pulls %'u; pushes %'u; storage %'llu / %'llu bytes\n" \
	"  sbrk      calls %'u; storage %'llu bytes\n" \
	"  mmap      calls %'u; storage %'llu / %'llu bytes\n" \
	"  munmap    calls %'u; storage %'llu / %'llu bytes\n" \
	"  threads   started %'lu; exited %'lu\n" \
	"  heaps     new %'lu; reused %'lu\n"

// Use "write" because streams may be shutdown when calls are made.
static int printStats( HeapStatistics & stats ) {		// see malloc_stats
	char helpText[sizeof(prtFmt) + 1024];				// space for message and values
	int len = snprintf( helpText, sizeof(helpText), prtFmt,
			stats.malloc_calls, stats.malloc_0_calls, stats.malloc_storage_request, stats.malloc_storage_alloc,
			stats.aalloc_calls, stats.aalloc_0_calls, stats.aalloc_storage_request, stats.aalloc_storage_alloc,
			stats.calloc_calls, stats.calloc_0_calls, stats.calloc_storage_request, stats.calloc_storage_alloc,
			stats.memalign_calls, stats.memalign_0_calls, stats.memalign_storage_request, stats.memalign_storage_alloc,
			stats.amemalign_calls, stats.amemalign_0_calls, stats.amemalign_storage_request, stats.amemalign_storage_alloc,
			stats.cmemalign_calls, stats.cmemalign_0_calls, stats.cmemalign_storage_request, stats.cmemalign_storage_alloc,
			stats.resize_calls, stats.resize_0_calls, stats.resize_storage_request, stats.resize_storage_alloc,
			stats.realloc_calls, stats.realloc_0_calls, stats.realloc_storage_request, stats.realloc_storage_alloc,
			stats.free_calls, stats.free_null_0_calls, stats.free_storage_request, stats.free_storage_alloc,
			stats.return_pulls, stats.return_pushes, stats.return_storage_request, stats.return_storage_alloc,
			heapMaster.sbrk_calls, heapMaster.sbrk_storage,
			stats.mmap_calls, stats.mmap_storage_request, stats.mmap_storage_alloc,
			stats.munmap_calls, stats.munmap_storage_request, stats.munmap_storage_alloc,
			heapMaster.threads_started, heapMaster.threads_exited,
			heapMaster.new_heap, heapMaster.reused_heap
		);
	return write( heapMaster.stats_fd, helpText, len );
} // printStats

#define prtFmtXML \
	"<malloc version=\"1\">\n" \
	"<heap nr=\"0\">\n" \
	"<sizes>\n" \
	"</sizes>\n" \
	"<total type=\"malloc\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"aalloc\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"calloc\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"memalign\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"amemalign\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"cmemalign\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"resize\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"realloc\" >0 count=\"%'u;\" 0 count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"free\" !null=\"%'u;\" 0 null/0=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"return\" pulls=\"%'u;\" 0 pushes=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"sbrk\" count=\"%'u;\" size=\"%'llu\"/> bytes\n" \
	"<total type=\"mmap\" count=\"%'u;\" size=\"%'llu / %'llu\" / > bytes\n" \
	"<total type=\"munmap\" count=\"%'u;\" size=\"%'llu / %'llu\"/> bytes\n" \
	"<total type=\"threads\" started=\"%'lu;\" exited=\"%'lu\"/>\n" \
	"<total type=\"heaps\" new=\"%'lu;\" reused=\"%'lu\"/>\n" \
	"</malloc>"

static int printStatsXML( HeapStatistics & stats, FILE * stream ) {	// see malloc_info
	char helpText[sizeof(prtFmtXML) + 1024];			// space for message and values
	int len = snprintf( helpText, sizeof(helpText), prtFmtXML,
			stats.malloc_calls, stats.malloc_0_calls, stats.malloc_storage_request, stats.malloc_storage_alloc,
			stats.aalloc_calls, stats.aalloc_0_calls, stats.aalloc_storage_request, stats.aalloc_storage_alloc,
			stats.calloc_calls, stats.calloc_0_calls, stats.calloc_storage_request, stats.calloc_storage_alloc,
			stats.memalign_calls, stats.memalign_0_calls, stats.memalign_storage_request, stats.memalign_storage_alloc,
			stats.amemalign_calls, stats.amemalign_0_calls, stats.amemalign_storage_request, stats.amemalign_storage_alloc,
			stats.cmemalign_calls, stats.cmemalign_0_calls, stats.cmemalign_storage_request, stats.cmemalign_storage_alloc,
			stats.resize_calls, stats.resize_0_calls, stats.resize_storage_request, stats.resize_storage_alloc,
			stats.realloc_calls, stats.realloc_0_calls, stats.realloc_storage_request, stats.realloc_storage_alloc,
			stats.free_calls, stats.free_null_0_calls, stats.free_storage_request, stats.free_storage_alloc,
			stats.return_pulls, stats.return_pushes, stats.return_storage_request, stats.return_storage_alloc,
			heapMaster.sbrk_calls, heapMaster.sbrk_storage,
			stats.mmap_calls, stats.mmap_storage_request, stats.mmap_storage_alloc,
			stats.munmap_calls, stats.munmap_storage_request, stats.munmap_storage_alloc,
			heapMaster.threads_started, heapMaster.threads_exited,
			heapMaster.new_heap, heapMaster.reused_heap
		);
	return write( fileno( stream ), helpText, len );
} // printStatsXML

static inline HeapStatistics & collectStats( HeapStatistics & stats ) {
	spin_acquire( &heapMaster.mgrLock );

	// Accumulate the heap master and all active thread heaps.
	stats += heapMaster.stats;
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		stats += heap->stats;							// calls HeapStatistics +=
	} // for

	spin_release(&heapMaster.mgrLock);
	return stats;
} // collectStats

static inline void clearStats() {
	spin_acquire( &heapMaster.mgrLock );

	// Zero the heap master and all active thread heaps.
	HeapStatisticsCtor( heapMaster.stats );
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		HeapStatisticsCtor( heap->stats );
	} // for

	spin_release(&heapMaster.mgrLock);
} // clearStats
#endif // __STATISTICS__


inline void noMemory() {
	abort( "**** Error **** heap memory exhausted at %zu bytes.\n"
		   "Possible cause is very large memory allocation and/or large amount of unfreed storage allocated by the program or system/library routines.",
		   ((char *)(sbrk( 0 )) - (char *)(heapMaster.heapBegin)) );
} // noMemory


static inline bool setMmapStart( size_t value ) {		// true => mmapped, false => sbrk
  if ( value < heapMaster.pageSize || bucketSizes[Heap::NoBucketSizes - 1] < value ) return false;
	heapMaster.mmapStart = value;						// set global

	// find the closest bucket size less than or equal to the mmapStart size
	heapMaster.maxBucketsUsed = Bsearchl( heapMaster.mmapStart, bucketSizes, Heap::NoBucketSizes ); // binary search

	assert( heapMaster.maxBucketsUsed < Heap::NoBucketSizes ); // subscript failure ?
	assert( heapMaster.mmapStart <= bucketSizes[heapMaster.maxBucketsUsed] ); // search failure ?
	return true;
} // setMmapStart


// <-------+----------------------------------------------------> bsize (bucket size)
// |header |addr
//==================================================================================
//                   align/offset |
// <-----------------<------------+-----------------------------> bsize (bucket size)
//                   |fake-header | addr
#define HeaderAddr( addr ) ((Heap::Storage::Header *)( (char *)addr - sizeof(Heap::Storage) ))
#define RealHeader( header ) ((Heap::Storage::Header *)((char *)header - header->kind.fake.offset))

// <-------<<--------------------- dsize ---------------------->> bsize (bucket size)
// |header |addr
//==================================================================================
//                   align/offset |
// <------------------------------<<---------- dsize --------->>> bsize (bucket size)
//                   |fake-header |addr
#define DataStorage( bsize, addr, header ) (bsize - ( (char *)addr - (char *)header ))


inline __attribute__((always_inline))
static void checkAlign( size_t alignment ) {
	if ( UNLIKELY( alignment < __ALIGN__ || ! Pow2( alignment ) ) ) {
		abort( "**** Error **** alignment %zu for memory allocation is less than %d and/or not a power of 2.", alignment, __ALIGN__ );
	} // if
} // checkAlign


inline __attribute__((always_inline))
static void checkHeader( bool check, const char name[], void * addr ) {
	if ( UNLIKELY( check ) ) {							// bad address ?
		abort( "**** Error **** attempt to %s storage %p with address outside the heap.\n"
			   "Possible cause is duplicate free on same block or overwriting of memory.",
			   name, addr );
	} // if
} // checkHeader


// Manipulate sticky bits stored in unused 3 low-order bits of an address.
//   bit0 => alignment => fake header
//   bit1 => zero filled (calloc)
//   bit2 => mapped allocation versus sbrk
#define StickyBits( header ) (((header)->kind.real.blockSize & 0x7))
#define ClearStickyBits( addr ) (decltype(addr))((uintptr_t)(addr) & ~7)
#define MarkAlignmentBit( align ) ((align) | 1)
#define AlignmentBit( header ) ((((header)->kind.fake.alignment) & 1))
#define ClearAlignmentBit( header ) (((header)->kind.fake.alignment) & ~1)
#define ZeroFillBit( header ) ((((header)->kind.real.blockSize) & 2))
#define ClearZeroFillBit( header ) ((((header)->kind.real.blockSize) &= ~2))
#define MarkZeroFilledBit( header ) ((header)->kind.real.blockSize |= 2)
#define MmappedBit( header ) ((((header)->kind.real.blockSize) & 4))
#define MarkMmappedBit( size ) ((size) | 4)


inline __attribute__((always_inline))
static void fakeHeader( Heap::Storage::Header *& header, size_t & alignment ) {
	if ( UNLIKELY( AlignmentBit( header ) ) ) {			// fake header ?
		alignment = ClearAlignmentBit( header );		// clear flag from value
		#ifdef __DEBUG__
		checkAlign( alignment );						// check alignment
		#endif // __DEBUG__
		header = RealHeader( header );					// backup from fake to real header
	} else {
		alignment = __ALIGN__;							// => no fake header
	} // if
} // fakeHeader


inline __attribute__((always_inline))
static bool headers( const char name[] __attribute__(( unused )), void * addr, Heap::Storage::Header *& header,
					 Heap::FreeHeader *& freeHead, size_t & size, size_t & alignment ) {
	header = HeaderAddr( addr );

	#ifdef __DEBUG__
	checkHeader( header < heapMaster.heapBegin, name, addr ); // bad low address ?
	#endif // __DEBUG__

	if ( LIKELY( ! StickyBits( header ) ) ) {			// no sticky bits ?
		freeHead = header->kind.real.home;
		alignment = __ALIGN__;
	} else {
		fakeHeader( header, alignment );
		if ( UNLIKELY( MmappedBit( header ) ) ) {		// mapped storage ?
			assert( addr < heapMaster.heapBegin || heapMaster.heapEnd < addr );
			size = ClearStickyBits( header->kind.real.blockSize ); // mmap size
			freeHead = nullptr;							// prevent uninitialized warning
			return true;
		} // if

		freeHead = ClearStickyBits( header->kind.real.home );
	} // if
	size = freeHead->blockSize;

	#ifdef __DEBUG__
	checkHeader( header < heapMaster.heapBegin || heapMaster.heapEnd < header, name, addr ); // bad address ? (offset could be + or -)

	Heap * homeManager;
	if ( UNLIKELY( freeHead == nullptr || // freed and only free-list node => null link
				   // freed and link points at another free block not to a bucket in the bucket array.
				   (homeManager = freeHead->homeManager, freeHead < &homeManager->freeLists[0] ||
					&homeManager->freeLists[Heap::NoBucketSizes] <= freeHead ) ) ) {
		abort( "**** Error **** attempt to %s storage %p with corrupted header.\n"
			   "Possible cause is duplicate free on same block or overwriting of header information.",
			   name, addr );
	} // if
	#endif // __DEBUG__

	return false;
} // headers


static inline void * master_extend( size_t size ) {
	spin_acquire( &heapMaster.extLock );

	ptrdiff_t rem = heapMaster.heapRemaining - size;
	if ( UNLIKELY( rem < 0 ) ) {						// negative ?
		// If the size requested is bigger than the current remaining storage, increase the size of the heap.

		size_t increase = Ceiling( size > heapMaster.heapExpand ? size : heapMaster.heapExpand, __ALIGN__ );
		if ( UNLIKELY( sbrk( increase ) == (void *)-1 ) ) {	// failed, no memory ?
			spin_release( &heapMaster.extLock );
			abort( NO_MEMORY_MSG, size );				// give up
		} // if
		rem = heapMaster.heapRemaining + increase - size;

		#ifdef __STATISTICS__
		heapMaster.sbrk_calls += 1;
		heapMaster.sbrk_storage += increase;
		#endif // __STATISTICS__
	} // if

	Heap::Storage * block = (Heap::Storage *)heapMaster.heapEnd;
	heapMaster.heapRemaining = rem;
	heapMaster.heapEnd = (char *)heapMaster.heapEnd + size;

	spin_release( &heapMaster.extLock );
	return block;
} // master_extend


static void * manager_extend( size_t size ) {
	ptrdiff_t rem = heapManager->heapReserve - size;

	if ( UNLIKELY( rem < 0 ) ) {						// negative ?
		// If the size requested is bigger than the current remaining reserve, use the current reserve to populate
		// smaller freeLists, and increase the reserve.

		rem = heapManager->heapReserve;					// positive

		if ( (decltype(bucketSizes[0]))rem >= bucketSizes[0] ) { // minimal size ? otherwise ignore
			Heap::FreeHeader * freeHead =
			#ifdef FASTLOOKUP
				rem < LookupSizes ? &(heapManager->freeLists[lookup[rem]]) :
			#endif // FASTLOOKUP
				&(heapManager->freeLists[Bsearchl( rem, bucketSizes, heapMaster.maxBucketsUsed )]); // binary search

			// The remaining storage may not be bucket size, whereas all other allocations are. Round down to previous
			// bucket size in this case.
			if ( UNLIKELY( freeHead->blockSize > (size_t)rem ) ) freeHead -= 1;
			Heap::Storage * block = (Heap::Storage *)heapManager->heapBuffer;

			block->header.kind.real.next = freeHead->freeList; // push on stack
			freeHead->freeList = block;
		} // if

		size_t increase = Ceiling( size > ( heapMaster.heapExpand / 10 ) ? size : ( heapMaster.heapExpand / 10 ), __ALIGN__ );
		heapManager->heapBuffer = master_extend( increase );
		rem = increase - size;
	} // if

	Heap::Storage * block = (Heap::Storage *)heapManager->heapBuffer;
	heapManager->heapReserve = rem;
	heapManager->heapBuffer = (char *)heapManager->heapBuffer + size;

	return block;
} // manager_extend


#ifdef __STATISTICS__
#define STAT_NAME __counter
#define STAT_PARM , unsigned int STAT_NAME
#define STAT_ARG( name ) , name
#define STAT_0_CNT( counter ) heapManager->stats.counters[counter].calls_0 += 1
#else
#define STAT_NAME
#define STAT_PARM
#define STAT_ARG( name )
#define STAT_0_CNT( counter )
#endif // __STATISTICS__

#define BOOT_HEAP_MANAGER \
  	if ( UNLIKELY( ! heapManagerBootFlag ) ) { \
		heapManagerCtor(); /* trigger for first heap */ \
	} /* if */ \
	assert( heapManager );

#define __NONNULL_0_ALLOC__ /* Uncomment to return non-null address for malloc( 0 ). */
#ifndef __NONNULL_0_ALLOC__
#define __NULL_0_ALLOC__( counter, ... ) /* 0 byte allocation returns null pointer */ \
	if ( UNLIKELY( size == 0 ) ) { \
		STAT_0_CNT( counter ); \
		__VA_ARGS__; /* call routine, if specified */ \
		return nullptr; \
	} /* if */
#else
#define __NULL_0_ALLOC__( counter, ... )
#endif // __NONNULL_0_ALLOC__

#ifdef __DEBUG__
#define __OVERFLOW_MALLOC__( ... ) \
	if ( UNLIKELY( size > ULONG_MAX - sizeof(Heap::Storage) ) ) { /* error check */ \
		__VA_ARGS__; /* call routine, if specified */ \
		return nullptr; \
	} /* if */
#else
#define __OVERFLOW_MALLOC__( ... )
#endif // __DEBUG__

#define PROLOG( counter, ... ) \
	BOOT_HEAP_MANAGER; \
	__NULL_0_ALLOC__( counter, __VA_ARGS__ ) \
	__OVERFLOW_MALLOC__( __VA_ARGS__ )

#define SCRUB_SIZE 1024lu
// Do not use '\xfe' for scrubbing because dereferencing an address composed of it causes a SIGSEGV *without* a valid IP
// pointer in the interrupt frame.
#define SCRUB '\xff'

static inline __attribute__((always_inline)) void * doMalloc( size_t size STAT_PARM ) {
	PROLOG( STAT_NAME );

	Heap::Storage * block;								// pointer to new block of storage

	// Look up size in the size list.  Make sure the user request includes space for the header that must be allocated
	// along with the block and is a multiple of the alignment size.
	size_t tsize = size + sizeof(Heap::Storage);		// total size needed
	Heap * heap = heapManager;							// optimization

	#ifdef __STATISTICS__
	#ifdef __NONNULL_0_ALLOC__
	if ( UNLIKELY( size == 0 ) )						// malloc( 0 ) ?
		heap->stats.counters[STAT_NAME].calls_0 += 1;
	else
	#endif // __NONNULL_0_ALLOC__
		heap->stats.counters[STAT_NAME].calls += 1;
	heap->stats.counters[STAT_NAME].request += size;
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	heap->allocUnfreed += size;
	#endif // __DEBUG__

	if ( LIKELY( tsize < heapMaster.mmapStart ) ) {		// small size => sbrk
		Heap::FreeHeader * freeHead =
			#ifdef FASTLOOKUP
			LIKELY( tsize < LookupSizes ) ? &(heap->freeLists[lookup[tsize]]) :
			#endif // FASTLOOKUP
			&(heapManager->freeLists[Bsearchl( tsize, bucketSizes, heapMaster.maxBucketsUsed )]); // binary search

		assert( freeHead <= &heap->freeLists[heapMaster.maxBucketsUsed] ); // subscripting error ?
		assert( tsize <= freeHead->blockSize );			// search failure ?

		tsize = freeHead->blockSize;					// total space needed for request
		#ifdef __STATISTICS__
		heap->stats.counters[STAT_NAME].alloc += tsize;
		#endif // __STATISTICS__

		block = freeHead->freeList;						// remove node from stack
		if ( UNLIKELY( block == nullptr ) ) {			// no free block ?
			// Freelist for this size is empty, so check return list (OWNERSHIP), or carve it out of the heap if there
			// is enough left, or get some more heap storage and carve it off.
			#ifdef OWNERSHIP
			if ( UNLIKELY( freeHead->returnList ) ) {	// race, get next time if lose race
				#ifdef RETURNSPIN
				spin_acquire( &freeHead->returnLock );
				block = freeHead->returnList;
				freeHead->returnList = nullptr;
				spin_release( &freeHead->returnLock );
				#else
				block = __atomic_exchange_n( &freeHead->returnList, nullptr, __ATOMIC_SEQ_CST );
				#endif // RETURNSPIN

				assert( block );
				#ifdef __STATISTICS__
				heap->stats.return_pulls += 1;
				#endif // __STATISTICS__

				freeHead->freeList = block->header.kind.real.next; // merge returnList into freeHead
			} else {
			#endif // OWNERSHIP
				block = (Heap::Storage *)manager_extend( tsize ); // mutual exclusion on call

				#ifdef __DEBUG__
				// Scrub new memory so subsequent uninitialized usages might fail. Only scrub the first SCRUB_SIZE bytes.
				memset( block->data, SCRUB, Min( SCRUB_SIZE, tsize - sizeof(Heap::Storage) ) );
				#endif // __DEBUG__
			#ifdef OWNERSHIP
			} // if
			#endif // OWNERSHIP
		} else {
			// Memory is scrubbed in doFree.
			freeHead->freeList = block->header.kind.real.next;
		} // if

		block->header.kind.real.home = freeHead;		// pointer back to free list of apropriate size
	} else {											// large size => mmap
  if ( UNLIKELY( size > ULONG_MAX - heapMaster.pageSize ) ) return nullptr; // error check
		tsize = Ceiling( tsize, heapMaster.pageSize );	// must be multiple of page size

		#ifdef __STATISTICS__
		heap->stats.counters[STAT_NAME].alloc += tsize;
		heap->stats.mmap_calls += 1;
		heap->stats.mmap_storage_request += size;
		heap->stats.mmap_storage_alloc += tsize;
		#endif // __STATISTICS__

		block = (Heap::Storage *)::mmap( 0, tsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
		if ( UNLIKELY( block == MAP_FAILED ) ) {		// failed ?
			if ( errno == ENOMEM ) abort( NO_MEMORY_MSG, tsize ); // no memory
			// Do not call strerror( errno ) as it may call malloc.
			abort( "**** Error **** attempt to allocate large object (> %zu) of size %zu bytes and mmap failed with errno %d.",
				   size, heapMaster.mmapStart, errno );
		} // if
		block->header.kind.real.blockSize = MarkMmappedBit( tsize ); // storage size for munmap

		#ifdef __DEBUG__
		// Scrub new memory so subsequent uninitialized usages might fail. Only scrub the first SCRUB_SIZE bytes. The
		// rest of the storage set to 0 by mmap.
		memset( block->data, SCRUB, Min( SCRUB_SIZE, tsize - sizeof(Heap::Storage) ) );
		#endif // __DEBUG__
	} // if

	block->header.kind.real.size = size;				// store allocation size
	void * addr = &(block->data);						// adjust off header to user bytes
	assert( ((uintptr_t)addr & (__ALIGN__ - 1)) == 0 ); // minimum alignment ?

	#ifdef __DEBUG__
	debug( "\tdoMalloc %p %zd %zd %jd\n", heap, size, tsize, heap->allocUnfreed );
	#endif // __DEBUG__

	return addr;
} // doMalloc


static inline __attribute__((always_inline)) void doFree( void * addr ) {
	assert( addr );
	Heap * heap = heapManager;							// optimization

	// detect free after thread-local storage destruction and use global stats in that case

	Heap::Storage::Header * header;
	Heap::FreeHeader * freeHead;
	size_t size, alignment;

	bool mapped = headers( "free", addr, header, freeHead, size, alignment );
	#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
	size_t rsize = header->kind.real.size;				// optimization
	#endif // __STATISTICS__ || __DEBUG__

	if ( UNLIKELY( mapped ) ) {							// mmapped ?
		#ifdef __STATISTICS__
		heap->stats.munmap_calls += 1;
		heap->stats.munmap_storage_request += rsize;
		heap->stats.munmap_storage_alloc += size;
		#endif // __STATISTICS__

		if ( UNLIKELY( munmap( header, size ) == -1 ) ) {
			// Do not call strerror( errno ) as it may call malloc.
			abort( "**** Error **** attempt to deallocate large object %p and munmap failed with errno %d.\n"
				   "Possible cause is invalid delete pointer: either not allocated or with corrupt header.",
				   addr, errno );
		} // if
	} else {
		assert( freeHead );
		#ifdef __DEBUG__
		// Scrub old memory so subsequent usages might fail. Only scrub the first/last SCRUB_SIZE bytes.
		char * data = ((Heap::Storage *)header)->data;	// data address
		size_t dsize = size - sizeof(Heap::Storage);	// data size
		if ( dsize <= SCRUB_SIZE * 2 ) {
			memset( data, SCRUB, dsize );				// scrub all
		} else {
			memset( data, SCRUB, SCRUB_SIZE );			// scrub front
			memset( data + dsize - SCRUB_SIZE, SCRUB, SCRUB_SIZE ); // scrub back
		} // if
		#endif // __DEBUG__

		#ifdef OWNERSHIP
		if ( LIKELY( heap == freeHead->homeManager ) ) { // belongs to this thread
			header->kind.real.next = freeHead->freeList; // push on stack
			freeHead->freeList = (Heap::Storage *)header;
		} else {										// return to thread owner
			#ifdef RETURNSPIN
			spin_acquire( &freeHead->returnLock );
			header->kind.real.next = freeHead->returnList; // push to bucket return list
			freeHead->returnList = (Heap::Storage *)header;
			spin_release( &freeHead->returnLock );
			#else										// lock free
			header->kind.real.next = freeHead->returnList; // link new node to top node
			// CAS resets header->kind.real.next = freeHead->returnList on failure
			while ( ! __atomic_compare_exchange_n( &freeHead->returnList, &header->kind.real.next, (Heap.Storage *)header,
												   false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ) );
			#endif // RETURNSPIN
		
			if ( UNLIKELY( heap == nullptr ) ) {
				// Use master heap counters as heap is reused by this point.
				#ifdef __STATISTICS__
				AtomicFetchAdd( heapMaster.stats.free_storage_request, rsize );
				AtomicFetchAdd( heapMaster.stats.free_storage_alloc, size );
				// Return push counters are not incremented because this is a self-return push, and there is no
				// corresponding pull counter that needs to match.
				#endif // __STATISTICS__

				#ifdef __DEBUG__
				AtomicFetchAdd( heapMaster.allocUnfreed, -rsize );
				debug( "\tdoFree2 %zd %zd %jd\n", size, rsize, heapMaster.allocUnfreed );
				#endif // __DEBUG__
				return;
			} // if

			#ifdef __STATISTICS__
			heap->stats.return_pushes += 1;
			heap->stats.return_storage_request += rsize;
			heap->stats.return_storage_alloc += size;
			#endif // __STATISTICS__
		} // if

		#else											// no OWNERSHIP

		if ( LIKELY( heap != nullptr ) ) {
			freeHead = &heap->freeLists[ClearStickyBits( header->kind.real.home ) - &freeHead->homeManager->freeLists[0]];
			header->kind.real.next = freeHead->freeList; // push on stack
			freeHead->freeList = (Heap::Storage *)header;
		} else {
			// thread_local storage is leaked. No-ownership is rife with storage that appears to be leaked in an
			// application, unless great effort is made to deal with it. A typical example is a producer creating
			// storage and a consumer deleting it. The deleted storage piles up in the consumer's heap, and hence,
			// appears like leaked storage from the producer's perspective.
		} // if
		#endif // OWNERSHIP
	} // if

	// Do not move these up because heap can be null!
	#ifdef __STATISTICS__
	#ifdef __NONNULL_0_ALLOC__
	if ( UNLIKELY( rsize == 0 ) )						// malloc( 0 ) ?
		heap->stats.free_null_0_calls += 1;
	else
	#endif // __NONNULL_0_ALLOC__
		heapManager->stats.free_calls += 1;				// count free amd implicit frees from resize/realloc
	heap->stats.free_storage_request += rsize;
	heap->stats.free_storage_alloc += size;
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	heap->allocUnfreed -= rsize;
	debug( "\tdoFree %p %zd %zd %jd\n", heap, size, rsize, heap->allocUnfreed );
	#endif // __DEBUG__
} // doFree


static inline __attribute__((always_inline)) void * memalignNoStats( size_t alignment, size_t size STAT_PARM ) {
	checkAlign( alignment );							// check alignment

	// if alignment <= default alignment or size == 0, do normal malloc as two headers are unnecessary
  if ( UNLIKELY( alignment <= __ALIGN__ || size == 0 ) ) return doMalloc( size STAT_ARG( STAT_NAME ) );

	// Allocate enough storage to guarantee an address on the alignment boundary, and sufficient space before it for
	// administrative storage. NOTE, WHILE THERE ARE 2 HEADERS, THE FIRST ONE IS IMPLICITLY CREATED BY DOMALLOC.
	//      .-------------v-----------------v----------------v----------,
	//      | Real Header | ... padding ... |   Fake Header  | data ... |
	//      `-------------^-----------------^-+--------------^----------'
	//      |<--------------------------------' offset/align |<-- alignment boundary

	// subtract __ALIGN__ because it is already the minimum alignment
	// add sizeof(Heap::Storage) for fake header
	size_t offset = alignment - __ALIGN__ + sizeof(Heap::Storage);
	char * addr = (char *)doMalloc( size + offset STAT_ARG( STAT_NAME ) );

	// address in the block of the "next" alignment address
	char * user = (char *)Ceiling( (uintptr_t)(addr + sizeof(Heap::Storage)), alignment );

	// address of header from malloc
	Heap::Storage::Header * realHeader = HeaderAddr( addr );
	realHeader->kind.real.size = size;					// correct size to eliminate above alignment offset
	#ifdef __DEBUG__
	heapManager->allocUnfreed -= offset;				// adjustment off the offset from call to doMalloc
	#endif // __DEBUG__

	// address of fake header *before* the alignment location
	Heap::Storage::Header * fakeHeader = HeaderAddr( user );

	// SKULLDUGGERY: insert the offset to the start of the actual storage block and remember alignment
	fakeHeader->kind.fake.offset = (char *)fakeHeader - (char *)realHeader;
	// SKULLDUGGERY: odd alignment implies fake header
	fakeHeader->kind.fake.alignment = MarkAlignmentBit( alignment );

	return user;
} // memalignNoStats

// Operators new and new [] call malloc; delete calls free


//####################### Memory Allocation Routines ####################


extern "C" {
	// Allocates size bytes and returns a pointer to the allocated memory.  The contents are undefined. If size is 0,
	// then malloc() returns a unique pointer value that can later be successfully passed to free().
	void * malloc( size_t size ) {
		return doMalloc( size STAT_ARG( HeapStatistics::MALLOC ) );
	} // malloc


	// Same as malloc() except size bytes is an array of dim elements each of elemSize bytes.
	void * aalloc( size_t dim, size_t elemSize ) {
		return doMalloc( dim * elemSize STAT_ARG( HeapStatistics::AALLOC ) );
	} // aalloc


	// Same as aalloc() with memory set to zero.
	void * calloc( size_t dim, size_t elemSize ) {
		size_t size = dim * elemSize;
		char * addr = (char *)doMalloc( size STAT_ARG( HeapStatistics::CALLOC ) );

	  if ( UNLIKELY( addr == NULL ) ) return NULL;		// stop further processing if 0p is returned

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, alignment;

		#ifndef __DEBUG__
		bool mapped =
		#endif // __DEBUG__
			headers( "calloc", addr, header, freeHead, bsize, alignment );

		#ifndef __DEBUG__
		// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero.
		if ( LIKELY( ! mapped ) )
		#endif // __DEBUG__
			// <-------0000000000000000000000000000UUUUUUUUUUUUUUUUUUUUUUUUU> bsize (bucket size) U => undefined
			// `-header`-addr                      `-size
			memset( addr, '\0', size );					// set to zeros

		MarkZeroFilledBit( header );					// mark as zero fill
		return addr;
	} // calloc


	// Change the size of the memory block pointed to by oaddr to size bytes. The contents are undefined.  If oaddr is
	// nullptr, then the call is equivalent to malloc(size), for all values of size; if size is equal to zero, and oaddr is
	// not nullptr, then the call is equivalent to free(oaddr). Unless oaddr is nullptr, it must have been returned by an earlier
	// call to malloc(), alloc(), calloc() or realloc(). If the area pointed to was moved, a free(oaddr) is done.
	void * resize( void * oaddr, size_t size ) {
	  if ( UNLIKELY( oaddr == nullptr ) ) {				// => malloc( size )
			return doMalloc( size STAT_ARG( HeapStatistics::RESIZE ) );
		} // if

		PROLOG( HeapStatistics::RESIZE, doFree( oaddr ) ); // => free( oaddr )

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, oalign;
		headers( "resize", oaddr, header, freeHead, bsize, oalign );

		size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket
		// same size, DO NOT PRESERVE STICKY PROPERTIES.
		if ( oalign == __ALIGN__ && size <= odsize && odsize <= size * 2 ) { // allow 50% wasted storage for smaller size
			ClearZeroFillBit( header );					// no alignment and turn off 0 fill
			#ifdef __DEBUG__
			heapManager->allocUnfreed += size - header->kind.real.size; // adjustment off the size difference
			#endif // __DEBUG__
			header->kind.real.size = size;				// reset allocation size
			#ifdef __STATISTICS__
			heapManager->stats.resize_calls += 1;
			#endif // __STATISTICS__
			return oaddr;
		} // if

		// change size, DO NOT PRESERVE STICKY PROPERTIES.
		doFree( oaddr );								// free previous storage

		return doMalloc( size STAT_ARG( HeapStatistics::RESIZE ) ); // create new area
	} // resize


	// Same as resize() but the contents are unchanged in the range from the start of the region up to the minimum of
	// the old and new sizes.
	void * realloc( void * oaddr, size_t size ) {
	  if ( UNLIKELY( oaddr == nullptr ) ) {				// => malloc( size )
		  return doMalloc( size STAT_ARG( HeapStatistics::REALLOC ) );
		} // if

		PROLOG( HeapStatistics::REALLOC, doFree( oaddr ) ); // => free( oaddr )

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, oalign;
		headers( "realloc", oaddr, header, freeHead, bsize, oalign );

		size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket
		size_t osize = header->kind.real.size;			// old allocation size
		bool ozfill = ZeroFillBit( header );			// old allocation zero filled
	  if ( UNLIKELY( size <= odsize ) && odsize <= size * 2 ) { // allow up to 50% wasted storage
			#ifdef __DEBUG__
			heapManager->allocUnfreed += size - header->kind.real.size; // adjustment off the size difference
			#endif // __DEBUG__
			header->kind.real.size = size;				// reset allocation size
	  		if ( UNLIKELY( ozfill ) && size > osize ) {	// previous request zero fill and larger ?
	  			memset( (char *)oaddr + osize, '\0', size - osize ); // initialize added storage
	  		} // if
			#ifdef __STATISTICS__
			heapManager->stats.realloc_calls += 1;
			#endif // __STATISTICS__
			return oaddr;
		} // if

		// change size and copy old content to new storage

		void * naddr;
		if ( UNLIKELY( oalign <= __ALIGN__ ) ) {		// previous request not aligned ?
			naddr = doMalloc( size STAT_ARG( HeapStatistics::REALLOC ) ); // create new area
		} else {
			naddr = memalignNoStats( oalign, size STAT_ARG( HeapStatistics::REALLOC ) ); // create new aligned area
		} // if

		headers( "realloc", naddr, header, freeHead, bsize, oalign );
		// To preserve prior fill, the entire bucket must be copied versus the size.
		memcpy( naddr, oaddr, Min( osize, size ) );		// copy bytes
		doFree( oaddr );								// free previous storage

		if ( UNLIKELY( ozfill ) ) {						// previous request zero fill ?
			MarkZeroFilledBit( header );				// mark new request as zero filled
			if ( size > osize ) {						// previous request larger ?
				memset( (char *)naddr + osize, '\0', size - osize ); // initialize added storage
			} // if
		} // if
		return naddr;
	} // realloc


	// Same as realloc() except the new allocation size is large enough for an array of nelem elements of size elsize.
	void * reallocarray( void * oaddr, size_t dim, size_t elemSize ) {
		return realloc( oaddr, dim * elemSize );
	} // reallocarray


	// Same as malloc() except the memory address is a multiple of alignment, which must be a power of two. (obsolete)
	void * memalign( size_t alignment, size_t size ) {
		return memalignNoStats( alignment, size STAT_ARG( HeapStatistics::MEMALIGN ) );
	} // memalign


	// Same as aalloc() with memory alignment.
	void * amemalign( size_t alignment, size_t dim, size_t elemSize ) {
		return memalignNoStats( alignment, dim * elemSize STAT_ARG( HeapStatistics::AMEMALIGN ) );
	} // amemalign


	// Same as calloc() with memory alignment.
	void * cmemalign( size_t alignment, size_t dim, size_t elemSize ) {
		size_t size = dim * elemSize;
		char * addr = (char *)memalignNoStats( alignment, size STAT_ARG( HeapStatistics::CMEMALIGN ) );

	  if ( UNLIKELY( addr == NULL ) ) return NULL;		// stop further processing if 0p is returned

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize;

		#ifndef __DEBUG__
		bool mapped =
		#endif // __DEBUG__
			headers( "cmemalign", addr, header, freeHead, bsize, alignment );

		// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero.
		#ifndef __DEBUG__
		if ( LIKELY( ! mapped ) )
		#endif // __DEBUG__
			// <-------0000000000000000000000000000UUUUUUUUUUUUUUUUUUUUUUUUU> bsize (bucket size) U => undefined
			// `-header`-addr                      `-size
			memset( addr, '\0', size );					// set to zeros

		MarkZeroFilledBit( header );					// mark as zero filled
		return addr;
	} // cmemalign


	// Same as memalign(), but ISO/IEC 2011 C11 Section 7.22.2 states: the value of size shall be an integral multiple
	// of alignment. This requirement is universally ignored.
	void * aligned_alloc( size_t alignment, size_t size ) {
		return memalign( alignment, size );
	} // aligned_alloc


	// Allocates size bytes and places the address of the allocated memory in *memptr. The address of the allocated
	// memory shall be a multiple of alignment, which must be a power of two and a multiple of sizeof(void *). If size
	// is 0, then posix_memalign() returns either nullptr, or a unique pointer value that can later be successfully
	// passed to free(3).
	int posix_memalign( void ** memptr, size_t alignment, size_t size ) {
	  if ( UNLIKELY( alignment < __ALIGN__ || ! Pow2( alignment ) ) ) return EINVAL; // check alignment
		*memptr = memalign( alignment, size );
		return 0;
	} // posix_memalign


	// Allocates size bytes and returns a pointer to the allocated memory. The memory address shall be a multiple of the
	// page size.  It is equivalent to memalign(sysconf(_SC_PAGESIZE),size).
	void * valloc( size_t size ) {
		return memalign( heapMaster.pageSize, size );
	} // valloc


	// Same as valloc but rounds size to multiple of page size.
	void * pvalloc( size_t size ) {
		return memalign( heapMaster.pageSize, Ceiling( size, heapMaster.pageSize ) );
	} // pvalloc


	// Frees the memory space pointed to by ptr, which must have been returned by a previous call to malloc(), calloc()
	// or realloc().  Otherwise, or if free(ptr) has already been called before, undefined behaviour occurs. If ptr is
	// nullptr, no operation is performed.
	void free( void * addr ) {
		// Detect free after thread-local storage destruction and use global stats in that case.
	  if ( UNLIKELY( addr == nullptr ) ) {				// special case
			#ifdef __STATISTICS__
			if ( LIKELY( heapManager ) ) heapManager->stats.free_null_0_calls += 1;
			else AtomicFetchAdd( heapMaster.stats.free_null_0_calls, 1 );
			#endif // __STATISTICS__
			return;
		} // if

		// Do not move this up ...
		BOOT_HEAP_MANAGER;								// trigger for first heap

		doFree( addr );									// handles heapManager == nullptr
	} // free


	// Returns the alignment of an allocation.
	size_t malloc_alignment( void * addr ) {
	  if ( UNLIKELY( addr == nullptr ) ) return __ALIGN__; // minimum alignment
		Heap::Storage::Header * header = HeaderAddr( addr );
		if ( UNLIKELY( AlignmentBit( header ) ) ) {		// fake header ?
			return ClearAlignmentBit( header );			// clear flag from value
		} else {
			return __ALIGN__;							// minimum alignment
		} // if
	} // malloc_alignment


	// Returns true if the allocation is zero filled, e.g., allocated by calloc().
	bool malloc_zero_fill( void * addr ) {
	  if ( UNLIKELY( addr == nullptr ) ) return false;	// null allocation is not zero fill
		Heap::Storage::Header * header = HeaderAddr( addr );
		if ( UNLIKELY( AlignmentBit( header ) ) ) {		// fake header ?
			header = RealHeader( header );				// backup from fake to real header
		} // if
		return ZeroFillBit( header );					// zero filled ?
	} // malloc_zero_fill


	// Returns original total allocation size (not bucket size) => array size is dimension * sizeof(T).
	size_t malloc_size( void * addr ) {
	  if ( UNLIKELY( addr == nullptr ) ) return 0;		// null allocation has zero size
		Heap::Storage::Header * header = HeaderAddr( addr );
		if ( UNLIKELY( AlignmentBit( header ) ) ) {		// fake header ?
			header = RealHeader( header );				// backup from fake to real header
		} // if
		return header->kind.real.size;
	} // malloc_size


	// Returns the number of usable bytes in the block pointed to by ptr, a pointer to a block of memory allocated by
	// malloc or a related function.
	size_t malloc_usable_size( void * addr ) {
	  if ( UNLIKELY( addr == nullptr ) ) return 0;		// null allocation has zero size
		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, alignment;

		headers( "malloc_usable_size", addr, header, freeHead, bsize, alignment );
		return DataStorage( bsize, addr, header );		// data storage in bucket
	} // malloc_usable_size


	// Prints (on default standard error) statistics about memory allocated by malloc and related functions.
	void malloc_stats() {
		#ifdef __STATISTICS__
		HeapStatistics stats;
		HeapStatisticsCtor( stats );
		if ( printStats( collectStats( stats ) ) == -1 ) {
		#else
		#define MALLOC_STATS_MSG "malloc_stats statistics disabled.\n"
		if ( write( STDERR_FILENO, MALLOC_STATS_MSG, sizeof( MALLOC_STATS_MSG ) - 1 /* size includes '\0' */ ) == -1 ) {
		#endif // __STATISTICS__
			abort( "**** Error **** write failed in malloc_stats" );
		} // if
	} // malloc_stats

	// Zero the heap master and all active thread heaps.
	void malloc_stats_clear() {
		#ifdef __STATISTICS__
		clearStats();
		#else
		#define MALLOC_STATS_MSG "malloc_stats statistics disabled.\n"
		if ( write( STDERR_FILENO, MALLOC_STATS_MSG, sizeof( MALLOC_STATS_MSG ) - 1 /* size includes '\0' */ ) == -1 ) {
			abort( "**** Error **** write failed in malloc_stats" );
		} // if
		#endif // __STATISTICS__
	} // malloc_stats_clear

	// Changes the file descriptor where malloc_stats() writes statistics.
	int malloc_stats_fd( int fd __attribute__(( unused )) ) {
		#ifdef __STATISTICS__
		int temp = heapMaster.stats_fd;
		heapMaster.stats_fd = fd;
		return temp;
		#else
		return -1;										// unsupported
		#endif // __STATISTICS__
	} // malloc_stats_fd


	// Prints an XML string that describes the current state of the memory-allocation implementation in the caller.
	// The string is printed on the file stream stream.  The exported string includes information about all arenas (see
	// malloc).
	int malloc_info( int options, FILE * stream __attribute__(( unused )) ) {
	  if ( options != 0 ) { errno = EINVAL; return -1; }
		#ifdef __STATISTICS__
		HeapStatistics stats;
		HeapStatisticsCtor( stats );
		return printStatsXML( collectStats( stats ), stream ); // returns bytes written or -1
		#else
		return 0;										// unsupported
		#endif // __STATISTICS__
	} // malloc_info


	// Adjusts parameters that control the behaviour of the memory-allocation functions (see malloc). The param argument
	// specifies the parameter to be modified, and value specifies the new value for that parameter.
	int mallopt( int option, int value ) {
	  if ( value < 0 ) return 0;
		switch( option ) {
		  case M_TOP_PAD:
			heapMaster.heapExpand = Ceiling( value, heapMaster.pageSize );
			return 1;
		  case M_MMAP_THRESHOLD:
			if ( setMmapStart( value ) ) return 1;
			break;
		} // switch
		return 0;										// error, unsupported
	} // mallopt


	// Attempt to release free memory at the top of the heap (by calling sbrk with a suitable argument).
	int malloc_trim( size_t ) {
		return 0;										// => impossible to release memory
	} // malloc_trim


	// Records the current state of all malloc internal bookkeeping variables (but not the actual contents of the heap
	// or the state of malloc_hook functions pointers).  The state is recorded in a system-dependent opaque data
	// structure dynamically allocated via malloc, and a pointer to that data structure is returned as the function
	// result.  (The caller must free this memory.)
	void * malloc_get_state( void ) {
		return nullptr;									// unsupported
	} // malloc_get_state


	// Restores the state of all malloc internal bookkeeping variables to the values recorded in the opaque data
	// structure pointed to by state.
	int malloc_set_state( void * ) {
		return 0;										// unsupported
	} // malloc_set_state


	// Sets the amount (bytes) to extend the heap when there is insufficent free storage to service an allocation.
	__attribute__((weak)) size_t malloc_expansion() { return __DEFAULT_HEAP_EXPANSION__; }

	// Sets the crossover point between allocations occuring in the sbrk area or separately mmapped.
	__attribute__((weak)) size_t malloc_mmap_start() { return __DEFAULT_MMAP_START__; }

	// Amount subtracted to adjust for unfreed program storage (debug only).
	__attribute__((weak)) size_t malloc_unfreed() { return __DEFAULT_HEAP_UNFREED__; }
} // extern "C"


// Must have C++ linkage to overload with C linkage realloc.
void * resize( void * oaddr, size_t nalign, size_t size ) __THROW {
  if ( UNLIKELY( oaddr == nullptr ) ) {					// => malloc( size )
		return memalignNoStats( nalign, size STAT_ARG( HeapStatistics::RESIZE ) );
	} // if

	PROLOG( HeapStatistics::RESIZE, doFree( oaddr ) );	// => free( oaddr )

	// Attempt to reuse existing alignment.
	Heap::Storage::Header * header = HeaderAddr( oaddr );
	bool isFakeHeader = AlignmentBit( header );			// old fake header ?
	size_t oalign;

	if ( UNLIKELY( isFakeHeader ) ) {
		checkAlign( nalign );							// check alignment
		oalign = ClearAlignmentBit( header );			// old alignment
		if ( UNLIKELY( (uintptr_t)oaddr % nalign == 0	// lucky match ?
			 && ( oalign <= nalign						// going down
				  || (oalign >= nalign && oalign <= 256) ) // little alignment storage wasted ?
			) ) {
			HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalign ); // update alignment (could be the same)
			Heap::FreeHeader * freeHead;
			size_t bsize, oalign;
			headers( "resize", oaddr, header, freeHead, bsize, oalign );
			size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket

			if ( size <= odsize && odsize <= size * 2 ) { // allow 50% wasted data storage
				HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalign ); // update alignment (could be the same)
				ClearZeroFillBit( header );				// turn off 0 fill
				#ifdef __DEBUG__
				heapManager->allocUnfreed += size - header->kind.real.size; // adjustment off the size difference
				#endif // __DEBUG__
				header->kind.real.size = size;			// reset allocation size
				#ifdef __STATISTICS__
				heapManager->stats.resize_calls += 1;
				#endif // __STATISTICS__
				return oaddr;
			} // if
		} // if
	} else if ( ! isFakeHeader							// old real header (aligned on libAlign) ?
				&& nalign == __ALIGN__ ) {				// new alignment also on libAlign => no fake header needed
		return resize( oaddr, size );					// duplicate special case checks
	} // if

	// change size, DO NOT PRESERVE STICKY PROPERTIES.
	doFree( oaddr );									// free previous storage
	return memalignNoStats( nalign, size STAT_ARG( HeapStatistics::RESIZE ) ); // create new aligned area
} // resize


void * realloc( void * oaddr, size_t nalign, size_t size ) __THROW {
  if ( UNLIKELY( oaddr == nullptr ) ) {					// => malloc( size )
		return memalignNoStats( nalign, size STAT_ARG( HeapStatistics::REALLOC ) );
	} // if

	PROLOG( HeapStatistics::REALLOC, doFree( oaddr ) ); // => free( oaddr )

	// Attempt to reuse existing alignment.
	Heap::Storage::Header * header = HeaderAddr( oaddr );
	bool isFakeHeader = AlignmentBit( header );			// old fake header ?
	size_t oalign;
	if ( UNLIKELY( isFakeHeader ) ) {
		checkAlign( nalign );							// check alignment
		oalign = ClearAlignmentBit( header );			// old alignment
		if ( UNLIKELY( (uintptr_t)oaddr % nalign == 0	// lucky match ?
			 && ( oalign <= nalign						// going down
				  || (oalign >= nalign && oalign <= 256) ) // little alignment storage wasted ?
			) ) {
			HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalign ); // update alignment (could be the same)
			return realloc( oaddr, size );				// duplicate special case checks
		} // if
	} else if ( ! isFakeHeader							// old real header (aligned on libAlign) ?
				&& nalign == __ALIGN__ ) {				// new alignment also on libAlign => no fake header needed
		return realloc( oaddr, size );					// duplicate special case checks
	} // if

	Heap::FreeHeader * freeHead;
	size_t bsize;
	headers( "realloc", oaddr, header, freeHead, bsize, oalign );

	// change size and copy old content to new storage

	size_t osize = header->kind.real.size;				// old allocation size
	bool ozfill = ZeroFillBit( header );				// old allocation zero filled

	void * naddr = memalignNoStats( nalign, size STAT_ARG( HeapStatistics::REALLOC ) ); // create new aligned area

	headers( "realloc", naddr, header, freeHead, bsize, oalign );
	memcpy( naddr, oaddr, Min( osize, size ) );			// copy bytes
	doFree( oaddr );									// free previous storage

	if ( UNLIKELY( ozfill ) ) {							// previous request zero fill ?
		MarkZeroFilledBit( header );					// mark new request as zero filled
		if ( size > osize ) {							// previous request larger ?
			memset( (char *)naddr + osize, '\0', size - osize ); // initialize added storage
		} // if
	} // if
	return naddr;
} // realloc


void * reallocarray( void * oaddr, size_t nalign, size_t dim, size_t elemSize ) __THROW {
	return realloc( oaddr, nalign, dim * elemSize );
} // reallocarray


// zip -r llheap.zip heap/README.md heap/llheap.h heap/llheap.cc heap/Makefile heap/affinity.h heap/test.cc heap/ownership.cc

// g++-10 -Wall -Wextra -g -O3 -DNDEBUG -D__STATISTICS__ -DTLS llheap.cc -fPIC -shared -o llheap.so

// Local Variables: //
// tab-width: 4 //
// compile-command: "g++-10 -Wall -Wextra -g -O3 -DNDEBUG -D__STATISTICS__ llheap.cc -c" //
// End: //
