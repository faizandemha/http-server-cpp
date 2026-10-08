#pragma once
// HTTP/1.1 message types, TCP stream framing, and a tiny router.

#include <functional>
#include <map>
#include <string>

struct HttpRequest {
    std::string method;   // "GET"
    std::string target;   // "/path?query" as sent
    std::string path;     // "/path" (query stripped)
    std::string version;  // "HTTP/1.1"
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::map<std::string, std::string> extra_headers;
    std::string body;

    std::string serialize() const;
};

HttpResponse make_error(int status);
const char* reason_phrase(int status);

// Limits that protect the server from abusive clients.
constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

// Result of reading one request off a socket.
//   kReadOk      -> `out` is filled in
//   kReadClosed  -> client went away / timed out before sending anything; just close
//   anything else-> an HTTP status code to send back (400, 413, 431, 501...)
constexpr int kReadOk = 0;
constexpr int kReadClosed = -1;

// Reads from `fd` until the full header block ("\r\n\r\n") and the full
// Content-Length body have arrived, however the bytes were split across recv() calls.
int read_request(int fd, HttpRequest& out);

// Parses the request line + headers (everything before "\r\n\r\n").
// Exposed separately so it can be unit-tested without a socket.
bool parse_head(const std::string& head, HttpRequest& out);

// send() may write fewer bytes than asked; loop until everything is out.
bool send_all(int fd, const std::string& data);

class Router {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void add(const std::string& method, const std::string& path, Handler h);
    HttpResponse dispatch(const HttpRequest& req) const;

private:
    // path -> (method -> handler)
    std::map<std::string, std::map<std::string, Handler>> routes_;
};
