#include "openplugin/rpc.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace opl::rpc {

namespace {

using Clock = std::chrono::steady_clock;

struct Incoming {
    enum class Kind { Request, Notification, Response };
    Kind kind = Kind::Notification;
    std::int64_t id = 0;
    std::string method;
    json::Value params;
    // Response
    bool isError = false;
    json::Value result;
    int code = 0;
    std::string message;
};

// Checks one message's shape. Throws json::Error describing what's wrong.
Incoming toIncoming(const json::Value& v) {
    if (!v.isObject()) throw json::Error("a message must be a JSON object");
    const json::Value* version = v.find("jsonrpc");
    if (!version || !version->isString() || version->asString() != "2.0")
        throw json::Error("a message must have \"jsonrpc\": \"2.0\"");
    Incoming in;
    const json::Value* id = v.find("id");
    if (const json::Value* method = v.find("method")) {
        if (!method->isString() || method->asString().empty()) throw json::Error("\"method\" must be a non-empty string");
        in.method = method->asString();
        if (const json::Value* params = v.find("params")) {
            if (!params->isObject() && !params->isArray()) throw json::Error("\"params\" must be an object or array");
            in.params = *params;
        }
        if (id) {
            if (!id->isInteger()) throw json::Error("request ids must be integers");
            in.kind = Incoming::Kind::Request;
            in.id = id->asInt();
        } else {
            in.kind = Incoming::Kind::Notification;
        }
        return in;
    }
    in.kind = Incoming::Kind::Response;
    const json::Value* result = v.find("result");
    const json::Value* error = v.find("error");
    if ((result != nullptr) == (error != nullptr)) throw json::Error("a response needs exactly one of \"result\" and \"error\"");
    if (error) {
        const json::Value* code = error->find("code");
        const json::Value* message = error->find("message");
        if (!code || !code->isInteger() || !message || !message->isString())
            throw json::Error("\"error\" needs an integer \"code\" and a string \"message\"");
        in.isError = true;
        in.code = static_cast<int>(code->asInt());
        in.message = message->asString();
    } else {
        in.result = *result;
    }
    if (!id || id->isNull()) {
        // Only legal for an error the plugin couldn't tie to a request (it couldn't parse ours).
        if (!error) throw json::Error("a response must have an id");
        in.id = -1;
    } else if (!id->isInteger()) {
        throw json::Error("response ids must be integers");
    } else {
        in.id = id->asInt();
    }
    return in;
}

std::string seconds(std::chrono::milliseconds ms) {
    const auto s = (ms.count() + 999) / 1000;
    return std::to_string(s) + (s == 1 ? " second" : " seconds");
}

}  // namespace

// ------------------------------------------------------------------- Channel::Impl

struct Channel::Impl {
    struct Pending {
        Clock::time_point deadline;
        std::chrono::milliseconds timeout;
        ResultCallback done;
    };

    std::unique_ptr<Process> process;
    ChannelLimits limits;

    mutable std::mutex m;
    std::condition_variable inboxCv;
    std::condition_variable outboxCv;
    std::deque<Incoming> inbox;
    std::deque<std::string> outbox;
    std::map<std::int64_t, Pending> pending;
    std::vector<std::pair<ResultCallback, Outcome>> ready;  // completed outside a response (cancel)
    std::int64_t nextId = 1;
    bool closed = false;
    bool closedByHost = false;
    bool readerDone = false;
    bool stopping = false;
    std::string reason;
    std::string log;
    RequestHandler requestHandler;
    NotificationHandler notificationHandler;

    std::thread reader, errReader, writer;

    // Requires m.
    void closeLocked(const std::string& why, bool byHost) {
        if (closed) return;
        closed = true;
        closedByHost = byHost;
        reason = why;
        outbox.clear();
        process->kill();
        inboxCv.notify_all();
        outboxCv.notify_all();
    }

