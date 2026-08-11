BOXEDWINE ?= /Users/alonamir/dev/boxedwine
EDUKE32 ?= /Users/alonamir/dev/eduke32
EMSDK ?= /Users/alonamir/dev/emsdk
OUT := build
BOXEDWINE_WASM_PATCH := patches/boxedwine-wasm-jit-scheduler.patch

.PHONY: all runtime app serve clean

all: runtime app

runtime:
	@if git -C $(BOXEDWINE) apply --reverse --check $(abspath $(BOXEDWINE_WASM_PATCH)) >/dev/null 2>&1; then \
		:; \
	else \
		git -C $(BOXEDWINE) apply $(abspath $(BOXEDWINE_WASM_PATCH)); \
	fi
	. $(EMSDK)/emsdk_env.sh && $(MAKE) -C $(BOXEDWINE)/project/emscripten jit
	mkdir -p $(OUT)
	cp $(BOXEDWINE)/project/emscripten/Build/Jit/boxedwine.{html,js,wasm,css} $(OUT)/
	cp $(BOXEDWINE)/project/emscripten/Build/Jit/boxedwine-shell.js $(OUT)/
	curl -fL --retry 2 https://boxedwine.org/v2/8/TinyCore15Wine11.0.zip -o $(OUT)/tinycore-wine11.zip

app:
	rm -rf staging
	mkdir -p staging/netduke32
	cp $(EDUKE32)/vibebuild32.exe staging/netduke32/netduke32.exe
	cp $(EDUKE32)/DUKE3D.GRP $(EDUKE32)/eduke32.dat $(EDUKE32)/vacation.grp staging/netduke32/
	cp $(EDUKE32)/netduke32.cfg staging/netduke32/vibebuild32.cfg
	cd staging && zip -q -r ../$(OUT)/netduke32.zip netduke32

serve:
	node serve.mjs $(OUT)

clean:
	rm -rf $(OUT) staging
