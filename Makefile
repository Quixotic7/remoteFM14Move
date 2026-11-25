# SPDX-License-Identifier: MPL-2.0
# RemoteFM14Move: FM-1 firmwares (.rfm1, compiled with wasm2c) on the Ableton Move, driven by its own controls.
#
#   make              everything, in the build image (docker/Dockerfile, linux/arm64): the host for the Move and for
#                     Move Remote's simulator, the compiled firmwares, the tools
#   make test         every test (tests/test.sh): Move Remote's own, then the rest in the image
#   make test-sampler the sampler's tests alone (tests/sampler_suite.sh, the kit's among them); make test-kit the kit's
#   make test-update  the page's Update (src/update.c, tools/make_update.py): natively, then on the simulator
#   make update       dist/updates/remotefm1-<version>-<commit>.rfm1u: this build as a signed update for the page's Update
#                     (tools/make_update.py with UPDATE_SIGNING_KEY, default ~/.config/remotefm14move/update-signing-key.pem)
#   make package      dist/remotefm1-module.tar.gz: the Schwung tool "RemoteFM1" (with the starter firmwares of
#                     module/bundle.txt and the catalog's snapshot, from dist/move-catalog when it is there; first
#                     version-check: src/main.c, module/module.json and release.json name the same version)
#   make catalog      dist/move-catalog: every published .rfm1 compiled for the Move, with its index.json and
#                     index.json.sig (tools/build_move_catalog.py, tools/sign_index.py with CATALOG_SIGNING_KEY,
#                     default ~/.config/remotefm14move/catalog-signing-key.pem; the files a release at the base URL holds)
#   make site         build/site: the page (RemoteFM1's web/site for the Move, scripts/build_site.sh; part of make)
#   make sim-run      the host on the simulator in the image, its page on http://localhost:7730/
#   make snapsafe     src/snapsafe.h again (tools/snapsafe.py): the firmwares whose States hold their working memory
#   make wifisim      RemoteFM1 Wi-Fi's own simulator for linux/arm64 (build/sim/remotefm1-wifi_host, for
#                     tests/fm1wifi_e2e.sh): its sources from RemoteFM1 (scripts/wifisim_src.sh: RFM1_WIFI_DIR, default
#                     third_party/RemoteFM1 with its base submodule), compiled in the image
#
# The input firmwares are never in git: FIRMWARES names the .rfm1 files (default: RFM1Manager's builds beside this
# repo). FM1VST (for the wasm3 reference, tools/rfm1_run at origin/rfm1-manager) is found beside it too.
GH := $(abspath ..)
FIRMWARES ?= $(GH)/RFM1Manager/out/felucca-1.8.2.rfm1 $(GH)/RFM1Manager/out/choralroot-0.43.rfm1
# an older ChoralRoot, for the catalog's update test (tests/dl_e2e.sh)
TEST_FIRMWARES ?= $(GH)/RFM1Manager/out/choralroot-0.40.rfm1
# development builds to try on a Move (make package DEV_FIRMWARES="a.rfm1 b.rfm1"): compiled into build/fw-dev and
# packaged as firmwares-dev/, which the host installs over the same <id>-<version> (README "Development builds");
# empty (the default): none, build/in-dev emptied
DEV_FIRMWARES ?=
FM1VST ?= $(GH)/FM1VST
FM1VST_REF ?= origin/rfm1-manager
IMAGE ?= rfm14move-build
DOCKER_RUN = docker run --rm -v "$(abspath .)":/w -w /w $(IMAGE)

.PHONY: all test package sim-run image inputs site clean catalog testcat testcat-v wifisim snapsafe buildinfo update test-update test-exit
all: image inputs site buildinfo
	$(DOCKER_RUN) make -j4 in-all

# build/gen/buildinfo.h: the commits and date the host names (scripts/buildinfo.sh: git, on the Mac)
buildinfo:
	scripts/buildinfo.sh

# the page's Update (README "Updating from the page"): this build as a signed .rfm1u
UPDATE_SIGNING_KEY ?= $(HOME)/.config/remotefm14move/update-signing-key.pem
update: all
	python3 tools/make_update.py --key "$(UPDATE_SIGNING_KEY)"

