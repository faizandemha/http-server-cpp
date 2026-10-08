// Multithreaded HTTP/1.1 server on raw POSIX sockets.
//
//   main thread : poll() + accept() -> hand socket to the pool (or 503 if full)
//   N workers   : read request -> route -> write response -> close
//   SIGINT      : stop accepting, drain in-flight + queued requests, join workers

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "http.hpp"
#include "thread_pool.hpp"

namespace {

// ---- graceful shutdown flag ------------------------------------------------
// A signal handler may only touch async-signal-safe things.
// Writing a volatile sig_atomic_t is the textbook safe option.
volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_signal(int) { g_stop = 1; }

// ---- thread-safe logging ---------------------------------------------------
std::mutex g_log_mu;
template <typename... Args>
void log(Args&&... args) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    std::cerr << "[" << std::this_thread::get_id() << "] ";
    (std::cerr << ... << args) << '\n';
}

struct Config {
    int port = 8080;
    std::size_t threads = 4;
    std::size_t queue = 64;
    int recv_timeout_sec = 5;
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        long v = std::strtol(argv[i + 1], nullptr, 10);
        if (k == "--port") c.port = static_cast<int>(v);
        else if (k == "--threads") c.threads = static_cast<std::size_t>(v);
        else if (k == "--queue") c.queue = static_cast<std::size_t>(v);
        else if (k == "--timeout") c.recv_timeout_sec = static_cast<int>(v);
        else { std::cerr << "unknown flag " << k << '\n'; std::exit(2); }
    }
    if (c.threads == 0 || c.queue == 0) { std::cerr << "threads/queue must be > 0\n"; std::exit(2); }
    return c;
}

int create_listen_socket(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); std::exit(1); }

    // Lets us restart immediately instead of waiting out TIME_WAIT.
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { std::perror("bind"); std::exit(1); }
    if (::listen(fd, SOMAXCONN) < 0) { std::perror("listen"); std::exit(1); }
    return fd;
}

// If we reply with an error before reading everything the client sent
// (e.g. 431 for huge headers), close() with unread bytes in the kernel buffer
// makes Linux send a TCP RST, and the client may never see our response.
// Fix: half-close our side (FIN), then briefly drain whatever is left.
void lingering_close_drain(int fd) {
    ::shutdown(fd, SHUT_WR);
    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char sink[4096];
    std::size_t drained = 0;
    while (drained < 64 * 1024) {  // bounded so a malicious client can't hold us
        ssize_t n = ::recv(fd, sink, sizeof(sink), 0);
        if (n <= 0) break;
        drained += static_cast<std::size_t>(n);
    }
}

Router build_router() {
    Router r;
    r.add("GET", "/", [](const HttpRequest&) {
        HttpResponse res;
        res.body = "Hello from a hand-rolled HTTP/1.1 server\n";
        return res;
    });
    r.add("GET", "/health", [](const HttpRequest&) {
        HttpResponse res;
        res.content_type = "application/json";
        res.body = "{\"status\":\"ok\"}\n";
        return res;
    });
    // Deliberately slow: useful for demonstrating 503 under load and graceful shutdown.
    r.add("GET", "/slow", [](const HttpRequest&) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        HttpResponse res;
        res.body = "finally done\n";
        return res;
    });
    // Echoes the request body: proves Content-Length framing works.
    r.add("POST", "/echo", [](const HttpRequest& req) {
        HttpResponse res;
        res.content_type = "application/octet-stream";
        res.body = req.body;
        return res;
    });
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    // Install SIGINT/SIGTERM handlers. No SA_RESTART, so blocking calls return EINTR.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    // Writing to a closed socket must not kill the process.
    std::signal(SIGPIPE, SIG_IGN);

    const Router router = build_router();
    int listen_fd = create_listen_socket(cfg.port);

    // Each worker runs this for one connection.
    auto handle_connection = [&router](int client_fd) {
        HttpRequest req;
        int rc = read_request(client_fd, req);
        if (rc != kReadClosed) {
            HttpResponse res = (rc == kReadOk) ? router.dispatch(req) : make_error(rc);
            send_all(client_fd, res.serialize());
            log(rc == kReadOk ? req.method + " " + req.target : std::string("<malformed>"),
                " -> ", res.status);
            if (rc != kReadOk) lingering_close_drain(client_fd);
        }
        ::close(client_fd);
    };

    ThreadPool pool(cfg.threads, cfg.queue, handle_connection);
    log("listening on :", cfg.port, " (threads=", cfg.threads, ", queue=", cfg.queue, ")");

    // ---- accept loop (the producer) ----
    // poll() with a timeout so we notice g_stop even if no client ever connects.
    // (The signal may be delivered to a worker thread, so we can't rely on
    //  accept() in this thread getting EINTR.)
    while (!g_stop) {
        pollfd pfd{listen_fd, POLLIN, 0};
        int ready = ::poll(&pfd, 1, 200 /*ms*/);
        if (ready <= 0) continue;  // timeout or EINTR -> re-check g_stop

        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // A slow/idle client must not pin a worker forever.
        timeval tv{cfg.recv_timeout_sec, 0};
        ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (!pool.try_submit(client_fd)) {
            // Backpressure: reject fast instead of queueing unboundedly.
            HttpResponse busy = make_error(503);
            busy.extra_headers["Retry-After"] = "1";
            send_all(client_fd, busy.serialize());
            ::close(client_fd);
            log("pool saturated -> 503");
        }
    }

    // ---- graceful shutdown ----
    log("shutdown requested: no longer accepting connections");
    ::close(listen_fd);   // 1. stop accepting
    pool.shutdown();      // 2. drain in-flight + queued requests, 3. join workers
    log("all workers joined, exiting cleanly");
    return 0;
}
