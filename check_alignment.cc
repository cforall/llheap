#include <iostream>
#include <cstdint>
using namespace std;
#include <malloc.h>

template< typename T > void test( const char * tname, T ) {
	T * tp = (T * )malloc( sizeof( T ) );
	cout << tname << " size " << sizeof( T ) << ' ' << (intptr_t)(tp) % alignof(max_align_t) << "(" << malloc_usable_size( tp ) << "), ";
}	

int main() {
	cout << "max_align_t " << alignof(max_align_t) << ", ";
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
	test( "long double _Complex", (double _Complex)0 );
	cout << endl;
}
