# Native-WASM Wine (Option B)

Goal: run **unmodified Windows executables** in the browser by compiling **Wine
itself to native WebAssembly** and emulating **only the application's x86 code**
— instead of emulating Wine *and* the app under a full x86+Linux emulator
(BoxedWine, the current web-showcase). Wine's file I/O, scheduling, registry,
loader and syscall handling then run at native wasm speed; the emulation tax is
paid only by the game's own instructions.

This is the ARM-Mac Wine model (native Wine + Rosetta for app code), ported to
the browser (native-wasm Wine + an x86→wasm emulator for app code).

## Status: the unix side boots as native WebAssembly ✅

`build-wasm4/` is a cross-build of Wine's normal Unix layout for
`wasm32-unknown-emscripten` (fork commit `3f3fa09d` added the host-arch
scaffolding: `configure.ac` recognises wasm32, `tools.h` aliases it to the i386
guest ABI, `winnt.h` gives an i386-shaped CONTEXT, `system.c` hard-codes a
"WebAssembly x86" cpu model). What was MISSING and is provided here as host
shims lets ntdll's unix half link and boot:

- **`wasm_cpu_bridge.c`** — the per-architecture seam normally in
  `signal_i386.c`/`signal_x86_64.c`. Init paths (`signal_init_process`,
  `signal_alloc_thread`, context getters) succeed; the *execution* entry points
  (`__wine_syscall_dispatcher`, `__wine_unix_call_dispatcher`,
  `signal_start_thread`, apc/exception dispatchers) `abort()` loudly — **these
  are exactly where the x86 emulator plugs in.** Also: xattr stubs (no xattr in
  emscripten libc) and a uid/gid bridge to node's real uid (NODERAWFS).
