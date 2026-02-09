#include "/u/pabuhr/software/llheap/llheap.h"

int i = 3;
void fred() {
	if ( i > 0 ) { i -= 1; fred(); }
	long int * ip = 0;
	*ip = 0;
//	free( ip );
//	free( ip );

	ip[-2] = 0xfffffffff0;
	free( ip );
}
int main() {
	// int * ip = nullptr;
	// *ip = 3;
	// **** Error **** Null pointer (nullptr) dereference.
	
	// int * ip = (int *)0xfffee0;
	// *ip = 3;
	// **** Error **** Segment fault at memory location 0xfffee0.
	// Possible cause is reading outside the address space or writing to a protected area within the address space with an invalid pointer or subscript.
	
	// int ** ipa = (int **)malloc( 10 * sizeof( *ipa ) ); // array of 10 pointers
	// free( ipa ); // free array and srub array with 0xff
	// *ipa[3] += 1; // reference scrubbed storage forcing segment fault
	// **** Error **** Using a scrubbed pointer address 0xffffffffffffffff.
    // Possible cause is using uninitialized storage or using storage after it has been freed.

	// memalign( 27, sizeof( int ) );
	// **** Error **** alignment 27 for memory allocation is not a power of 2.

	// long int * ip = (long int *)malloc( 64 * 1024 * 1024 ); // mapped allocation
	// ip[-2] &= ~0x4l; // turn off mapped bit
	// free( ip );
	// **** Error **** attempt to free storage 0x7ffff33af010 outside the heap range 0x555555577000<->0x555555617000.
	// Possible cause is duplicate free on same block or overwriting of memory.

	long int * ip = (long int *)malloc( sizeof( &ip ) );
	free( ip );
	free( ip );

//	ip[-2] = 0xfffffffff0;
//	free( ip );
//	fred();
	// **** Error **** attempt to free storage 0x55555558b010 with corrupted header.
	// Possible cause is duplicate free on same allocation or overwriting of header information.

	// ip[-2] = 0xffffffffffffffff;
	// free( ip );
	// **** Error **** attempt to free storage 0x55555558b010 marked as mapped but inside the heap range 0x555555577000<->0x555555617000.
	// Possible cause is duplicate free on same block or overwriting of memory.

	// ip[-1] = 0xfffff;
	// free( ip );
	// **** Error **** attempt to free storage 0x55555558b010 with corrupted header.
	// Possible cause is duplicate free on same allocation or overwriting of header information.
	
	// long int * ip = (long int *)malloc( sizeof( &ip ) );
	// **** Warning **** (UNIX pid:2815283) : program terminating with 8(0x8) bytes of storage allocated but not freed.
	// Possible cause is unfreed storage allocated by the program or system/library routines called from the program.
}

// Local Variables: //
// compile-command: "g++-14 -Wall -Wextra -g -O3 check_debugging.cc libllheap-debug.o -lpthread -D`hostname`" //
// End: //
