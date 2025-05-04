#include "llheap.h"

#include <new>											// set_new_handler
#include <cstdlib>										// abort
#include <cstring>										// strlen, memset, memcpy
#include <climits>										// ULONG_MAX
#include <cstdarg>										// va_start, va_end
#include <cerrno>										// errno, ENOMEM, EINVAL
#include <cassert>										// assert
#include <cstdint>										// uintptr_t, uint64_t, uint32_t
#include <unistd.h>										// STDERR_FILENO, sbrk, sysconf, write
#include <sys/mman.h>									// mmap, munmap
#include <sys/sysinfo.h>								// get_nprocs

#define str(s) #s
#define xstr(s) str(s)
#define WARNING( s ) xstr( GCC diagnostic ignored str( -W ## s ) )
#define NOWARNING( statement, warning ) \
	_Pragma( "GCC diagnostic push" ) \
	_Pragma( WARNING( warning ) ) \
	statement ;	\
	_Pragma ( "GCC diagnostic pop" )

#define __FASTLOOKUP__	// use O(1) table lookup from allocation size to bucket size for small allocations
#define __OWNERSHIP__	// return freed memory to owner thread
//#define __REMOTESPIN__	// toggle spinlock / lockfree queue
#if ! defined( __OWNERSHIP__ ) && defined( __REMOTESPIN__ )
#warning "REMOTESPIN is ignored without OWNERSHIP; suggest commenting out REMOTESPIN"
#endif // ! __OWNERSHIP__ && __REMOTESPIN__

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define CACHE_ALIGN 64
#define CALIGN __attribute__(( aligned(CACHE_ALIGN) ))

#ifdef TLS
#define TLSMODEL __attribute__(( tls_model("initial-exec") ))
#else
#define TLSMODEL
#endif // TLS

//#define __LL_DEBUG__
#ifdef __LL_DEBUG__
#define LLDEBUG( stmt ) stmt
#else
#define LLDEBUG( stmt )
#endif // __LL_DEBUG__


// The return code from "write" is ignored. Basically, if write fails, trying to write out why it failed will likely
// fail, too, so just continue.


//######################### Helpers #########################


enum { BufSize = 1024 };