    // Requires m.
    void sendLocked(const json::Value& message) {
        if (closed) return;
        outbox.push_back(json::write(message) + "\n");
        outboxCv.notify_one();
    }

    void send(const json::Value& message) {
        std::lock_guard<std::mutex> lock(m);
        sendLocked(message);
    }

    void appendLog(std::string_view text) {
        std::lock_guard<std::mutex> lock(m);
        log.append(text.data(), text.size());
        if (log.size() > limits.maxLogBytes) log.erase(0, log.size() - limits.maxLogBytes);
    }

    void writerLoop() {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(m);
                outboxCv.wait(lock, [&] { return stopping || closed || !outbox.empty(); });
                if (stopping || closed) return;
                line = std::move(outbox.front());
                outbox.pop_front();
            }
            if (!process->write(line)) {
                std::lock_guard<std::mutex> lock(m);
                closeLocked("the plugin stopped reading its input", false);
                return;
            }
        }
    }

    void readerLoop() {
        std::string buf;
        std::size_t scanned = 0;  // bytes of buf already known to hold no newline
        std::vector<char> chunk(64 * 1024);
        std::string violation;
        for (;;) {
            const std::size_t n = process->readStdout(chunk.data(), chunk.size());
            if (n == 0) break;
            buf.append(chunk.data(), n);
            std::size_t start = 0;
            for (;;) {
                const std::size_t nl = buf.find('\n', std::max(start, scanned));
                if (nl == std::string::npos) break;
                std::string_view line(buf.data() + start, nl - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                start = nl + 1;
                if (line.empty()) continue;
                if (line.size() > limits.maxMessageBytes) {
                    violation = "the plugin sent a message larger than the " + std::to_string(limits.maxMessageBytes >> 20) +
                                " MiB limit";
                    break;
                }
                try {
                    json::Limits jl;
                    jl.maxBytes = limits.maxMessageBytes;
                    jl.maxDepth = limits.maxDepth;
                    Incoming in = toIncoming(json::parse(line, jl));
                    std::lock_guard<std::mutex> lock(m);
                    if (in.kind == Incoming::Kind::Response && in.id == -1) {
                        log += "[openplugin] the plugin reported an error: " + in.message + "\n";
                        continue;
                    }
                    inbox.push_back(std::move(in));
                    inboxCv.notify_all();
                } catch (const json::Error& e) {
                    violation = std::string("the plugin sent an invalid message (") + e.what() + ")";
                    break;
                }
            }
            if (!violation.empty()) break;
            buf.erase(0, start);
            scanned = buf.size();
            if (buf.size() > limits.maxMessageBytes) {
                violation = "the plugin sent a message larger than the " + std::to_string(limits.maxMessageBytes >> 20) +
                            " MiB limit";
                break;
            }
        }

        std::string why = violation;
        if (why.empty()) {
            // Output ended: give the process a moment to finish exiting, for a useful message.
            std::optional<int> code;
            for (int i = 0; i < 100 && !(code = process->exitCode()); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            why = code ? "the plugin exited (code " + std::to_string(*code) + ")" : "the plugin closed its output";
        }
        std::lock_guard<std::mutex> lock(m);
        closeLocked(why, false);
        readerDone = true;
        inboxCv.notify_all();
    }

    void errReaderLoop() {
        std::vector<char> chunk(16 * 1024);
        for (;;) {
            const std::size_t n = process->readStderr(chunk.data(), chunk.size());
            if (n == 0) return;
            appendLog(std::string_view(chunk.data(), n));
        }
    }
};

// ------------------------------------------------------------------- Responder

struct Responder::State {
    std::weak_ptr<Channel::Impl> channel;
    std::atomic<bool> answered{false};
};

Responder::Responder(std::shared_ptr<State> state, std::int64_t id) : state_(std::move(state)), id_(id) {}

bool Responder::answered() const { return state_->answered.load(); }

void Responder::result(json::Value value) const {
    if (state_->answered.exchange(true)) return;
    auto channel = state_->channel.lock();
    if (!channel) return;
    json::Value message = json::Object{{"jsonrpc", "2.0"}, {"id", id_}, {"result", std::move(value)}};
    try {
        channel->send(message);
    } catch (const json::Error& e) {
        // The app produced something unwritable (e.g. invalid UTF-8 from a file name).
        channel->send(json::Object{{"jsonrpc", "2.0"},
                                   {"id", id_},
                                   {"error", json::Object{{"code", static_cast<int>(InternalError)}, {"message", e.what()}}}});
    }
}

void Responder::error(int code, const std::string& message) const {
    if (state_->answered.exchange(true)) return;
    auto channel = state_->channel.lock();
    if (!channel) return;
    std::string text = json::isValidUtf8(message) ? message : "error";
    channel->send(json::Object{
        {"jsonrpc", "2.0"}, {"id", id_}, {"error", json::Object{{"code", code}, {"message", std::move(text)}}}});
}

// --------------------------------------------------------------------- Channel

Channel::Channel(std::unique_ptr<Process> process, ChannelLimits limits) : impl_(std::make_shared<Impl>()) {
    impl_->process = std::move(process);
    impl_->limits = limits;
    Impl* impl = impl_.get();
    impl_->reader = std::thread([impl] { impl->readerLoop(); });
    impl_->errReader = std::thread([impl] { impl->errReaderLoop(); });
    impl_->writer = std::thread([impl] { impl->writerLoop(); });
}

Channel::~Channel() {
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->closeLocked("stopped by the app", true);
        impl_->stopping = true;
        impl_->outboxCv.notify_all();
    }
    impl_->writer.join();
    impl_->reader.join();
    impl_->errReader.join();
}

