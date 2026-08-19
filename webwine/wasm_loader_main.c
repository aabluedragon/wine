/* M1 spike: static wasm Wine loader — call ntdll's __wine_main directly
 * instead of dlopen (emscripten FAKE_DYLIBS makes ntdll.so a static object). */
#include <stdio.h>
extern void __wine_main( int argc, char *argv[] );
int main( int argc, char *argv[] )
{
    fprintf( stderr, "wine-wasm: static loader entering __wine_main\n" );
    __wine_main( argc, argv );
    fprintf( stderr, "wine-wasm: __wine_main returned\n" );
    return 1;
}
