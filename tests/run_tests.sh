#!/usr/bin/env bash
# End-to-end tests: every claim on the resume bullet gets a test.
# Usage: bash tests/run_tests.sh ./server
set -u
BIN=${1:-./server}
PORT=18080
PASS=0; FAIL=0
: > /tmp/server.log

check() {  # check "name" expected actual
    if [[ "$3" == *"$2"* ]]; then echo "  PASS  $1"; PASS=$((PASS+1))
    else echo "  FAIL  $1  (expected '$2', got '$3')"; FAIL=$((FAIL+1)); fi
}

start() { "$BIN" --port $PORT "$@" 2>>/tmp/server.log & SERVER=$!; sleep 0.3; }
stop()  { kill -INT $SERVER 2>/dev/null; wait $SERVER 2>/dev/null; }

# Send raw bytes with optional pauses between chunks, print first response line.
# usage: raw "chunk1" [delay "chunk2" ...]
raw() {
    exec 3<>/dev/tcp/127.0.0.1/$PORT
    printf '%b' "$1" >&3; shift
    while [ $# -ge 2 ]; do sleep "$1"; printf '%b' "$2" >&3; shift 2; done
    head -n1 <&3 | tr -d '\r'
    exec 3>&-
}

echo "== routing & status codes =="
start
check "GET /        -> 200" "200" "$(curl -s -o /dev/null -w '%{http_code}' localhost:$PORT/)"
check "GET /health  -> 200" "200" "$(curl -s -o /dev/null -w '%{http_code}' localhost:$PORT/health)"
check "GET /missing -> 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' localhost:$PORT/missing)"
check "DELETE /     -> 405" "405" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE localhost:$PORT/)"
check "garbage request line -> 400" "400" "$(raw 'HELLO\r\n\r\n')"
check "missing Host header  -> 400" "400" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "bad HTTP version     -> 400" "400" "$(raw 'GET / HTTP/9.9\r\nHost: x\r\n\r\n')"
check "bad Content-Length   -> 400" "400" "$(raw 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 12abc\r\n\r\n')"

echo "== TCP stream framing =="
check "request line split mid-token across writes" "200" \
    "$(raw 'GET / HT' 0.3 'TP/1.1\r\nHo' 0.3 'st: x\r\n\r' 0.3 '\n')"
BODY=$(exec 3<>/dev/tcp/127.0.0.1/$PORT
       printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nhello' >&3
       sleep 0.3; printf ' world' >&3
       cat <&3 | tail -c 11; exec 3>&-)
check "body split across writes reassembled via Content-Length" "hello world" "$BODY"
BIG=$(head -c 20000 /dev/zero | tr '\0' 'a')
check "header block > 8KB -> 431" "431" "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX-Big: $BIG\r\n\r\n")"
stop

echo "== backpressure: 1 worker + queue of 1, 6 concurrent slow requests =="
start --threads 1 --queue 1
for i in $(seq 6); do curl -s -o /dev/null -w '%{http_code}\n' localhost:$PORT/slow > /tmp/r$i & done
wait $(jobs -p | grep -v "^$SERVER$") 2>/dev/null
CODES=$(cat /tmp/r{1..6} | sort | uniq -c | tr '\n' ' ')
echo "        status counts: $CODES"
check "some requests served (200)" "200" "$CODES"
check "excess requests rejected (503)" "503" "$CODES"
stop

echo "== graceful SIGINT shutdown =="
start --threads 2
curl -s -o /tmp/inflight -w '%{http_code}' localhost:$PORT/slow > /tmp/inflight_code &
CURL=$!
sleep 0.5
kill -INT $SERVER
wait $CURL
wait $SERVER; EXIT=$?
check "in-flight /slow request still completed" "200" "$(cat /tmp/inflight_code)"
check "server exited with status 0" "0" "$EXIT"
check "new connections refused after shutdown" "000" "$(curl -s -o /dev/null -w '%{http_code}' localhost:$PORT/ 2>/dev/null)"
check "log shows workers joined" "all workers joined" "$(cat /tmp/server.log)"

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
