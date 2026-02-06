#include <new>
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

