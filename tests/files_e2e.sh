#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page's file browser (src/web.c /api/fs, src/files.c) on the simulator, in the build image: every op (list,
# download, a folder as a .zip that unzips to the same files, upload, rename, delete, a new folder) and the refusals
# (the path rules, links out, firmwares/ and catalog/ read-only, the size cap and the free-space floor, what a running
# firmware or the open project needs); the sampler's takes in a sample library outside the folder (--takes-dir):
# audio/rec, their link, the one link followed (listed, a take downloaded, zipped, renamed, uploaded into, deleted;
# the link itself kept; a link inside the takes and .. refused).
#   tests/files_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14fs.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17770
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
start() {
    env "$@" build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site \
        --fw felucca ${XARGS:-} >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' && wait_for 5 '[ "$(field session)" = 2 ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
# code METHOD URL [curl args]: the HTTP code (the body in $T/out)
code() { _m=$1; _u=$2; shift 2; curl -s -o "$T/out" -w '%{http_code}' -X "$_m" "$@" "$U/api/fs/$_u"; }
put() { _f=$1; shift; code POST "put?$1" --data-binary "@$_f" -H 'Content-Type: application/octet-stream'; }
err() { sed -n 's/.*"error":"\([^"]*\)".*/\1/p' "$T/out"; }
le32() { printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"; }
mkwav() { _d=$(($2 * 4)); { printf 'RIFF'; le32 $((36 + _d)); printf 'WAVEfmt '; le32 16; printf '\001\000\002\000'; le32 44100; le32 176400
      printf '\004\000\020\000data'; le32 $_d; head -c $_d /dev/urandom; } > "$1"; }

echo "files_e2e"
start || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
wait_for 10 'status | grep -q "\"id\":\"felucca\""'

# ---- list
c=$(code GET "list?path=")
[ "$c" = 200 ] && grep -q '"writable":false' "$T/out" && for d in audio code data firmwares move states; do grep -q "\"name\":\"$d\",\"size\":0,\"mtime\":[0-9]*,\"dir\":true" "$T/out" || c=x; done
[ "$c" = 200 ] && grep -q '"name":"catalog-url","size":19' "$T/out" && grep -q '"busy":\["felucca","",""\]' "$T/out" \
    && ok "list the root: its folders first, then its files (catalog-url, 19 bytes), read-only; the slots' firmwares" \
    || bad "list the root: $c $(head -c 400 "$T/out")"
[ "$(code GET 'list?path=audio')" = 200 ] && grep -q '"writable":true' "$T/out" && grep -q '"free":[1-9]' "$T/out" \
    && ok "list audio/: writable, the free space" || bad "list audio/: $(head -c 300 "$T/out")"
[ "$(code GET 'list?path=nothing')" = 404 ] && [ "$(code GET 'list?path=catalog-url')" = 400 ] && ok "list: a missing folder 404, a file 400" \
    || bad "list a missing folder / a file: $(cat "$T/out")"

# ---- make a folder, upload, download
[ "$(code POST 'mkdir?path=audio/My%20Kits')" = 201 ] && [ -d "$ROOT/audio/My Kits" ] && [ "$(code POST 'mkdir?path=audio/My%20Kits')" = 409 ] \
    && ok "mkdir audio/My Kits: 201; again: 409" || bad "mkdir: $(cat "$T/out")"
mkwav "$T/kick.wav" 22050
c=$(put "$T/kick.wav" 'path=audio/My%20Kits&name=kick%20(1).wav')
[ "$c" = 201 ] && cmp -s "$T/kick.wav" "$ROOT/audio/My Kits/kick (1).wav" && [ -z "$(ls -A "$ROOT/audio/My Kits" | grep -v '^kick (1).wav$')" ] \
    && ok "put a WAV into audio/My Kits/ (a space and brackets): 201, the same bytes, no temporary file left" || bad "put: $c $(cat "$T/out")"
mkwav "$T/slow.wav" 30000   # (120 KB at 200 KB/s: the body comes over many reads, as it does over Wi-Fi)
c=$(code POST 'put?path=audio&name=slow.wav' --data-binary "@$T/slow.wav" --limit-rate 200k)
[ "$c" = 201 ] && cmp -s "$T/slow.wav" "$ROOT/audio/slow.wav" && ok "put a WAV that arrives slowly: 201, the same bytes" || bad "a slow put: $c $(cat "$T/out")"
printf 'hello\n' > "$T/notes.txt"
[ "$(put "$T/notes.txt" 'path=audio&name=notes.txt')" = 415 ] && [ ! -f "$ROOT/audio/notes.txt" ] && ok "audio/ takes WAV and AIFF only: a text file 415 ($(err))" || bad "audio/ text: $(cat "$T/out")"
mkdir -p "$ROOT/data/myapp"
c1=$(put "$T/notes.txt" 'path=data/myapp&name=notes.txt'); c2=$(put "$T/kick.wav" 'path=data/myapp&name=notes.txt')
c3=$(put "$T/kick.wav" 'path=data/myapp&name=notes.txt&replace=1')
[ "$c1" = 201 ] && [ "$c2" = 409 ] && [ "$c3" = 201 ] && cmp -s "$T/kick.wav" "$ROOT/data/myapp/notes.txt" \
    && ok "put into data/myapp: 201; the same name again 409; replace=1 takes its place" || bad "put/replace: $c1 $c2 $c3"
: > "$T/empty"
[ "$(put "$T/empty" 'path=code&name=empty.lua')" = 201 ] && [ -f "$ROOT/code/empty.lua" ] && [ ! -s "$ROOT/code/empty.lua" ] \
    && ok "put an empty file: 201" || bad "an empty file: $(cat "$T/out")"
c=$(curl -s -D "$T/hdr" -o "$T/got" -w '%{http_code}' "$U/api/fs/get?path=audio/My%20Kits/kick%20(1).wav")
[ "$c" = 200 ] && cmp -s "$T/got" "$T/kick.wav" && grep -qi "content-disposition: attachment; filename\*=UTF-8''kick%20%281%29.wav" "$T/hdr" \
    && grep -qi "content-length: $(wc -c < "$T/kick.wav" | tr -d ' ')" "$T/hdr" && ok "get: the file, streamed, as an attachment with its name and length" \
    || bad "get: $c $(head -c 300 "$T/hdr")"
c=$(curl -s -o "$T/fw.json" -w '%{http_code}' "$U/api/fs/get?path=firmwares/felucca-1.8.2.json")
[ "$c" = 200 ] && cmp -s "$T/fw.json" "$ROOT/firmwares/felucca-1.8.2.json" && ok "get from firmwares/ (read-only, not hidden)" || bad "get a firmware's manifest: $c"
[ "$(code GET 'get?path=audio')" = 400 ] && [ "$(code GET 'get?path=audio/none.wav')" = 404 ] && ok "get: a folder 400, a missing file 404" || bad "get refusals"

# ---- a folder as a zip: unzips to the same files
mkdir -p "$ROOT/states/old/sub"; head -c 300000 /dev/urandom > "$ROOT/states/old/state-00.bin"; printf '{}' > "$ROOT/states/old/state-00.json"
c=$(curl -s -D "$T/zhdr" -o "$T/states.zip" -w '%{http_code}' "$U/api/fs/zip?path=states")
mkdir -p "$T/unz"; ( cd "$T/unz" && busybox unzip -q "$T/states.zip" ) 2>"$T/unz.err"
same=1; for f in $(cd "$ROOT" && find states -type f); do cmp -s "$ROOT/$f" "$T/unz/$f" || same=0; done
n=$(cd "$ROOT" && find states -type f | wc -l | tr -d ' '); m=$(cd "$T/unz" && find states -type f | wc -l | tr -d ' ')
[ "$c" = 200 ] && [ $same = 1 ] && [ "$n" = "$m" ] && [ -d "$T/unz/states/old/sub" ] && grep -qi "content-length: $(wc -c < "$T/states.zip" | tr -d ' ')" "$T/zhdr" \
    && grep -qi "filename\*=UTF-8''states.zip" "$T/zhdr" \
    && ok "zip of states/ (Export states): $(wc -c < "$T/states.zip" | tr -d ' ') bytes, its length said first; unzips to the same $n files, the empty folder too" \
    || bad "the states zip: $c, same $same, $n / $m files; $(cat "$T/unz.err")"
c=$(curl -s -o "$T/root.zip" -w '%{http_code}' "$U/api/fs/zip?path=")
mkdir -p "$T/unr"; ( cd "$T/unr" && busybox unzip -q "$T/root.zip" ) 2>"$T/unr.err"
[ "$c" = 200 ] && cmp -s "$T/unr/RemoteFM1/firmwares/felucca-1.8.2.rfm1m" "$ROOT/firmwares/felucca-1.8.2.rfm1m" \
    && cmp -s "$T/unr/RemoteFM1/audio/My Kits/kick (1).wav" "$T/kick.wav" \
    && ok "zip of the whole folder: RemoteFM1/ with every file ($(wc -c < "$T/root.zip" | tr -d ' ') bytes)" || bad "the root zip: $c $(cat "$T/unr.err")"
[ "$(code GET 'zip?path=nothing')" = 404 ] && ok "zip of a missing folder: 404" || bad "zip missing: $(cat "$T/out")"

# ---- rename
[ "$(code POST 'rename?path=audio/My%20Kits/kick%20(1).wav&to=audio/kick.wav')" = 200 ] && [ -f "$ROOT/audio/kick.wav" ] && [ ! -e "$ROOT/audio/My Kits/kick (1).wav" ] \
    && ok "rename (a move into another folder): audio/My Kits/kick (1).wav -> audio/kick.wav" || bad "rename: $(cat "$T/out")"
c1=$(code POST 'rename?path=audio/kick.wav&to=data/myapp/notes.txt'); c2=$(code POST 'rename?path=audio&to=audio/My%20Kits/x')
c3=$(code POST 'rename?path=audio/My%20Kits&to=audio/My%20Kits/inner'); c4=$(code POST 'rename?path=audio/kick.wav&to=firmwares/kick.wav')
c5=$(code POST 'rename?path=audio/kick.wav&to=audio/a%3Fb.wav'); c6=$(code POST 'rename?path=audio/none.wav&to=audio/x.wav')
[ "$c1" = 409 ] && [ "$c2" = 403 ] && [ "$c3" = 400 ] && [ "$c4" = 403 ] && [ "$c5" = 400 ] && [ "$c6" = 404 ] \
    && ok "rename refused: onto a name there (409), a folder the Move needs (403), into itself (400), into firmwares/ (403), a ? in the new name (400), a missing file (404)" \
    || bad "rename refusals: $c1 $c2 $c3 $c4 $c5 $c6"

# ---- the path rules and links
cs=""; for p in '..' 'audio/..' '%2e%2e/etc' 'audio/.hidden' 'audio//x' 'audio/' '/audio' 'audio%00x' 'audio/a%5Cb' 'audio/%C3' 'audio/%01x'; do cs="$cs $(code GET "list?path=$p")"; done
[ "$(echo $cs | tr ' ' '\n' | grep -vc '^400$')" = 0 ] && ok "refused (400): .., %2e%2e, a hidden name, //, a trailing /, absolute, %00, \\, bad UTF-8, a control character" || bad "the path rules: $cs"
ln -s /etc "$ROOT/data/out"; ln -s /etc/passwd "$ROOT/audio/pw.wav"
c1=$(code GET 'list?path=data/out'); c2=$(code GET 'get?path=data/out/passwd'); c3=$(code GET 'get?path=audio/pw.wav'); c4=$(code GET 'zip?path=data')
unzip_l=$(cd "$T" && curl -s "$U/api/fs/zip?path=data" -o d.zip && busybox unzip -l d.zip 2>/dev/null)
code GET 'list?path=data' >/dev/null; listed=$(grep -c '"name":"out"' "$T/out")
[ "$c1" = 403 ] && [ "$c2" = 403 ] && [ "$c3" = 403 ] && [ "$c4" = 200 ] && ! echo "$unzip_l" | grep -q "data/out" && [ "$listed" = 0 ] \
    && echo "$unzip_l" | grep -q "data/myapp/notes.txt" \
    && ok "links out of the folder: not followed (403), not listed, not in a zip (the rest of data/ is)" || bad "links: $c1 $c2 $c3 $c4 listed $listed"

# ---- firmwares/, catalog/, the root: read-only; what is in use
c1=$(put "$T/notes.txt" 'path=firmwares&name=evil.rfm1m'); e1=$(err)
c2=$(code POST 'delete?path=firmwares/felucca-1.8.2.rfm1m'); c3=$(put "$T/notes.txt" 'path=&name=x.txt'); c4=$(code POST 'mkdir?path=newdir')
c5=$(code POST 'delete?path=catalog-url')
[ "$c1" = 403 ] && [ "$c2" = 403 ] && [ "$c3" = 403 ] && [ "$c4" = 403 ] && [ "$c5" = 403 ] && [ -f "$ROOT/firmwares/felucca-1.8.2.rfm1m" ] && [ ! -e "$ROOT/firmwares/evil.rfm1m" ] \
    && ok "firmwares/ read-only (\"$e1\"), the root's files and new folders there refused" || bad "read-only: $c1 $c2 $c3 $c4 $c5"
mkdir -p "$ROOT/states/felucca"; [ -f "$ROOT/states/felucca/flash.bin" ] || printf 'f' > "$ROOT/states/felucca/flash.bin"
c1=$(code POST 'delete?path=states/felucca/flash.bin'); e1=$(err); c2=$(code POST 'delete?path=move/open-project'); c3=$(code POST 'delete?path=states')
[ "$c1" = 403 ] && [ "$c2" = 403 ] && [ "$c3" = 403 ] && [ -f "$ROOT/states/felucca/flash.bin" ] \
    && ok "in use, refused with a reason: Felucca's flash.bin (\"$e1\"), move/open-project, states/ itself" || bad "in use: $c1 $c2 $c3 ($e1)"
op=$(cat "$ROOT/move/open-project" 2>/dev/null || echo 0); pf=$(printf 'move/projects/move-project-%02d.rfmmove' $((op + 1)))
mkdir -p "$ROOT/move/projects"; [ -f "$ROOT/$pf" ] || printf 'x' > "$ROOT/$pf"
c1=$(code POST "delete?path=$pf"); e1=$(err); printf 'y' > "$ROOT/move/projects/move-project-32.rfmmove"; c2=$(code POST 'delete?path=move/projects/move-project-32.rfmmove')
[ "$c1" = 403 ] && [ "$c2" = 200 ] && [ -f "$ROOT/$pf" ] && ok "the open project's file stays (\"$e1\"); another project deletes" || bad "projects: $c1 $c2 ($e1)"

# ---- delete
c1=$(code POST 'delete?path=audio/My%20Kits')
[ "$c1" = 200 ] && [ ! -d "$ROOT/audio/My Kits" ] && ok "delete an empty folder: 200" || bad "delete an empty folder: $c1"
mkdir -p "$ROOT/audio/My Kits/deep/er"; printf 'z' > "$ROOT/audio/My Kits/deep/er/a.wav"
c2=$(code POST 'delete?path=audio/My%20Kits'); c3=$(code POST 'delete?path=audio/My%20Kits&recursive=1'); c4=$(code POST 'delete?path=audio/My%20Kits&recursive=1&confirm=Kits')
c5=$(code POST 'delete?path=audio/My%20Kits&recursive=1&confirm=My%20Kits')
[ "$c2" = 409 ] && [ "$c3" = 409 ] && [ "$c4" = 409 ] && [ "$c5" = 200 ] && [ ! -e "$ROOT/audio/My Kits" ] \
    && ok "a folder with things in it: 409, also with recursive=1 alone or the wrong confirm; recursive=1&confirm=its name deletes it all" \
    || bad "recursive delete: $c2 $c3 $c4 $c5"
c1=$(code POST 'delete?path=data/out'); [ "$c1" = 403 ] && [ -e /etc/passwd ] && ok "a link: not deleted through (403)" || bad "delete a link: $c1"
[ "$(code POST 'delete?path=audio/kick.wav')" = 200 ] && [ ! -e "$ROOT/audio/kick.wav" ] && [ "$(code POST 'delete?path=audio/kick.wav')" = 404 ] \
    && ok "delete a file: 200; again: 404" || bad "delete a file"

# ---- the size cap and the free-space floor
c=$(curl -s -o "$T/out" -w '%{http_code}' -X POST -H 'Content-Length: 268435460' --data-binary "@$T/notes.txt" "$U/api/fs/put?path=data&name=big.bin")
[ "$c" = 413 ] && [ ! -e "$ROOT/data/big.bin" ] && ok "over 256 MB: 413" || bad "the size cap: $c"
stop
start RFM14_TEST_DISK_FREE_MB=64
c=$(put "$T/kick.wav" 'path=audio&name=k.wav'); e=$(err)
[ "$c" = 507 ] && [ ! -e "$ROOT/audio/k.wav" ] && ok "64 MB free on the disk: refused, 64 MB stay free (507: \"$e\")" || bad "the floor: $c"
[ -z "$(find "$ROOT" -name '*.part')" ] && ok "no temporary file left anywhere" || bad "left: $(find "$ROOT" -name '*.part')"
stop

# ---- the sampler's takes in a sample library outside the folder: audio/rec a link to them, the one link followed
LIB=$T/Samples/RemoteFM1; DAY=$LIB/2026-11-24; mkdir -p "$DAY/1124-153000 Stems 120bpm 1b"
mkwav "$DAY/1124-153000 Felucca 120bpm 1b.wav" 88200; mkwav "$DAY/1124-153000 Stems 120bpm 1b/Master.wav" 1000
ln -s /etc "$LIB/out"   # (a link inside the takes pointing out)
rmdir "$ROOT/audio/rec" 2>/dev/null   # (the recorder's own audio/rec from the runs above: empty)
XARGS="--takes-dir $LIB" start || { bad "the host did not start (--takes-dir)"; tail -20 "$LOG"; exit 1; }
[ -L "$ROOT/audio/rec" ] && [ "$(field rec_link)" = 1 ] || bad "audio/rec is not the takes' link: $(ls -l "$ROOT/audio")"
c=$(code GET 'list?path=audio')
[ "$c" = 200 ] && grep -q '"name":"rec","size":0,"mtime":[0-9]*,"dir":true,"link":true}' "$T/out" \
    && ok "list audio/: rec, the takes' link, as a folder (\"link\":true)" || bad "audio/rec in the list: $c $(head -c 400 "$T/out")"
c=$(code GET 'list?path=audio/rec/2026-11-24')
[ "$c" = 200 ] && grep -q '"name":"1124-153000 Felucca 120bpm 1b.wav","size":352844,' "$T/out" && grep -q '"name":"1124-153000 Stems 120bpm 1b","size":0,.*"dir":true' "$T/out" \
    && code GET 'list?path=audio/rec' >/dev/null && ! grep -q '"name":"out"' "$T/out" \
    && ok "list audio/rec/2026-11-24/: the take (352844 bytes) and a stems folder; the link out inside the takes not listed" \
    || bad "the takes' list: $c $(head -c 400 "$T/out")"
c=$(curl -s -o "$T/take.wav" -w '%{http_code}' "$U/api/fs/get?path=audio/rec/2026-11-24/1124-153000%20Felucca%20120bpm%201b.wav")
[ "$c" = 200 ] && cmp -s "$T/take.wav" "$DAY/1124-153000 Felucca 120bpm 1b.wav" && ok "get a take through audio/rec: byte for byte" || bad "get a take: $c"
c=$(curl -s -o "$T/rec.zip" -w '%{http_code}' "$U/api/fs/zip?path=audio/rec")
mkdir -p "$T/unrec"; ( cd "$T/unrec" && busybox unzip -q "$T/rec.zip" ) 2>"$T/unrec.err"
[ "$c" = 200 ] && cmp -s "$T/unrec/rec/2026-11-24/1124-153000 Stems 120bpm 1b/Master.wav" "$DAY/1124-153000 Stems 120bpm 1b/Master.wav" \
    && [ ! -e "$T/unrec/rec/out" ] && ok "zip of audio/rec: the takes, the stems folder's files; not the link out" || bad "zip of audio/rec: $c $(cat "$T/unrec.err")"
c1=$(code POST 'rename?path=audio/rec/2026-11-24/1124-153000%20Felucca%20120bpm%201b.wav&to=audio/rec/2026-11-24/My%20take.wav')
[ "$c1" = 200 ] && [ -f "$DAY/My take.wav" ] && [ ! -e "$DAY/1124-153000 Felucca 120bpm 1b.wav" ] && ok "rename a take inside audio/rec: 200, renamed in the sample library" || bad "rename a take: $c1 $(cat "$T/out")"
c1=$(put "$T/kick.wav" 'path=audio/rec&name=up.wav')
[ "$c1" = 201 ] && cmp -s "$T/kick.wav" "$LIB/up.wav" && ok "put into audio/rec: 201, in the sample library" || bad "put into audio/rec: $c1 $(cat "$T/out")"
c1=$(code POST 'delete?path=audio/rec/2026-11-24/My%20take.wav'); c2=$(code POST 'delete?path=audio/rec/up.wav')
[ "$c1" = 200 ] && [ "$c2" = 200 ] && [ ! -e "$DAY/My take.wav" ] && [ ! -e "$LIB/up.wav" ] && ok "delete takes inside audio/rec: 200, gone from the sample library" || bad "delete a take: $c1 $c2"
c1=$(code POST 'delete?path=audio/rec'); e1=$(err); c2=$(code POST 'rename?path=audio/rec&to=audio/x'); c3=$(code GET 'get?path=audio/rec/out/passwd')
c4=$(code GET 'list?path=audio/rec/out'); c5=$(code GET 'get?path=audio/rec/../../catalog-url'); c6=$(code GET 'get?path=data/out/passwd')
[ "$c1" = 403 ] && [ "$c2" = 403 ] && [ -L "$ROOT/audio/rec" ] && [ "$c3" = 403 ] && [ "$c4" = 403 ] && [ "$c5" = 400 ] && [ "$c6" = 403 ] \
    && ok "audio/rec itself stays (403: \"$e1\"); a link inside the takes, .., another link: refused" || bad "the takes' refusals: $c1 $c2 $c3 $c4 $c5 $c6"
stop
echo "files_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
