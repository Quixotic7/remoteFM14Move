#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# wasm2c 1.0.32's runtime made per-thread (README "Threads"), in the build image:
#   tools/w2c_mt.sh host OUTDIR   OUTDIR/wasm-rt-impl.c, wasm-rt-impl.h: the host's runtime (the Makefile builds it
#                                 into every program that loads firmware objects)
#   tools/w2c_mt.sh fw OUTDIR     OUTDIR/wasm-rt.h: the header a firmware object is compiled against (build_fw.sh)
# wasm2c's runtime keeps its trap state in process globals: the jmp_buf a trap longjmps to, the unwind target, the
# exception being thrown, and the call depth every wasm function counts (++ on entry, -- on exit, a trap past 500).
# Two firmwares on two threads would corrupt the counter and longjmp into each other's stacks. Patched here:
#   - host: the jmp_buf, the unwind target, the exception state and the saved depths are __thread; the firmware
#     objects' counter is wasm_rt_call_stack_depth_tls (__thread, defined here, exported by -rdynamic). The old
#     wasm_rt_call_stack_depth stays for objects built before this (no rfm1m_caps): fwrt.c runs every call into those
#     under one lock, and a trap restores that counter only on the thread that holds the lock
#     (rfm14_w2c_legacy_held);
#   - fw: wasm_rt_call_stack_depth is wasm_rt_call_stack_depth_tls, initial-exec TLS (the variable is the
#     executable's: one load and an add, no __tls_get_addr). The generated C is untouched.
# Every substitution is checked: a wasm2c whose sources differ fails here instead of building a wrong runtime.
set -eu
MODE="$1"
OUT="$2"
RT="${W2C_RT:-/usr/src/wasm2c}"
mkdir -p "$OUT"
need() {   # need FILE COUNT PATTERN: the patched file has the pattern COUNT times
    n=$(grep -c -F -- "$3" "$1" || true)
    [ "$n" = "$2" ] || { echo "w2c_mt: $1: '$3' found $n times, expected $2 (another wasm2c?)" >&2; exit 1; }
}
case "$MODE" in
fw)
    need "$RT/wasm-rt.h" 1 'extern uint32_t wasm_rt_call_stack_depth;'
    sed -e 's/^extern uint32_t wasm_rt_call_stack_depth;$/extern __thread uint32_t wasm_rt_call_stack_depth_tls __attribute__((tls_model("initial-exec")));\n#define wasm_rt_call_stack_depth wasm_rt_call_stack_depth_tls/' \
        "$RT/wasm-rt.h" > "$OUT/wasm-rt.h.tmp"
    need "$OUT/wasm-rt.h.tmp" 1 'extern __thread uint32_t wasm_rt_call_stack_depth_tls'
    need "$OUT/wasm-rt.h.tmp" 1 '#define wasm_rt_call_stack_depth wasm_rt_call_stack_depth_tls'
    mv "$OUT/wasm-rt.h.tmp" "$OUT/wasm-rt.h"
    ;;
