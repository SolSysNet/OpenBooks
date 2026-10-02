#pragma once

// Plugins in OpenBooks: how plugins see the books (views), how they change them (proposals), and
// PluginHost, which runs them for the command line and the desktop app.
//
// The design is in third_party/openplugin/docs/design.md. In short: a plugin is a separate process
// (normally a Lua script run by openplugin-runner, the only program in OpenBooks with network code).
// It sees only the views its granted permissions cover, never the file or its password, and every
// change it wants goes through the same validation as a change typed by the user, after the user
// reviewed it.

#include "openbooks/book.hpp"

#include "openplugin/json.hpp"
#include "openplugin/registry.hpp"
#include "openplugin/rpc.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ob::plugins {

namespace json = opl::json;

// ---- Where things live

std::string pluginsDirectory();  // <settings folder>/plugins
std::string registryFile();      // <settings folder>/plugins.json (what the user approved)
std::string runnerPath();        // openplugin-runner next to this program (may not exist)
opl::AppIdentity appIdentity();

// ---- Views (host/read)

struct ViewInfo {
    const char* name;
    const char* permission;
    const char* description;
};
const std::vector<ViewInfo>& views();

// A read-only snapshot for a plugin. Throws rpc::RemoteError: PermissionDenied when `granted` doesn't
// cover the view, InvalidParams for an unknown view or a bad query.
json::Value readView(const Book& b, const std::string& view, const json::Value& query,
                     const std::vector<std::string>& granted);

// ---- Proposals (host/propose)

struct ProposedChange {
    std::string op;           // "recordPayment", "postTransaction" or "addContact"
    json::Value data;
    std::string description;  // plain language, for the review screen
};

struct Proposal {
    std::string pluginId;
    std::string pluginName;
    std::string summary;
    std::vector<ProposedChange> changes;
    std::vector<std::pair<std::string, std::string>> store;  // plugin data saved along with the changes
};

// Checks the shape, the permissions and that everything referred to exists, and describes each
// change. Throws rpc::RemoteError (InvalidParams, PermissionDenied).
Proposal parseProposal(const Book& b, const std::string& pluginId, const std::string& pluginName,
                       const json::Value& params, const std::vector<std::string>& granted);

// Applies the selected changes (all of them when `selected` is empty), then the plugin data. Throws
// ob::Error or opl::StoreError part way through, so apply to a copy: a failure then changes nothing.
// Returns the ids created, one per applied change.
std::vector<int> applyProposal(Book& b, const Proposal& p, const std::vector<bool>& selected = {});

// ---- Running plugins

// What the front end (command line or desktop app) does for the host.
class Delegate {
public:
    virtual ~Delegate() = default;

    virtual const Book* books() = 0;  // the open books, or nullptr
    virtual bool encrypted() = 0;     // whether the open file has a password
    // Applies `change` to a copy of the books, saves, then swaps the copy in. Throws (and changes
    // nothing) on failure.
    virtual void commit(const std::function<void(Book&)>& change) = 0;

    // Shows a proposal for review. Call `decide` once the user has decided, now or many frames
    // later. It returns an empty string on success, or why applying failed (the review can stay open
    // so the user can deselect the problem change or decline).
    using Decide = std::function<std::string(bool apply, std::vector<bool> selected)>;
    virtual void review(Proposal proposal, Decide decide) = 0;

    // A plugin wants to save a secret (an API key) in a file without a password. Asked once per
    // plugin per file; `answer(true)` saves it anyway.
    virtual void confirmUnencryptedSecret(const std::string& pluginName, std::function<void(bool)> answer) = 0;

    // ui/show, ui/update, ui/close. Return false when this front end can't show plugin UI.
    virtual bool pluginUi(const std::string& pluginId, const std::string& method, const json::Value& params) = 0;
    virtual void notify(const std::string& pluginId, const std::string& level, const std::string& message) = 0;
    // files/save: the path written, or nullopt if the user cancelled. Throw if unsupported.
    virtual std::optional<std::string> saveFile(const std::string& suggestedName, const std::string& content) = 0;
};

enum class RunState { Stopped, Starting, Ready, Failed };

class PluginHost {
public:
    using Done = std::function<void(const opl::rpc::Outcome&)>;

    explicit PluginHost(Delegate& delegate, std::string pluginsDir = pluginsDirectory(),
                        std::string registryPath = registryFile(), std::string runner = runnerPath());
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // ---- Installed plugins and approvals
    opl::Registry& registry() { return registry_; }
    // Reloads approvals; returns an error message if the settings file is damaged (every plugin is
    // then not enabled).
    std::string reload();
    std::vector<opl::PluginEntry> scan() const { return registry_.scan(); }
    std::optional<opl::PluginEntry> find(const std::string& pluginId) const;
    bool runnerAvailable() const;

    // ---- Per-file opt-in ("Use with this file"), saved in the books through the delegate.
    bool usedWithFile(const std::string& pluginId) const;
    void setUsedWithFile(const std::string& pluginId, bool used);
    // "Remove plugin data": stops the plugin and deletes everything it saved in this file.
    void removeData(const std::string& pluginId);

    // ---- Running
    // Starts an enabled plugin that is used with this file. Throws opl::Error saying what's missing.
    void start(const std::string& pluginId);
    void stop(const std::string& pluginId);
    void stopAll();
    RunState state(const std::string& pluginId) const;
    std::string failure(const std::string& pluginId) const;  // why it failed or stopped
    std::string log(const std::string& pluginId) const;

    // Calls into a plugin, starting it first if needed. `done` runs from poll(). Returns a call id.
    std::int64_t runCommand(const std::string& pluginId, const std::string& commandId, json::Value context, Done done);
    std::int64_t call(const std::string& pluginId, const std::string& method, json::Value params,
                      std::chrono::milliseconds timeout, Done done, std::string title = {});
    void cancel(const std::string& pluginId, std::int64_t callId);
    // App events ("fileSaved", ...) for every running plugin.
    void event(const std::string& name, const json::Value& details);

    // Delivers whatever plugins sent, waiting up to `wait` if nothing has arrived. Call once per frame
    // (GUI) or in a loop (command line). Callbacks run here.
    void poll(std::chrono::milliseconds wait = std::chrono::milliseconds(0));
    bool busy() const;  // starting, or calls in progress

    struct ActiveCall {
        std::string pluginId;
        std::string pluginName;
        std::int64_t id = 0;
        std::string title;
        std::string progress;
        double fraction = -1;  // -1 = unknown
    };
    std::vector<ActiveCall> activeCalls() const;

    struct Impl;

private:
    Delegate& delegate_;
    opl::Registry registry_;
    std::string runner_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ob::plugins
