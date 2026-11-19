#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The firmware catalog on Move Remote's simulator, end to end, in the build image: the Move's browser (rfm1cs's
# projects view, Mute: the catalog) downloads and updates firmwares from a local server, as the Move does from the
# catalog's base URL; loads while another firmware plays; the firmwares' log lines.
#   tests/dl_e2e.sh       (after make and the test catalog: build/testcat, tools/build_move_catalog.py --only
#                          amb,choralroot,felucca; build/testfw/choralroot-0.40.*, an older ChoralRoot to update)
# Downloads go through the host's own client (src/https.c): plain http (phase A: a base URL the owner set) from BusyBox
# httpd on 127.0.0.1 (the catalog; a copy whose AMB-1 object is damaged); HTTPS through the image's OpenSSL 3 (phases B,
# F) from `openssl s_server` with a test CA (the host's --ca-dir, a test-build option): cat.test redirects every file to
# objects.test (another name, its own certificate), wrong.test reaches objects.test's server (a name its certificate
# lacks), and a self-signed cat.test (a CA the host does not trust). The names are 127.0.0.1 in the container's
# /etc/hosts. Every index is signed (build/testkey's test key, tools/sign_index.py); phases F and G refuse an index with
# no signature, a bad one, or a changed index, over the network and as the bundled snapshot.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14dl.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17731; GOOD=18080; BAD=18081; RED=18443; OBJ=18444; SELF=18445
mkdir -p "$SIM" "$T/bundled"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
status() { curl -s "http://127.0.0.1:$PORT/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
INDEX=build/testcat/index.json
start() {   # start [VAR=value ...]: the host, with those in its environment (XARGS: more arguments; INDEX: the snapshot)
    env "$@" build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares "$T/bundled" \
        --site build/site --catalog-index "$INDEX" --ca-dir "$T/tls/cadir" --fw felucca ${XARGS:-} >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' &&
        wait_for 5 '[ "$(field session)" = 2 ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
url() { echo "$1" > "$ROOT/catalog-url"; }
view() { cmd "cc 43 127"; sleep 0.15; press "note 16"; cmd "cc 43 0"; sleep 0.3; }   # Track 1 held + step 1
back() { press "cc 51"; press "cc 51"; press "cc 51"; press "cc 51"; }
U="http://127.0.0.1:$PORT"
post() { curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "$1" "$U/api/load"; }
files() { ls "$ROOT/firmwares" | grep "^$1-" | tr '\n' ' '; }
phase() { case "${PHASES:-ABCDEFGHR}" in *$1*) return 0;; esac; return 1; }   # (PHASES=AD: only those)
raw() { status | grep -o "\"$1\":\"[^\"]*\"" | cut -d'"' -f4; }   # (a string field with commas in it)

echo "dl_e2e"
for f in build/fw/felucca-1.8.2.* build/testfw/choralroot-0.40.*; do cp "$f" "$T/bundled/"; done
AMB=$(cd build/testcat && ls amb-*.rfm1m); AMB=${AMB%.rfm1m}
CR=$(cd build/testcat && ls choralroot-*.rfm1m); CR=${CR%.rfm1m}
[ -n "$AMB" ] && [ -n "$CR" ] || { echo "  FAIL  no test catalog in build/testcat"; exit 1; }
# the servers: the catalog, and a copy with one byte of AMB-1's object changed (its SHA-256 no longer the index's)
mkdir -p "$T/badcat"; cp build/testcat/* "$T/badcat/"
printf 'X' | dd of="$T/badcat/$AMB.rfm1m" bs=1 seek=4096 conv=notrunc 2>/dev/null
[ -f build/testcat/index.json.sig ] || { echo "  FAIL  build/testcat/index.json is not signed (make testcat)"; exit 1; }
busybox httpd -f -p 127.0.0.1:$GOOD -h build/testcat & S1=$!
busybox httpd -f -p 127.0.0.1:$BAD -h "$T/badcat" & S2=$!
# HTTPS: a test CA (the host trusts only it: --ca-dir), certificates for cat.test and objects.test, a self-signed one
grep -q " cat.test" /etc/hosts || echo "127.0.0.1 cat.test objects.test wrong.test" >> /etc/hosts
X=$T/tls; mkdir -p "$X/cadir"
cert() {   # cert NAME [self]: NAME.pem / NAME.key, signed by the test CA (or by itself)
    if [ "${2:-}" = self ]; then
        openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$X/$1-self.key" -out "$X/$1-self.pem" \
            -days 2 -subj "/CN=$1" -addext "subjectAltName=DNS:$1" 2>/dev/null
    else
        openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$X/$1.key" -out "$X/$1.csr" \
            -subj "/CN=$1" 2>/dev/null
        printf 'subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' "$1" > "$X/$1.ext"
        openssl x509 -req -in "$X/$1.csr" -CA "$X/ca.pem" -CAkey "$X/ca.key" -CAcreateserial -out "$X/$1.pem" -days 2 \
            -extfile "$X/$1.ext" 2>/dev/null
    fi
}
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$X/ca.key" -out "$X/ca.pem" -days 2 \
    -subj /CN=RemoteFM1-test-CA -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign 2>/dev/null
cp "$X/ca.pem" "$X/cadir/$(openssl x509 -hash -noout -in "$X/ca.pem").0"
cert cat.test; cert objects.test; cert cat.test self
# objects.test's files: good/ (the catalog), nosig/ (no index.json.sig), badsig/ (another index's signature), tampered/
# (one byte of the index changed, the signature the original's); cat.test redirects each file to objects.test
SRV=$T/srv; mkdir -p "$SRV/good" "$SRV/nosig" "$SRV/badsig" "$SRV/tampered" "$T/red"
cp build/testcat/* "$SRV/good/"; cp build/testcat/* "$SRV/nosig/"; rm -f "$SRV/nosig/index.json.sig"
cp build/testcat/* "$SRV/badsig/"; cp build/testcat/* "$SRV/tampered/"
sed 's/"generated":"/"generated":"9/' build/testcat/index.json > "$SRV/badsig/index.json"   # (later: it would win)
sed 's/A generative ambient/A generative AMBIENT/' build/testcat/index.json > "$SRV/tampered/index.json"
cmp -s "$SRV/tampered/index.json" build/testcat/index.json && echo "  info  the tampered index is not changed"
for f in "$SRV"/*/*; do
    rel=${f#$SRV/}; mkdir -p "$T/red/$(dirname "$rel")"
    printf 'HTTP/1.0 302 Found\r\nLocation: https://objects.test:%s/%s\r\nContent-Length: 0\r\n\r\n' $OBJ "$rel" > "$T/red/$rel"
done
# (s_server -WWW answers 200 for a missing file: cat.test says 404 for nosig's .sig itself; downgrade/ redirects to http)
printf 'HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n' > "$T/red/nosig/index.json.sig"
mkdir -p "$T/red/downgrade"
printf 'HTTP/1.0 302 Found\r\nLocation: http://127.0.0.1:%s/index.json\r\nContent-Length: 0\r\n\r\n' $GOOD > "$T/red/downgrade/index.json"
(cd "$T/red" && exec openssl s_server -quiet -HTTP -accept $RED -cert "$X/cat.test.pem" -key "$X/cat.test.key") >/dev/null 2>&1 & S3=$!
(cd "$SRV" && exec openssl s_server -quiet -WWW -accept $OBJ -cert "$X/objects.test.pem" -key "$X/objects.test.key") >/dev/null 2>&1 & S4=$!
(cd "$SRV" && exec openssl s_server -quiet -WWW -accept $SELF -cert "$X/cat.test-self.pem" -key "$X/cat.test-self.key") >/dev/null 2>&1 & S5=$!
sleep 0.5

# ---------------------------------------------------------------- phase A: plain http (a base URL the owner set)
if phase A; then
mkdir -p "$ROOT"; url "http://127.0.0.1:$BAD"
start || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
[ "$(field cat_n)" = 3 ] && [ "$(field cat_installed)" = 2 ] && [ "$(field cat_from)" = 1 ] \
    && ok "the catalog: 3 firmwares from the bundled index ($(field cat_gen)), 2 installed (Felucca, ChoralRoot 0.40)" \
    || bad "the catalog: $(field cat_n) / $(field cat_installed) / from $(field cat_from)"
wait_for 8 '[ "$(field index_ok)" = 1 ]' && ok "the index (and its signature) fetched at start over plain http (the owner's catalog-url), the same as the bundled one: no new list" \
    || bad "the index at start: ok $(field index_ok), failed $(field index_failed): $(field index_err)"
# Mute: the catalog; A-Z: AMB-1 (not here), ChoralRoot (an update), Felucca; the first pad selects AMB-1
view; press "cc 88"; press "note 92"
cmd shot; sleep 0.4
# the thumbnail: the screen's right half (x 64..127) is the index's 64 x 64 bits, exactly
sed -n 's/.*"id":"amb","name":[^{]*"thumb":"\([^"]*\)".*/\1/p' build/testcat/index.json | base64 -d > "$T/thumb.bin"
xxd -b -c 8 "$T/thumb.bin" | awk '{ s = ""; for (i = 2; i <= 9; i++) s = s $i; print s }' > "$T/thumb.txt"
tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | cut -c65-128 > "$T/screen.txt"
want=$(tr -cd 1 < "$T/thumb.txt" | wc -c); got=$(tr -cd 1 < "$T/screen.txt" | wc -c)
[ "$(wc -l < "$T/thumb.txt")" = 64 ] && [ "$want" -gt 100 ] && cmp -s "$T/thumb.txt" "$T/screen.txt" \
    && ok "the catalog shows AMB-1's thumbnail from the index: the screen's 64 x 64 is its bits ($got pixels lit, $want in the index)" \
    || bad "the thumbnail: $got pixels lit on the screen's right half, $want in the index"
# a damaged file: refused
press "cc 3"
wait_for 10 '[ "$(field dl_state)" = 3 ]' && field dl_err | grep -q "refused: not the catalog's SHA-256" \
    && [ -z "$(files amb)" ] && [ -z "$(ls "$ROOT/firmwares/.dl")" ] \
    && ok "a file whose SHA-256 is not the index's is refused: -1, nothing installed, nothing left in .dl ($(field dl_err | cut -c1-70)..)" \
    || bad "the bad SHA: state $(field dl_state), err '$(field dl_err)', files '$(files amb)'"
# no network: -1, and a clear state (the next one works)
url "http://127.0.0.1:1"; sleep 2.2   # (the failure's popup)
press "cc 3"
wait_for 10 '[ "$(field dl_failed)" = 2 ]' && [ -z "$(files amb)" ] && [ -z "$(ls "$ROOT/firmwares/.dl")" ] \
    && ok "no network: -1 ($(field dl_err | cut -c1-60)), nothing installed, nothing left" \
    || bad "no network: failed $(field dl_failed), err '$(field dl_err)'"
url "http://127.0.0.1:$GOOD"; sleep 2.2
press "cc 3"
wait_for 15 '[ "$(field dl_state)" = 2 ]' && [ "$(field dl_pct)" = 100 ] && [ -f "$ROOT/firmwares/$AMB.rfm1m" ] \
    && [ -f "$ROOT/firmwares/$AMB.json" ] && [ -f "$ROOT/firmwares/$AMB.licenses.txt" ] \
    && ok "Mute, AMB-1, jog press: downloaded (progress 100), checked, installed: $(files amb)" \
    || bad "the download: state $(field dl_state), pct $(field dl_pct), err '$(field dl_err)', files '$(files amb)'"
fws=$(curl -s "$U/api/firmwares" | tr -d '\n')
echo "$fws" | grep -q '"id":"amb"' && [ "$(field cat_installed)" = 3 ] \
    && echo "$fws" | grep -q '"id":"amb"[^}]*"description":"A generative ambient' \
    && echo "$fws" | grep -q '"id":"amb"[^}]*"categories":\["Synth","Other"\]' \
    && ok "listed installed (3 now): /api/firmwares has AMB-1, with its description and categories" \
    || bad "/api/firmwares after the download: $(echo "$fws" | head -c 300)"
[ "$(post '{"slot":1,"id":"amb"}')" = 202 ] && wait_for 10 '[ "$(field id1)" = amb ]' \
    && ok "the downloaded firmware loads (slot 2: AMB-1 $(field ver1))" || bad "AMB-1 does not load: slot 2 '$(field id1)'"
back
stop
[ "$RC" = 0 ] && ok "phase A: the host left cleanly" || bad "phase A: exit $RC"
fi

# ---------------------------------------------------------------- phase B: HTTPS, a redirect to another host; an update
if phase B; then
url "https://cat.test:$RED/good"
start
[ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field id0)" = choralroot ]' && [ "$(field ver0)" = 0.40 ] \
    && ok "slot 1: ChoralRoot 0.40" || bad "slot 1 is '$(field id0)' $(field ver0)"
b0=$(field boot0); sleep 2.6   # (its flash saved)
view; press "cc 88"; press "note 93"; press "cc 3"
wait_for 15 '[ "$(field dl_ok)" = 1 ]' && [ -f "$ROOT/firmwares/$CR.rfm1m" ] && [ -f "$ROOT/firmwares/choralroot-0.40.rfm1m" ] \
    && ok "the update over HTTPS (cat.test redirecting each file to objects.test): $CR installed beside 0.40, which slot 1 still runs" \
    || bad "the update: ok $(field dl_ok), state $(field dl_state), err '$(field dl_err)', files '$(files choralroot)'"
[ "$(field ver0)" = 0.40 ] && [ "$(field boot0)" = "$b0" ] && ok "slot 1 kept playing 0.40 through the update (not restarted)" \
    || bad "slot 1: $(field ver0), boot $b0 -> $(field boot0)"
fws=$(curl -s "$U/api/firmwares" | tr -d '\n')
echo "$fws" | grep -q "\"id\":\"choralroot\",\"name\":\"ChoralRoot\",\"version\":\"${CR#choralroot-}\"" \
    && ok "/api/firmwares lists the new version" || bad "/api/firmwares: $(echo "$fws" | grep -o '"id":"choralroot"[^,]*,[^,]*,[^,]*')"
back
[ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field ver0)" = "${CR#choralroot-}" ]' \
    && ok "loaded again: slot 1 runs ChoralRoot $(field ver0)" || bad "the reload: slot 1 runs $(field ver0)"
grep -q "slot 1: ChoralRoot ${CR#choralroot-} (its saved flash)" "$LOG" && ok "with the flash 0.40 saved (states are keyed by id)" \
    || bad "the new version did not start from the saved flash"
wait_for 5 '[ ! -f "$ROOT/firmwares/choralroot-0.40.rfm1m" ] && [ ! -f "$ROOT/firmwares/choralroot-0.40.json" ]' \
    && ok "0.40's files removed once no slot runs it: $(files choralroot)" || bad "0.40 is still there: $(files choralroot)"
grep -q "https: the system's OpenSSL 3, CA certificates from $T/tls/cadir" "$LOG" && [ -f "$ROOT/catalog/index.json.sig" ] \
    && cmp -s "$ROOT/catalog/index.json" build/testcat/index.json \
    && ok "the index came over HTTPS too (the image's libssl.so.3, dlopen'd; the test CA) and was kept with its signature" \
    || bad "phase B's index: ok $(field index_ok), err '$(raw index_err)'"
stop
fi

# ---------------------------------------------------------------- phase F: refused over HTTPS: certificates, signatures
# each case: a new root (no index of its own), the base URL set, the index fetched again every second
# (RFM14_TEST_INDEX_MS, test build only); the case's error, nothing kept, the bundled index still the one used
if phase F; then
LOG=$T/host-f.log; ROOT=$T/root-f; mkdir -p "$ROOT"; url "http://127.0.0.1:1"
start RFM14_TEST_INDEX_MS=700 || { bad "phase F: the host did not start"; tail -20 "$LOG"; }
refused() {   # refused BASE WHAT GREP: the index from BASE refused with GREP in its error
    n=$(field index_failed); want=$3; url "$1"
    if wait_for 10 '[ "$(field index_failed)" -gt "$n" ] && [ "$(raw index_err)" != "" ]' \
        && wait_for 3 'raw index_err | grep -q "$want"' && [ ! -f "$ROOT/catalog/index.json" ] && [ ! -f "$ROOT/catalog/index.json.sig" ] \
        && [ -z "$(ls "$ROOT/catalog" 2>/dev/null)" ] && [ "$(field cat_from)" = 1 ] && [ "$(field cat_n)" = 3 ]; then
        ok "$2: refused, nothing kept, the bundled index still used ($(raw index_err | cut -c1-110))"
    else bad "$2: failed $(field index_failed), from $(field cat_from), err '$(raw index_err)', catalog/: $(ls "$ROOT/catalog" 2>/dev/null)"; fi
}
refused "https://wrong.test:$OBJ/good" "a host name the certificate does not name (wrong.test, objects.test's certificate)" "hostname mismatch"
refused "https://cat.test:$SELF/good" "an untrusted certificate (cat.test, self-signed)" "certificate is refused"
refused "https://cat.test:$RED/nosig" "an index with no signature (index.json.sig: 404)" "index.json.sig: HTTP 404"
refused "https://cat.test:$RED/badsig" "an index with another index's signature" "bad signature"
refused "https://cat.test:$RED/tampered" "an index changed after it was signed (one word)" "bad signature"
refused "https://cat.test:$RED/downgrade" "a redirect from https to plain http" "from https to plain http"
# a download from a host whose certificate is wrong: refused, nothing installed
url "https://wrong.test:$OBJ/good"
view; press "cc 88"; press "note 92"; press "cc 3"
wait_for 15 '[ "$(field dl_state)" = 3 ]' && raw dl_err | grep -q "hostname mismatch" && [ -z "$(files amb)" ] \
    && [ -z "$(ls "$ROOT/firmwares/.dl" 2>/dev/null)" ] \
    && ok "a firmware download from wrong.test: refused (the certificate's host name), nothing installed, nothing left" \
    || bad "the download from wrong.test: state $(field dl_state), err '$(raw dl_err)', files '$(files amb)'"
back
# then the good one: fetched, kept with its signature
n=$(field index_ok); url "https://cat.test:$RED/good"
wait_for 10 '[ "$(field index_ok)" -gt "$n" ]' && cmp -s "$ROOT/catalog/index.json" build/testcat/index.json \
    && cmp -s "$ROOT/catalog/index.json.sig" build/testcat/index.json.sig \
    && ok "then the signed index over HTTPS: kept, with its signature" || bad "the good index: ok $(field index_ok), err '$(raw index_err)'"
stop
# a kept index whose signature no longer checks (changed on disk): not used at the next start
sed -e 's/A generative ambient/A generative AMBIENT/' -e 's/"generated":"/"generated":"9/' build/testcat/index.json > "$ROOT/catalog/index.json"
start
[ "$(field cat_from)" = 1 ] && grep -q "catalog/index.json refused: a bad signature" "$LOG" \
    && ok "a kept index changed on disk (and dated later): refused at start, the bundled one used" \
    || bad "the changed kept index: from $(field cat_from)"
stop
fi

# ---------------------------------------------------------------- phase G: the bundled snapshot is checked the same way
if phase G; then
LOG=$T/host-g.log; ROOT=$T/root-g; mkdir -p "$ROOT" "$T/snap"; url "http://127.0.0.1:1"
cp build/testcat/index.json "$T/snap/index.json"
INDEX=$T/snap/index.json start
[ "$(field cat_from)" = 0 ] && [ "$(field cat_n)" = 2 ] \
    && ok "a bundled snapshot with no signature: not used (only the 2 installed firmwares listed)" \
    || bad "the unsigned snapshot: from $(field cat_from), $(field cat_n) listed"
stop
cp "$SRV/badsig/index.json" "$T/snap/index.json"; cp build/testcat/index.json.sig "$T/snap/index.json.sig"
INDEX=$T/snap/index.json start
[ "$(field cat_from)" = 0 ] && ok "a bundled snapshot whose signature is not its own: not used" \
    || bad "the badly signed snapshot: from $(field cat_from)"
stop
fi

# ---------------------------------------------------------------- phase H: no libssl on the system
if phase H; then
LOG=$T/host-h.log; ROOT=$T/root-h; mkdir -p "$ROOT"; url "https://cat.test:$RED/good"
start RFM14_TEST_LIBSSL=libssl-missing.so.3
wait_for 10 '[ "$(field index_failed)" -ge 1 ]' && raw index_err | grep -q "no TLS on this system" \
    && [ "$(field cat_from)" = 1 ] && [ "$(field id0)" = felucca ] && [ "$(field boot0)" -ge 1 ] \
    && ok "no libssl.so.3: the index fetch says so ($(raw index_err | cut -c1-80)..), the bundled index and Felucca as before" \
    || bad "no libssl: failed $(field index_failed), err '$(raw index_err)', from $(field cat_from), slot 1 '$(field id0)'"
view; press "cc 88"; press "note 92"; press "cc 3"
wait_for 10 '[ "$(field dl_state)" = 3 ]' && raw dl_err | grep -q "no TLS on this system" && [ -z "$(files amb)" ] \
    && ok "  .. and a download: refused with the same reason, nothing installed" || bad "no libssl, a download: $(field dl_state) '$(raw dl_err)'"
back
stop
fi

# ---------------------------------------------------------------- phase C: loads while another firmware plays
late_run() {   # late_run ENV: Felucca plays in slot 1 while slot 2 loads ChoralRoot, AMB-1, ChoralRoot; the late blocks
    rm -f "$ROOT/move/library.rmvl"
    start RFM14_TEST_BOOT_MS=40 "$1"
    cmd "note 84 110" "note 88 110" "note 91 110"; sleep 0.5
    for id in choralroot amb choralroot; do
        n=$(field boot1); post "{\"slot\":1,\"id\":\"$id\"}" >/dev/null
        wait_for 10 '[ "$(field boot1)" != "$n" ] && [ "$(field id1)" = $id ]' || echo "  info  $id did not load"
        sleep 0.4
    done
    cmd "note 84 0" "note 88 0" "note 91 0"; sleep 1.2
    LATE=$(field late); PEAK=$(field peak)
    stop
}
if phase C; then
late_run RFM14_NO_PREROLL=1; control=$LATE
late_run RFM14_NO_PREROLL=0
[ "$LATE" = 0 ] && [ "$control" -gt 0 ] \
    && ok "3 loads into slot 2 while Felucca plays in slot 1, each start held 40 ms (RFM14_TEST_BOOT_MS, a slow Move): 0 late blocks (without the ring filled first: $control)" \
    || bad "late blocks during loads: $LATE (without the preroll: $control)"
fi

# ---------------------------------------------------------------- phase D: the firmwares' log lines
# ChoralRoot prints three lines a key ("touch: key 7 vel 100 fixed", "chord: C (1 note)", "chord: -"): 30 keys in 6 s
# (pad 88: its key 7)
if phase D; then
LOG=$T/host-d.log; ROOT=$T/root-d; mkdir -p "$ROOT"; url "http://127.0.0.1:1"
XARGS="--fw choralroot" start
wait_for 10 '[ "$(field id0)" = choralroot ]'
sleep 1
i=0; while [ $i -lt 30 ]; do cmd "note 88 100"; sleep 0.12; cmd "note 88 0"; sleep 0.08; i=$((i + 1)); done
sleep 1.5
stop
said=$(cat "$LOG".1 "$LOG" 2>/dev/null | grep -c '^\[choralroot\] ')
dropped=$(cat "$LOG".1 "$LOG" 2>/dev/null | sed -n 's/^\[choralroot\] (\([0-9]*\) lines dropped.*/\1/p' | awk '{ s += $1 } END { print s + 0 }')
info "ChoralRoot's lines: $said in the log, $dropped dropped (30 keys in 6 s: 90 lines)"
[ "$dropped" -gt 20 ] && [ "$said" -le 55 ] && ok "its lines are rate-limited (at most 4 a second after a burst of 20; $dropped dropped, said so)" \
    || bad "the rate limit: $said lines, $dropped dropped"
[ -n "${DEBUG:-}" ] && cat "$LOG"
fi

# ---------------------------------------------------------------- phase E: the log's cap
if phase E; then
LOG=$T/host-e.log; ROOT=$T/root-e; mkdir -p "$ROOT"; url "http://127.0.0.1:1"
XARGS="--log-cap 600" start
sleep 4
stop
[ -f "$LOG.1" ] && grep -q "^remotefm1-host .*: root" "$LOG.1" && grep -q "the lines before are in $LOG.1" "$LOG" \
    && [ "$(stat -c %s "$LOG.1")" -lt 3000 ] \
    && ok "the log past its cap (600 bytes here, 1 MB on the Move) moved to .1, a new one in its place: $(stat -c %s "$LOG.1") + $(stat -c %s "$LOG") bytes" \
    || bad "the log's cap: $(ls -l "$LOG"* 2>/dev/null | awk '{print $5}' | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- phase R: a rebuild of the installed version
# The same <id>-<version> built again (another .rfm1: new glue, the same release): offered as an update, as the apps
# offer it; downloaded over the installed files; the slot running the old build loads the new one when asked (another
# file at the same path). The installed build: an rd1 ChoralRoot of the catalog's version among the development builds
# (make DEV_FIRMWARES=..: build/fw-dev), else skipped
sha_of() { sed -n 's/^ *"sha256": "\([0-9a-f]*\)".*/\1/p' "$1" | head -1; }
if phase R; then
REB=build/fw-dev/$CR
if [ -f "$REB.rfm1m" ] && [ "$(sha_of "$REB.json")" != "$(sha_of "build/testcat/$CR.json")" ]; then
    LOG=$T/host-r.log; ROOT=$T/root-r; mkdir -p "$ROOT/firmwares"; url "http://127.0.0.1:$GOOD"
    cp "$REB".* "$ROOT/firmwares/"
    old=$(sha_of "$REB.json"); new=$(sha_of "build/testcat/$CR.json")
    start
    [ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field id0)" = choralroot ]' && [ "$(field sha0)" = "$(echo "$old" | cut -c1-12)" ] \
        && ok "slot 1: the installed build of $CR ($(field sha0))" || bad "slot 1: '$(field id0)' $(field sha0)"
    u0=$(field cat_updates)
    [ "${u0:-0}" -ge 1 ] && ok "the same version built again is offered as an update ($u0 offered)" || bad "no update offered: $u0"
    b0=$(field boot0); sleep 2.6   # (its flash saved)
    view; press "cc 88"; press "note 93"; press "cc 3"
    wait_for 15 '[ "$(field dl_ok)" = 1 ]' && [ "$(sha_of "$ROOT/firmwares/$CR.json")" = "$new" ] && [ "$(files choralroot)" = "$CR.json $CR.licenses.txt $CR.rfm1m $CR.rfm1skin $CR.rfm1skin.bg $CR.rfm1skin.bgl " ] \
        && [ "$(field cat_updates)" = $((u0 - 1)) ] && [ "$(field sha0)" = "$(echo "$old" | cut -c1-12)" ] \
        && ok "downloaded over it ($CR, $(echo "$new" | cut -c1-12)); no longer an update; slot 1 still runs the old build" \
        || bad "the rebuild's download: ok $(field dl_ok), err '$(field dl_err)', sha $(sha_of "$ROOT/firmwares/$CR.json" | cut -c1-12), updates $(field cat_updates), slot $(field sha0), files '$(files choralroot)'"
    back
    [ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field boot0)" != "$b0" ] && [ "$(field sha0)" = "$(echo "$new" | cut -c1-12)" ]' \
        && ok "loaded again, slot 1 runs the new build ($(field sha0)): another file at the same path, not the old one dlopen had" \
        || bad "after the rebuild: slot 1 boot $(field boot0) (was $b0), sha $(field sha0), want $(echo "$new" | cut -c1-12)"
    stop
else
    info "phase R skipped: no other build of $CR in build/fw-dev"
fi
fi

kill $S1 $S2 $S3 $S4 $S5 2>/dev/null
echo "dl_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T/host.log"; }
mv "$T" "$T.done" 2>/dev/null
exit $fail