#define always_assert(expr) \
     (static_cast <bool> (expr)	\
      ? void (0) \
      : __assert_fail(#expr, __FILE__, __LINE__, __extension__ __PRETTY_FUNCTION__))

// Called by macro assert in assert.h. Replace to prevent recursive call to malloc.
extern "C" void __assert_fail( const char assertion[], const char file[], unsigned int line, const char function[] ) {
	extern const char * __progname;						// global name of running executable (argv[0])
	char helpText[BufSize];
	int len = snprintf( helpText, sizeof(helpText), "Internal assertion error \"%s\" from program \"%s\" in \"%s\" at line %d in file \"%s.\n",
						assertion, __progname, function, line, file );
	int unused __attribute__(( unused )) = write( STDERR_FILENO, helpText, len ); // file might be closed
	abort();
	// CONTROL NEVER REACHES HERE!
} // __assert_fail

#ifdef __DEBUG__
static bool signal_p = false;
static void backtrace( int start );						// forward
#endif // __DEBUG__

static void abort( const char fmt[], ... ) __attribute__(( format(printf, 1, 2), __nothrow__, __noreturn__ ));
static void abort( const char fmt[], ... ) {			// overload real abort
	va_list args;
	va_start( args, fmt );
	char buf[BufSize];
	int len = vsnprintf( buf, BufSize, fmt, args );
	int unused __attribute__(( unused )) = write( STDERR_FILENO, buf, len ); // file might be closed
	if ( fmt[strlen( fmt ) - 1] != '\n' ) {				// add optional newline if missing at the end of the format text
		int unused __attribute__(( unused )) = write( STDERR_FILENO, "\n", 1 ); // file might be closed
	} // if
	va_end( args );
	#ifdef __DEBUG__
	backtrace( signal_p ? 4 : 2 );
	#endif // __DEBUG__
	abort();											// call the real abort
	// CONTROL NEVER REACHES HERE!
} // abort

static void debugprt( const char fmt[], ... ) __attribute__(( format(printf, 1, 2), unused ));
static void debugprt( const char fmt[] __attribute__(( unused )), ... ) {
	va_list args;
	va_start( args, fmt );
	char buf[BufSize];
	int len = vsnprintf( buf, BufSize, fmt, args );
	int unused __attribute__(( unused )) = write( STDERR_FILENO, buf, len ); // file might be closed
	va_end( args );
} // debugprt

static inline __attribute__((always_inline)) bool Pow2( unsigned long int value ) {
	// clears all bits below value, rounding value down to the next lower multiple of value
	return (value & (value - 1)) == 0;
} // Pow2

static inline __attribute__((always_inline)) unsigned long int Floor( unsigned long int value, unsigned long int alignment ) {
	assert( Pow2( alignment ) );
	// clears all bits above or equal to alignment, getting (value % alignment), the phase of value with regards to alignment
	return value & -alignment;
} // Floor

static inline __attribute__((always_inline)) unsigned long int Ceiling( unsigned long int value, unsigned long int alignment ) {
	assert( Pow2( alignment ) );
	// "negate, round down, negate" is the same as round up
	return -Floor( -value, alignment );
} // Ceiling


// std::min and std::lower_bound do not always inline, so substitute hand-coded versions.

#define Min( x, y ) (x < y ? x : y)

static inline __attribute__((always_inline)) size_t Bsearchl( unsigned int key, const unsigned int vals[], size_t dimension ) {
	size_t l = 0, m, h = dimension;
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


// reference parameters
#define Fai( change, Inc ) __atomic_fetch_add( (&(change)), (Inc), __ATOMIC_SEQ_CST )
#define Tas( lock ) __atomic_test_and_set( (&(lock)), __ATOMIC_ACQUIRE )
#define Clr( lock ) __atomic_clear( (&(lock)), __ATOMIC_RELEASE )
#define Fas( change, assn ) __atomic_exchange_n( (&(change)), (assn), __ATOMIC_SEQ_CST )
#define Cas( change, comp, assn ) ({decltype(comp) __temp = (comp); __atomic_compare_exchange_n( (&(change)), (&(__temp)), (assn), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ); })
#define Casv( change, comp, assn ) __atomic_compare_exchange_n( (&(change)), (comp), (assn), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST )

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
	  if ( *lock == 0 && Tas( *lock ) == 0 ) break;		// Fence
		for ( volatile unsigned int s = 0; s < spin; s += 1 ) Pause(); // exponential spin
		spin += spin;									// powers of 2
		//if ( i % 64 == 0 ) spin += spin;				// slowly increase by powers of 2
		if ( spin > SPIN_END ) spin = SPIN_END;			// cap spinning
	} // for
} // spin_lock

static inline __attribute__((always_inline)) void spin_release( volatile SpinLock_t * lock ) {
	Clr( *lock );										// Fence
} // spin_unlock


//######################### Back Trace #########################


// Need to compile with -rdynamic to get symbol names in back trace.

#ifdef __DEBUG__
#include <execinfo.h>									// backtrace, backtrace_symbols
#include <cxxabi.h>										// __cxa_demangle

static void backtrace( int start ) {
	enum {
		Frames = 50,									// maximum number of stack frames
		Last = 2,										// skip last N stack frames
	};

	void * array[Frames];
	size_t size = ::backtrace( array, Frames );
	char ** messages = ::backtrace_symbols( array, size ); // does not demangle names
	char helpText[256];
	int len;

	*index( messages[0], '(' ) = '\0';					// find executable name
	len = snprintf( helpText, 256, "Stack back trace for: %s\n", messages[0] );
	debugprt( helpText, len );

	// skip stack frames after (below) main
	for ( unsigned int i = start; i < size - Last && messages != nullptr; i += 1 ) {
		char * mangled_name = nullptr, * offset_begin = nullptr, * offset_end = nullptr;

		for ( char * p = messages[i]; *p; ++p ) {		// find parantheses and +offset
			if ( *p == '(' ) {
				mangled_name = p;
			} else if ( *p == '+' ) {
				offset_begin = p;
			} else if ( *p == ')' ) {
				offset_end = p;
				break;
			} // if
		} // for

		// if line contains symbol, attempt to demangle
		int frameNo = i - start;
		if ( mangled_name && offset_begin && offset_end && mangled_name < offset_begin ) {
			*mangled_name++ = '\0';						// delimit strings
			*offset_begin++ = '\0';
			*offset_end++ = '\0';

			int status;
			char * real_name = __cxxabiv1::__cxa_demangle( mangled_name, 0, 0, &status );
			// bug in __cxa_demangle for single-character lower-case non-mangled names
			if ( status == 0 ) {						// demangling successful ?
				len = snprintf( helpText, 256, "(%d) %s %s+%s%s\n",
								frameNo, messages[i], real_name, offset_begin, offset_end );
			} else {									// otherwise, output mangled name
				len = snprintf( helpText, 256, "(%d) %s %s(/*unknown*/)+%s%s\n",
								frameNo, messages[i], mangled_name, offset_begin, offset_end );
			} // if

			free( real_name );
		} else {										// otherwise, print the whole line
			len = snprintf( helpText, 256, "(%d) %s\n", frameNo, messages[i] );
		} // if
		debugprt( helpText, len );
	} // for
	free( messages );
} // backtrace
#endif // __DEBUG__


//####################### SIGSEGV/SIGBUS ####################


#ifdef __DEBUG__
#include <signal.h>										// get_nprocs

#define __SIGCXT__ ucontext_t *
#define __SIGPARMS__ int sig __attribute__(( unused )), siginfo_t * sfp __attribute__(( unused )), __SIGCXT__ cxt __attribute__(( unused ))

static void signal( int sig, void (*handler)(__SIGPARMS__), int flags ) { // name clash with uSignal statement
	struct sigaction act;

	act.sa_sigaction = (void (*)(int, siginfo_t *, void *))handler;
	sigemptyset( &act.sa_mask );
	sigaddset( &act.sa_mask, SIGALRM );					// disabled during signal handler
	sigaddset( &act.sa_mask, SIGUSR1 );
	sigaddset( &act.sa_mask, SIGSEGV );
	sigaddset( &act.sa_mask, SIGBUS );
	sigaddset( &act.sa_mask, SIGILL );
	sigaddset( &act.sa_mask, SIGFPE );
	sigaddset( &act.sa_mask, SIGHUP );					// revert to default on second delivery
	sigaddset( &act.sa_mask, SIGTERM );
	sigaddset( &act.sa_mask, SIGINT );

	act.sa_flags = flags;

	if ( sigaction( sig, &act, nullptr ) == -1 ) {
		char helpText[256];
		int len = snprintf( helpText, 256, "signal( sig:%d, handler:%p, flags:%d ), problem installing signal handler, error(%d) %s.\n",
							sig, handler, flags, errno, strerror( errno ) );
		if ( write( STDERR_FILENO, helpText, len ) ) {
			_exit( EXIT_FAILURE );
		} // if
	} // signal
}

static void sigSegvBusHandler( __SIGPARMS__ ) {
	signal_p = true;
	if ( sfp->si_addr == nullptr ) {
		abort( "Null pointer (nullptr) dereference." );
	} else if ( sfp->si_addr ==
#if __WORDSIZE == 32
				(void *)0xffff'ffff
#else
				(void *)0xffff'ffff'ffff'ffff
#endif // __WORDSIZE == 32
		) {
		abort( "Using a scrubbed pointer address %p.\n"
			   "Possible cause is using uninitialized storage or using storage after it has been freed.",
			   sfp->si_addr );
	} else {
		abort( "%s at memory location %p.\n"
			   "Possible cause is reading outside the address space or writing to a protected area within the address space with an invalid pointer or subscript.",
			   (sig == SIGSEGV ? "Segment fault" : "Bus error"), sfp->si_addr );
	} // if
} // sigSegvBusHandler
#endif // __DEBUG__


//####################### Heap Statistics ####################


#ifdef __STATISTICS__
enum { CntTriples = 13 };								// number of counter triples
struct HeapStatistics {
	enum { MALLOC, AALLOC, CALLOC, MEMALIGN, AMEMALIGN, CMEMALIGN, RESIZE, REALLOC, FREE };
	union {
		// Statistic counters are unsigned long long int => use 64-bit counters on both 32 and 64 bit architectures.
		// On 32-bit architectures, the 64-bit counters are simulated with multi-precise 32-bit computations.
		struct {										// minimum qualification
			unsigned long long int malloc_calls, malloc_0_calls;
			unsigned long long int malloc_storage_request, malloc_storage_alloc;
			unsigned long long int aalloc_calls, aalloc_0_calls;
			unsigned long long int aalloc_storage_request, aalloc_storage_alloc;
			unsigned long long int calloc_calls, calloc_0_calls;
			unsigned long long int calloc_storage_request, calloc_storage_alloc;
			unsigned long long int memalign_calls, memalign_0_calls;
			unsigned long long int memalign_storage_request, memalign_storage_alloc;
			unsigned long long int amemalign_calls, amemalign_0_calls;
			unsigned long long int amemalign_storage_request, amemalign_storage_alloc;
			unsigned long long int cmemalign_calls, cmemalign_0_calls;
			unsigned long long int cmemalign_storage_request, cmemalign_storage_alloc;
			unsigned long long int resize_calls, resize_0_calls;
			unsigned long long int resize_storage_request, resize_storage_alloc;
			unsigned long long int realloc_calls, realloc_0_calls;
			unsigned long long int realloc_storage_request, realloc_storage_alloc;
			unsigned long long int realloc_copy, realloc_smaller;
			unsigned long long int realloc_align, realloc_0_fill;
			unsigned long long int free_calls, free_null_0_calls;
			unsigned long long int free_storage_request, free_storage_alloc;
			unsigned long long int remote_pushes, remote_pulls;
			unsigned long long int remote_storage_request, remote_storage_alloc;
			unsigned long long int mmap_calls, mmap_0_calls; // no zero calls
			unsigned long long int mmap_storage_request, mmap_storage_alloc;
			unsigned long long int munmap_calls, munmap_0_calls; // no zero calls
			unsigned long long int munmap_storage_request, munmap_storage_alloc;
		};
		struct {										// overlay for iteration
			unsigned long long int calls, calls_0;
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


enum {
	// The minimum allocation alignment in bytes, which must be <= smallest free bucket.
	__ALIGN__ = __BIGGEST_ALIGNMENT__,					// magic CPP variable
}; // enum

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
		#ifdef __OWNERSHIP__
		#ifdef __REMOTESPIN__
		SpinLock_t remoteLock;
		#endif // __REMOTESPIN__
		Storage * remoteList;							// other thread remote list
		#endif // __OWNERSHIP__

		Storage * freeList;								// thread free list
		Heap * homeManager;								// heap owner (free storage to bucket, from bucket to heap)
		size_t blockSize;								// size of allocations on this list
		#if defined( __STATISTICS__ )
		size_t allocations, reuses;
		#endif // __STATISTICS__
	}; // FreeHeader

	// Recursive definitions: HeapManager needs size of bucket array and bucket area needs sizeof HeapManager storage.
	// Break recursion by hardcoding number of buckets and statically checking number is correct after bucket array defined.
	enum { NoBucketSizes = 96 };						// number of bucket sizes

	FreeHeader freeLists[NoBucketSizes];				// buckets for different allocation sizes
	void * bufStart;									// start of current buffer
	size_t bufRemaining;								// remaining free storage in buffer

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


// Size of array must harmonize with NoBucketSizes and individual bucket sizes must be multiple of 16.
// Smaller multiples of 16 and powers of 2 are common allocation sizes, so make them generate the minimum required bucket size.
static const unsigned int CALIGN bucketSizes[] = {		// different bucket sizes
	// There is no 0-sized bucket becasue it is better to create a 16 byte bucket for rare malloc(0), which can be
	// reused later by a 16-byte allocation.
	16 + sizeof(Heap::Storage), 32 + sizeof(Heap::Storage), 48 + sizeof(Heap::Storage), 64 + sizeof(Heap::Storage), // 4
	80 + sizeof(Heap::Storage), 96 + sizeof(Heap::Storage), 112 + sizeof(Heap::Storage), 128 + sizeof(Heap::Storage), // 4
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
	6'291'456, 8'388'608 + sizeof(Heap::Storage), 12'582'912, 16'777'216 + sizeof(Heap::Storage), // 4
};

static_assert( Heap::NoBucketSizes == sizeof(bucketSizes) / sizeof(bucketSizes[0] ), "size of bucket array is wrong" );

#ifdef __FASTLOOKUP__
enum { LookupSizes = 65'536 + sizeof(Heap::Storage) };	// number of fast lookup sizes
static unsigned char CALIGN lookup[LookupSizes];		// O(1) lookup for small sizes
#endif // __FASTLOOKUP__


// Manipulate sticky bits stored in unused 3 low-order bits of an address.
//   bit0 => alignment => fake header
//   bit1 => zero filled (calloc)
//   bit2 => mapped allocation versus sbrk
#define StickyBits( header ) (((header)->kind.real.blockSize & 0x7))
#define ClearStickyBits( addr ) (decltype(addr))((uintptr_t)(addr) & ~7)
#define MarkAlignmentBit( alignment ) ((alignment) | 1)
#define AlignmentBit( header ) ((((header)->kind.fake.alignment) & 1))
#define ClearAlignmentBit( header ) (((header)->kind.fake.alignment) & ~1)
#define ZeroFillBit( header ) ((((header)->kind.real.blockSize) & 2))
#define ClearZeroFillBit( header ) ((((header)->kind.real.blockSize) &= ~2))
#define MarkZeroFilledBit( header ) ((header)->kind.real.blockSize |= 2)
#define MmappedBit( header ) ((((header)->kind.real.blockSize) & 4))
#define MarkMmappedBit( size ) ((size) | 4)


enum {
	// The default extension heap amount in units of bytes. When the current heap reaches the brk address, the brk
	// address is extended by the extension amount.
	__DEFAULT_HEAP_EXTEND__ = 8 * 1024 * 1024,

	// The mmap crossover point during allocation. Allocations less than this amount are allocated from buckets; values
	// greater than or equal to this value are mmap from the operating system.
	__DEFAULT_MMAP_START__ = 8 * 1024 * 1024 + sizeof(Heap::Storage),

	// The default unfreed storage amount in units of bytes. When the program ends it subtracts this amount from
	// the malloc/free counter to adjust for storage the program does not free.
	__DEFAULT_HEAP_UNFREED__ = 0
}; // enum

static void heapManagerCtor();
static void heapManagerDtor();


namespace {												// hide static members
	// Used solely to detect when a thread terminates. Thread creation is handled by a fastpath dynamic check.
	struct ThreadManager {
		volatile bool trigger;							// used to trigger allocation of thread-local object, otherwise unused
		~ThreadManager() { heapManagerDtor(); }			// called automagically when thread terminates
	}; // ThreadManager

	struct HeapMaster {
		SpinLock_t extLock;								// protects allocation-buffer extension
		SpinLock_t mgrLock;								// protects freeHeapManagersList, heapManagersList, heapManagersStorage, heapManagersStorageEnd

		void * sbrkStart;								// start of sbrk storage
		void * sbrkEnd;									// end of sbrk area (logical end of heap)
		size_t sbrkRemaining;							// amount of free storage at end of sbrk area
		size_t sbrkExtend;								// sbrk extend amount
		size_t pageSize;								// architecture pagesize
		size_t mmapStart;								// cross over point for mmap
		size_t maxBucketsUsed;							// maximum number of buckets in use

		#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
		Heap * heapManagersList;						// heap stack head
		#endif // __STATISTICS__ || __DEBUG__
		Heap * freeHeapManagersList;					// free stack head

		// Heap superblocks are not linked; heaps in superblocks are linked via intrusive links.
		Heap * heapManagersStorage;						// next heap to use in heap superblock
		Heap * heapManagersStorageEnd;					// logical heap outside of superblock's end

		#ifdef __DEBUG__
		ptrdiff_t allocUnfreed;							// running total of allocations minus frees; can be negative
		#endif // __DEBUG__

		#ifdef __STATISTICS__
		HeapStatistics stats;							// global stats for thread-local heaps to add there counters when exiting
		unsigned long long int nremainder, remainder;	// counts mostly unusable storage at the end of a thread's reserve block
		unsigned long long int threads_started, threads_exited; // counts threads that have started and exited
		unsigned long long int reused_heap, new_heap;	// counts reusability of heaps
		unsigned long long int sbrk_calls;
		unsigned long long int sbrk_storage;
		int stats_fd;
		#endif // __STATISTICS__

		static void heapMasterCtor();
		static Heap * getHeap();
	}; // HeapMaster
} // namespace

static volatile bool heapMasterBootFlag = false;		// trigger for first heap
static HeapMaster heapMaster;							// program global


// Thread-local storage is allocated lazily when the storage is accessed.
static thread_local size_t PAD1 CALIGN TLSMODEL __attribute__(( unused )); // protect prior false sharing
static thread_local ThreadManager threadManager CALIGN TLSMODEL;
static thread_local Heap * heapManager CALIGN TLSMODEL = (Heap *)1; // singleton
static thread_local size_t PAD2 CALIGN TLSMODEL __attribute__(( unused )); // protect further false sharing


// declare helper functions for HeapMaster
extern "C" void noMemory( void );						// forward, called by "builtin_new" when malloc returns 0


void HeapMaster::heapMasterCtor() {
	// Singleton pattern to initialize heap master

	heapMaster.pageSize = sysconf( _SC_PAGESIZE );

	heapMaster.extLock = 0;
	heapMaster.mgrLock = 0;

	char * end = (char *)sbrk( 0 );
	heapMaster.sbrkStart = heapMaster.sbrkEnd = sbrk( (char *)Ceiling( (long unsigned int)end, __ALIGN__ ) - end ); // move start of heap to multiple of alignment
	heapMaster.sbrkRemaining = 0;
	heapMaster.sbrkExtend = malloc_extend();
	heapMaster.mmapStart = malloc_mmap_start();

	// find the closest bucket size less than or equal to the mmapStart size
	heapMaster.maxBucketsUsed = Bsearchl( heapMaster.mmapStart, bucketSizes, Heap::NoBucketSizes ); // binary search

	assert( (heapMaster.mmapStart >= heapMaster.pageSize) && (bucketSizes[Heap::NoBucketSizes - 1] >= heapMaster.mmapStart) );
	assert( heapMaster.maxBucketsUsed < Heap::NoBucketSizes ); // subscript failure ?
	assert( heapMaster.mmapStart <= bucketSizes[heapMaster.maxBucketsUsed] ); // search failure ?

	#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
	heapMaster.heapManagersList = nullptr;
	#endif // __STATISTICS__ || __DEBUG__
	heapMaster.freeHeapManagersList = nullptr;

	heapMaster.heapManagersStorage = nullptr;
	heapMaster.heapManagersStorageEnd = nullptr;

	#ifdef __STATISTICS__
	HeapStatisticsCtor( heapMaster.stats );				// clear statistic counters
	heapMaster.nremainder = heapMaster.remainder = 0;
	heapMaster.threads_started = heapMaster.threads_exited = 0;
	heapMaster.reused_heap = heapMaster.new_heap = 0;
	heapMaster.sbrk_calls = heapMaster.sbrk_storage = 0;
	heapMaster.stats_fd = STDERR_FILENO;
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	LLDEBUG( debugprt( "MCtor %jd set to zero\n", heapMaster.allocUnfreed ) );
	heapMaster.allocUnfreed = 0;
	signal( SIGSEGV, sigSegvBusHandler, SA_SIGINFO | SA_ONSTACK ); // Invalid memory reference (default: Core)
	signal( SIGBUS,  sigSegvBusHandler, SA_SIGINFO | SA_ONSTACK ); // Bus error, bad memory access (default: Core)
	#endif // __DEBUG__

	#ifdef __FASTLOOKUP__
	for ( unsigned int i = 0, idx = 0; i < LookupSizes; i += 1 ) {
		if ( i > bucketSizes[idx] ) idx += 1;
		lookup[i] = idx;
		always_assert( i <= bucketSizes[idx] ||			// overflow buckets ?
					   i > bucketSizes[idx - 1] );		// overlapping bucket sizes ?
	} // for
	#endif // __FASTLOOKUP__

	std::set_new_handler( noMemory );					// do not throw exception as the default

	heapMasterBootFlag = true;
} // HeapMaster::heapMasterCtor


#define NO_MEMORY_MSG "**** Error **** insufficient heap memory available to allocate %zd new bytes."

Heap * HeapMaster::getHeap() {
	Heap * heap;
	if ( heapMaster.freeHeapManagersList ) {			// free heap for reused ?
		// pop heap from stack of free heaps for reusability
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
			int dimension = get_nprocs();				// get_nprocs_conf does not work
			size_t size = dimension * sizeof( Heap );

			heapMaster.heapManagersStorage = (Heap *)mmap( 0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
			if ( UNLIKELY( heapMaster.heapManagersStorage == MAP_FAILED ) ) { // failed ?
				if ( errno == ENOMEM ) abort( NO_MEMORY_MSG, size ); // no memory
				// Do not call strerror( errno ) as it may call malloc.
				abort( "**** Error **** attempt to allocate block of heaps of size %zu bytes and mmap failed with errno %d.", size, errno );
			} // if
			heapMaster.heapManagersStorageEnd = &heapMaster.heapManagersStorage[dimension]; // outside array
		} // if

		heap = heapMaster.heapManagersStorage;
		heapMaster.heapManagersStorage = heapMaster.heapManagersStorage + 1; // bump next heap

		#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
		heap->nextHeapManager = heapMaster.heapManagersList;
		heapMaster.heapManagersList = heap;
		#endif // __STATISTICS__ || __DEBUG__

		#ifdef __STATISTICS__
		heapMaster.new_heap += 1;
		#endif // __STATISTICS__

		for ( unsigned int j = 0; j < Heap::NoBucketSizes; j += 1 ) { // initialize free lists
			heap->freeLists[j] = (Heap::FreeHeader){
				#ifdef __OWNERSHIP__
				#ifdef __REMOTESPIN__
				.remoteLock = 0,
				#endif // __REMOTESPIN__
				.remoteList = nullptr,
				#endif // __OWNERSHIP__

				.freeList = nullptr,
				.homeManager = heap,
				.blockSize = bucketSizes[j],

				#if defined( __STATISTICS__ )
				.allocations = 0,
				.reuses = 0,
				#endif // __STATISTICS__
			};
		} // for

		heap->bufStart = nullptr;
		heap->bufRemaining = 0;
		heap->nextFreeHeapManager = nullptr;

		#ifdef __DEBUG__
		heap->allocUnfreed = 0;
		#endif // __DEBUG__
	} // if

	return heap;
} // HeapMaster::getHeap


static void heapManagerCtor() {
	if ( UNLIKELY( ! heapMasterBootFlag ) ) HeapMaster::heapMasterCtor();

	spin_acquire( &heapMaster.mgrLock );				// protect heapMaster counters

	// get storage for heap manager

	heapManager = HeapMaster::getHeap();

	#ifdef __STATISTICS__
	HeapStatisticsCtor( heapManager->stats );			// heap local
	heapMaster.threads_started += 1;
	#endif // __STATISTICS__

	spin_release( &heapMaster.mgrLock );

	// Trigger thread_local storage implicit allocation, which causes a dynamic allocation but not a recursive entry
	// because heapManager is set above.
	threadManager.trigger = true;						// any value works
} // heapManagerCtor


static void heapManagerDtor() {
	spin_acquire( &heapMaster.mgrLock );				// protect heapMaster counters

	// push heap from stack of free heaps for reusability
	heapManager->nextFreeHeapManager = heapMaster.freeHeapManagersList;
	heapMaster.freeHeapManagersList = heapManager;

	#ifdef __DEBUG__
	LLDEBUG( debugprt( "HDtor %p %jd %jd\n", heapManager, heapManager->allocUnfreed, heapMaster.allocUnfreed ) );
	#endif // __DEBUG__

	#ifdef __STATISTICS__
	heapMaster.stats += heapManager->stats;				// retain this heap's statistics
	HeapStatisticsCtor( heapManager->stats );			// reset heap counters for next usage
	heapMaster.threads_exited += 1;
	#endif // __STATISTICS__

	// SKULLDUGGERY: The thread heap ends BEFORE the last free(s) occurs from the thread-local storage allocations for
	// the thread. This final allocation must be handled in doFree for this thread and its terminated heap. However,
	// this heap has just been put on the heap freelist, and hence there is a race returning the thread-local storage
	// and a new thread using this heap. The current thread detects it is executing its last free in doFree via
	// heapManager being null. The trick is for this thread to place the last free onto the current heap's remote-list as
	// the free-storage header points are this heap. Now, even if other threads are pushing to the remote list, it is safe
	// because of the locking.

	heapManager = nullptr;								// => heap not in use
	spin_release( &heapMaster.mgrLock );
} // heapManagerDtor


//####################### Memory Allocation Routines Helpers ####################


NOWARNING( __attribute__(( constructor( 100 ) )) static void startup( void ) {, prio-ctor-dtor ) // singleton => called once at start of program
	if ( ! heapMasterBootFlag ) { heapManagerCtor(); }	// sanity check
	#ifdef __DEBUG__
	heapManager->allocUnfreed = 0;						// clear prior allocation counts
	#endif // __DEBUG__
} // startup

NOWARNING( __attribute__(( destructor( 100 ) )) static void shutdown( void ) {, prio-ctor-dtor ) // singleton => called once at end of program
	if ( getenv( "MALLOC_STATS" ) ) {					// check for external printing
		malloc_stats();

		#ifdef __STATISTICS__
		char helpText[128];
		int len = snprintf( helpText, sizeof(helpText), "\nFree Bucket Usage: (bucket-size/allocations/reuses)\n" );
		int unused __attribute__(( unused )) = write( STDERR_FILENO, helpText, len ); // file might be closed

		size_t th = 0, total = 0;
		for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager, th += 1 ) {
			enum { Columns = 8 };
			len = snprintf( helpText, sizeof(helpText), "Heap %'zd\n", th );
			unused = write( STDERR_FILENO, helpText, len ); // file might be closed
			for ( size_t b = 0, c = 0; b < Heap::NoBucketSizes; b += 1 ) {
				if ( heap->freeLists[b].allocations != 0 ) {
					total += heap->freeLists[b].blockSize * heap->freeLists[b].allocations;
					len = snprintf( helpText, sizeof(helpText), "%'zd/%'zd/%'zd, ",
									heap->freeLists[b].blockSize, heap->freeLists[b].allocations, heap->freeLists[b].reuses );
					unused = write( STDERR_FILENO, helpText, len ); // file might be closed
					if ( ++c % Columns == 0 )
						unused = write( STDERR_FILENO, "\n", 1 ); // file might be closed
				} // if
			} // for
			unused = write( STDERR_FILENO, "\n", 1 );	// file might be closed
		} // for
		len = snprintf( helpText, sizeof(helpText), "Total bucket storage %'zd\n", total );
		unused = write( STDERR_FILENO, helpText, len ); // file might be closed
		#endif // __STATISTICS__
	} // if

	#ifdef __DEBUG__
	// allocUnfreed is set to 0 when a heap is created and it accumulates any unfreed storage during its multiple thread
	// usages.  At the end, add up each heap allocUnfreed value across all heaps to get the total unfreed storage.
	ptrdiff_t allocUnfreed = heapMaster.allocUnfreed;
	LLDEBUG( debugprt( "shutdown1 %jd\n", heapMaster.allocUnfreed ) );
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		LLDEBUG( debugprt( "shutdown2 %p %jd\n", heap, heap->allocUnfreed ) );
		allocUnfreed += heap->allocUnfreed;
	} // for

	allocUnfreed -= malloc_unfreed();					// subtract any user specified unfreed storage
	LLDEBUG( debugprt( "shutdown3 %td %zd\n", allocUnfreed, malloc_unfreed() ) );
	if ( allocUnfreed > 0 ) {
		// DO NOT USE STREAMS AS THEY MAY BE UNAVAILABLE AT THIS POINT.
		char helpText[BufSize];
		int len = snprintf( helpText, sizeof(helpText), "**** Warning **** (UNIX pid:%ld) : program terminating with %td(%#tx) bytes of storage allocated but not freed.\n"
							"Possible cause is unfreed storage allocated by the program or system/library routines called from the program.\n",
							(long int)getpid(), allocUnfreed, allocUnfreed ); // always print the UNIX pid
		int unused __attribute__(( unused )) = write( STDERR_FILENO, helpText, len ); // file might be closed
	} // if
	#endif // __DEBUG__
} // shutdown


#ifdef __STATISTICS__
#define prtFmt \
	"\nPID: %d Heap%s statistics: (storage request/allocation)\n" \
	"  malloc    >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  aalloc    >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  calloc    >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  memalign  >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  amemalign >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  cmemalign >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  resize    >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  realloc   >0 calls %'llu; 0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"            copies %'llu; smaller %'llu; alignment %'llu; 0 fill %'llu\n" \
	"  free      !null calls %'llu; null/0 calls %'llu; storage %'llu/%'llu bytes\n" \
	"  remote    pushes %'llu; pulls %'llu; storage %'llu/%'llu bytes\n" \
	"  sbrk      calls %'llu; storage %'llu bytes\n" \
	"  mmap      calls %'llu; storage %'llu/%'llu bytes\n" \
	"  munmap    calls %'llu; storage %'llu/%'llu bytes\n" \
	"  remainder calls %'llu; storage %'llu bytes\n" \
	"  threads   started %'llu; exited %'llu\n" \
	"  heaps     new %'llu; reused %'llu\n"

// Use "write" because streams may be shutdown when calls are made.
static int printStats( HeapStatistics & stats, const char * title = "" ) { // see malloc_stats
	char helpText[sizeof(prtFmt) + 1024];				// space for message and values
	int len = snprintf( helpText, sizeof(helpText), prtFmt,	getpid(), title,
			stats.malloc_calls, stats.malloc_0_calls, stats.malloc_storage_request, stats.malloc_storage_alloc,
			stats.aalloc_calls, stats.aalloc_0_calls, stats.aalloc_storage_request, stats.aalloc_storage_alloc,
			stats.calloc_calls, stats.calloc_0_calls, stats.calloc_storage_request, stats.calloc_storage_alloc,
			stats.memalign_calls, stats.memalign_0_calls, stats.memalign_storage_request, stats.memalign_storage_alloc,
			stats.amemalign_calls, stats.amemalign_0_calls, stats.amemalign_storage_request, stats.amemalign_storage_alloc,
			stats.cmemalign_calls, stats.cmemalign_0_calls, stats.cmemalign_storage_request, stats.cmemalign_storage_alloc,
			stats.resize_calls, stats.resize_0_calls, stats.resize_storage_request, stats.resize_storage_alloc,
			stats.realloc_calls, stats.realloc_0_calls, stats.realloc_storage_request, stats.realloc_storage_alloc,
			stats.realloc_copy, stats.realloc_smaller, stats.realloc_align, stats.realloc_0_fill,
			stats.free_calls, stats.free_null_0_calls, stats.free_storage_request, stats.free_storage_alloc,
			stats.remote_pushes, stats.remote_pulls, stats.remote_storage_request, stats.remote_storage_alloc,
			heapMaster.sbrk_calls, heapMaster.sbrk_storage,
			stats.mmap_calls, stats.mmap_storage_request, stats.mmap_storage_alloc,
			stats.munmap_calls, stats.munmap_storage_request, stats.munmap_storage_alloc,
			heapMaster.nremainder, heapMaster.remainder,
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
	"<total type=\"malloc\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"aalloc\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"calloc\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"memalign\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"amemalign\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"cmemalign\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"resize\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"realloc\" >0 count=\"%'llu;\" 0 count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"       \" copy count=\"%'llu;\" smaller count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"free\" !null=\"%'llu;\" 0 null/0=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"remote\" pushes=\"%'llu;\" 0 pulls=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"sbrk\" count=\"%'llu;\" size=\"%'llu\"/> bytes\n" \
	"<total type=\"mmap\" count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"munmap\" count=\"%'llu;\" size=\"%'llu/%'llu\"/> bytes\n" \
	"<total type=\"remainder\" count=\"%'llu;\" size=\"%'llu\"/> bytes\n" \
	"<total type=\"threads\" started=\"%'llu;\" exited=\"%'llu\"/>\n" \
	"<total type=\"heaps\" new=\"%'llu;\" reused=\"%'llu\"/>\n" \
	"</malloc>"

static int printStatsXML( HeapStatistics & stats, FILE * stream ) { // see malloc_info
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
			stats.realloc_copy, stats.realloc_smaller, stats.realloc_align, stats.realloc_0_fill,
			stats.free_calls, stats.free_null_0_calls, stats.free_storage_request, stats.free_storage_alloc,
			stats.remote_pushes, stats.remote_pulls, stats.remote_storage_request, stats.remote_storage_alloc,
			heapMaster.sbrk_calls, heapMaster.sbrk_storage,
			stats.mmap_calls, stats.mmap_storage_request, stats.mmap_storage_alloc,
			stats.munmap_calls, stats.munmap_storage_request, stats.munmap_storage_alloc,
			heapMaster.nremainder, heapMaster.remainder,
			heapMaster.threads_started, heapMaster.threads_exited,
			heapMaster.new_heap, heapMaster.reused_heap
		);
	return write( fileno( stream ), helpText, len );
} // printStatsXML

static HeapStatistics & collectStats( HeapStatistics & stats ) {
	spin_acquire( &heapMaster.mgrLock );

	// Accumulate the heap master and all active thread heaps.
	stats += heapMaster.stats;
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		stats += heap->stats;							// calls HeapStatistics +=
	} // for

	spin_release(&heapMaster.mgrLock);
	return stats;
} // collectStats

static void clearStats() {
	spin_acquire( &heapMaster.mgrLock );

	// Zero the heap master and all active thread heaps.
	HeapStatisticsCtor( heapMaster.stats );
	for ( Heap * heap = heapMaster.heapManagersList; heap; heap = heap->nextHeapManager ) {
		HeapStatisticsCtor( heap->stats );
	} // for

	spin_release(&heapMaster.mgrLock);
} // clearStats
#endif // __STATISTICS__


extern "C" void noMemory( void ) {
	abort( "**** Error **** heap memory exhausted at %zu bytes.\n"
		   "Possible cause is very large memory allocation and/or large amount of unfreed storage allocated by the program or system/library routines.",
		   ((char *)(sbrk( 0 )) - (char *)(heapMaster.sbrkStart)) );
} // noMemory


static inline __attribute__((always_inline)) bool setMmapStart( size_t value ) { // true => mmapped, false => sbrk
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
//               alignment/offset |
// <-----------------<------------+-----------------------------> bsize (bucket size)
//                   |fake-header | addr
#define HeaderAddr( addr ) ((Heap::Storage::Header *)( (char *)addr - sizeof(Heap::Storage) ))
#define RealHeader( header ) ((Heap::Storage::Header *)((char *)header - header->kind.fake.offset))

// <-------<<--------------------- dsize ---------------------->> bsize (bucket size)
// |header |addr
//==================================================================================
//               alignment/offset |
// <------------------------------<<---------- dsize --------->>> bsize (bucket size)
//                   |fake-header |addr
#define DataStorage( bsize, addr, header ) (bsize - ( (char *)addr - (char *)header ))


static inline __attribute__((always_inline)) void checkAlign( size_t alignment ) {
	if ( UNLIKELY( alignment < __ALIGN__ || ! Pow2( alignment ) ) ) {
		abort( "**** Error **** alignment %zu for memory allocation is less than %d and/or not a power of 2.", alignment, __ALIGN__ );
	} // if
} // checkAlign


static inline __attribute__((always_inline)) void checkHeader( bool check, const char name[], void * addr ) {
	if ( UNLIKELY( check ) ) {							// bad address ?
		abort( "**** Error **** attempt to %s storage %p with address outside the heap range %p<->%p.\n"
			   "Possible cause is duplicate free on same block or overwriting of memory.",
			   name, addr, heapMaster.sbrkStart, heapMaster.sbrkEnd );
	} // if
} // checkHeader


static inline __attribute__((always_inline)) void fakeHeader( Heap::Storage::Header *& header, size_t & alignment ) {
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


static inline __attribute__((always_inline))
bool headers( const char name[] __attribute__(( unused )), void * addr, Heap::Storage::Header *& header,
					 Heap::FreeHeader *& freeHead, size_t & size, size_t & alignment ) {
	header = HeaderAddr( addr );

	#ifdef __DEBUG__
	// Mapped addresses can be any values.
	if ( LIKELY( ! MmappedBit( header ) ) ) checkHeader( header < heapMaster.sbrkStart, name, addr ); // bad low address ?
	#endif // __DEBUG__

	if ( LIKELY( ! StickyBits( header ) ) ) {			// no sticky bits ?
		freeHead = header->kind.real.home;
		alignment = __ALIGN__;
	} else {
		fakeHeader( header, alignment );
		if ( UNLIKELY( MmappedBit( header ) ) ) {		// mapped storage ?
			assert( addr < heapMaster.sbrkStart || heapMaster.sbrkEnd < addr );
			size = ClearStickyBits( header->kind.real.blockSize ); // mmap size
			freeHead = nullptr;							// prevent uninitialized warning
			return true;
		} // if

		freeHead = ClearStickyBits( header->kind.real.home );
	} // if
	size = freeHead->blockSize;

	#ifdef __DEBUG__
	checkHeader( header < heapMaster.sbrkStart || heapMaster.sbrkEnd < header, name, addr ); // bad address ? (offset could be + or -)

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


static inline __attribute__((always_inline)) void * master_extend( size_t size ) {
	spin_acquire( &heapMaster.extLock );

	ptrdiff_t rem = heapMaster.sbrkRemaining - size;
	if ( UNLIKELY( rem < 0 ) ) {						// negative ?
		// If the size requested is bigger than the current remaining storage, increase the size of the heap.
		size_t increase = Ceiling( size > heapMaster.sbrkExtend ? size : heapMaster.sbrkExtend, __ALIGN__ );
		if ( UNLIKELY( sbrk( increase ) == (void *)-1 ) ) {	// failed, no memory ?
			spin_release( &heapMaster.extLock );
			abort( NO_MEMORY_MSG, size );				// give up
		} // if
		rem = heapMaster.sbrkRemaining + increase - size;

		#ifdef __STATISTICS__
		heapMaster.sbrk_calls += 1;
		heapMaster.sbrk_storage += increase;
		#endif // __STATISTICS__
	} // if

	Heap::Storage * block = (Heap::Storage *)heapMaster.sbrkEnd;
	heapMaster.sbrkRemaining = rem;
	heapMaster.sbrkEnd = (char *)heapMaster.sbrkEnd + size;

	spin_release( &heapMaster.extLock );
	return block;
} // master_extend


static void * manager_extend( size_t size ) {
	// If the size requested is bigger than the current remaining reserve, so increase the reserve.
	size_t increase = Ceiling( size > ( heapMaster.sbrkExtend / 16 ) ? size : ( heapMaster.sbrkExtend / 16 ), __ALIGN__ );
	void * newblock = master_extend( increase );

	// Check if the new reserve block is contiguous with the old block (The only good storage is contiguous storage!)
	// For sequential programs, this check is always true.
	if ( newblock != (char *)heapManager->bufStart + heapManager->bufRemaining ) {
		// Otherwise, find the closest bucket size to the remaining storage in the reserve block and chain it onto
		// that free list. Distributing the storage across multiple free lists is an option but takes time.
		ptrdiff_t rem = heapManager->bufRemaining;		// positive

		if ( (decltype(bucketSizes[0]))rem >= bucketSizes[0] ) { // minimal size ? otherwise ignore
			#ifdef __STATISTICS__
			heapMaster.nremainder += 1;
			heapMaster.remainder += rem;
			#endif // __STATISTICS__

			Heap::FreeHeader * freeHead =
			#ifdef __FASTLOOKUP__
				rem < LookupSizes ? &(heapManager->freeLists[lookup[rem]]) :
			#endif // __FASTLOOKUP__
				&(heapManager->freeLists[Bsearchl( rem, bucketSizes, heapMaster.maxBucketsUsed )]); // binary search

			// The remaining storage may not be bucket size, whereas all other allocations are. Round down to previous
			// bucket size in this case.
			if ( UNLIKELY( freeHead->blockSize > (size_t)rem ) ) freeHead -= 1;
			Heap::Storage * block = (Heap::Storage *)heapManager->bufStart;

			block->header.kind.real.next = freeHead->freeList; // push on stack
			freeHead->freeList = block;
		} // if
	} // if

	heapManager->bufRemaining = increase - size;
	heapManager->bufStart = (char *)newblock + size;
	return newblock;
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

#define BOOT_HEAP_MANAGER() \
  	if ( UNLIKELY( heapManager == (Heap *)1 ) ) { /* new thread ? */ \
		heapManagerCtor(); /* trigger for first heap, singleton */ \
		assert( heapManager ); \
	} /* if */

// NULL_0_ALLOC is disabled because a lot of programs incorrectly check for out of memory by just checking for a NULL
// return from malloc, rather than checking both NULL return and errno == ENOMEM.
//#define __NULL_0_ALLOC__ /* Uncomment to return null address for malloc( 0 ). */

// Do not use '\xfe' for scrubbing because dereferencing an address composed of it causes a SIGSEGV *without* a valid IP
// pointer in the interrupt frame.
#define SCRUB '\xff'									// scrub value
#define SCRUB_SIZE 1024lu								// scrub size, front/back/all of allocation area

static inline __attribute__((always_inline)) void * doMalloc( size_t size STAT_PARM ) {
	BOOT_HEAP_MANAGER();

	#ifdef __NULL_0_ALLOC__
	if ( UNLIKELY( size == 0 ) ) {
		STAT_0_CNT( STAT_NAME );
		return nullptr;
	} // if
	#endif // __NULL_0_ALLOC__

	#ifdef __DEBUG__
	if ( UNLIKELY( size > ULONG_MAX - sizeof(Heap::Storage) ) ) {
		errno = ENOMEM;
		return nullptr;
	} // if
	#endif // __DEBUG__

	Heap::Storage * block;								// pointer to new block of storage

	// Look up size in the size list.

	// The user request must include space for the header allocated along with the storage block and is a multiple of
	// the alignment size.
	size_t tsize = size + sizeof(Heap::Storage);		// total request space needed
	Heap * heap = heapManager;							// optimization

	#ifdef __STATISTICS__
	if ( UNLIKELY( size == 0 ) ) {						// malloc( 0 ) ?
		heap->stats.counters[STAT_NAME].calls_0 += 1;
	} else {
		heap->stats.counters[STAT_NAME].calls += 1;
		heap->stats.counters[STAT_NAME].request += size;
	} // if
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	heap->allocUnfreed += size;
	#endif // __DEBUG__

	// Memory is scrubbed in doFree for debug, so no scrubbing on allocation side.

	if ( LIKELY( size < heapMaster.mmapStart ) ) {		// small size => sbrk
		Heap::FreeHeader * freeHead =
			#ifdef __FASTLOOKUP__
			LIKELY( tsize < LookupSizes ) ? &(heap->freeLists[lookup[tsize]]) :
			#endif // __FASTLOOKUP__
			&(heapManager->freeLists[Bsearchl( tsize, bucketSizes, heapMaster.maxBucketsUsed )]); // binary search

		assert( freeHead <= &heap->freeLists[heapMaster.maxBucketsUsed] ); // subscripting error ?
		assert( tsize <= freeHead->blockSize );			// search failure ?

		#ifdef __STATISTICS__
		heap->stats.counters[STAT_NAME].alloc += freeHead->blockSize; // total space needed for request
		#endif // __STATISTICS__

		// The checking order for freed storage versus bump storage has a performance difference, if there are lots of
		// allocations before frees. The following checks for freed storage first in an attempt to reduce the storage
		// footprint, i.e., starting using freed storage before using all the free block.

		block = freeHead->freeList;						// remove node from stack
		if ( LIKELY( block != nullptr ) ) {				// free block ?
			// Get storage from the corresponding free list.
			freeHead->freeList = block->header.kind.real.next;

			#ifdef __STATISTICS__
			freeHead->reuses += 1;
			#endif // __STATISTICS__
		} else {
			// Get storage from free block using bump allocation.
			tsize = freeHead->blockSize;				// optimization, bucket size for request
			ptrdiff_t rem = heapManager->bufRemaining - tsize;
			if ( LIKELY( rem >= 0 ) ) {					// bump storage ?
				heapManager->bufRemaining = rem;
				block = (Heap::Storage *)heapManager->bufStart;
				heapManager->bufStart = (char *)heapManager->bufStart + tsize;
			#ifdef __OWNERSHIP__
				// Race with adding thread, get next time if lose race.
			} else if ( UNLIKELY( freeHead->remoteList ) ) { // returned space ?
				// Get storage by removing entire remote list and chain onto appropriate free list.
				#ifdef __REMOTESPIN__
				spin_acquire( &freeHead->remoteLock );
				block = freeHead->remoteList;
				freeHead->remoteList = nullptr;
				spin_release( &freeHead->remoteLock );
				#else
				block = Fas( freeHead->remoteList, nullptr );
				#endif // __REMOTESPIN__

				assert( block );
				#ifdef __STATISTICS__
				heap->stats.remote_pulls += 1;
				#endif // __STATISTICS__

				freeHead->freeList = block->header.kind.real.next; // merge remoteList into freeHead
			#endif // __OWNERSHIP__
			} else {
				// Get storage from a *new* free block using bump alocation.
				block = (Heap::Storage *)manager_extend( tsize ); // mutual exclusion on call

				#ifdef __DEBUG__
				// Scrub new memory so subsequent uninitialized usages might fail. Only scrub the first SCRUB_SIZE bytes.
				memset( block->data, SCRUB, Min( SCRUB_SIZE, tsize - sizeof(Heap::Storage) ) );
				#endif // __DEBUG__
			} // if

			#ifdef __STATISTICS__
			freeHead->allocations += 1;
			#endif // __STATISTICS__
		} // if

		block->header.kind.real.home = freeHead;		// pointer back to free list of apropriate size
	} else {											// large size => mmap
		#ifdef __DEBUG__
		/// Recheck because of minimum allocation size (page size).
		if ( UNLIKELY( size > ULONG_MAX - heapMaster.pageSize ) ) {
			errno = ENOMEM;
			return nullptr;
		} // if
		#endif // __DEBUG__

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
	LLDEBUG( debugprt( "\tdoMalloc %p %zd %zd %jd\n", heap, size, tsize, heap->allocUnfreed ) );
	#endif // __DEBUG__

	return addr;
} // doMalloc


static inline __attribute__((always_inline)) void doFree( void * addr ) {
	#if defined( __STATISTICS__ ) || defined( __DEBUG__ ) || ! defined( __OWNERSHIP__ )
	// A thread can run without a heap, and hence, have an uninitialized heapManager. For example, in the ownership
	// program, the consumer thread does not allocate storage, it only frees storage back to the owning producer
	// thread. Hence, the consumer never calls doMalloc to trigger heap creation for its thread. However, this situation
	// is a problem for statistics, the consumer needs a heap to gather statistics. Hence, the special case to trigger a
	// heap if there is none.
	BOOT_HEAP_MANAGER();								// singlton
	#endif // __STATISTICS__ || __DEBUG__

	// At this point heapManager can be null because there is a thread_local deallocation *after* the destructor for the
	// thread termination is run. The code below is prepared to handle this case and uses the master heap for statistics.

	assert( addr );
	Heap * heap = heapManager;							// optimization

	Heap::Storage::Header * header;
	Heap::FreeHeader * freeHead;
	size_t tsize, alignment;

	bool mapped = headers( "free", addr, header, freeHead, tsize, alignment );
	#if defined( __STATISTICS__ ) || defined( __DEBUG__ )
	size_t size = header->kind.real.size;				// optimization
	#endif // __STATISTICS__ || __DEBUG__

	if ( LIKELY( ! mapped ) ) {							// sbrk ?
		assert( freeHead );
		#ifdef __DEBUG__
		// Scrub old memory so subsequent usages might fail. Only scrub the first/last SCRUB_SIZE bytes.
		char * data = ((Heap::Storage *)header)->data;	// data address
		size_t dsize = tsize - sizeof(Heap::Storage);	// data size
		if ( dsize <= SCRUB_SIZE * 2 ) {
			memset( data, SCRUB, dsize );				// scrub all
		} else {
			memset( data, SCRUB, SCRUB_SIZE );			// scrub front
			memset( data + dsize - SCRUB_SIZE, SCRUB, SCRUB_SIZE ); // scrub back
		} // if
		#endif // __DEBUG__

		#ifdef __OWNERSHIP__
		if ( LIKELY( heap == freeHead->homeManager ) ) { // belongs to this thread
			header->kind.real.next = freeHead->freeList; // push on stack
			freeHead->freeList = (Heap::Storage *)header;
		} else {										// return to thread owner
			#ifdef __REMOTESPIN__
			spin_acquire( &freeHead->remoteLock );
			header->kind.real.next = freeHead->remoteList; // push entire remote list to bucket
			freeHead->remoteList = (Heap::Storage *)header;
			spin_release( &freeHead->remoteLock );
			#else										// lock free
			header->kind.real.next = freeHead->remoteList; // link new node to top node
			// CAS resets header->kind.real.next = freeHead->remoteList on failure
			while ( ! Casv( freeHead->remoteList, &header->kind.real.next, (Heap::Storage *)header ) ) Pause();
			#endif // __REMOTESPIN__

			if ( UNLIKELY( heap == nullptr ) ) {
				// Use master heap counters as heap is reused by this point.
				#ifdef __STATISTICS__
				Fai( heapMaster.stats.remote_pushes, 1 );
				Fai( heapMaster.stats.free_storage_request, size );
				Fai( heapMaster.stats.free_storage_alloc, tsize );
				// Return push counters are not incremented because this is a self-return push, and there is no
				// corresponding pull counter that needs to match.
				#endif // __STATISTICS__

				#ifdef __DEBUG__
				Fai( heapMaster.allocUnfreed, -size );
				LLDEBUG( debugprt( "\tdoFree2 %zd %zd %jd\n", tsize, size, heapMaster.allocUnfreed ) );
				#endif // __DEBUG__
				return;
			} // if

			#ifdef __STATISTICS__
			heap->stats.remote_pushes += 1;
			heap->stats.remote_storage_request += size;
			heap->stats.remote_storage_alloc += tsize;
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
		#endif // __OWNERSHIP__
	} else {											// mmapped
		#ifdef __STATISTICS__
		heap->stats.munmap_calls += 1;
		heap->stats.munmap_storage_request += size;
		heap->stats.munmap_storage_alloc += tsize;
		#endif // __STATISTICS__

		if ( UNLIKELY( munmap( header, tsize ) == -1 ) ) {
			// Do not call strerror( errno ) as it may call malloc.
			abort( "**** Error **** attempt to deallocate large object %p and munmap failed with errno %d.\n"
				   "Possible cause is invalid delete pointer: either not allocated or with corrupt header.",
				   addr, errno );
		} // if
	} // if

	// Do not move these up because heap can be null!
	#ifdef __STATISTICS__
	#ifndef __NULL_0_ALLOC__
	if ( UNLIKELY( size == 0 ) )						// malloc( 0 ) ?
		heap->stats.free_null_0_calls += 1;
	else
	#endif // __NULL_0_ALLOC__
		heap->stats.free_calls += 1;					// count free amd implicit frees from resize/realloc
	heap->stats.free_storage_request += size;
	heap->stats.free_storage_alloc += tsize;
	#endif // __STATISTICS__

	#ifdef __DEBUG__
	heap->allocUnfreed -= size;
	LLDEBUG( debugprt( "\tdoFree %p %zd %zd %jd\n", heap, tsize, size, heap->allocUnfreed ) );
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


	// Same as malloc() except size bytes is an array of dimension elements each of elemSize bytes.
	void * aalloc( size_t dimension, size_t elemSize ) {
		return doMalloc( dimension * elemSize STAT_ARG( HeapStatistics::AALLOC ) );
	} // aalloc


	// Same as aalloc() with memory set to zero.
	void * calloc( size_t dimension, size_t elemSize ) {
		size_t size = dimension * elemSize;
		char * addr = (char *)doMalloc( size STAT_ARG( HeapStatistics::CALLOC ) );

	  if ( UNLIKELY( addr == NULL ) ) return NULL;		// stop further processing if NULL is returned

		Heap::Storage::Header * header = HeaderAddr( addr ); // optimization

		#ifndef __DEBUG__
		// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero.
		if ( LIKELY( ! MmappedBit( header ) ) )
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

	  if ( UNLIKELY( size == 0 ) ) {
			STAT_0_CNT( HeapStatistics::RESIZE );
			doFree( oaddr );
			return nullptr;
		} // if

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, oalignment;
		headers( "resize", oaddr, header, freeHead, bsize, oalignment );

		size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket
		// same size, DO NOT PRESERVE STICKY PROPERTIES.
		if ( oalignment == __ALIGN__ && size <= odsize && odsize <= size * 2 ) { // allow 50% wasted storage for smaller size
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


	// Same as resize() except the new allocation size is large enough for an array of nelem elements of size elemSize.
	void * resizearray( void * oaddr, size_t dimension, size_t elemSize ) {
		return resize( oaddr, dimension * elemSize );
	} // resizearray


	// Same as resize() but the contents are unchanged in the range from the start of the region up to the minimum of
	// the old and new sizes.
	void * realloc( void * oaddr, size_t size ) {
	  if ( UNLIKELY( oaddr == nullptr ) ) {				// => malloc( size )
		  return doMalloc( size STAT_ARG( HeapStatistics::REALLOC ) );
		} // if

	  if ( UNLIKELY( size == 0 ) ) {
			STAT_0_CNT( HeapStatistics::REALLOC );
			doFree( oaddr );
			return nullptr;
		} // if

		Heap::Storage::Header * header;
		Heap::FreeHeader * freeHead;
		size_t bsize, oalignment;
		headers( "realloc", oaddr, header, freeHead, bsize, oalignment );

		size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket
		size_t osize = header->kind.real.size;			// old allocation size
		bool ozfill = ZeroFillBit( header );			// old allocation zero filled
	  if ( UNLIKELY( size <= odsize ) && odsize <= size * 2 ) { // allow up to 50% wasted storage
			#ifdef __DEBUG__
			heapManager->allocUnfreed += size - header->kind.real.size; // adjustment off the size difference
			#endif // __DEBUG__
			header->kind.real.size = size;				// reset allocation size
	  		if ( UNLIKELY( ozfill ) && size > osize ) {	// previous request zero fill and larger ?
				#ifdef __STATISTICS__
				heapManager->stats.realloc_0_fill += 1;
				#endif // __STATISTICS__
	  			memset( (char *)oaddr + osize, '\0', size - osize ); // initialize added storage
	  		} // if
			#ifdef __STATISTICS__
			heapManager->stats.realloc_calls += 1;
			heapManager->stats.realloc_smaller += 1;
			#endif // __STATISTICS__
			return oaddr;
		} // if

		// change size and copy old content to new storage

		#ifdef __STATISTICS__
		heapManager->stats.realloc_copy += 1;
		#endif // __STATISTICS__

		void * naddr;
		if ( LIKELY( oalignment <= __ALIGN__ ) ) {			// previous request not aligned ?
			naddr = doMalloc( size STAT_ARG( HeapStatistics::REALLOC ) ); // create new area
		} else {
			#ifdef __STATISTICS__
			heapManager->stats.realloc_align += 1;
			#endif // __STATISTICS__
			naddr = memalignNoStats( oalignment, size STAT_ARG( HeapStatistics::REALLOC ) ); // create new aligned area
		} // if

		header = HeaderAddr( naddr );					// new header
		size_t alignment;
		fakeHeader( header, alignment );				// could have a fake header

		// To preserve prior fill, the entire bucket must be copied versus the size.
		memcpy( naddr, oaddr, Min( osize, size ) );		// copy bytes
		doFree( oaddr );								// free previous storage

		if ( UNLIKELY( ozfill ) ) {						// previous request zero fill ?
			MarkZeroFilledBit( header );				// mark new request as zero filled
			if ( size > osize ) {						// previous request larger ?
				#ifdef __STATISTICS__
				heapManager->stats.realloc_0_fill += 1;
				#endif // __STATISTICS__
				memset( (char *)naddr + osize, '\0', size - osize ); // initialize added storage
			} // if
		} // if
		return naddr;
	} // realloc


	// Same as realloc() except the new allocation size is large enough for an array of nelem elements of size elemSize.
	void * reallocarray( void * oaddr, size_t dimension, size_t elemSize ) {
		return realloc( oaddr, dimension * elemSize );
	} // reallocarray


	void * aligned_resize( void * oaddr, size_t nalignment, size_t size ) {
	  if ( UNLIKELY( oaddr == nullptr ) ) {				// => malloc( size )
			return memalignNoStats( nalignment, size STAT_ARG( HeapStatistics::RESIZE ) );
		} // if

	  if ( UNLIKELY( size == 0 ) ) {
			STAT_0_CNT( HeapStatistics::RESIZE );
			doFree( oaddr );
			return nullptr;
		} // if

		// Attempt to reuse existing alignment.
		Heap::Storage::Header * header = HeaderAddr( oaddr );
		bool isFakeHeader = AlignmentBit( header );		// old fake header ?
		size_t oalignment;

		if ( UNLIKELY( isFakeHeader ) ) {
			checkAlign( nalignment );					// check alignment
			oalignment = ClearAlignmentBit( header );	// old alignment

			if ( UNLIKELY( (uintptr_t)oaddr % nalignment == 0 // lucky match ?
				 && ( oalignment <= nalignment			// going down
					  || (oalignment >= nalignment && oalignment <= 256) ) // little alignment storage wasted ?
				) ) {
				HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalignment ); // update alignment (could be the same)
				Heap::FreeHeader * freeHead;
				size_t bsize;
				headers( "aligned_resize", oaddr, header, freeHead, bsize, oalignment );
				size_t odsize = DataStorage( bsize, oaddr, header ); // data storage available in bucket

				if ( size <= odsize && odsize <= size * 2 ) { // allow 50% wasted data storage
					HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalignment ); // update alignment (could be the same)

					ClearZeroFillBit( header );			// turn off 0 fill
					#ifdef __DEBUG__
					heapManager->allocUnfreed += size - header->kind.real.size; // adjustment off the size difference
					#endif // __DEBUG__
					header->kind.real.size = size;		// reset allocation size
					#ifdef __STATISTICS__
					heapManager->stats.resize_calls += 1;
					#endif // __STATISTICS__
					return oaddr;
				} // if
			} // if
		} else if ( ! isFakeHeader						// old real header (aligned on libAlign) ?
					&& nalignment == __ALIGN__ ) {		// new alignment also on libAlign => no fake header needed
			return resize( oaddr, size );				// duplicate special case checks
		} // if

		// change size, DO NOT PRESERVE STICKY PROPERTIES.
		doFree( oaddr );								// free previous storage
		return memalignNoStats( nalignment, size STAT_ARG( HeapStatistics::RESIZE ) ); // create new aligned area
	} // aligned_resize


	void * aligned_resizearray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) {
		return aligned_resize( oaddr, nalignment, dimension * elemSize );
	} // aligned_resizearray


	void * aligned_realloc( void * oaddr, size_t nalignment, size_t size ) {
	  if ( UNLIKELY( oaddr == nullptr ) ) {				// => malloc( size )
			return memalignNoStats( nalignment, size STAT_ARG( HeapStatistics::REALLOC ) );
		} // if

	  if ( UNLIKELY( size == 0 ) ) {
			STAT_0_CNT( HeapStatistics::REALLOC );
			doFree( oaddr );
			return nullptr;
		} // if

		// Attempt to reuse existing alignment.
		Heap::Storage::Header * header = HeaderAddr( oaddr );
		bool isFakeHeader = AlignmentBit( header );		// old fake header ?
		size_t oalignment;

		if ( UNLIKELY( isFakeHeader ) ) {
			checkAlign( nalignment );					// check alignment
			oalignment = ClearAlignmentBit( header );	// old alignment
			if ( UNLIKELY( (uintptr_t)oaddr % nalignment == 0 // lucky match ?
				 && ( oalignment <= nalignment			// going down
					  || (oalignment >= nalignment && oalignment <= 256) ) // little alignment storage wasted ?
				) ) {
				HeaderAddr( oaddr )->kind.fake.alignment = MarkAlignmentBit( nalignment ); // update alignment (could be the same)
				return realloc( oaddr, size );			// duplicate special case checks
			} // if
		} else if ( ! isFakeHeader						// old real header (aligned on libAlign) ?
					&& nalignment == __ALIGN__ ) {		// new alignment also on libAlign => no fake header needed
			return realloc( oaddr, size );				// duplicate special case checks
		} // if

		Heap::FreeHeader * freeHead;
		size_t bsize;
		headers( "aligned_realloc", oaddr, header, freeHead, bsize, oalignment );

		// change size and copy old content to new storage

		size_t osize = header->kind.real.size;			// old allocation size
		bool ozfill = ZeroFillBit( header );			// old allocation zero filled

		void * naddr = memalignNoStats( nalignment, size STAT_ARG( HeapStatistics::REALLOC ) ); // create new aligned area

		header = HeaderAddr( naddr );					// new header
		size_t alignment;
		fakeHeader( header, alignment );				// could have a fake header

		memcpy( naddr, oaddr, Min( osize, size ) );		// copy bytes
		doFree( oaddr );								// free previous storage

		if ( UNLIKELY( ozfill ) ) {						// previous request zero fill ?
			MarkZeroFilledBit( header );				// mark new request as zero filled
			if ( size > osize ) {						// previous request larger ?
				memset( (char *)naddr + osize, '\0', size - osize ); // initialize added storage
			} // if
		} // if
		return naddr;
	} // aligned_realloc


	void * aligned_reallocarray( void * oaddr, size_t nalignment, size_t dimension, size_t elemSize ) {
		return aligned_realloc( oaddr, nalignment, dimension * elemSize );
	} // aligned_reallocarray


	// Same as malloc() except the memory address is a multiple of alignment, which must be a power of two. (obsolete)
	void * memalign( size_t alignment, size_t size ) {
		return memalignNoStats( alignment, size STAT_ARG( HeapStatistics::MEMALIGN ) );
	} // memalign


	// Same as aalloc() with memory alignment.
	void * amemalign( size_t alignment, size_t dimension, size_t elemSize ) {
		return memalignNoStats( alignment, dimension * elemSize STAT_ARG( HeapStatistics::AMEMALIGN ) );
	} // amemalign


	// Same as calloc() with memory alignment.
	void * cmemalign( size_t alignment, size_t dimension, size_t elemSize ) {
		size_t size = dimension * elemSize;
		char * addr = (char *)memalignNoStats( alignment, size STAT_ARG( HeapStatistics::CMEMALIGN ) );

	  if ( UNLIKELY( addr == NULL ) ) return NULL;		// stop further processing if NULL is returned

		Heap::Storage::Header * header = HeaderAddr( addr ); // optimization
		fakeHeader( header, alignment );				// must have a fake header

		#ifndef __DEBUG__
		// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero.
		if ( LIKELY( ! MmappedBit( header ) ) )
		#endif // __DEBUG__
			// <-------0000000000000000000000000000UUUUUUUUUUUUUUUUUUUUUUUUU> bsize (bucket size) U => undefined
			// `-header`-addr                      `-size
			memset( addr, '\0', size );					// set to zeros

		MarkZeroFilledBit( header );					// mark as zero fill
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
			if ( LIKELY( heapManager > (Heap *)1 ) ) heapManager->stats.free_null_0_calls += 1;
			else Fai( heapMaster.stats.free_null_0_calls, 1 );
			#endif // __STATISTICS__
			return;
		} // if

		doFree( addr );									// handles heapManager == nullptr
	} // free


	// Sets the amount (bytes) to extend the heap when there is insufficent free storage to service an allocation.
	__attribute__((weak)) size_t malloc_extend() { return __DEFAULT_HEAP_EXTEND__; }

	// Sets the crossover point between allocations occuring in the sbrk area or separately mmapped.
	__attribute__((weak)) size_t malloc_mmap_start() { return __DEFAULT_MMAP_START__; }

	// Amount subtracted to adjust for unfreed program storage (debug only).
	__attribute__((weak)) size_t malloc_unfreed() { return __DEFAULT_HEAP_UNFREED__; }


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


	bool malloc_remote( void * addr ) {
	  if ( UNLIKELY( addr == nullptr ) ) return false;	// null allocation is not zero fill
		Heap::Storage::Header * header = HeaderAddr( addr );
		if ( UNLIKELY( AlignmentBit( header ) ) ) {		// fake header ?
			header = RealHeader( header );				// backup from fake to real header
		} // if
		return heapManager == (ClearStickyBits( header->kind.real.home ))->homeManager;
	} // malloc_remote


	// Prints (on default standard error) statistics about memory allocated by malloc and related functions.
	void malloc_stats() {
		#ifdef __STATISTICS__
		HeapStatistics stats;
		HeapStatisticsCtor( stats );
		int unused __attribute__(( unused )) = printStats( collectStats( stats ) );	// file might be closed
		#else
		#define MALLOC_STATS_MSG "malloc_stats statistics disabled.\n"
		int unused __attribute__(( unused )) = write( STDERR_FILENO, MALLOC_STATS_MSG, sizeof( MALLOC_STATS_MSG ) - 1 /* size includes '\0' */ ); // file might be closed
		#endif // __STATISTICS__
	} // malloc_stats

	// Zero the heap master and all active thread heaps.
	void malloc_stats_clear() {
		#ifdef __STATISTICS__
		clearStats();
		#endif // __STATISTICS__
	} // malloc_stats_clear

	// Set file descriptor where malloc_stats malloc_info writes statistics.
	int malloc_stats_fd( int fd __attribute__(( unused )) ) {
		#ifdef __STATISTICS__
		int temp = heapMaster.stats_fd;
		heapMaster.stats_fd = fd;
		return temp;
		#else
		return -1;										// unsupported
		#endif // __STATISTICS__
	} // malloc_stats_fd

	void heap_stats() {
		#ifdef __STATISTICS__
		char title[32];
		snprintf( title, 32, " (%p)", heapManager );	// always puts a null terminator
		int unused __attribute__(( unused )) = printStats( heapManager->stats, title );	// file might be closed
		#else
		#define MALLOC_STATS_MSG "malloc_stats statistics disabled.\n"
		int unused __attribute__(( unused )) = write( STDERR_FILENO, MALLOC_STATS_MSG, sizeof( MALLOC_STATS_MSG ) - 1 /* size includes '\0' */ ); // file might be closed
		#endif // __STATISTICS__
	} // heap_stats


	// Prints an XML string that describes the current state of the memory-allocation implementation in the caller.
	// The string is printed on the file stream.  The exported string includes information about all arenas (see
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
			heapMaster.sbrkExtend = Ceiling( value, heapMaster.pageSize );
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
} // extern "C"


// zip -r llheap.zip heap/README.md heap/llheap.h heap/llheap.cc heap/Makefile heap/affinity.h heap/test.cc heap/ownership.cc

// g++-10 -Wall -Wextra -g -O3 -DNDEBUG -D__STATISTICS__ -DTLS llheap.cc -fPIC -shared -o llheap.so

// Local Variables: //
// tab-width: 4 //
// compile-command: "g++-11 -Wall -Wextra -g -O3 -DNDEBUG -D__STATISTICS__ llheap.cc -c" //
// End: //
