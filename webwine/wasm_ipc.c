/*
 * In-process AF_UNIX transport + terminal ioctl for the single-module wasm Wine.
 *
 * The wine client and wineserver live in ONE wasm module (cooperative,
 * single-threaded; the client drives the server via wineserver_inproc_drive()).
 * The only Unix plumbing emscripten lacks for that arrangement is the AF_UNIX
 * socket between them (byte stream + SCM_RIGHTS fd passing) and working
 * reply/request pipes. Both ends share one process and one fd table, so fd
 * passing is the identity function (refcounted: passing a fd co-owns its
 * channel, close frees at zero) and the transport is a pair of ring buffers +
 * an fd queue behind "magic" fd numbers.
 *
 * These are STRONG symbol overrides (not -Wl,--wrap, which does not intercept
 * Wine's calls into emscripten's precompiled libc). read/write/readv/writev/
 * recvmsg/close/poll/fcntl route magic fds through the rings and delegate real
 * fds to node fs via EM_JS; ioctl handles the terminal queries emscripten's
 * node tty stub crashes on.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <emscripten.h>


#define MAGIC_BASE  0x300
#define MAGIC_COUNT 64
#define RING_SIZE   0x20000
#define FDQ_SIZE    64

struct chan
{
    int   used;
    int   refs;
    int   listener;         /* master-socket dummy: never readable */
    int   peer;             /* index of peer chan */
    char  ring[RING_SIZE];  /* data written BY the peer, read by us */
    unsigned int head, tail;
    int   fdq[FDQ_SIZE];    /* fds passed BY the peer */
    unsigned int fdq_head, fdq_tail;
};

static struct chan chans[MAGIC_COUNT];

extern void wineserver_inproc_drive(void) __attribute__((weak));
extern void wasm_vm_sync_shared(void);  /* refresh MAP_SHARED client mirrors */

