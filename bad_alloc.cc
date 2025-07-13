#include <new>
#include <iostream>
using namespace std;

void handler() {
    cout << "Memory allocation failed, terminating\n";
    set_new_handler( nullptr );
}
 
int main() {
    set_new_handler( handler );
    try {
        for ( ;; ) new int[10'000ul]{};		// initialization prevents -O3 elision to prevent spinning
    } catch( const bad_alloc & e ) {
        cout << e.what() << endl;
    }
}
