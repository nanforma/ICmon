#!/bin/bash
# End-to-end check of the real xmarchive binary against the TEST-ONLY mock camera.
# Proves the CLI, library and demuxer work together on synthetic data. It does NOT test a real camera.
set -u
XMARCHIVE=$1
MOCK=$2
DIR=$(mktemp -d)
trap 'kill $FEED 2>/dev/null; rm -rf "$DIR"' EXIT

mkfifo "$DIR/in"
"$MOCK" < "$DIR/in" > "$DIR/port" &
exec 3> "$DIR/in"            # keep the mock's stdin open until we exit
FEED=$!
for _ in $(seq 50); do grep -q PORT "$DIR/port" 2>/dev/null && break; sleep 0.1; done
PORT=$(awk '/PORT/{print $2}' "$DIR/port")
[ -n "$PORT" ] || { echo "FAIL: mock did not start"; exit 1; }
export CAM_PASS=secret
fail=0
check() { if [ "$2" = ok ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi; }

OUT=$("$XMARCHIVE" list --host 127.0.0.1 --port "$PORT" --user shield --date 2026-10-31 2>&1)
echo "$OUT" | head -3
[ "$(echo "$OUT" | head -1 | cut -d' ' -f1)" = 150 ] && check "list returns all 150 files over several pages" ok || check "list returns all 150 files" bad

(cd "$DIR" && "$XMARCHIVE" fetch --host 127.0.0.1 --port "$PORT" --user shield --start "2026-10-31 00:52:30" --secs 20 --out clip) > "$DIR/fetch.log" 2>&1
cat "$DIR/fetch.log"
[ -s "$DIR/clip.h265" ] && check "fetch wrote a non-empty .h265" ok || check "fetch wrote a non-empty .h265" bad
head -c 6 "$DIR/clip.h265" | od -An -tx1 | tr -d ' \n' | grep -q '^000000014001' && check "video starts with an Annex-B VPS (first I-frame)" ok || check "video starts with Annex-B VPS" bad
grep -q "dropped before the first I-frame of each file: 4" "$DIR/fetch.log" && check "4 frames before the first I-frame were dropped" ok || check "dropped-frame count" bad

(cd "$DIR" && "$XMARCHIVE" fetch --host 127.0.0.1 --port "$PORT" --user shield --start "2026-10-31 00:54:55" --secs 20 --out cross) > "$DIR/cross.log" 2>&1
cat "$DIR/cross.log"
grep -q "2 file(s) used" "$DIR/cross.log" && check "a window across two files uses both files" ok || check "cross-file window" bad
grep -q "WARNING" "$DIR/cross.log" && check "no resync warnings" bad || check "no resync warnings" ok

CAM_PASS=wrong "$XMARCHIVE" list --host 127.0.0.1 --port "$PORT" --user shield --date 2026-10-31 > "$DIR/bad.log" 2>&1
grep -q "Ret=203" "$DIR/bad.log" && check "wrong password reports Ret=203" ok || check "wrong password reports Ret=203" bad

exit $fail
