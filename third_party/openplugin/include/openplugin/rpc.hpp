#pragma once

// JSON-RPC 2.0 over a plugin process's stdin/stdout, one message per line (see docs/design.md,
// section 8.1).
//
// Threading: background threads do all the pipe I/O, so nothing here ever blocks the UI. Every
// callback (results of our requests, the plugin's requests and notifications) runs on the thread
// that calls poll(). The GUI calls poll() once per frame; the command line calls it in a loop. App
// data is therefore only ever touched from that one thread.
//
// The plugin is untrusted. Anything that isn't a well-formed message (bad JSON, a line over the
// size limit, a response with no id, ...) closes the channel and kills the process, with a reason
// that can be shown to the user.

#include "openplugin/json.hpp"
#include "openplugin/process.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace opl::rpc {

// Standard JSON-RPC codes, plus the ones the plugin protocol adds.
enum ErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    PermissionDenied = -32001,  // the user didn't grant what this call needs
    Rejected = -32002,          // the user (or validation) turned a proposal down
    PluginError = -32003,       // the plugin's own code raised an error (message from the plugin)
    FilesChanged = -32004,      // the plugin's files don't match what the user approved
    RequestCancelled = -32800,
};

// Thrown by a request handler to send an error response.
class RemoteError : public opl::Error {
public:
    RemoteError(int code, const std::string& message) : opl::Error(message), code_(code) {}
    int code() const { return code_; }

private:
    int code_;
};

struct Outcome {
    enum class Status { Ok, Error, Timeout, Cancelled, Closed };
    Status status = Status::Ok;
    json::Value result;   // Ok
    int code = 0;         // Error
    std::string message;  // Error, Timeout, Closed: why

    bool ok() const { return status == Status::Ok; }
};

struct ChannelLimits {
    std::size_t maxMessageBytes = 16u << 20;  // per line, either direction
    int maxDepth = 64;
    std::size_t maxLogBytes = 1u << 20;       // stderr kept for "Show log" (the newest part)
};

class Channel;

// Answers one request from the plugin, now or later. Copyable; the first answer wins, and answering
// after the channel closed does nothing.
class Responder {
public:
    void result(json::Value value) const;
    void error(int code, const std::string& message) const;
    bool answered() const;
    std::int64_t id() const { return id_; }

    struct State;  // defined in rpc.cpp; only a Channel can make one
    Responder(std::shared_ptr<State> state, std::int64_t id);

private:
    std::shared_ptr<State> state_;
    std::int64_t id_ = 0;
};

class Channel {
public:
    using ResultCallback = std::function<void(const Outcome&)>;
    // A request from the plugin. Answer through `respond` (now or later). If the handler throws
    // RemoteError (or any opl::Error) before answering, that becomes the error response.
    using RequestHandler = std::function<void(const std::string& method, const json::Value& params, Responder respond)>;
    using NotificationHandler = std::function<void(const std::string& method, const json::Value& params)>;

    explicit Channel(std::unique_ptr<Process> process, ChannelLimits limits = {});
    ~Channel();  // kills the process if it is still running
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Without a handler, requests get MethodNotFound and notifications are dropped.
    void onRequest(RequestHandler handler);
    void onNotification(NotificationHandler handler);

    // Sends a request. `done` runs from poll() with the result, an error, a timeout (after which
    // $/cancelRequest is sent), or Closed. Returns the request id.
    std::int64_t request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                         ResultCallback done);
    void notify(const std::string& method, json::Value params);
    // Stops waiting: `done` gets Cancelled from the next poll(), and the plugin gets $/cancelRequest.
    void cancel(std::int64_t id);
    // Pushes a pending request's deadline back (when the plugin reports progress).
    void extend(std::int64_t id, std::chrono::milliseconds timeout);

    // Delivers whatever has arrived, waiting up to `wait` for something if nothing has. Returns true
    // if any callback ran.
    bool poll(std::chrono::milliseconds wait = std::chrono::milliseconds(0));

    // Kills the plugin. Pending requests complete with Closed on the next poll().
    void close(const std::string& reason = "stopped by the app");
    bool closed() const;
    std::string closeReason() const;
    std::size_t pendingRequests() const;

    // Everything the plugin wrote to stderr (the newest maxLogBytes).
    std::string log() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace opl::rpc
