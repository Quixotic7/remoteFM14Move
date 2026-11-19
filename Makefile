# SPDX-License-Identifier: MPL-2.0
# RemoteFM14Move: FM-1 firmwares (.rfm1, compiled with wasm2c) on the Ableton Move, driven by its own controls.
#
#   make              everything, in the build image (docker/Dockerfile, linux/arm64): the host for the Move and for
#                     Move Remote's simulator, the compiled firmwares, the tools
#   make test         every test (tests/test.sh): Move Remote's own, then the rest in the image
#   make package      dist/remotefm1-module.tar.gz: the Schwung tool "RemoteFM1" (with the starter firmwares of
#                     module/bundle.txt and the catalog's snapshot, from dist/move-catalog when it is there)
#   make catalog      dist/move-catalog: every published .rfm1 compiled for the Move, with its index.json and
#                     index.json.sig (tools/build_move_catalog.py, tools/sign_index.py with CATALOG_SIGNING_KEY,
#                     default ~/.config/remotefm14move/catalog-signing-key.pem; the files a release at the base URL holds)
#   make site         build/site: the page (RemoteFM1's web/site for the Move, scripts/build_site.sh; part of make)
#   make sim-run      the host on the simulator in the image, its page on http://localhost:7730/
#
# The input firmwares are never in git: FIRMWARES names the .rfm1 files (default: RFM1Manager's builds beside this
# repo). FM1VST (for the wasm3 reference, tools/rfm1_run at origin/rfm1-manager) is found beside it too.
GH := $(abspath ..)
FIRMWARES ?= $(GH)/RFM1Manager/out/felucca-1.8.2.rfm1 $(GH)/RFM1Manager/out/choralroot-0.41.rfm1
# an older ChoralRoot, for the catalog's update test (tests/dl_e2e.sh)
TEST_FIRMWARES ?= $(GH)/RFM1Manager/out/choralroot-0.40.rfm1
FM1VST ?= $(GH)/FM1VST
FM1VST_REF ?= origin/rfm1-manager
IMAGE ?= rfm14move-build
DOCKER_RUN = docker run --rm -v "$(abspath .)":/w -w /w $(IMAGE)

.PHONY: all test package sim-run image inputs site clean catalog testcat testcat-v
all: image inputs site
	$(DOCKER_RUN) make -j4 in-all

# the page: RemoteFM1's web/site built for the Move (scripts/build_site.sh: git, patch and rsync, on the Mac)
site:
	scripts/build_site.sh

image:
	docker build -q -t $(IMAGE) docker >/dev/null

# the inputs, copied in (the image sees only this folder): the .rfm1 files, FM1VST's rfm1_run and wasm3
inputs:
	mkdir -p build/in build/in-test build/ref
	# (a file RFM1Manager no longer has, as it keeps only the newest: the copy already in build/ stands; the tests are
	#  written for these exact versions)
	for f in $(FIRMWARES); do if [ -f "$$f" ]; then cp -p "$$f" build/in/; \
	    elif [ -f "build/in/$$(basename "$$f")" ]; then echo "inputs: $$f gone, build/in's copy kept"; \
	    else echo "inputs: $$f missing" >&2; exit 1; fi; done
	for f in $(TEST_FIRMWARES); do if [ -f "$$f" ]; then cp -p "$$f" build/in-test/; \
	    elif [ -f "build/in-test/$$(basename "$$f")" ]; then echo "inputs: $$f gone, build/in-test's copy kept"; \
	    else echo "inputs: $$f missing" >&2; exit 1; fi; done
	if [ ! -f build/ref/.done ]; then git -C "$(FM1VST)" archive "$(FM1VST_REF)" tools/rfm1_run third_party/wasm3 \
	    core-api | tar -x -C build/ref && git -C "$(FM1VST)" rev-parse "$(FM1VST_REF)" > build/ref/.done; fi

test: all testcat testcat-v
	tests/test.sh

