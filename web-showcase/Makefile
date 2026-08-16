BOXEDWINE ?= /Users/alonamir/dev/boxedwine
BOXEDWINE_BUILD ?= $(BOXEDWINE)/project/emscripten/Build/Jit
NETDUKE32_DIR ?= /Users/alonamir/games/netduke32_v1.2.1
EMSDK ?= /Users/alonamir/dev/emsdk
OUT := build
APP_ZIP ?= netduke32-v1.2.1.zip
APP_STAGE ?= staging-netduke32-v1.2.1
NETDUKE32_SHA256 := 547dea93d40114dee7757a049f20e0f7659cbd0c221ae9cf4258338e94c33878
WIN32_CC ?= i686-w64-mingw32-gcc
NETDUKE32_COMPAT_SRC := compat/netduke32-wine-launcher.c

.PHONY: all runtime app cache serve clean

all: runtime app

runtime:
	@test -f $(BOXEDWINE_BUILD)/boxedwine.wasm || { \
		echo "missing prebuilt BoxedWine runtime: $(BOXEDWINE_BUILD)" >&2; \
		echo "this Wine project will not edit or build the sibling checkout" >&2; \
		exit 1; \
	}
	mkdir -p $(OUT)
	cp $(BOXEDWINE_BUILD)/boxedwine.{html,js,wasm,css} $(OUT)/
	cp $(BOXEDWINE_BUILD)/boxedwine-shell.js $(OUT)/
	node tools/patch-webgl-version.mjs $(OUT)/boxedwine.js
	curl -fL --retry 2 https://boxedwine.org/v2/8/TinyCore15Wine11.0.zip -o $(OUT)/tinycore-wine11.zip

# Package the exact user-supplied NetDuke32 v1.2.1 PE32 build. This target
# never builds or reads source from a sibling project.
app:
	@printf '%s  %s\n' '$(NETDUKE32_SHA256)' '$(NETDUKE32_DIR)/netduke32.exe' | shasum -a 256 -c -
	rm -rf $(APP_STAGE)
	rm -f $(OUT)/$(APP_ZIP)
	mkdir -p $(APP_STAGE)/netduke32 $(OUT)
	cp $(NETDUKE32_DIR)/netduke32.exe $(APP_STAGE)/netduke32/netduke32.exe
	cp $(NETDUKE32_DIR)/DUKE3D.GRP $(NETDUKE32_DIR)/eduke32.dat $(NETDUKE32_DIR)/ebacktrace1.dll $(APP_STAGE)/netduke32/
	cp $(NETDUKE32_DIR)/netduke32.cfg $(APP_STAGE)/netduke32/netduke32.cfg
	$(WIN32_CC) -Os -s -Wall -Wextra -Werror -municode -mwindows \
		-Wl,--no-insert-timestamp \
		-o $(APP_STAGE)/netduke32/netduke32-wine-launcher.exe \
		$(NETDUKE32_COMPAT_SRC) -ladvapi32 -luser32
	# A Wine desktop is required for the Windows SDL/OpenGL window to become
	# visible in the browser canvas; launching the EXE directly leaves it black.
	printf '%s\n' 'reg add "HKCU\Software\Wine\X11 Driver" /v UseTakeFocus /d N /f' 'reg add "HKCU\Software\Wine\X11 Driver" /v UseEGL /d N /f' 'explorer /desktop=duke,640x480 netduke32-wine-launcher.exe %*' > $(APP_STAGE)/netduke32/run.bat
	# NetDuke32 selects its hardware Polymost renderer from the 32-bit mode.
	# Keep this modest browser-friendly resolution, but do not downshift it to
	# the 8-bit software renderer when packaging the showcase.
	perl -0pi -e 's/^ForceSetup = .*/ForceSetup = 0/m; s/^ScreenBPP = .*/ScreenBPP = 32/m; s/^ScreenWidth = .*/ScreenWidth = 640/m; s/^ScreenHeight = .*/ScreenHeight = 480/m; s/^ScreenMode = .*/ScreenMode = 0/m' $(APP_STAGE)/netduke32/netduke32.cfg
	cd $(APP_STAGE) && zip -q -r ../$(OUT)/$(APP_ZIP) netduke32

# Regenerate build/netduke32-jit-modules.zip (the prebuilt x86->WASM JIT cache the
# shell auto-loads so blocks LOAD instead of COMPILE at runtime -- see
# tools/gen-jit-cache.mjs). MUST be regenerated whenever the boxedwine runtime OR
# netduke32.zip changes; the cache is keyed to that exact build.
cache: app
	BOXEDWINE_BUILD=$(BOXEDWINE)/project/emscripten/Build/Jit node tools/gen-jit-cache.mjs $(OUT)

serve:
	node serve.mjs $(OUT)

clean:
	rm -rf $(OUT) $(APP_STAGE)
