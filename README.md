# Multithreaded HTTP/1.1 Server (C++17, raw POSIX sockets)

No web frameworks, no Boost.Asio: just `socket/bind/listen/poll/accept/recv/send`, `std::thread`, `std::mutex` and `std::condition_variable`.

## Build & run

POSIX sockets need Linux or macOS. **On Windows, use WSL** (`wsl --install`, then open this folder from the Ubuntu shell: `cd "/mnt/c/Projects/New folder/http-server"`).

```bash
sudo apt install g++ make curl     # once
make                               # builds ./server
./server --port 8080 --threads 4 --queue 64
curl -i localhost:8080/
make test                          # 17 end-to-end tests
make tsan && bash tests/run_tests.sh ./server_tsan   # same tests under ThreadSanitizer
```

| Route | Purpose |
|---|---|
| `GET /` | hello |
| `GET /health` | JSON health check |
| `GET /slow` | sleeps 2s (demo for 503 + graceful shutdown) |
| `POST /echo` | echoes the body (demo for Content-Length framing) |

## File map: resume bullet → code

| Bullet | Where |
|---|---|
| HTTP/1.1 on raw POSIX TCP sockets, GET routing, 200/404/400 | `main.cpp` (`create_listen_socket`, accept loop), `http.cpp` (`parse_head`, `Router::dispatch`) |
| Fixed thread pool + bounded producer-consumer queue, 503 under overload | `bounded_queue.hpp`, `thread_pool.hpp`, the `try_submit` branch in `main.cpp` |
| TCP stream framing (`\r\n\r\n` + `Content-Length`) | `read_request` in `http.cpp` |
| Graceful SIGINT shutdown | `on_signal` + the end of `main()` + `ThreadPool::shutdown` |

## Request lifecycle

```
main thread                         worker thread (×N)
-----------                         ------------------
poll(listen_fd, 200ms)              queue.pop()  ← blocks on condvar
accept() → client_fd                read_request(): recv() until \r\n\r\n,
set SO_RCVTIMEO=5s                    parse, recv() until Content-Length bytes
pool.try_submit(fd)                 router.dispatch() → handler
  ├─ ok   → queue.push, notify_one   send_all() (loops over partial send)
  └─ full → send 503, close          close(fd)
```

## Interview questions you should be able to answer

1. **Why is the push non-blocking but the pop blocking?** If the accept thread blocked when the queue was full, *all* new clients would hang in the kernel backlog with no feedback. Failing fast with 503 + `Retry-After` is backpressure the client can act on. Workers, on the other hand, have nothing to do without work, so sleeping on the condvar is right.
2. **Why `wait(lock, predicate)` instead of plain `wait(lock)`?** Spurious wakeups, and so a worker woken by `close()` re-checks state instead of popping from an empty deque.
3. **Why `notify_one` after unlocking?** The woken thread would otherwise immediately block on the mutex we still hold.
4. **Why can't you just `recv()` once?** TCP is a byte stream. One request can arrive in many segments, and one `recv` may also return bytes of the *next* request. `tests/run_tests.sh` sends `GET / HT` … `TP/1.1` with pauses to prove this.
5. **Why rescan the last 3 bytes for `\r\n\r\n`?** The terminator can be split across two reads (`...\r\n\r` | `\n`).
6. **What stops a slow client from hogging a worker (Slowloris)?** `SO_RCVTIMEO` (5s) → 408; header cap (8KB) → 431; body cap (1MB) → 413. A per-request *total* deadline would be the next improvement.
7. **What does the signal handler do, and why so little?** Only async-signal-safe operations are allowed in a handler. It sets a `volatile sig_atomic_t`. The main loop uses `poll()` with a 200ms timeout to notice it, because in a multithreaded process the signal can land on *any* thread, so you can't count on `accept()` in the main thread returning `EINTR`.
8. **Shutdown order?** (1) stop the accept loop, (2) `close(listen_fd)` so new connections are refused, (3) `queue.close()` wakes all workers, (4) workers finish their current request *and* drain anything already queued, `pop()` returns `nullopt`, the loop exits, (5) `join()` each thread.
9. **Why `MSG_NOSIGNAL` / `SIGPIPE` ignored?** Writing to a socket the peer already closed raises SIGPIPE, which kills the process by default.
10. **Why reject conflicting duplicate `Content-Length`?** That's a request-smuggling vector when a proxy and a server disagree on where a request ends.
11. **What's the "lingering close" in `main.cpp`?** If we send an error before reading the client's whole request, `close()` with unread data makes Linux send an RST, and the client can lose our response. We `shutdown(SHUT_WR)` and briefly drain first. (This was a real bug the 431 test caught.)
12. **Why `Connection: close`?** It keeps one request per connection, so there's no keep-alive state machine or pipelining. Easy to defend, easy to extend.

## Known limits / next steps
- No keep-alive or pipelining (one request per connection).
- No chunked transfer encoding (replies 501).
- Thread-per-request blocking I/O. The scalable alternative is `epoll` + non-blocking sockets (event loop), which is the natural v2.
- No static files, HTTPS, or IPv6.