# the Move catalog (on the Mac: fetches, then builds in the image): all of it, or the test's three
# signed with the catalog's key (CATALOG_SIGNING_KEY; its public half must be src/catalog_key.h's)
CATALOG_SIGNING_KEY ?= $(HOME)/.config/remotefm14move/catalog-signing-key.pem
catalog: all
	python3 tools/build_move_catalog.py --out dist/move-catalog
	python3 tools/sign_index.py --key "$(CATALOG_SIGNING_KEY)" --expect src/catalog_key.h dist/move-catalog/index.json
testcat: all
	python3 tools/build_move_catalog.py --only amb,choralroot,felucca --out build/testcat --jobs 3
	python3 tools/sign_index.py --key build/testkey/test-key.pem build/testcat/index.json
# the Fantasy CFWs' (tests/fantasy_e2e.sh): Elasticat and Ripple, which read the RemoteFM1 folder
testcat-v: all
	python3 tools/build_move_catalog.py --only elasticat,ripple --out build/testcat-v --jobs 2
	python3 tools/sign_index.py --key build/testkey/test-key.pem build/testcat-v/index.json

package: all
	$(DOCKER_RUN) make in-package
	python3 tools/bundle.py build/pkg/remotefm1
	mkdir -p dist
	COPYFILE_DISABLE=1 tar -C build/pkg -czf dist/remotefm1-module.tar.gz remotefm1
	ls -l dist/remotefm1-module.tar.gz

sim-run: all
	docker run --rm -it -p 7730:7730 -v "$(abspath .)":/w -w /w $(IMAGE) tests/sim_run.sh

clean:
	mv build build.old-$$(date +%s) 2>/dev/null || true