# its tests: the format, the signatures, the payload's rules and gzip natively (tests/update_test.sh, which also makes
# the fixtures with tools/make_update.py and the test key), then an install and an Undo on the simulator
test-update: all
	tests/update_test.sh && $(DOCKER_RUN) tests/update_e2e.sh

# the page: RemoteFM1's web/site built for the Move (scripts/build_site.sh: git, patch and rsync, on the Mac)
site:
	scripts/build_site.sh

image:
	docker build -q -t $(IMAGE) docker >/dev/null

# the inputs, copied in (the image sees only this folder): the .rfm1 files, FM1VST's rfm1_run and wasm3
inputs:
	mkdir -p build/in build/in-test build/in-dev build/ref
	rm -f build/in-dev/*.rfm1
	for f in $(DEV_FIRMWARES); do cp -p "$$f" build/in-dev/ || exit 1; done
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

# the sampler's tests alone (the recorder, the take library, the app's e2e, the page's Sampler window): minutes, for
# work on it; make test stays the proof before an install or a release
.PHONY: test-sampler
test-sampler: all
	tests/sampler_suite.sh

# the kit's tests alone (rfm1cs stage 3a): src/kit.c natively (plain, ASan + UBSan, TSan), then the kit on the
# simulator in the image; about a minute
.PHONY: test-kit
test-kit: all
	tests/kit_test.sh && $(DOCKER_RUN) tests/kit_e2e.sh

# Global settings' Exit (the host leaves through Move Remote's QUIT) on the simulator
test-exit: all
	$(DOCKER_RUN) tests/exit_e2e.sh

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

# README "The working memory": src/snapsafe.h, the modules whose working memory a snapshot captures whole, from every .rfm1 at hand
# (the inputs, the test fixtures, the catalog's cache tools/build_move_catalog.py keeps); committed, run when the
# catalog gains firmwares (one not in it keeps the flash-only states)
.PHONY: snapsafe
snapsafe:
	python3 tools/snapsafe.py --header src/snapsafe.h $(wildcard build/in/*.rfm1 build/in-test/*.rfm1 build/catalog-cache/rfm1/*.rfm1) \
	    tests/cpu2fix/cpu2fix-1.0.rfm1 tests/tkdemo/tkdemo-1.0.rfm1 | grep -v '^safe' || true

# the release's version: src/main.c's VERSION; module/module.json and release.json must say the same (make package)
VERSION := $(shell sed -n 's/^\#define VERSION "\(.*\)"/\1/p' src/main.c)
.PHONY: version-check
version-check:
	@python3 -c 'import json,sys; v=sys.argv[1]; m=json.load(open("module/module.json"))["version"]; \
	    r=json.load(open("release.json"))["version"]; \
	    sys.exit(0 if v==m==r else "version: src/main.c %s, module/module.json %s, release.json %s" % (v,m,r))' "$(VERSION)"
	@echo "version $(VERSION)"

package: version-check all
	$(DOCKER_RUN) make in-package
	python3 tools/bundle.py build/pkg/remotefm1
	mkdir -p dist
	COPYFILE_DISABLE=1 tar -C build/pkg -czf dist/remotefm1-module.tar.gz remotefm1
	ls -l dist/remotefm1-module.tar.gz

wifisim: image
	scripts/wifisim_src.sh
	$(DOCKER_RUN) make in-wifisim

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
FW_DEV_OUT := $(patsubst build/in-dev/%.rfm1,build/fw-dev/%.rfm1m,$(wildcard build/in-dev/*.rfm1))

# rfm1cs-abletonmove (its core, session and store) and Move Remote's client half it uses (mr_decode, the 1-bit
# graphics); the font is Move Remote's device copy (the same file)
CS_SRC := $(addprefix $(CS)/src/,move.c screen.c v2.c seq.c seq_ui.c project.c projfw.c fw.c session.c store.c speech.c takeover.c engine.c sampler.c kit.c) \
          $(CS)/third_party/moveremote/host/mr_client.c $(CS)/third_party/moveremote/host/mr_gfx.c
CS_INC := -I$(CS)/include -I$(CS)/third_party/moveremote/include
MR_SRC := $(MR)/device/mr_core.c $(MR)/device/mr_ws.c $(MR)/common/font5x7.c
HOST_SRC := src/main.c src/update.c src/gz.c src/engine.c src/fwrt.c src/memsnap.c src/wav.c src/fsvc.c src/storage.c src/projmem.c src/catalog.c src/sha256.c src/web.c src/files.c src/mdns.c src/minijson.c \
            src/fx.c src/https.c src/catsig.c src/ed25519.c src/fm1link.c src/announce.c src/midiout.c \
            src/fmwords.c src/fmreader.c src/recorder.c src/takes.c src/kit.c
# the FM-1 on USB-A (src/fm1link.c): libremotefm1's portable core (MPL-2.0) from RemoteFM1; the simulator's host also
# gets its fake unit (src/fm1sim.c, --fake-fm1)
RF := third_party/RemoteFM1
RF_SRC := $(addprefix $(RF)/host/src/,codec.c screen.c panel.c session.c audio.c)
RF_SIM_SRC := $(addprefix $(RF)/host/src/,fake.c tiles_ref.c) src/fm1sim.c
RF_INC := -I$(RF)/host/include -I$(RF)/proto -I$(RF)/firmware/src
HOST_HDR := $(wildcard src/*.h) build/gen/web_page.h build/gen/bridge_page.h build/gen/buildinfo.h

.PHONY: in-all in-host in-fw in-testfw in-tools in-ref in-package in-fxmods in-wifisim in-cpu2fix in-tkdemo
in-all: in-tools in-fw in-testfw in-cpu2fix in-tkdemo in-ref in-host in-fxmods $(if $(wildcard build/wifisim-src/app-wifi),in-wifisim)
in-tools: build/tools/rfm1info build/tools/fwrun build/tools/unit build/tools/wsprobe build/tools/ainchk build/tools/fake-tts \
          build/tools/rtpmidi-peer
in-fw: $(FW_OUT)
in-testfw: $(FW_TEST_OUT) $(FW_DEV_OUT)
# the core-1 test firmware (tests/cpu2fix: its .rfm1 kept in git, made on the Mac by its build.sh), for tests/threads_e2e.sh
in-cpu2fix: build/cpu2fix/cpu2fix-1.0.rfm1m
build/cpu2fix/cpu2fix-1.0.rfm1m: tests/cpu2fix/cpu2fix-1.0.rfm1 tools/build_fw.sh tools/w2c_mt.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/cpu2fix build/w2c >/dev/null
# the takeover's test firmware (tests/tkdemo: FM1VST's tools/rfm1/tkdemo, wants tk1; its .rfm1 kept in git, GPL-3.0-only
# like FM1VST), for tests/tk_e2e.sh
in-tkdemo: build/tkdemo/tkdemo-1.0.rfm1m
build/tkdemo/tkdemo-1.0.rfm1m: tests/tkdemo/tkdemo-1.0.rfm1 tools/build_fw.sh tools/w2c_mt.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/tkdemo build/w2c >/dev/null
in-ref: build/ref/rfm1_run
in-host: build/move/remotefm1-host build/move/remotefm1-heal build/sim/remotefm1-host

build/tools/ainchk: tests/ainchk.c src/wav.c src/wav.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ tests/ainchk.c src/wav.c -lm

# Schwung's speech program as the tests have it (tests/speech_e2e.sh): the same shared memory, a log of what it is asked
build/tools/fake-tts: tests/fake_tts.c
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ $< -lrt -lm

build/tools/rfm1info: tools/rfm1info.c src/minijson.c src/minijson.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ tools/rfm1info.c src/minijson.c

# the runtime the firmware objects link against: wasm2c's, built into every program that loads them (-rdynamic),
# made per-thread (tools/w2c_mt.sh: its trap state __thread, the objects' call depth thread-local). Its memory.grow is
# fwrt.c's wasm_rt_grow_memory (a memory a core-1 twin shares never moves), the runtime's own renamed under it
W2C_MT := build/gen/w2c
build/gen/w2c/wasm-rt-impl.c: tools/w2c_mt.sh $(W2C_RT)/wasm-rt-impl.c $(W2C_RT)/wasm-rt-impl.h
	tools/w2c_mt.sh host build/gen/w2c
build/obj/wasm-rt-impl.o: build/gen/w2c/wasm-rt-impl.c
	mkdir -p build/obj
	$(CC) $(CFLAGS) -fPIC $(W2C_DEFS) -Dwasm_rt_grow_memory=w2c_rt_grow_memory -I$(W2C_RT) -c $< -o $@

build/tools/fwrun: tools/fwrun.c src/fwrt.c src/fwrt.h src/rfm1m.h src/snapsafe.h src/memsnap.c src/memsnap.h src/sha256.c \
                   src/fsvc.c src/fsvc.h src/minijson.c src/wav.c src/wav.h build/obj/wasm-rt-impl.o
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) $(W2C_DEFS) -I$(W2C_MT) -I$(W2C_RT) -rdynamic -o $@ tools/fwrun.c src/fwrt.c src/memsnap.c src/sha256.c \
	    src/fsvc.c src/minijson.c src/wav.c build/obj/wasm-rt-impl.o -ldl -lm -lpthread

build/fw/%.rfm1m: build/in/%.rfm1 tools/build_fw.sh tools/w2c_mt.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/fw build/w2c >/dev/null
build/fw-dev/%.rfm1m: build/in-dev/%.rfm1 tools/build_fw.sh tools/w2c_mt.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
	tools/build_fw.sh $< build/fw-dev build/w2c >/dev/null
build/testfw/%.rfm1m: build/in-test/%.rfm1 tools/build_fw.sh tools/w2c_mt.sh fw/fwglue.c src/rfm1m.h build/tools/rfm1info
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
HOST_CFLAGS = $(CFLAGS) $(W2C_DEFS) -I$(W2C_MT) -I$(W2C_RT) -Ibuild/gen -Isrc $(CS_INC) -I$(MR)/device -I$(SW) $(RF_INC) -DMR_PLAT_MAIN=mr_plat_main
HOST_LIBS = -rdynamic -ldl -lm -lpthread

# the simulator's host is the tests' (RFM14_TEST_BUILD: it also trusts build/testkey's catalog key, takes --ca-dir);
# the Move's trusts only src/catalog_key.h's (tests/in_image.sh checks)
TEST_BUILD = -DRFM14_TEST_BUILD -Ibuild/testkey
build/%/remotefm1-host: $(HOST_SRC) $(HOST_HDR) $(CS_SRC) $(MR_SRC) $(MR)/device/plat_sim.c $(MR)/device/plat_move.c \
                        $(RF_SRC) $(RF_SIM_SRC) build/obj/wasm-rt-impl.o build/testkey/catalog_test_key.h \
                        build/testkey/update_test_key.h
	mkdir -p build/$*
	$(CC) $(HOST_CFLAGS) $(WARN) -DRFM14_PLAT_$(shell echo $* | tr a-z A-Z) $(if $(filter sim,$*),$(TEST_BUILD)) -o $@ $(HOST_SRC) \
	    $(MR)/device/plat_$*.c $(MR_SRC) $(CS_SRC) $(RF_SRC) $(if $(filter sim,$*),$(RF_SIM_SRC)) build/obj/wasm-rt-impl.o \
	    $(HOST_LIBS) $(if $(filter move,$*),-lrt)

# RemoteFM1 Wi-Fi's simulator (make wifisim; tests/fm1wifi_e2e.sh): the flags its build.py --host gives it
WS := build/wifisim-src
in-wifisim: build/sim/remotefm1-wifi_host
build/sim/remotefm1-wifi_host: $(shell find $(WS) -name '*.[ch]' 2>/dev/null)
	mkdir -p build/obj/wifisim build/sim
	$(CC) -O2 -ffp-contract=off -std=c99 -w -DPLAT_HOST -DFM1B_CPU2=1 -DFM1B_USB_HOST=1 -I$(WS)/fm1-base/device \
	    -I$(WS)/fm1-base/include -I$(WS)/fm1-base/lib -I$(WS)/app-wifi -I$(WS)/app-wifi/build/gen -I$(WS)/app-wifi/src \
	    -DRF_WIFI_O2=1 -I$(WS)/app-wifi/sa -c $(WS)/app-wifi/sa/sa_synth.c -o build/obj/wifisim/sa_synth.o
	$(CC) -O2 -ffp-contract=off -std=c99 -w -DPLAT_HOST -DFM1B_CPU2=1 -DFM1B_USB_HOST=1 -DFM1B_BUILD=0x57494649u \
	    -DFM1B_WIFI=1 -I$(WS)/fm1-base/device -I$(WS)/fm1-base/include -I$(WS)/fm1-base/lib -I$(WS)/app-wifi \
	    -I$(WS)/app-wifi/build/gen -I$(WS)/app-wifi/src -o $@ $(WS)/fm1-base/host/plat_host.c build/obj/wifisim/sa_synth.o -lm

build/move/remotefm1-heal: $(MR)/device/heal.c
	mkdir -p build/move
	$(CC) -O2 $(WARN) -static -DHEAL_HOST='"remotefm1-host"' $< -o $@

build/tools/unit: tests/unit.c src/minijson.c src/mdns.c src/sha256.c src/engine_dsp.h src/fx.c src/fx.h src/catsig.c \
                  src/ed25519.c src/catsig.h src/ed25519.h src/catalog_key.h build/testkey/catalog_test_key.h src/fsvc.c src/fsvc.h \
                  src/files.c src/files.h src/fmwords.c src/fmwords.h src/projmem.c src/projmem.h src/memsnap.c src/memsnap.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -Isrc $(CS_INC) -I$(SW) -DFX_UNIT $(TEST_BUILD) -rdynamic -o $@ tests/unit.c src/minijson.c \
	    src/mdns.c src/sha256.c src/fx.c src/catsig.c src/ed25519.c src/fsvc.c src/files.c src/fmwords.c src/projmem.c \
	    src/memsnap.c -lm -lpthread -ldl

# the tests' catalog signing key (never the real one: that is on the owner's Mac only, outside the repo): made once
# under build/, its public half a header for the test build
build/testkey/catalog_test_key.h:
	mkdir -p build/testkey
	[ -f build/testkey/test-key.pem ] || (umask 077; openssl genpkey -algorithm ed25519 -out build/testkey/test-key.pem)
	{ echo '/* the tests catalog key (Makefile; build/testkey/test-key.pem): RFM14_TEST_BUILD only */'; \
	  echo 'static const uint8_t CAT_TEST_KEY[32] = {'; \
	  openssl pkey -in build/testkey/test-key.pem -pubout -outform DER | tail -c 32 | xxd -i; echo '};'; } > $@.tmp
	mv $@.tmp $@