host)
    for p in 'jmp_buf wasm_rt_jmp_buf;' 'uint32_t wasm_rt_saved_call_stack_depth;' 'static jmp_buf* g_unwind_target;' \
             'static uint32_t g_active_exception_tag;' 'static uint8_t g_active_exception[MAX_EXCEPTION_SIZE];' \
             'static uint32_t g_active_exception_size;' '  wasm_rt_call_stack_depth = wasm_rt_saved_call_stack_depth;'; do
        need "$RT/wasm-rt-impl.c" 1 "$p"
    done
    sed -e 's/^jmp_buf wasm_rt_jmp_buf;$/__thread jmp_buf wasm_rt_jmp_buf;/' \
        -e 's/^uint32_t wasm_rt_saved_call_stack_depth;$/__thread uint32_t wasm_rt_saved_call_stack_depth;\n__thread uint32_t wasm_rt_saved_call_stack_depth_tls;\n__attribute__((visibility("default"))) __thread uint32_t wasm_rt_call_stack_depth_tls;\n__thread int rfm14_w2c_legacy_held;/' \
        -e 's/^static jmp_buf\* g_unwind_target;$/static __thread jmp_buf* g_unwind_target;/' \
        -e 's/^static uint32_t g_active_exception_tag;$/static __thread uint32_t g_active_exception_tag;/' \
        -e 's/^static uint8_t g_active_exception\[MAX_EXCEPTION_SIZE\];$/static __thread uint8_t g_active_exception[MAX_EXCEPTION_SIZE];/' \
        -e 's/^static uint32_t g_active_exception_size;$/static __thread uint32_t g_active_exception_size;/' \
        -e 's/^  wasm_rt_call_stack_depth = wasm_rt_saved_call_stack_depth;$/  wasm_rt_call_stack_depth_tls = wasm_rt_saved_call_stack_depth_tls;\n  if (rfm14_w2c_legacy_held)\n    wasm_rt_call_stack_depth = wasm_rt_saved_call_stack_depth;/' \
        "$RT/wasm-rt-impl.c" > "$OUT/wasm-rt-impl.c.tmp"
    for p in '__thread jmp_buf wasm_rt_jmp_buf;' '__thread uint32_t wasm_rt_saved_call_stack_depth;' \
             '__thread uint32_t wasm_rt_call_stack_depth_tls;' 'static __thread jmp_buf* g_unwind_target;' \
             'static __thread uint32_t g_active_exception_tag;' 'static __thread uint8_t g_active_exception[' \
             'static __thread uint32_t g_active_exception_size;' \
             '  wasm_rt_call_stack_depth_tls = wasm_rt_saved_call_stack_depth_tls;' '  if (rfm14_w2c_legacy_held)'; do
        need "$OUT/wasm-rt-impl.c.tmp" 1 "$p"
    done
    need "$RT/wasm-rt-impl.h" 1 'extern jmp_buf wasm_rt_jmp_buf;'
    need "$RT/wasm-rt-impl.h" 1 'extern uint32_t wasm_rt_saved_call_stack_depth;'
    need "$RT/wasm-rt-impl.h" 1 '  wasm_rt_saved_call_stack_depth = wasm_rt_call_stack_depth'
    sed -e 's/^extern jmp_buf wasm_rt_jmp_buf;$/extern __thread jmp_buf wasm_rt_jmp_buf;\nextern __thread uint32_t wasm_rt_call_stack_depth_tls, wasm_rt_saved_call_stack_depth_tls;\nextern __thread int rfm14_w2c_legacy_held;/' \
        -e 's/^extern uint32_t wasm_rt_saved_call_stack_depth;$/extern __thread uint32_t wasm_rt_saved_call_stack_depth;/' \
        -e 's/^  wasm_rt_saved_call_stack_depth = wasm_rt_call_stack_depth$/  (wasm_rt_saved_call_stack_depth = wasm_rt_call_stack_depth, wasm_rt_saved_call_stack_depth_tls = wasm_rt_call_stack_depth_tls)/' \
        "$RT/wasm-rt-impl.h" > "$OUT/wasm-rt-impl.h.tmp"
    need "$OUT/wasm-rt-impl.h.tmp" 1 'extern __thread jmp_buf wasm_rt_jmp_buf;'
    need "$OUT/wasm-rt-impl.h.tmp" 1 'extern __thread uint32_t wasm_rt_saved_call_stack_depth;'
    need "$OUT/wasm-rt-impl.h.tmp" 1 'wasm_rt_saved_call_stack_depth_tls = wasm_rt_call_stack_depth_tls)'
    mv "$OUT/wasm-rt-impl.c.tmp" "$OUT/wasm-rt-impl.c"
    mv "$OUT/wasm-rt-impl.h.tmp" "$OUT/wasm-rt-impl.h"
    ;;
*)
    echo "usage: tools/w2c_mt.sh host|fw OUTDIR" >&2
    exit 2
    ;;
esac