# ==================================================================== inside the image (linux/arm64) ===
CC := gcc
CFLAGS ?= -O2 -g
WARN := -std=gnu11 -Wall -Wextra -Wno-format-truncation -Wno-array-parameter
W2C_RT := /usr/src/wasm2c
W2C_DEFS := -DWASM_RT_MEMCHECK_SIGNAL_HANDLER=0
MR := third_party/abletonmoveremote
CS := third_party/rfm1cs-abletonmove
FW_IN := $(wildcard build/in/*.rfm1)
FW_OUT := $(patsubst build/in/%.rfm1,build/fw/%.rfm1m,$(FW_IN))
FW_TEST_OUT := $(patsubst build/in-test/%.rfm1,build/testfw/%.rfm1m,$(wildcard build/in-test/*.rfm1))

# rfm1cs-abletonmove (its core, session and store) and Move Remote's client half it uses (mr_decode, the 1-bit
# graphics); the font is Move Remote's device copy (the same file)
CS_SRC := $(addprefix $(CS)/src/,move.c screen.c v2.c seq.c seq_ui.c project.c fw.c session.c store.c) \
          $(CS)/third_party/moveremote/host/mr_client.c $(CS)/third_party/moveremote/host/mr_gfx.c
CS_INC := -I$(CS)/include -I$(CS)/third_party/moveremote/include
MR_SRC := $(MR)/device/mr_core.c $(MR)/device/mr_ws.c $(MR)/common/font5x7.c
HOST_SRC := src/main.c src/engine.c src/fwrt.c src/wav.c src/fsvc.c src/storage.c src/catalog.c src/sha256.c src/web.c src/files.c src/mdns.c src/minijson.c \
            src/fx.c src/https.c src/catsig.c src/ed25519.c src/fm1link.c
# the FM-1 on USB-A (src/fm1link.c): libremotefm1's portable core (MPL-2.0) from RemoteFM1; the simulator's host also
# gets its fake unit (src/fm1sim.c, --fake-fm1)
RF := third_party/RemoteFM1
RF_SRC := $(addprefix $(RF)/host/src/,codec.c screen.c panel.c session.c audio.c)
RF_SIM_SRC := $(addprefix $(RF)/host/src/,fake.c tiles_ref.c) src/fm1sim.c
RF_INC := -I$(RF)/host/include -I$(RF)/proto -I$(RF)/firmware/src
HOST_HDR := $(wildcard src/*.h) build/gen/web_page.h build/gen/bridge_page.h

.PHONY: in-all in-host in-fw in-testfw in-tools in-ref in-package in-fxmods
in-all: in-tools in-fw in-testfw in-ref in-host in-fxmods
in-tools: build/tools/rfm1info build/tools/fwrun build/tools/unit build/tools/wsprobe build/tools/ainchk
in-fw: $(FW_OUT)
in-testfw: $(FW_TEST_OUT)
in-ref: build/ref/rfm1_run
in-host: build/move/remotefm1-host build/move/remotefm1-heal build/sim/remotefm1-host

build/tools/ainchk: tests/ainchk.c src/wav.c src/wav.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ tests/ainchk.c src/wav.c -lm

build/tools/rfm1info: tools/rfm1info.c src/minijson.c src/minijson.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ tools/rfm1info.c src/minijson.c

# the runtime the firmware objects link against: wasm2c's, built into every program that loads them (-rdynamic)
build/obj/wasm-rt-impl.o: $(W2C_RT)/wasm-rt-impl.c
	mkdir -p build/obj
	$(CC) $(CFLAGS) -fPIC $(W2C_DEFS) -I$(W2C_RT) -c $< -o $@

build/tools/fwrun: tools/fwrun.c src/fwrt.c src/fwrt.h src/rfm1m.h src/fsvc.c src/fsvc.h src/minijson.c src/wav.c src/wav.h build/obj/wasm-rt-impl.o
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) $(W2C_DEFS) -I$(W2C_RT) -rdynamic -o $@ tools/fwrun.c src/fwrt.c src/fsvc.c src/minijson.c src/wav.c \
	    build/obj/wasm-rt-impl.o -ldl -lm -lpthread

build/fw/%.rfm1m: build/in/%.rfm1 tools/build_fw.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/fw build/w2c >/dev/null
build/testfw/%.rfm1m: build/in-test/%.rfm1 tools/build_fw.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/testfw build/w2c >/dev/null

# FM1VST's rfm1_run (wasm3), the reference the compiled firmwares are compared with
build/ref/rfm1_run: build/ref/.done
	W=build/ref/third_party/wasm3/source; for f in bind code compile core emit env exec function info module \
	    optimize parse; do $(CC) -std=gnu11 -O3 -fomit-frame-pointer -fno-stack-check -fno-stack-protector -w -I$$W \
	    -c $$W/m3_$$f.c -o build/ref/m3_$$f.o || exit 1; done
	$(CC) -std=gnu11 -O2 -w -Ibuild/ref/third_party/wasm3/source -Ibuild/ref/core-api \
	    -o $@ build/ref/tools/rfm1_run/rfm1_run.c build/ref/m3_*.o -lm

# the pages, as C arrays
build/gen/web_page.h: $(MR)/web/index.html
	mkdir -p build/gen
	cd $(MR)/web && xxd -i index.html | sed 's/index_html/mr_web_page/g' > $(abspath $@)
build/gen/bridge_page.h: $(MR)/web/bridge.html
	mkdir -p build/gen
	cd $(MR)/web && xxd -i bridge.html | sed 's/bridge_html/mr_bridge_page/g' > $(abspath $@)

SW := third_party/schwung-api
HOST_CFLAGS = $(CFLAGS) $(W2C_DEFS) -I$(W2C_RT) -Ibuild/gen -Isrc $(CS_INC) -I$(MR)/device -I$(SW) $(RF_INC) -DMR_PLAT_MAIN=mr_plat_main
HOST_LIBS = -rdynamic -ldl -lm -lpthread

# the simulator's host is the tests' (RFM14_TEST_BUILD: it also trusts build/testkey's catalog key, takes --ca-dir);
# the Move's trusts only src/catalog_key.h's (tests/in_image.sh checks)
TEST_BUILD = -DRFM14_TEST_BUILD -Ibuild/testkey
build/%/remotefm1-host: $(HOST_SRC) $(HOST_HDR) $(CS_SRC) $(MR_SRC) $(MR)/device/plat_sim.c $(MR)/device/plat_move.c \
                        $(RF_SRC) $(RF_SIM_SRC) build/obj/wasm-rt-impl.o build/testkey/catalog_test_key.h
	mkdir -p build/$*
	$(CC) $(HOST_CFLAGS) $(WARN) -DRFM14_PLAT_$(shell echo $* | tr a-z A-Z) $(if $(filter sim,$*),$(TEST_BUILD)) -o $@ $(HOST_SRC) \
	    $(MR)/device/plat_$*.c $(MR_SRC) $(CS_SRC) $(RF_SRC) $(if $(filter sim,$*),$(RF_SIM_SRC)) build/obj/wasm-rt-impl.o \
	    $(HOST_LIBS) $(if $(filter move,$*),-lrt)

build/move/remotefm1-heal: $(MR)/device/heal.c
	mkdir -p build/move
	$(CC) -O2 $(WARN) -static -DHEAL_HOST='"remotefm1-host"' $< -o $@

build/tools/unit: tests/unit.c src/minijson.c src/mdns.c src/sha256.c src/engine_dsp.h src/fx.c src/fx.h src/catsig.c \
                  src/ed25519.c src/catsig.h src/ed25519.h src/catalog_key.h build/testkey/catalog_test_key.h src/fsvc.c src/fsvc.h \
                  src/files.c src/files.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -Isrc $(CS_INC) -I$(SW) -DFX_UNIT $(TEST_BUILD) -rdynamic -o $@ tests/unit.c src/minijson.c \
	    src/mdns.c src/sha256.c src/fx.c src/catsig.c src/ed25519.c src/fsvc.c src/files.c -lm -lpthread -ldl

# the tests' catalog signing key (never the real one: that is on the owner's Mac only, outside the repo): made once
# under build/, its public half a header for the test build
build/testkey/catalog_test_key.h:
	mkdir -p build/testkey
	[ -f build/testkey/test-key.pem ] || (umask 077; openssl genpkey -algorithm ed25519 -out build/testkey/test-key.pem)
	{ echo '/* the tests catalog key (Makefile; build/testkey/test-key.pem): RFM14_TEST_BUILD only */'; \
	  echo 'static const uint8_t CAT_TEST_KEY[32] = {'; \
	  openssl pkey -in build/testkey/test-key.pem -pubout -outform DER | tail -c 32 | xxd -i; echo '};'; } > $@.tmp
	mv $@.tmp $@

# the test FX modules (the simulator's "installed Schwung modules"): Schwung's own arp, chord and freeverb (MIT, their
# sources at the commit third_party/schwung-api names) and three of ours (crashfx: a crash in create; hogfx: CPU;
# slowfx: 80 ms over its saved state; pagesfx: a multi-level ui_hierarchy, its parameter pages)
FXM := build/fxmods
FXM_SO := $(FXM)/midi_fx/arp/dsp.so $(FXM)/midi_fx/chord/dsp.so $(FXM)/audio_fx/freeverb/freeverb.so \
          $(FXM)/audio_fx/crashfx/crashfx.so $(FXM)/audio_fx/hogfx/hogfx.so \
          $(FXM)/audio_fx/slowfx/slowfx.so $(FXM)/audio_fx/pagesfx/pagesfx.so
in-fxmods: $(FXM_SO)
$(FXM)/midi_fx/%/dsp.so: tests/fxmods/midi_fx/%/dsp/*.c tests/fxmods/midi_fx/%/module.json
	mkdir -p $(dir $@)
	cp tests/fxmods/midi_fx/$*/module.json $(dir $@)
	$(CC) -O2 -shared -fPIC -w -I$(SW) -o $@ tests/fxmods/midi_fx/$*/dsp/*.c -lm
$(FXM)/audio_fx/%.so: tests/fxmods/audio_fx/%.c
	mkdir -p $(dir $@)
	cp $(dir $<)module.json $(dir $@)
	$(CC) -O2 -shared -fPIC -w -I$(SW) -o $@ $< -lm

build/tools/wsprobe: tests/wsprobe.c
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ $<

in-package: in-all
	scripts/package.sh
