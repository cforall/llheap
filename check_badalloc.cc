// 
// Check if allocator correctly handles running out of memory. Set shell data area to before running:
//
//   ulimit -d 1000000
//
// Should print:
//
//   Memory allocation failed
//   std::bad_alloc 1
// 

#include <iostream>
using namespace std;

static inline void * pass( void * v ) {					// prevent eliding, cheaper than volatile
	__asm__ __volatile__ ( "" : "+r"(v) );
	return v ;
} // pass

static void handler() {
	cout << "Memory allocation failed\n";
	set_new_handler( nullptr );
}
 
int main() {
	set_new_handler( handler );
	try {
		for ( ;; ) pass( new char[50] );				// unbounded allocation
	} catch( const bad_alloc & e ) {
		cout << e.what() << ' ' << (errno == ENOMEM) << endl;
	}
}

