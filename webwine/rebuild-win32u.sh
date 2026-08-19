#!/bin/bash
# Rebuild win32u.so with 6 helpers renamed (avoid collision with ntdll.so),
# reusing make's exact per-file compile commands + the link command.
set -e
cd ~/dev/wine/build-wasm4
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
SC=/private/tmp/claude-501/-Users-alonamir-dev-wine/27c6d9ae-34ca-4bc0-89a8-0edba8f20f0b/scratchpad
DEFS="-Dinit_startup_info=w32u_init_startup_info -Dis_desktop_class=w32u_is_desktop_class -Dis_message_class=w32u_is_message_class -Dis_window_visible=w32u_is_window_visible -Dget_virtual_screen_rect=w32u_get_virtual_screen_rect -Dshared_session=w32u_shared_session"
rm -f dlls/win32u/*.o dlls/win32u/win32u.so
# get all commands (compiles + final link), join line-continuations
arch -x86_64 make -n dlls/win32u/win32u.so 2>/dev/null | perl -0pe 's/\\\n\s*/ /g' > "$SC/w32cmds.txt"
echo "commands: $(wc -l <"$SC/w32cmds.txt")"
while IFS= read -r cmd; do
  case "$cmd" in
    *emcc*-c\ *win32u*.c*)  eval "$cmd $DEFS" ;;          # compile: append renames
    *emcc*-shared*win32u.so*) echo "linking win32u.so..."; eval "$cmd" ;;   # link
    *) : ;;
  esac
done < "$SC/w32cmds.txt"
echo "win32u.so: $(ls -la dlls/win32u/win32u.so 2>/dev/null | awk '{print $5}') bytes"
