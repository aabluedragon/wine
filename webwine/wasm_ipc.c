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


/* Magic transport-channel fd numbers.  These must sit ABOVE any real node fd:
 * the guest opens hundreds of files/dirs at once (asset scanning), so real fds
 * climb well past the old 0x300 base and would collide with a channel marker,
 * which is_magic() then misreads -> transport corruption ("Bad message").  A
 * billion is unreachable by real fds (node hits RLIMIT_NOFILE long before). */
#define MAGIC_BASE  0x40000000
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

/* REAL (node) fds passed from the in-process server to the client are the SAME
 * fd the server owns (identity transfer in one shared table).  We must NOT dup
 * them (node's dup gives an unreadable VFS-stream fd) and the CLIENT must never
 * close them — only the SERVER (which owns the underlying file object) may.
 * Mark a received fd "server-owned"; the client's close() of it is a no-op, and
 * the server's close() (recognised because it runs inside the cooperative drive,
 * g_wasm_in_server) actually closes it and clears the mark.  This closes each fd
 * exactly once, regardless of how many times it was fetched, so fds don't leak
 * (leaking climbs the fd number into the magic-channel range and corrupts the
 * transport). */
#define FD_OWN_MAX 65536
static unsigned char fd_srv_owned[FD_OWN_MAX];
int g_wasm_in_server;  /* set while executing the in-process wineserver */
void wasm_ipc_enter_server( void ) { g_wasm_in_server++; }
void wasm_ipc_leave_server( void ) { if (g_wasm_in_server) g_wasm_in_server--; }

extern void wineserver_inproc_drive(void) __attribute__((weak));
extern void wasm_vm_sync_shared(void);  /* refresh MAP_SHARED client mirrors */

/* Delegate non-magic fds to node's fs.  NODERAWFS gives open()ed files real
 * node fds, but a dup()'d fd — created both client-side and, more importantly,
 * server-side (dup_fd_object shares one fd across handles to the same inode) —
 * is an emscripten VFS *stream* whose number is not a node fd, so a direct
 * fs.readSync/writeSync on it returns EBADF (the mmap/pread path works because
 * it goes through emscripten's fd translation).  Try the raw fd first, then on
 * failure fall back to the stream's underlying node fd (FS.streams[fd].nfd).
 * With the fd-borrow refcount keeping fd numbers stable, this is deterministic. */
extern long __syscall_poll( long fds, long nfds, long timeout );
/* Safe single-fd poll: emscripten's __poll_js dereferences stream.stream_ops.poll
 * and throws on fds whose FS stream is missing or has no stream_ops (raw node
 * fds, dup'd stream fds).  Mirror its logic but guard every step; a real fd we
 * cannot classify is reported ready for the events it asked for. POLL* are the
 * Linux values emscripten uses: IN=1 OUT=4 ERR=8 HUP=16 NVAL=32. */
#ifdef WEBWINE_MEMFS
/* Browser build: files live in emscripten MEMFS (no NODERAWFS / node fds).
 * A non-magic fd is a MEMFS stream fd; do I/O through the FS API, which both
 * advances the stream position and works with the guest's lseek()s. */
