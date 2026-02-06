// 
// Copyright (C) Peter A. Buhr and Dave Dice 2026
// 
// checkheader.cc -- 
// 
// Author           : Peter A. Buhr
// Created On       : Thu Feb  5 20:50:12 2026
// Last Modified By : Peter A. Buhr
// Last Modified On : Thu Feb  5 20:51:29 2026
// Update Count     : 2
// 

// Test if a memory allocator is using a header for each allocation.

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <malloc.h>
#include <limits.h>

#define iabs( v ) ((size_t)(v < 0 ? -v : v) )

int main() {
    size_t mingap = UINT_MAX;
	// Objects can be allocated in different containers/pages and still have headers, so search for minimum gap across
	// multiple, sized allocations.
    for ( size_t sz = 16 ; sz <= 1024 ; sz += 1 ) { 
		char * ptr, * prev;
		prev = (char *)malloc( sz );
		for ( size_t i = 0; i < 100; i += 1 ) {
			ptr = (char *)malloc( sz );
			// Remove bucketing effect.
			size_t gap = iabs( (ptr - prev) ) - malloc_usable_size( ptr );
			prev = ptr;
			if ( gap < mingap ) mingap = gap;
		}
	}
	if ( mingap > 0 ) printf( "header size: %ld\n ", mingap );
}
