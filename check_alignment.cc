#include <iostream>
#include <cstdint>										// intptr_t
using namespace std;
#include <malloc.h>

template< typename T > void test( const char * tname, T ) {
	// Test multiple allocations in case boundary happens to be alignof(max_align_t).
	for ( size_t i = 0; i < 100; i += 1 ) {
		T * tp = (T * )malloc( sizeof( T ) );
		size_t align = (intptr_t)(tp) % alignof(max_align_t);
		if ( align != 0 ) {
			cout << tname << ' ' << align << ", ";
			break;
		}
	}
}	

int main() {
	test( "char", (char)0 );
	test( "short int", (short int)0 );
	test( "int", (int)0 );
	test( "long int", (long int)0 );
	test( "long long int", (long long int)0 );
	test( "float", (float)0 );
	test( "double", (double)0 );
	test( "long double", (long double)0 );
	test( "_Complex", (_Complex)0 );
	test( "double _Complex", (double _Complex)0 );
	test( "long double _Complex", (long double _Complex)0 );
	cout << endl;
}