/* real libc entry points, reached for non-magic fds (linker --wrap) */
/* Delegate non-magic fds to node's fs (NODERAWFS uses real OS fds). */
extern long __syscall_poll( long fds, long nfds, long timeout );
EM_JS(long, host_read, (int fd, void *buf, size_t n), {
  try { var b = Buffer.from(HEAPU8.buffer, buf, n);
        return require('fs').readSync(fd, b, 0, n, null); }
  catch(e) { return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_write, (int fd, const void *buf, size_t n), {
  try { var b = Buffer.from(HEAPU8.buffer, buf, n);
        return require('fs').writeSync(fd, b, 0, n, null); }
  catch(e) { return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_close, (int fd), {
  try { require('fs').closeSync(fd); return 0; }
  catch(e) { return -(e.errno ? Math.abs(e.errno) : 9); }
});

static int is_magic( int fd ) { return fd >= MAGIC_BASE && fd < MAGIC_BASE + MAGIC_COUNT && chans[fd - MAGIC_BASE].used; }
static struct chan *chan_of( int fd ) { return &chans[fd - MAGIC_BASE]; }

static int ipc_trace = -1;
static int trace(void)
{
    if (ipc_trace < 0)
    {
        const char *e = getenv( "WINEWASMIPCTRACE" );
        ipc_trace = e && *e && *e != '0';
    }
    return ipc_trace;
}

static int alloc_chan(void)
{
    int i;
    for (i = 0; i < MAGIC_COUNT; i++) if (!chans[i].used) { memset( &chans[i], 0, sizeof(chans[i]) ); chans[i].used = 1; return i; }
    errno = ENFILE;
    return -1;
}

static unsigned int ring_avail( struct chan *c ) { return c->tail - c->head; }
static unsigned int ring_space( struct chan *c ) { return RING_SIZE - ring_avail( c ); }

static void ring_write( struct chan *c, const void *buf, unsigned int len )
{
    unsigned int i;
    for (i = 0; i < len; i++) c->ring[(c->tail + i) % RING_SIZE] = ((const char *)buf)[i];
    c->tail += len;
}

static unsigned int ring_read( struct chan *c, void *buf, unsigned int len )
{
    unsigned int n = ring_avail( c );
    unsigned int i;
    if (n > len) n = len;
    for (i = 0; i < n; i++) ((char *)buf)[i] = c->ring[(c->head + i) % RING_SIZE];
    c->head += n;
    return n;
}

/* ---- explicit channel factory (only the client<->server pair) ---- */

int webwine_make_channel( int sv[2] )
{
    int a = alloc_chan();
    int b = alloc_chan();
    if (a < 0 || b < 0) return -1;
    chans[a].peer = b;
    chans[b].peer = a;
    chans[a].refs = 1;
    chans[b].refs = 1;
    sv[0] = MAGIC_BASE + a;
    sv[1] = MAGIC_BASE + b;
    if (trace()) fprintf( stderr, "wasm_ipc: channel -> %d,%d\n", sv[0], sv[1] );
    return 0;
}

/* ---- data-plane wrappers (only touch magic fds) ---- */

ssize_t sendmsg( int fd, const struct msghdr *msg, int flags )
{
    struct chan *self, *peer;
    struct cmsghdr *cmsg;
    size_t total = 0, i;

    if (!is_magic( fd )) { errno = EBADF; return -1; }
    self = chan_of( fd );
    if (self->peer < 0) { errno = ENOTCONN; return -1; }
    peer = &chans[self->peer];

    for (i = 0; i < (size_t)msg->msg_iovlen; i++) total += msg->msg_iov[i].iov_len;
    if (ring_space( peer ) < total ) { errno = EAGAIN; return -1; }

    for (cmsg = CMSG_FIRSTHDR( msg ); cmsg; cmsg = CMSG_NXTHDR( (struct msghdr *)msg, cmsg ))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        {
            int *fds = (int *)CMSG_DATA( cmsg );
            size_t nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (i = 0; i < nfds; i++)
            {
                if (peer->fdq_tail - peer->fdq_head >= FDQ_SIZE) { errno = ENOBUFS; return -1; }
                peer->fdq[peer->fdq_tail++ % FDQ_SIZE] = fds[i];
                if (is_magic( fds[i] )) chan_of( fds[i] )->refs++;  /* receiver co-owns */
                if (trace()) fprintf( stderr, "wasm_ipc: fd %d passed over %d (refs=%d)\n", fds[i], fd, is_magic(fds[i])?chan_of(fds[i])->refs:-1 );
            }
        }
    }
    for (i = 0; i < (size_t)msg->msg_iovlen; i++)
        ring_write( peer, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len );
    return total;
}

ssize_t recvmsg( int fd, struct msghdr *msg, int flags )
{
    struct chan *self;
    size_t got = 0, i;
    int spins = 0;

    if (!is_magic( fd )) { errno = EBADF; return -1; }
    self = chan_of( fd );

    while (!ring_avail( self ) && !(self->fdq_tail - self->fdq_head))
    {
        /* cooperative single thread: let the server produce the data */
        if (&wineserver_inproc_drive && wineserver_inproc_drive) wineserver_inproc_drive();
        if (++spins > 2000000) { errno = EAGAIN; return -1; }
    }
    if (spins) wasm_vm_sync_shared();

    for (i = 0; i < (size_t)msg->msg_iovlen && ring_avail( self ); i++)
        got += ring_read( self, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len );

    if (msg->msg_control && (self->fdq_tail - self->fdq_head))
    {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR( msg );
        size_t maxfds = (msg->msg_controllen - CMSG_LEN(0)) / sizeof(int);
        size_t n = 0;
        int *out;
        if (cmsg && maxfds)
        {
            out = (int *)CMSG_DATA( cmsg );
            /* Wine's protocol pairs exactly ONE fd with one send_fd payload per
             * recvmsg; delivering more here would starve later receive_fd calls
             * of their fd (they would read the data but get no descriptor). */
            if (n < maxfds && (self->fdq_tail - self->fdq_head))
                out[n++] = self->fdq[self->fdq_head++ % FDQ_SIZE];
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;
            cmsg->cmsg_len   = CMSG_LEN( n * sizeof(int) );
            msg->msg_controllen = CMSG_SPACE( n * sizeof(int) );
        }
    }
    else msg->msg_controllen = 0;
    return got;
}

/* writev/readv: Wine's send_request/send_reply/read_request use these on the
 * socket + reply/request pipes, so they must honor magic fds too. */
extern ssize_t write( int fd, const void *buf, size_t count );
extern ssize_t read( int fd, void *buf, size_t count );

ssize_t writev( int fd, const struct iovec *iov, int iovcnt )
{
    if (is_magic( fd ))
    {
        struct chan *self = chan_of( fd ), *peer;
        size_t total = 0; int i;
        if (self->peer < 0) { errno = ENOTCONN; return -1; }
        peer = &chans[self->peer];
        for (i = 0; i < iovcnt; i++) total += iov[i].iov_len;
        if (ring_space( peer ) < total) { errno = EAGAIN; return -1; }
        for (i = 0; i < iovcnt; i++) ring_write( peer, iov[i].iov_base, iov[i].iov_len );
        return total;
    }
    {
        ssize_t total = 0; int i;
        for (i = 0; i < iovcnt; i++)
        {
            ssize_t r = write( fd, iov[i].iov_base, iov[i].iov_len );
            if (r < 0) return total ? total : -1;
            total += r;
            if ((size_t)r < iov[i].iov_len) break;
        }
        return total;
    }
}

ssize_t readv( int fd, const struct iovec *iov, int iovcnt )
{
    if (is_magic( fd ))
    {
        struct chan *self = chan_of( fd );
        size_t got = 0; int i, spins = 0;
        while (!ring_avail( self ))
        {
            if (&wineserver_inproc_drive && wineserver_inproc_drive) wineserver_inproc_drive();
            if (++spins > 2000000) { errno = EAGAIN; return -1; }
        }
        if (spins) wasm_vm_sync_shared();
        for (i = 0; i < iovcnt && ring_avail( self ); i++)
            got += ring_read( self, iov[i].iov_base, iov[i].iov_len );
        return got;
    }
    {
        ssize_t total = 0; int i;
        for (i = 0; i < iovcnt; i++)
        {
            ssize_t r = read( fd, iov[i].iov_base, iov[i].iov_len );
            if (r < 0) return total ? total : -1;
            total += r;
            if ((size_t)r < iov[i].iov_len) break;
        }
        return total;
    }
}

/* byte-stream read/write on magic fds; delegate real fds to WASI */

ssize_t read( int fd, void *buf, size_t count )
{
    if (is_magic( fd ))
    {
        struct chan *self = chan_of( fd );
        int spins = 0;
        while (!ring_avail( self ))
        {
            if (&wineserver_inproc_drive && wineserver_inproc_drive) wineserver_inproc_drive();
            if (++spins > 2000000) { errno = EAGAIN; return -1; }
        }
        if (spins) wasm_vm_sync_shared();
        return ring_read( self, buf, count );
    }
    {
        long r = host_read( fd, buf, count );
        if (r < 0) { errno = -r; return -1; }
        return r;
    }
}

ssize_t write( int fd, const void *buf, size_t count )
{
    if (is_magic( fd ))
    {
        struct chan *self = chan_of( fd );
        struct chan *peer;
        if (self->peer < 0) { errno = ENOTCONN; return -1; }
        peer = &chans[self->peer];
        if (ring_space( peer ) < count) { errno = EAGAIN; return -1; }
        ring_write( peer, buf, count );
        return count;
    }
    {
        long r = host_write( fd, buf, count );
        if (r < 0) { errno = -r; return -1; }
        return r;
    }
}

int close( int fd )
{
    if (is_magic( fd )) { struct chan *c = chan_of( fd ); if (--c->refs <= 0) c->used = 0; return 0; }
    /* In-process (single process) mode: the wineserver shares node's fd table,
     * so closing the std streams (0/1/2) would close node's real stdio and kill
     * the process. The guest's std handles alias these fds; keep them open. */
    if (fd >= 0 && fd <= 2) return 0;
    {
        if (getenv("WINEWASMLOADTRACE")) { struct stat st; int rr=fstat(fd,&st);
            fprintf(stderr,"wasm_ipc: close real fd=%d ino=%llu\n", fd, rr==0?(unsigned long long)st.st_ino:0); }
        long r = host_close( fd );
        if (r < 0) { errno = -r; return -1; }
        return 0;
    }
}

/* poll: answer for magic fds ourselves, delegate the rest */
int poll( struct pollfd *fds, nfds_t nfds, int timeout )
{
    int ready = 0;
    nfds_t i;
    int have_real = 0;

    for (i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd >= 0 && is_magic( fds[i].fd ))
        {
            struct chan *c = chan_of( fds[i].fd );
            if (!c->listener)
            {
                if ((fds[i].events & POLLIN) && (ring_avail( c ) || (c->fdq_tail - c->fdq_head)))
                    fds[i].revents |= POLLIN;
                if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
            }
            if (fds[i].revents) ready++;
        }
        else if (fds[i].fd >= 0) have_real = 1;
    }
    if (have_real)
    {
        /* delegate the real fds with timeout 0 (never block while holding
         * magic-fd state; the cooperative loop re-polls anyway) */
        struct pollfd tmp[64];
        int map[64], n = 0, r;
        for (i = 0; i < nfds && n < 64; i++)
        {
            if (fds[i].fd >= 0 && !is_magic( fds[i].fd ))
            {
                tmp[n] = fds[i];
                map[n++] = i;
            }
        }
        r = __syscall_poll( (long)tmp, n, 0 );
        if (r > 0)
        {
            int j;
            for (j = 0; j < n; j++)
                if (tmp[j].revents) { fds[map[j]].revents = tmp[j].revents; ready++; }
        }
    }
    return ready;
}

/* fcntl: accept anything on magic fds, delegate the rest */
int fcntl( int fd, int cmd, ... )
{
    va_list args;
    intptr_t arg;
    va_start( args, cmd );
    arg = va_arg( args, intptr_t );
    va_end( args );
    if (trace()) fprintf( stderr, "wasm_ipc: fcntl fd=%d cmd=%d magic=%d\n", fd, cmd, is_magic(fd) );
    if (is_magic( fd ))
    {
        switch (cmd)
        {
        case F_GETFL: return O_RDWR;
        case F_SETFL: return 0;
        case F_GETFD: return 0;
        case F_SETFD: return 0;
        default: return 0;
        }
    }
    switch (cmd)
    {
    case F_GETFL: return O_RDWR;
    case F_GETFD: return 0;
    case F_SETFL: case F_SETFD: return 0;
    default: return 0;
    }
}


/* emscripten's ___syscall_ioctl crashes on TIOCGWINSZ for non-tty node fds
 * (stream.tty is undefined). Provide sane terminal ioctls ourselves. */
int ioctl( int fd, int request, ... )
{
    va_list ap;
    void *arg;
    va_start( ap, request );
    arg = va_arg( ap, void * );
    va_end( ap );

    switch (request)
    {
    case TIOCGWINSZ:
    {
        struct winsize *ws = arg;
        if (ws) { ws->ws_row = 25; ws->ws_col = 80; ws->ws_xpixel = 0; ws->ws_ypixel = 0; }
        return 0;
    }
    case TIOCGPGRP: if (arg) *(int *)arg = getpid(); return 0;
    case TIOCSPGRP: return 0;
    case FIONREAD:
        if (is_magic( fd )) { struct chan *c = chan_of( fd ); if (arg) *(int *)arg = (int)ring_avail( c ); return 0; }
        if (arg) *(int *)arg = 0; return 0;
    case FIONBIO: return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}
