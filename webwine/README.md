# Native-WASM Wine (Option B)

Goal: run **unmodified Windows executables** in the browser by compiling **Wine
itself to native WebAssembly** and emulating **only the application's x86 code**
— instead of emulating Wine *and* the app under a full x86+Linux emulator
(BoxedWine, the current web-showcase). Wine's file I/O, scheduling, registry,
loader and syscall handling then run at native wasm speed; the emulation tax is
paid only by the game's own instructions.

This is the ARM-Mac Wine model (native Wine + Rosetta for app code), ported to
the browser (native-wasm Wine + an x86→wasm emulator for app code).

## Status: an unmodified i386 exe runs end-to-end and returns 42 ✅✅

`node out/webwine.js 'c:\ret42.exe'` **exits with code 42**. ret42.exe is an
unmodified i386 PE that calls `ExitProcess(42)`; it boots through PE ntdll and
kernel32 (400+ syscalls, all serviced by the native-wasm Wine unix side + the
in-process wineserver) and runs its own entry point. Only the guest x86 is
emulated, by the standalone interpreter `wasm_x86.c`. No BoxedWine.

Getting here took six interpreter-correctness fixes (adc/sbb carry-in, the
`fs:` segment base on both ModRM and moffs `mov`s, TEB->WOW32Reserved as the
`call fs:[0xc0]` syscall-dispatcher pointer, shift/inc-dec flags, SHLD/SHRD,
and in-interpreter **NtContinue**) plus keeping the shared-fd-table wineserver
from closing node's std streams. See the `webwine-native-wasm-ret42` memory for
the full list and the exact reconstruction/run recipe (the run needs a `c:`
drive mapping and a `C:\windows\system32` symlink to the builtin PE dlls so the
loader resolves kernel32).

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

## LANDMARK: native-wasm Wine boots to the CPU-bridge seam ✅✅

`webwine/build-combined.sh` links the wine client (ntdll unix side) + the whole
wineserver + the ring-buffer transport into ONE wasm module. Run under node with
a synthetic install tree (`webwine/make-fakeroot.sh`), it now boots **all the way
through Wine's process initialization** and stops exactly where the x86 emulator
must take over:

```
webwine: booting in-process wineserver (fd 768)
webwine: entering wine client __wine_main
0024: init_first_thread() = 0 { ... machines={014c} }        # server handshake
0024: open_mapping(...__wine_user_shared_data) = 0            # + fd passing
0024: get_token_sid() = 0 { sid=S-1-5-21-0-0-0-1000 }
0024: create_key(...\Software\Wine) ...                       # registry
... (a full flood of real client<->server RPCs) ...
wine-wasm: CPU bridge entry 'signal_start_thread' reached (guest execution requested)
```

So the **entire Wine runtime runs as native WebAssembly**: the loader, the
virtual-memory manager, the registry, NLS, process/thread init, the in-process
wineserver protocol (requests, replies, SCM_RIGHTS fd passing), and the **PE
`ntdll.dll` is loaded**. `signal_start_thread` is the seam where Wine hands
control to the guest's entry point — the one remaining subsystem is the x86->wasm
emulator that runs there (and at `__wine_syscall_dispatcher`).

### What made it work (all in this tree / webwine/)

- **Transport** (`wasm_ipc.c`): strong symbol overrides for the AF_UNIX data
  plane (not `-Wl,--wrap`, which misses Wine's calls into precompiled libc);
  ring buffers + fd queue behind magic fds; **refcounted** channels (passing a
  fd co-owns it, close frees at zero — fixes fd-number reuse from normal
  pipe-close semantics); **`writev`/`readv` overrides** (Wine's
  send_request/send_reply use them — the decisive fix); one-fd-per-`recvmsg`
  (Wine pairs one fd per `send_fd`); blocking magic-fd reads (Wine's wait_reply
  treats EAGAIN as fatal); non-magic fds delegate to node `fs` via `EM_JS`;
  `ioctl` handles the terminal queries emscripten's node tty stub crashes on.
- **Tree** (all `__wasm32__`-guarded / additive): `server/registry.c` machine
  case; `server/fd.c` poll backend; `server/sock.c` BPF guard; `server/main.c`
  `wineserver_main` + foreground; `server/request.c` skip master AF_UNIX socket;
  `server/thread.c` request pipe via the channel factory;
  `dlls/ntdll/unix/server.c` `server_pipe` via the channel factory.
- **Link/run**: `-sENVIRONMENT=node` (default multi-env asserts corrupt the
  reply path); the install-tree unix dir MUST be named `i386-unix` (current
  machine is i386 on wasm) so the loader resolves the PE dir to `i386-windows`.

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

---
### Correction to the frontier diagnosis (latest)

The blocker is **not** `current` (which `call_req_handler` sets). It is an
**fd-arrival ordering race**: `call_req_handler` sends the reply only if
`current->reply_fd` is set, else it `kill_thread`s ("no way to continue without
reply fd"). The client passes its reply/wait fds (773/775) over the **socket**
channel (769->768) via separate `sendmsg`s, while the request arrives on the
**request** channel (770). The drive's `poll` returns both 768 and 770 ready; if
it dispatches the request (770) before draining the passed fds from 768,
`reply_fd` is still NULL and the thread is killed (drive then idles forever —
the exact observed trace). Fix: drain the socket fd (delivers passed fds via
`wine_server_receive_fd`) before dispatching the request fd, or defer request
dispatch until the thread has a reply_fd.