void Channel::onRequest(RequestHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->requestHandler = std::move(handler);
}

void Channel::onNotification(NotificationHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->notificationHandler = std::move(handler);
}

std::int64_t Channel::request(const std::string& method, json::Value params, std::chrono::milliseconds timeout,
                              ResultCallback done) {
    json::Object message{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isNull()) message.set("params", std::move(params));
    std::lock_guard<std::mutex> lock(impl_->m);
    const std::int64_t id = impl_->nextId++;
    message.set("id", id);
    if (impl_->closed) {
        // Report it through poll() like any other outcome, so callers have one code path.
        Outcome o;
        o.status = Outcome::Status::Closed;
        o.message = impl_->reason;
        impl_->ready.emplace_back(std::move(done), std::move(o));
        impl_->inboxCv.notify_all();
        return id;
    }
    impl_->sendLocked(json::Value(std::move(message)));  // may throw json::Error before anything is recorded
    const auto deadline = timeout.count() > 0 ? Clock::now() + timeout : Clock::time_point::max();
    impl_->pending[id] = Impl::Pending{deadline, timeout, std::move(done)};
    return id;
}

void Channel::notify(const std::string& method, json::Value params) {
    json::Object message{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isNull()) message.set("params", std::move(params));
    impl_->send(json::Value(std::move(message)));
}

void Channel::cancel(std::int64_t id) {
    std::lock_guard<std::mutex> lock(impl_->m);
    auto it = impl_->pending.find(id);
    if (it == impl_->pending.end()) return;
    Outcome o;
    o.status = Outcome::Status::Cancelled;
    o.message = "cancelled";
    impl_->ready.emplace_back(std::move(it->second.done), std::move(o));
    impl_->pending.erase(it);
    impl_->sendLocked(json::Object{{"jsonrpc", "2.0"}, {"method", "$/cancelRequest"}, {"params", json::Object{{"id", id}}}});
    impl_->inboxCv.notify_all();
}

void Channel::extend(std::int64_t id, std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(impl_->m);
    auto it = impl_->pending.find(id);
    if (it == impl_->pending.end() || timeout.count() <= 0) return;
    it->second.deadline = Clock::now() + timeout;
    it->second.timeout = timeout;
}

