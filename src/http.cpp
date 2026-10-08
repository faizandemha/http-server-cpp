#include "http.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <sstream>

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string HttpResponse::serialize() const {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       // One request per connection keeps the design simple and correct.
       << "Connection: close\r\n";
    for (const auto& [k, v] : extra_headers) os << k << ": " << v << "\r\n";
    os << "\r\n" << body;
    return os.str();
}

HttpResponse make_error(int status) {
    HttpResponse r;
    r.status = status;
    r.body = std::to_string(status) + " " + reason_phrase(status) + "\n";
    return r;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const char* ws = " \t";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// RFC 9110 "tchar": the characters allowed in a method or header name.
bool is_token(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) continue;
        if (std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) == std::string::npos)
            return false;
    }
    return true;
}

// Strict decimal parse: rejects "", "-1", "12abc", "1e3", and overflow.
bool parse_size(const std::string& s, std::size_t& out) {
    if (s.empty() || s.size() > 18) return false;
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

bool parse_head(const std::string& head, HttpRequest& out) {
    // ---- request line: METHOD SP TARGET SP VERSION ----
    auto line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end);

    auto sp1 = request_line.find(' ');
    if (sp1 == std::string::npos) return false;
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    if (request_line.find(' ', sp2 + 1) != std::string::npos) return false;  // exactly 3 parts

    out.method = request_line.substr(0, sp1);
    out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = request_line.substr(sp2 + 1);

    if (!is_token(out.method)) return false;
    if (out.target.empty() || out.target[0] != '/') return false;
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") return false;

    out.path = out.target.substr(0, out.target.find('?'));

    // ---- header fields: "Name: value" per line ----
    std::size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
        auto next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        std::string line = head.substr(pos, next - pos);
        pos = next + 2;

        auto colon = line.find(':');
        if (colon == std::string::npos) return false;
        std::string name = line.substr(0, colon);
        if (!is_token(name)) return false;  // also rejects "Name :" (space before colon)

        name = to_lower(name);
        std::string value = trim(line.substr(colon + 1));

        // Conflicting duplicate Content-Length is a request-smuggling vector -> reject.
        auto it = out.headers.find(name);
        if (it != out.headers.end()) {
            if (name == "content-length" && it->second != value) return false;
            it->second += ", " + value;
        } else {
            out.headers.emplace(std::move(name), std::move(value));
        }
    }

    // HTTP/1.1 requires a Host header (RFC 9112 §3.2).
    if (out.version == "HTTP/1.1" && out.headers.count("host") == 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// TCP stream framing
// ---------------------------------------------------------------------------
// TCP is a byte stream, not a message stream: one recv() may return half a
// request line, or headers + part of the body, or (with pipelining) more than
// one request. So we buffer until we can *prove* we have a complete message.

namespace {

// Returns bytes read (>0), 0 on orderly close, -1 on error/timeout.
ssize_t recv_some(int fd, std::string& buf) {
    char chunk[4096];
    while (true) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) buf.append(chunk, static_cast<std::size_t>(n));
        if (n < 0 && errno == EINTR) continue;  // interrupted by a signal: retry
        return n;  // EAGAIN/EWOULDBLOCK here means SO_RCVTIMEO expired
    }
}

}  // namespace

int read_request(int fd, HttpRequest& out) {
    std::string buf;
    std::size_t header_end;
    std::size_t search_from = 0;

    // Phase 1: read until the blank line that ends the header block.
    while ((header_end = buf.find("\r\n\r\n", search_from)) == std::string::npos) {
        if (buf.size() > kMaxHeaderBytes) return 431;
        // The terminator can straddle two reads, so re-scan the last 3 bytes.
        search_from = buf.size() >= 3 ? buf.size() - 3 : 0;

        ssize_t n = recv_some(fd, buf);
        if (n == 0) return buf.empty() ? kReadClosed : 400;  // closed mid-headers
        if (n < 0) return buf.empty() ? kReadClosed : 408;   // timeout / error
    }
    if (header_end > kMaxHeaderBytes) return 431;

    if (!parse_head(buf.substr(0, header_end), out)) return 400;

    // We don't implement chunked bodies; refuse rather than mis-frame.
    if (out.headers.count("transfer-encoding")) return 501;

    // Phase 2: read exactly Content-Length body bytes.
    std::size_t content_length = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) {
        if (!parse_size(it->second, content_length)) return 400;
        if (content_length > kMaxBodyBytes) return 413;
    }

    const std::size_t body_start = header_end + 4;
    while (buf.size() - body_start < content_length) {
        ssize_t n = recv_some(fd, buf);
        if (n == 0) return 400;  // client promised more bytes than it sent
        if (n < 0) return 408;
    }

    // Any bytes beyond Content-Length belong to a next (pipelined) request;
    // since we use Connection: close, they are ignored.
    out.body = buf.substr(body_start, content_length);
    return kReadOk;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: if the client vanished, get EPIPE instead of a process-killing SIGPIPE.
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

void Router::add(const std::string& method, const std::string& path, Handler h) {
    routes_[path][method] = std::move(h);
}

HttpResponse Router::dispatch(const HttpRequest& req) const {
    auto by_path = routes_.find(req.path);
    if (by_path == routes_.end()) return make_error(404);

    auto by_method = by_path->second.find(req.method);
    if (by_method == by_path->second.end()) {
        // Path exists but not for this method -> 405 + the methods that ARE allowed.
        HttpResponse r = make_error(405);
        std::string allow;
        for (const auto& [m, _] : by_path->second) allow += (allow.empty() ? "" : ", ") + m;
        r.extra_headers["Allow"] = allow;
        return r;
    }

    try {
        return by_method->second(req);
    } catch (...) {
        return make_error(500);  // a buggy handler must not kill the worker thread
    }
}