- **`wasm_vm.c`** — mmap/munmap/mprotect for wasm. The wasm linear memory *is*
  the guest i386 address space (identity map): native Wine lives at
  `GLOBAL_BASE=0x70000000`, the guest region is `[0, 0x70000000)`, KUSER_SHARED
  at `0x7ffe0000`. "Mapping" is bookkeeping into pre-grown 2GB memory; PROT_* is
  ignored (there is no wasm page protection — SMC/W^X is the emulator's job).
  Address-space *probes* (PROT_NONE + munmap) reclaim LIFO; `msync`/`mincore`
  report ENOMEM ("free") so Wine's free-range checks pass; address *hints* from
  Wine's view manager (the sole owner of the guest region) are honored exactly.
- **`wasm_loader_main.c`** — static entry that calls ntdll's `__wine_main`
  directly (emscripten `FAKE_DYLIBS` makes `ntdll.so` a static object, so the
  normal dlopen loader in `tools/wine/wine.c` can't be used yet).

With those, `webwine/build.sh` links a `wine.js` that, run under node with a
synthetic install tree, executes ntdll's loader → virtual-memory init →
registry → NLS → process init entirely as **native wasm**, reaching
`server_connect` (i.e. "could not exec wineserver"). Every earlier boot wall
(dynamic-linking, view-block alloc, shared-user-data map, uid check) is cleared.

## Update: in-process wineserver runs in the SAME module ✅

`webwine/build-combined.sh` links the wine client (ntdll unix side) **and** the
whole wineserver **and** the ring-buffer transport into ONE wasm module and boots
both. Results:

- The server's symbol overlap with ntdll in one address space is tiny: **2
  functions** (`get_thread_context`/`set_thread_context` in `server/thread.c`,
  ntdll wins) and **5 data globals** (`user_shared_data`, `server_start_time`,
  `native_machine`, `supported_machines`, `supported_machines_count`), all
  resolved by `-D` renames at compile. `server main()` -> `wineserver_main()`.
- The only missing OS primitive is the client<->server AF_UNIX socket:
  `wasm_ipc.c` provides it as in-memory ring buffers + an fd queue behind "magic"
  fd numbers, linked with `-Wl,--wrap=` on the **data plane only**
  (read/write/close/poll/fcntl/sendmsg/recvmsg); the server's own real socket
  calls (`sock_check_pollhup`, master socket) are left to emscripten. The
  client/server channel is created explicitly (`webwine_make_channel`).
- Tree changes that make the server build+run for wasm (committed): `server/fd.c`
  epoll guard (`WINE_INPROC_POLL`), `server/registry.c` wasm32 machine case,
  `server/sock.c` BPF guard, `server/main.c` `wineserver_main`/foreground,
  `server/request.c` master-socket skip on `__wasm32__`.

**Boot reaches the client<->server request/reply exchange.** The server initializes fully
(sock_init, registry, client injection), returns from `wineserver_main` in COOP
mode, and the client enters `__wine_main` and connects to fd 769. Transport
proven: data + SCM_RIGHTS fd passing both cross the ring buffer (earlier run
logged `fd 21/23 passed over 769`).

### Current frontier (the resumption point)

Data now flows **both directions** over the transport, plus SCM_RIGHTS fd
passing and real-file reads (NLS/registry via node `fs`). The boot advanced
through: server init -> client inject -> `__wine_main` -> config dir -> NLS +
registry -> reply/wait pipes -> **the first server request writes, and the
client blocks reading the reply**. Fixes that got here, all channel-backed via
`webwine_make_channel` (ring buffers behind magic fds): `server_pipe()` in
`dlls/ntdll/unix/server.c` and `pipe(request_pipe)` in `server/thread.c`; the
transport uses strong symbol overrides (not `-Wl,--wrap`, which does not
intercept Wine's calls into precompiled libc) and delegates non-magic fds to
node `fs` via `EM_JS`.

**Solved since:** identity-fd channels needed **refcounting** — passing a pipe
end via SCM_RIGHTS then closing the local copy (normal pipe semantics) was
destroying the channel the peer still used; `wasm_ipc.c` now refs++ on pass,
refs-- on close, free at zero. The request/reply channels are created correctly
(no fd reuse), **the request is written by the client, delivered through the
ring, and read by the server** (drive poll returns ready, read_request runs).

**The one remaining piece:** `wineserver_inproc_drive()` must process the
pending request. The client's `wait_reply` read spins the drive but the reply
is not produced within the spin budget (`read: Resource temporarily
unavailable`). The drive does non-blocking `poll()` sweeps over the server fd
set + `fd_poll_event`; the request arrives on the server's request_fd (a magic
channel) — verify `poll()` reports POLLIN for it (the override does) and that
`fd_poll_event`/`read_request` runs and writes the reply to the client's
reply channel. Likely a small wiring/ordering fix (register the request_fd's
events, or raise the read-side spin/drive budget). Once the version handshake
completes, the client loads `ntdll.dll` (PE) and hits the CPU-bridge seam.

## What's left (the two hard subsystems)

1. **In-process wineserver.** Committed already (`server/main.c`,
   `server/fd.c` `wineserver_inproc_drive`, `dlls/ntdll/unix/server.c` — the iOS
   cooperative single-thread server, env-gated by `WINE_INPROC_COOP` /
   `WINE_INPROC_CLIENT_FD` / `WINE_INPROC_DRIVE_PTR` / `WINE_NO_SERVER_SPAWN`).
   The one missing OS primitive is the AF_UNIX socket between client and server;
   `wasm_ipc.c` implements it as in-memory ring buffers + an fd queue behind
   "magic" fd numbers (fd passing is identity — one process, one fd table), and
   delegates real fds to WASI. **Next step:** link all `server/*.o` + `wasm_ipc.o`
   into the same module, run `server main()` once (renamed) to init + inject the
   client fd, export `wineserver_inproc_drive` to `WINE_INPROC_DRIVE_PTR`, then
   let `server_init_process` do its version handshake over the ring buffer.
   Server objects already compile for wasm (tree fixes: `server/registry.c`
   wasm32 machine case, `server/fd.c` epoll guard, `server/sock.c` BPF guard).

2. **The x86→wasm CPU emulator.** `__wine_syscall_dispatcher` &co. must transfer
   control between native-wasm Wine and emulated i386 PE code. The proven-fast
   option is reusing the web-showcase JIT (BoxedWine's x86→wasm block JIT, incl.
   the dynamic-code amnesty) as a *library* driven at these seams, rather than
   as a whole-system emulator. `NtCurrentTeb`/CONTEXT are already declared as
   "data exchanged with the emulator, not wasm machine state" (`winnt.h`).

## Build / run

```sh
# one-time: configure the wasm cross-build (reuses the native wine-tools in ../build)
cd ~/dev/wine && mkdir -p build-wasm4 && cd build-wasm4
arch -x86_64 ../configure --host=wasm32-unknown-emscripten --with-wine-tools=../build \
  CC=emcc CXX=em++ AR=emar RANLIB=emranlib BISON=/opt/homebrew/opt/bison/bin/bison \
  --disable-tests --without-x --without-freetype

# build + link the native-wasm wine loader
~/dev/wine/webwine/build.sh

# boot it (synthetic install tree; see build.sh env)
WINELOADERNOEXEC=1 WINE_NO_SERVER_SPAWN=1 \
WINEUNIXDIR=<fakeroot>/lib/wine/wasm32-unix WINEDLLPATH=<fakeroot>/lib/wine \
WINEDATADIR=~/dev/wine/wine-macos/share/wine WINEPREFIX=<prefix> \
node out/wine.js ret42.exe
```
