#include <stdlib.h>										// abort
#include <errno.h>										// errno
#include <pthread.h>
#include <assert.h>
#include <malloc.h>
#include <locale.h>										// setlocale
#include "affinity.h"

template<typename T> class BoundedBuffer {				// barging
	pthread_mutex_t mutex;
	pthread_cond_t prod, cons;
	const unsigned int size;
	unsigned int front = 0, back = 0, count = 0;
	T * buffer;
  public:
	BoundedBuffer( const unsigned int size = 10 ) : size( size ) {
		buffer = new T[size];
		pthread_mutex_init( &mutex, nullptr );
		pthread_cond_init( &prod, nullptr );
		pthread_cond_init( &cons, nullptr );
	} // BoundedBuffer::BoundedBuffer

	~BoundedBuffer() {
		assert( count == 0 );
		pthread_mutex_lock( &mutex );					// must be mutex
		delete [] buffer;
		pthread_cond_destroy( &cons );
		pthread_cond_destroy( &prod );
		pthread_mutex_destroy( &mutex );
	} // BoundedBuffer::~BoundedBuffer

	void insert( T elem ) {
		pthread_mutex_lock( &mutex );

		while ( count == size ) {						// buffer full ?
			pthread_cond_wait( &prod, &mutex );			// block until empty buffer slot
		} // while

		buffer[back] = elem;							// insert element into buffer
		count += 1;
		back = ( back + 1 ) % size;

		pthread_cond_signal( &cons );
		pthread_mutex_unlock( &mutex );
	} // BoundedBuffer::insert

	T remove() {
		pthread_mutex_lock( &mutex );

		while ( count == 0 ) {							// buffer empty ?
			pthread_cond_wait( &cons, &mutex );			// block until full buffer slot
		} // while

		T temp = buffer[front];							// remove element from buffer
		count -= 1;
		front = ( front + 1 ) % size;

		pthread_cond_signal( &prod );
		pthread_mutex_unlock( &mutex );
		return temp;
	} // BoundedBuffer::remove
};

enum { BufSize = 10'000, ElemSize = 64 };
BoundedBuffer<char *> buffer( BufSize );

enum { N = 5'000'000 };

void * Prod( void * ) {
	for ( unsigned int i = 0; i < N; i += 1 ) {
		char * data = (char *)malloc( ElemSize );
		data[0] = 'a'; data[ElemSize - 1] = 'b';
		buffer.insert( data );
	} // for
	return nullptr;
};

void * Cons( void * ) {
	for ( unsigned int i = 0; i < N; i += 1 ) {
		char * data = buffer.remove();
		assert( data[0] == 'a' && data[ElemSize - 1] == 'b' );
		free( data );
	} // for
	return nullptr;
};

extern "C" size_t malloc_unfreed() { return 4 * 304 /* pthreads */; }

int main() {
	pthread_t prod1, prod2, cons1, cons2;
	cpu_set_t mask;

	if ( pthread_create( &cons1, NULL, Cons, NULL ) < 0 ) abort();
	affinity( 0, mask );
	if ( pthread_setaffinity_np( cons1, sizeof(cpu_set_t), &mask ) ) abort();

	if ( pthread_create( &cons2, NULL, Cons, NULL ) < 0 ) abort();
	affinity( 1, mask );
	if ( pthread_setaffinity_np( cons2, sizeof(cpu_set_t), &mask ) ) abort();

	if ( pthread_create( &prod1, NULL, Prod, NULL ) < 0 ) abort();
	affinity( 2, mask );
	if ( pthread_setaffinity_np( prod1, sizeof(cpu_set_t), &mask ) ) abort();

	if ( pthread_create( &prod2, NULL, Prod, NULL ) < 0 ) abort();
	affinity( 3, mask );
	if ( pthread_setaffinity_np( prod2, sizeof(cpu_set_t), &mask ) ) abort();

	if ( pthread_join( prod2, NULL ) < 0 ) abort();
	if ( pthread_join( prod1, NULL ) < 0 ) abort();
	if ( pthread_join( cons1, NULL ) < 0 ) abort();
	if ( pthread_join( cons2, NULL ) < 0 ) abort();
	malloc_stats();
}

// repeat 3 \time -f "%Uu %Ss %Er %Mkb" a.out

// g++-10 -Wall -Wextra -g -O3 -D`hostname` ownership.cc libllheap.so -lpthread -Wl,-rpath=/u/pabuhr/heap -L/u/pabuhr/heap

// Local Variables: //
// compile-command: "g++-10 -Wall -Wextra -g -O3 ownership.cc libllheap-stats-debug.o -lpthread -D`hostname`" //
// End: //