bool Channel::poll(std::chrono::milliseconds wait) {
    using Action = std::function<void()>;
    std::vector<Action> actions;
    RequestHandler requestHandler;
    NotificationHandler notificationHandler;
    {
        std::unique_lock<std::mutex> lock(impl_->m);
        Impl& s = *impl_;
        const auto until = Clock::now() + wait;
        const auto closedForGood = [&] { return s.closed && (s.readerDone || s.closedByHost); };
        const auto earliestDeadline = [&] {
            auto t = Clock::time_point::max();
            for (const auto& [id, p] : s.pending) t = std::min(t, p.deadline);
            return t;
        };
        for (;;) {
            const auto now = Clock::now();
            if (!s.inbox.empty() || !s.ready.empty() || (closedForGood() && !s.pending.empty()) ||
                earliestDeadline() <= now || now >= until)
                break;
            s.inboxCv.wait_until(lock, std::min(until, earliestDeadline()));
        }

        requestHandler = s.requestHandler;
        notificationHandler = s.notificationHandler;

        // Messages first, in arrival order.
        while (!s.inbox.empty()) {
            Incoming in = std::move(s.inbox.front());
            s.inbox.pop_front();
            if (in.kind == Incoming::Kind::Response) {
                auto it = s.pending.find(in.id);
                if (it == s.pending.end()) continue;  // late answer to a request we gave up on
                Outcome o;
                if (in.isError) {
                    o.status = Outcome::Status::Error;
                    o.code = in.code;
                    o.message = std::move(in.message);
                } else {
                    o.result = std::move(in.result);
                }
                actions.push_back([done = std::move(it->second.done), o = std::move(o)] { done(o); });
                s.pending.erase(it);
            } else if (in.kind == Incoming::Kind::Request) {
                auto state = std::make_shared<Responder::State>();
                state->channel = impl_;
                Responder respond(state, in.id);
                actions.push_back([&requestHandler, respond, in = std::move(in)] {
                    try {
                        if (requestHandler) requestHandler(in.method, in.params, respond);
                        else respond.error(MethodNotFound, "unknown method " + in.method);
                    } catch (const RemoteError& e) {
                        respond.error(e.code(), e.what());
                    } catch (const json::Error& e) {
                        respond.error(InvalidParams, e.what());
                    } catch (const std::exception& e) {
                        respond.error(InternalError, e.what());
                    }
                });
            } else {
                actions.push_back([&notificationHandler, in = std::move(in)] {
                    if (notificationHandler) notificationHandler(in.method, in.params);
                });
            }
        }

        for (auto& [done, o] : s.ready) actions.push_back([done = std::move(done), o = std::move(o)] { done(o); });
        s.ready.clear();

        const auto now = Clock::now();
        for (auto it = s.pending.begin(); it != s.pending.end();) {
            Outcome o;
            if (closedForGood()) {
                o.status = Outcome::Status::Closed;
                o.message = s.reason;
            } else if (it->second.deadline <= now) {
                o.status = Outcome::Status::Timeout;
                o.message = "the plugin didn't answer within " + seconds(it->second.timeout);
                s.sendLocked(json::Object{
                    {"jsonrpc", "2.0"}, {"method", "$/cancelRequest"}, {"params", json::Object{{"id", it->first}}}});
            } else {
                ++it;
                continue;
            }
            actions.push_back([done = std::move(it->second.done), o = std::move(o)] { done(o); });
            it = s.pending.erase(it);
        }
    }
    for (Action& a : actions) a();
    return !actions.empty();
}

void Channel::close(const std::string& reason) {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->closeLocked(reason, true);
}

bool Channel::closed() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->closed;
}

std::string Channel::closeReason() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->reason;
}

std::size_t Channel::pendingRequests() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->pending.size();
}

std::string Channel::log() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->log;
}

}  // namespace opl::rpc