EM_JS(int, host_poll_fd, (int fd, int events), {
  try { var s = FS.getStream(fd);
        if (s && s.stream_ops && s.stream_ops.poll) return s.stream_ops.poll(s,-1) & (events|8|16);
        return events & (1 | 4); }
  catch(e){ return events & (1 | 4); }
});
EM_JS(long, host_read, (int fd, void *buf, size_t n), {
  try { var s = FS.getStream(fd); if (!s) return -9;
        return FS.read(s, HEAPU8, buf, n); }   /* uses s.position, advances it */
  catch(e){ return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_write, (int fd, const void *buf, size_t n), {
  try { var s = FS.getStream(fd); if (!s) return -9;
        return FS.write(s, HEAPU8, buf, n); }
  catch(e){ return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_close, (int fd), {
  try { var s = FS.getStream(fd); if (!s) return -9; FS.close(s); return 0; }
  catch(e){ return -(e.errno ? Math.abs(e.errno) : 9); }
});
#else
EM_JS(int, host_poll_fd, (int fd, int events), {
  try {
    var stream = (typeof FS !== 'undefined' && FS.getStream) ? FS.getStream(fd) : null;
    if (stream && stream.stream_ops && stream.stream_ops.poll) {
      var m = stream.stream_ops.poll(stream, -1);
      return m & (events | 8 | 16);
    }
    if (stream) return events & (1 | 4);          /* stream w/o poll op: assume ready */
    try { require('fs').fstatSync(fd); return events & (1 | 4); } catch(e) { return 32; } /* POLLNVAL */
  } catch(e) { return events & (1 | 4); }
});
/* When readSync/writeSync on a stream fd fails, retry on the underlying node fd
 * (FS.streams[fd].nfd) AT THE STREAM'S OWN OFFSET — the guest seeks via lseek on
 * the stream fd, which advances the stream's .position, not the node fd's, so
 * reading the node fd at its own position would return data from the wrong
 * offset.  Read at s.position and advance it to mirror a sequential read. */
EM_JS(long, host_read, (int fd, void *buf, size_t n), {
  var fs = require('fs'); var b = Buffer.from(HEAPU8.buffer, buf, n);
  try { return fs.readSync(fd, b, 0, n, null); }
  catch(e) {
    try { var s = (typeof FS !== 'undefined' && FS.streams) ? FS.streams[fd] : null;
          if (s && s.nfd != null && s.nfd !== fd) {
            var pos = (s.position != null) ? s.position : null;
            var r = fs.readSync(s.nfd, b, 0, n, pos);
            if (s.position != null) s.position += r;
            return r; } } catch(e2) {}
    return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_write, (int fd, const void *buf, size_t n), {
  var fs = require('fs'); var b = Buffer.from(HEAPU8.buffer, buf, n);
  try { return fs.writeSync(fd, b, 0, n, null); }
  catch(e) {
    try { var s = (typeof FS !== 'undefined' && FS.streams) ? FS.streams[fd] : null;
          if (s && s.nfd != null && s.nfd !== fd) {
            var pos = (s.position != null) ? s.position : null;
            var r = fs.writeSync(s.nfd, b, 0, n, pos);
            if (s.position != null) s.position += r;
            return r; } } catch(e2) {}
    return -(e.errno ? Math.abs(e.errno) : 9); }
});
EM_JS(long, host_close, (int fd), {
  try { require('fs').closeSync(fd); return 0; }
  catch(e) { return -(e.errno ? Math.abs(e.errno) : 9); }
});
#endif  /* WEBWINE_MEMFS */

/* ---- browser display: hand a de-palettised RGB frame to the page ----------
 * Called by the interpreter's frame poller (wasm_x86.c).  In the browser the
 * module runs inside a Web Worker; post the frame to the main thread which
 * blits it to a <canvas>.  A no-op stub is provided for the node build so the
 * same wasm_x86.c present path links either way. */
#ifdef WEBWINE_MEMFS
EM_JS(void, webwine_present, (const void *rgba, int w, int h), {
  var n = w * h * 4;
  /* copy out of the wasm heap (postMessage transfer needs an owned buffer).
     The pixels are already RGBA, so the page can wrap this buffer in an
     ImageData directly - no per-pixel conversion on the main thread. */
  var out = new Uint8Array(n);
  out.set(HEAPU8.subarray(rgba, rgba + n));
  postMessage({ type: 'frame', w: w, h: h, rgba: out.buffer }, [out.buffer]);
});
#else
void webwine_present( const void *rgb, int w, int h ) { (void)rgb; (void)w; (void)h; }
#endif

/* ---- browser display: the OpenGL path -------------------------------------
 *
 * When the game runs its Polymost renderer there is no 8-bpp frameplace to
 * de-palettise: the picture lives in the WebGL default framebuffer of the
 * OffscreenCanvas the worker was given (see browser/bw-pre.js).  win32u's EGL
 * driver calls these at the drawable's swap and resize (weak symbols there, so
 * the node build links without them).
 *
 * transferToImageBitmap() hands the finished picture to the page as a GPU-side
 * ImageBitmap, so the frame never round-trips through the wasm heap the way
 * glReadPixels would.  It also does NOT need the worker to yield, which matters
 * because this thread is inside the interpreter for the whole session and never
 * returns to its event loop - a transferred (compositing) canvas would simply
 * never paint. */
/* Frames of grace left for the GL path.  Each GL present tops it up and each
 * page flip spends one, so the 8-bpp present stays out of the way while GL is
 * driving the screen and comes straight back if the game returns to the classic
 * renderer (the video menu can switch either way at any time). */
int webwine_gl_active;

#ifdef WEBWINE_MEMFS
EM_JS(void, webwine_gl_present_js, (void), {
  var c = Module['canvas'];
  if (!c || !c.transferToImageBitmap) return;
  /* Back-pressure.  The interpreter can finish frames several times faster than
     the page can draw them, and every one posted is an ImageBitmap holding a GPU
     surface; with nothing to stop it the main thread's queue grows until the
     bitmaps stop arriving intact and the canvas goes blank.  The page counts
     what it has drawn in the shared control block, so drop a frame rather than
     queue a third one - a frame the display will never show is pure waste. */
  var d = self.__wwCtl;
  if (d && Atomics.load(d, 3) - Atomics.load(d, 2) >= 2) return;
  /* Rate cap.  Back-pressure bounds how many frames are in flight but not how
     fast they are made, and every one is a fresh GPU surface: at several hundred
     a second the browser churns through them until the drawing buffer stops
     coming back with anything in it and the canvas goes blank.  No display shows
     more than its refresh rate anyway. */
  var now = performance.now();
  if (self.__wwLastPresent !== undefined && now - self.__wwLastPresent < 15) return;
  self.__wwLastPresent = now;
  /* WASM_GLPIX=1: sample the middle of the framebuffer before the transfer, so a
     blank picture can be blamed on the rendering or on the transfer. */
  if (Module.__glpix && (Module.__glpixN = (Module.__glpixN|0) + 1) % 300 === 1) {
    try {
      var gl = Module['ctx'], px = new Uint8Array(4), out = '', k;
      var pts = [[c.width>>1, c.height>>1], [c.width>>2, c.height>>1], [c.width>>1, c.height>>2], [4, 4]];
      for (k = 0; k < pts.length; k++) {
        gl.readPixels(pts[k][0], pts[k][1], 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        out += ' ' + pts[k][0] + ',' + pts[k][1] + '=' + px[0] + '/' + px[1] + '/' + px[2] + '/' + px[3];
      }
      var cm = gl.getParameter(gl.COLOR_WRITEMASK) || [], cc = gl.getParameter(gl.COLOR_CLEAR_VALUE) || [];
      var at = gl.getContextAttributes ? (gl.getContextAttributes() || {}) : {};
      postMessage({ type: 'log', line: 'wasm_x86: glpix' + out + ' lost=' + gl.isContextLost() +
                                       ' err=' + gl.getError() + ' mask=' + cm.join(',') +
                                       ' clear=' + cc.join(',') + ' pdb=' + at.preserveDrawingBuffer +
                                       ' blend=' + gl.isEnabled(gl.BLEND) +
                                       ' scis=' + gl.isEnabled(gl.SCISSOR_TEST) });
    } catch (e) { postMessage({ type: 'log', line: 'wasm_x86: glpix failed ' + e }); }
  }
  var bmp = c.transferToImageBitmap();
  if (d) Atomics.add(d, 3, 1);
  postMessage({ type: 'glframe', w: c.width, h: c.height, bmp: bmp }, [bmp]);
});
EM_JS(void, webwine_gl_resize, (int w, int h), {
  var c = Module['canvas'];
  if (c && (c.width !== w || c.height !== h)) { c.width = w; c.height = h; }
});
void webwine_gl_present( void )
{
    /* WASM_NO_GLPRESENT=1 keeps the 8-bpp frameplace present in charge even when
     * the game is drawing through GL.  The engine blits its classic 8-bit buffer
     * with a GL shader too, so both renderers normally share this path; this is
     * how you tell a picture the engine drew from one the GL present mangled. */
    static int off = -1;
    if (off == -1)
    {
        off = getenv( "WASM_NO_GLPRESENT" ) ? 1 : 0;
        if (getenv( "WASM_GLPIX" )) emscripten_run_script( "Module.__glpix = 1;" );
    }
    if (off) return;
    webwine_gl_active = 8;
    webwine_gl_present_js();
}
#else
void webwine_gl_present( void ) {}
void webwine_gl_resize( int w, int h ) { (void)w; (void)h; }
#endif

/* ---- browser input ring ----
 *
 * The worker thread is blocked inside the interpreter for the whole session and
 * can never service postMessage, so the page hands it a SharedArrayBuffer up
 * front (worker.js installs an onmessage handler BEFORE loading this module) and
 * writes input events into it as a lock-free ring.  We drain it from win32u's
 * PeekMessage path, which is exactly where SDL polls for messages.
 *
 * Layout (Int32Array): [0]=head (producer), [1]=tail (consumer), then N slots of
 * 4 ints: {type, a, b, c}.  Only the page writes head; only we write tail.
 */
EM_JS(int, webwine_poll_input, (int *ev), {
  var r = self.__wwInput;
  if (!r) return 0;
  var head = Atomics.load(r, 0), tail = Atomics.load(r, 1);
  if (head === tail) return 0;
  var slots = (r.length - 2) >> 2;
  var b = 2 + tail * 4;
  HEAP32[(ev >> 2) + 0] = r[b + 0];
  HEAP32[(ev >> 2) + 1] = r[b + 1];
  HEAP32[(ev >> 2) + 2] = r[b + 2];
  HEAP32[(ev >> 2) + 3] = r[b + 3];
  Atomics.store(r, 1, (tail + 1) % slots);
  return 1;
});

/* ---- native-call watchdog ----------------------------------------------
 * When the interpreter blocks inside a native call there is nothing left to
 * print with: the worker never returns to its event loop and the whole log
 * relay is on that thread.  Write the call it is in into a tiny
 * SharedArrayBuffer instead - the page can read that while the worker is stuck,
 * and it reports it in the ?beacon=1 line.  Gated on WASM_DIAG because it costs
 * a JS call per unix call. */
EM_JS(void, webwine_diag, (int a, int b), {
  var d = self.__wwCtl;
  if (d) { d[0] = a; d[1] = b; }
});

/* ---- audio bridge ----
 * The interpreter calls the game's SDL audio callback (there is no guest thread
 * to run it on) and pushes the PCM here.  It goes into a SharedArrayBuffer ring
 * that an AudioWorklet on the main thread drains, converted to interleaved
 * stereo float32 on the way; a ring rather than postMessage because the audio
 * thread must never wait on the main thread's message loop. */
EM_JS(void, webwine_audio_open, (int freq, int channels), {
  postMessage({ type: 'audio', freq: freq, channels: channels });
});

/* Frames the ring can still accept - the interpreter only runs the game's
 * callback when there is room, which is what paces the whole thing. */
EM_JS(int, webwine_audio_want, (void), {
  var a = self.__wwAudio;
  if (!a) return 0;
  var w = Atomics.load(a.idx, 0), r = Atomics.load(a.idx, 1);
  return a.cap - 1 - ((w - r + a.cap) % a.cap);
});

/* Frames already queued - the pump uses this to keep only a small cushion.
 * Rendering far ahead is wasted work: the mixer and the OPL3 synth are guest
 * code, so every queued frame costs interpreted instructions. */
EM_JS(int, webwine_audio_queued, (void), {
  var a = self.__wwAudio;
  if (!a) return 1 << 30;                 /* no ring: never ask for audio */
  var w = Atomics.load(a.idx, 0), r = Atomics.load(a.idx, 1);
  return (w - r + a.cap) % a.cap;
});

EM_JS(void, webwine_audio_push, (const void *buf, int frames, int channels, int fmt), {
  var a = self.__wwAudio;
  if (!a) return;
  var cap = a.cap, d = a.data;
  var w = Atomics.load(a.idx, 0), r = Atomics.load(a.idx, 1);
  var free = cap - 1 - ((w - r + cap) % cap);
  if (frames > free) frames = free;
  /* SDL_AudioFormat: bits 0-7 size, bit 8 float, bit 12 big-endian, bit 15 signed.
   * Float is bit 8 - NOT "signed and 32-bit", which would misread AUDIO_S32LSB
   * (0x8020) as float. */
  var bits = fmt & 0xff, isFloat = (fmt & 0x0100) !== 0, isSigned = (fmt & 0x8000) !== 0;
  for (var i = 0; i < frames; i++) {
    var l = 0, rr = 0;
    if (isFloat) {
      var o2 = (buf + i * channels * 4) >> 2;
      l = HEAPF32[o2]; rr = channels > 1 ? HEAPF32[o2 + 1] : l;
    } else if (bits === 16) {
      var o = (buf + i * channels * 2) >> 1;
      if (isSigned) { l = HEAP16[o] / 32768; rr = channels > 1 ? HEAP16[o + 1] / 32768 : l; }
      else          { l = (HEAPU16[o] - 32768) / 32768; rr = channels > 1 ? (HEAPU16[o + 1] - 32768) / 32768 : l; }
    } else if (bits === 32) {
      var o4 = (buf + i * channels * 4) >> 2;
      l = HEAP32[o4] / 2147483648; rr = channels > 1 ? HEAP32[o4 + 1] / 2147483648 : l;
    } else if (bits === 8) {
      var o3 = buf + i * channels;
      if (isSigned) { l = HEAP8[o3] / 128; rr = channels > 1 ? HEAP8[o3 + 1] / 128 : l; }
      else          { l = (HEAPU8[o3] - 128) / 128; rr = channels > 1 ? (HEAPU8[o3 + 1] - 128) / 128 : l; }
    }
    var p = ((w + i) % cap) * 2;
    d[p] = l; d[p + 1] = rr;
  }
  Atomics.store(a.idx, 0, (w + frames) % cap);
});

static int is_magic( int fd ) { return fd >= MAGIC_BASE && fd < MAGIC_BASE + MAGIC_COUNT && chans[fd - MAGIC_BASE].used; }
/* Exported so the client's fd-receive path (dlls/ntdll/unix/server.c) can tell a
 * real (dup-able, must-dup) fd from a magic transport channel passed by identity.
 * Real file fds can be numerically >= MAGIC_BASE, so an fd-value threshold is
 * wrong; only the channel table is authoritative. */
int wasm_ipc_is_magic( int fd ) { return is_magic( fd ); }
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
    wasm_vm_sync_shared();

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
            {
                int rfd = self->fdq[self->fdq_head++ % FDQ_SIZE];
                out[n++] = rfd;
                /* real fd: the server owns it; the client must not close it */
                if (!is_magic( rfd ) && rfd >= 0 && rfd < FD_OWN_MAX) fd_srv_owned[rfd] = 1;
            }
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
        wasm_vm_sync_shared();
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
        wasm_vm_sync_shared();
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
    /* A server-owned fd (received by the client from the server) may only be
     * closed by the SERVER — recognised because it runs inside the cooperative
     * drive (g_wasm_in_server).  The client's close of it is a no-op. */
    if (fd >= 0 && fd < FD_OWN_MAX && fd_srv_owned[fd])
    {
        if (!g_wasm_in_server) return 0;   /* client borrowing: keep it open */
        fd_srv_owned[fd] = 0;              /* server closing its own fd: fall through */
    }
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
        /* Poll each real fd through host_poll_fd (a crash-safe reimplementation
         * of emscripten's __poll_js — see above) rather than the raw syscall,
         * which throws on raw node fds / dup'd stream fds. */
        for (i = 0; i < nfds; i++)
        {
            if (fds[i].fd >= 0 && !is_magic( fds[i].fd ) && !fds[i].revents)
            {
                fds[i].revents = host_poll_fd( fds[i].fd, fds[i].events );
                if (fds[i].revents) ready++;
            }
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