# the tests' update signing key (as the catalog's: never the real one, ~/.config/remotefm14move/update-signing-key.pem)
build/testkey/update_test_key.h:
	mkdir -p build/testkey
	[ -f build/testkey/update-test-key.pem ] || (umask 077; openssl genpkey -algorithm ed25519 -out build/testkey/update-test-key.pem)
	{ echo '/* the tests update key (Makefile; build/testkey/update-test-key.pem): RFM14_TEST_BUILD only */'; \
	  echo 'static const uint8_t UPD_TEST_KEY[32] = {'; \
	  openssl pkey -in build/testkey/update-test-key.pem -pubout -outform DER | tail -c 32 | xxd -i; echo '};'; } > $@.tmp
	mv $@.tmp $@

# (made on the Mac by scripts/buildinfo.sh; a build without it, in the image alone, names no commit)
build/gen/buildinfo.h:
	mkdir -p build/gen
	printf '#define RFM14_GIT "unknown"\n#define RFM14_GIT_CORE "unknown"\n#define RFM14_BUILT ""\n' > $@

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

# a tiny AppleMIDI initiator, as a Mac's Audio MIDI Setup (tests/midiout_e2e.sh)
build/tools/rtpmidi-peer: tests/rtpmidi_peer.c
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ $<

in-package: in-all
	scripts/package.sh
