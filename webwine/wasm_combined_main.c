/* Combined harness: run the in-process wineserver + the wine client in ONE
 * wasm module, to exercise the ring-buffer transport and the server handshake. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int wineserver_main( int argc, char *argv[] );   /* renamed server main() */
extern void wineserver_inproc_drive( void );
extern void __wine_main( int argc, char *argv[] );
extern int webwine_make_channel( int sv[2] );

int main( int argc, char *argv[] )
{
    int sv[2];
    char buf[32];

    fprintf( stderr, "webwine: creating in-process client/server socketpair\n" );
    if (webwine_make_channel( sv ))
    {
        perror( "socketpair" );
        return 1;
    }

    /* server takes sv[0]; client uses sv[1] as WINESERVERSOCKET */
    snprintf( buf, sizeof(buf), "%d", sv[0] );
    setenv( "WINE_INPROC_CLIENT_FD", buf, 1 );
    setenv( "WINE_INPROC_COOP", "1", 1 );

    snprintf( buf, sizeof(buf), "%d", sv[1] );
    setenv( "WINESERVERSOCKET", buf, 1 );

    snprintf( buf, sizeof(buf), "%llx", (unsigned long long)(uintptr_t)wineserver_inproc_drive );
    setenv( "WINE_INPROC_DRIVE_PTR", buf, 1 );
    setenv( "WINE_NO_SERVER_SPAWN", "1", 1 );

    fprintf( stderr, "webwine: booting in-process wineserver (fd %d)\n", sv[0] );
    {
        char *sargv[2] = { (char *)"wineserver", NULL };
        wineserver_main( 1, sargv );   /* returns immediately in COOP mode */
    }

    fprintf( stderr, "webwine: entering wine client __wine_main argc=%d argv0=%s argv1=%s\n",
             argc, argc>0?argv[0]:"(null)", argc>1?argv[1]:"(none)" );
    __wine_main( argc, argv );

    fprintf( stderr, "webwine: __wine_main returned\n" );
    return 1;
}
