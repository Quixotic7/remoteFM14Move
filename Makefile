# SPDX-License-Identifier: MPL-2.0
# RemoteFM14Move: FM-1 firmwares (.rfm1, compiled with wasm2c) on the Ableton Move, driven by its own controls.
#
#   make              everything, in the build image (docker/Dockerfile, linux/arm64): the host for the Move and for
#                     Move Remote's simulator, the compiled firmwares, the tools
#   make test         every test (tests/test.sh): Move Remote's own, then the rest in the image
#   make package      dist/remotefm1-module.tar.gz: the Schwung tool "RemoteFM1" (with the starter firmwares of
#                     module/bundle.txt and the catalog's snapshot, from dist/move-catalog when it is there)
#   make catalog      dist/move-catalog: every published .rfm1 compiled for the Move, with its index.json
#                     (tools/build_move_catalog.py; the files a release at the catalog's base URL holds)
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

.PHONY: all test package sim-run image inputs site clean catalog testcat
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
	for f in $(FIRMWARES); do cp -p "$$f" build/in/; done
	for f in $(TEST_FIRMWARES); do cp -p "$$f" build/in-test/; done
	if [ ! -f build/ref/.done ]; then git -C "$(FM1VST)" archive "$(FM1VST_REF)" tools/rfm1_run third_party/wasm3 \
	    core-api | tar -x -C build/ref && git -C "$(FM1VST)" rev-parse "$(FM1VST_REF)" > build/ref/.done; fi

test: all testcat
	tests/test.sh

# the Move catalog (on the Mac: fetches, then builds in the image): all of it, or the test's three
catalog: all
	python3 tools/build_move_catalog.py --out dist/move-catalog
testcat: all
	python3 tools/build_move_catalog.py --only amb,choralroot,felucca --out build/testcat --jobs 3

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
HOST_SRC := src/main.c src/engine.c src/fwrt.c src/storage.c src/catalog.c src/sha256.c src/web.c src/mdns.c src/minijson.c
HOST_HDR := $(wildcard src/*.h) build/gen/web_page.h build/gen/bridge_page.h

.PHONY: in-all in-host in-fw in-testfw in-tools in-ref in-package
in-all: in-tools in-fw in-testfw in-ref in-host
in-tools: build/tools/rfm1info build/tools/fwrun build/tools/unit build/tools/wsprobe
in-fw: $(FW_OUT)
in-testfw: $(FW_TEST_OUT)
in-ref: build/ref/rfm1_run
in-host: build/move/remotefm1-host build/move/remotefm1-heal build/sim/remotefm1-host

build/tools/rfm1info: tools/rfm1info.c src/minijson.c src/minijson.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ tools/rfm1info.c src/minijson.c

# the runtime the firmware objects link against: wasm2c's, built into every program that loads them (-rdynamic)
build/obj/wasm-rt-impl.o: $(W2C_RT)/wasm-rt-impl.c
	mkdir -p build/obj
	$(CC) $(CFLAGS) -fPIC $(W2C_DEFS) -I$(W2C_RT) -c $< -o $@

build/tools/fwrun: tools/fwrun.c src/fwrt.c src/fwrt.h src/rfm1m.h build/obj/wasm-rt-impl.o
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) $(W2C_DEFS) -I$(W2C_RT) -rdynamic -o $@ tools/fwrun.c src/fwrt.c build/obj/wasm-rt-impl.o -ldl -lm

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

HOST_CFLAGS = $(CFLAGS) $(W2C_DEFS) -I$(W2C_RT) -Ibuild/gen -Isrc $(CS_INC) -I$(MR)/device -DMR_PLAT_MAIN=mr_plat_main
HOST_LIBS = -rdynamic -ldl -lm -lpthread

build/%/remotefm1-host: $(HOST_SRC) $(HOST_HDR) $(CS_SRC) $(MR_SRC) $(MR)/device/plat_sim.c $(MR)/device/plat_move.c \
                        build/obj/wasm-rt-impl.o
	mkdir -p build/$*
	$(CC) $(HOST_CFLAGS) $(WARN) -DRFM14_PLAT_$(shell echo $* | tr a-z A-Z) -o $@ $(HOST_SRC) \
	    $(MR)/device/plat_$*.c $(MR_SRC) $(CS_SRC) build/obj/wasm-rt-impl.o $(HOST_LIBS) $(if $(filter move,$*),-lrt)

build/move/remotefm1-heal: $(MR)/device/heal.c
	mkdir -p build/move
	$(CC) -O2 $(WARN) -static -DHEAL_HOST='"remotefm1-host"' $< -o $@

build/tools/unit: tests/unit.c src/minijson.c src/mdns.c src/sha256.c src/engine_dsp.h
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -Isrc -o $@ tests/unit.c src/minijson.c src/mdns.c src/sha256.c -lm -lpthread

build/tools/wsprobe: tests/wsprobe.c
	mkdir -p build/tools
	$(CC) $(CFLAGS) $(WARN) -o $@ $<

in-package: in-all
	scripts/package.sh
