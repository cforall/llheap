// 
// Verify debug checks. Can only run one at a time for errors.
// 

#include "llheap.h"

int main() {
	// int * ip = nullptr;
	// *ip = 3;
	// **** Error **** Null pointer (nullptr) dereference.
	
	// int * ip = (int *)0xff'fee0;
	// *ip = 3;
	// **** Error **** Segment fault at memory location 0xfffee0.
	// Possible cause is reading outside the address space or writing to a protected area within the address space with an invalid pointer or subscript.

	// int ** ipa = (int **)malloc( 10 * sizeof( *ipa ) ); // array of 10 pointers
	// free( ipa ); // free array and srub array with 0xff
	// *ipa[3] += 1; // reference scrubbed storage forcing segment fault
	// **** Error **** Using a scrubbed pointer address 0xffffffffffffffff.
    // Possible cause is using uninitialized storage or using storage after it has been freed.

	long int * ip = (long int *)malloc( sizeof( &ip ) );
	free( ip );
	free( ip );

	// long int * ip = (long int *)malloc( 64 * 1024 * 1024 ); // mapped allocation
	// ip[-2] &= ~0x4l; // turn off mapped bit
	// free( ip );
	// **** Error **** attempt to free storage 0x7ffff33af010 outside the heap range 0x555555577000<->0x555555617000.
	// Possible cause is duplicate free on same block or overwriting of memory.

	// ip[-2] = 0xff'ffff'fff0;
	// free( ip );
	// **** Error **** attempt to free storage 0x55555558b010 with corrupted header.
	// Possible cause is duplicate free on same allocation or overwriting of header information.

	// ip[-2] = 0xffff'ffff'ffff'fff;
	// free( ip );
	// **** Error **** attempt to free storage 0x55555558b010 marked as mapped but inside the heap range 0x555555577000<->0x555555617000.
	// Possible cause is duplicate free on same block or overwriting of memory.

	// ip[-1] = 0xf'ffff;
	// free( ip );
	// **** Error **** attempt to free storage 0x55555558b010 with corrupted header.
	// Possible cause is duplicate free on same allocation or overwriting of header information.

	// memalign( 27, sizeof( int ) );
	// **** Error **** alignment 27 for memory allocation is not a power of 2.
	
	// long int * ip = (long int *)malloc( sizeof( &ip ) );
	// **** Warning **** (UNIX pid:2815283) : program terminating with 8(0x8) bytes of storage allocated but not freed.
	// Possible cause is unfreed storage allocated by the program or system/library routines called from the program.
}

// Local Variables: //
// compile-command: "g++-14 -Wall -g -D`hostname` check_debugging.cc libllheap-debug.o -lpthread -rdynamic" //
// End: //
