# NetDuke32 Web Showcase

This packages the 32-bit NetDuke32-compatible `vibebuild32.exe` build into
BoxedWine: Wine 11, a Linux syscall layer, and an x86-to-WebAssembly JIT. Its
Windows configuration is installed as `vibebuild32.cfg` inside the overlay.

`make` builds the runtime and application overlay. The Duke data files come
from the local EDuke32 checkout and remain untracked. Start it with `make
serve`, then open `http://localhost:8080/`.

The executable is deliberately run as a PE32 Windows program under Wine; it
is not rebuilt as a native web port.
