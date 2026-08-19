#!/bin/bash
# Rebuild win32u.so with 6 helpers renamed (avoid collision with ntdll.so),
# reusing make's exact per-file compile commands + the link command.
set -e
cd ~/dev/wine/build-wasm4
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
SC=/private/tmp/claude-501/-Users-alonamir-dev-wine/27c6d9ae-34ca-4bc0-89a8-0edba8f20f0b/scratchpad
# win32u shares many same-named helpers with the wineserver (both are linked
# statically into the one wasm module).  --allow-multiple-definition would merge
# them, so a win32u call can bind to the server's differently-typed function and
# wasm-ld emits an `unreachable` signature-mismatch stub (e.g. alloc_user_handle
# is 2-arg in win32u, 3-arg in the server).  Rename every EXTERN win32u helper
# that collides with a server symbol so win32u always calls its own copy.
RENAMES="init_startup_info is_desktop_class is_message_class is_window_visible get_virtual_screen_rect shared_session \
alloc_user_handle free_user_handle get_window_thread get_parent set_window_pos is_desktop_window \
send_notify_message client_to_screen screen_to_client mirror_region destroy_thread_windows get_desktop_window \
map_dpi_point map_dpi_rect map_dpi_region d3dkmt_object_create d3dkmt_object_open d3dkmt_object_get_fd \
get_d3dkmt_object enum_key"
DEFS=""; for s in $RENAMES; do DEFS="$DEFS -D$s=w32u_$s"; done
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
