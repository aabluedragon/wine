#!/bin/bash
# Rebuild ONE win32u source file (plus the .so link) with the same symbol
# renames rebuild-win32u.sh applies.  Rebuilding all of win32u takes minutes and
# most iteration touches a single file.  Usage: rebuild-win32u-one.sh opengl
set -e
F="${1:?usage: rebuild-win32u-one.sh <basename, e.g. opengl>}"
cd ~/dev/wine/build-wasm4
source ~/dev/emsdk/emsdk_env.sh >/dev/null 2>&1
SC=/private/tmp/claude-501/-Users-alonamir-dev-wine/27c6d9ae-34ca-4bc0-89a8-0edba8f20f0b/scratchpad
RENAMES="init_startup_info is_desktop_class is_message_class is_window_visible get_virtual_screen_rect shared_session \
alloc_user_handle free_user_handle get_window_thread get_parent set_window_pos is_desktop_window \
send_notify_message client_to_screen screen_to_client mirror_region destroy_thread_windows get_desktop_window \
map_dpi_point map_dpi_rect map_dpi_region d3dkmt_object_create d3dkmt_object_open d3dkmt_object_get_fd \
get_d3dkmt_object enum_key"
DEFS=""; for s in $RENAMES; do DEFS="$DEFS -D$s=w32u_$s"; done
arch -x86_64 make -n dlls/win32u/win32u.so 2>/dev/null | perl -0pe 's/\\\n\s*/ /g' > "$SC/w32cmds.txt"
while IFS= read -r cmd; do
  case "$cmd" in
    *emcc*-c\ *win32u/$F.c*)  echo "compiling $F.c"; eval "$cmd $DEFS" ;;
    *emcc*-shared*win32u.so*) echo "linking win32u.so"; eval "$cmd" ;;
  esac
done < "$SC/w32cmds.txt"
echo "win32u.so: $(ls -la dlls/win32u/win32u.so | awk '{print $5}') bytes"
