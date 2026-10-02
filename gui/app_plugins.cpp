// Plugins in the desktop app: the Plugins menu and screen, approving a plugin, reviewing the changes
// it proposes, and drawing the windows it describes. The rules (what a plugin may see and do) live in
// ob::plugins (src/plugins.cpp); this file is only the UI.

#include "app.hpp"

#include "imgui.h"
#include "imgui_stdlib.h"
#include "openbooks/util.hpp"
#include "platform.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>

namespace obgui {

namespace fs = std::filesystem;
namespace json = opl::json;
using namespace ob;

namespace {

const char* kReviewPopup = "Review Changes##plugin";
const char* kSecretPopup = "Save Without a Password?##plugin";
const char* kConsentPopup = "Enable Plugin##plugin";
const char* kFilesPopup = "Plugin Files##plugin";
const char* kLogPopup = "Plugin Log##plugin";

std::string str(const json::Value& v, const char* key) {
    const json::Value* f = v.find(key);
    return f && f->isString() ? f->asString() : std::string();
}

ImVec4 statusColor(opl::PluginStatus s) {
    switch (s) {
        case opl::PluginStatus::Enabled: return colorPositive();
        case opl::PluginStatus::NeedsApproval: return colorWarning();
        case opl::PluginStatus::Invalid:
        case opl::PluginStatus::Incompatible: return colorNegative();
        default: return colorMuted();
    }
}

std::string statusLabel(opl::PluginStatus s) {
    std::string label = opl::toString(s);
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

bool anyPopupOpen() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

}  // namespace

// ============================================================ the delegate

const Book* GuiPluginDelegate::books() { return app_.book_ ? &*app_.book_ : nullptr; }

bool GuiPluginDelegate::encrypted() { return app_.key_ != nullptr; }

void GuiPluginDelegate::commit(const std::function<void(Book&)>& change) {
    if (!app_.book_) throw Error("no books are open");
    Book draft = *app_.book_;
    change(draft);
    draft.save(app_.path_, app_.key_.get());
    *app_.book_ = std::move(draft);
    ++app_.version_;
}

void GuiPluginDelegate::review(plugins::Proposal proposal, Decide decide) {
    PluginReview r;
    r.selected.assign(proposal.changes.size(), 1);
    r.proposal = std::move(proposal);
    r.decide = std::move(decide);
    app_.reviews_.push_back(std::move(r));
}

void GuiPluginDelegate::confirmUnencryptedSecret(const std::string& pluginName, std::function<void(bool)> answer) {
    app_.secretQuestions_.push_back(PluginSecretQuestion{pluginName, std::move(answer)});
}

bool GuiPluginDelegate::pluginUi(const std::string& pluginId, const std::string& method, const json::Value& params) {
    app_.handlePluginUi(pluginId, method, params);
    return true;
}

void GuiPluginDelegate::notify(const std::string& pluginId, const std::string& level, const std::string& message) {
    std::string name = pluginId;
    for (const auto& e : app_.pluginsScreen_.entries)
        if (e.id() == pluginId && e.manifest) name = e.manifest->name;
    app_.notify(name + ": " + message, level == "error");
}

std::optional<std::string> GuiPluginDelegate::saveFile(const std::string& suggestedName, const std::string& content) {
    if (!nativeFileDialogsAvailable()) throw opl::Error("no file dialog is available on this system");
    // The name comes from the plugin: keep only a plain file name.
    std::string name;
    for (char c : fs::u8path(suggestedName).filename().u8string())
        if (static_cast<unsigned char>(c) >= 0x20 && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|') name += c;
    if (name.empty() || name == "." || name == "..") name = "export.txt";
    const std::string ext = fs::u8path(name).extension().u8string();
    const std::string pattern = ext.empty() ? "*.*" : "*" + ext;
    const auto path = saveFileDialog("Save file from plugin", {"Files", pattern.c_str()}, ext.empty() ? "" : ext.c_str() + 1, name);
    if (!path) return std::nullopt;
    std::ofstream out(fs::u8path(*path), std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) throw opl::Error("cannot write '" + *path + "'");
    return path;
}

// ============================================================== lifecycle

void App::initPlugins() {
    pluginDelegate_ = std::make_unique<GuiPluginDelegate>(*this);
    plugins_ = std::make_unique<plugins::PluginHost>(*pluginDelegate_);
    pluginsScreen_.registryError = plugins_->reload();
}

void App::resetPluginSession() {
    if (plugins_) plugins_->stopAll();
    for (PluginReview& r : reviews_)
        if (r.decide) r.decide(false, {});
    reviews_.clear();
    for (PluginSecretQuestion& q : secretQuestions_)
        if (q.answer) q.answer(false);
    secretQuestions_.clear();
    pluginViews_.clear();
    pluginPanel_.clear();
    pluginsScreen_.scanned = false;
}

void App::refreshPlugins() {
    if (!plugins_) return;
    pluginsScreen_.registryError = plugins_->reload();
    pluginsScreen_.entries = plugins_->scan();
    pluginsScreen_.scanned = true;
}

const opl::PluginEntry* App::pluginEntry(const std::string& selected) const {
    for (const auto& e : pluginsScreen_.entries)
        if (e.id() == selected || (!e.manifest && e.folder.filename().u8string() == selected)) return &e;
    return nullptr;
}

void App::runPluginCommand(const std::string& pluginId, const std::string& commandId, const std::string& title) {
    try {
        plugins_->runCommand(pluginId, commandId, json::Object{}, [this, title](const opl::rpc::Outcome& o) {
            if (o.ok()) {
                const json::Value* message = o.result.find("message");
                notify(message && message->isString() ? message->asString() : title + ": done");
            } else if (o.status != opl::rpc::Outcome::Status::Cancelled) {
                notify(title + ": " + o.message, true);
            }
        });
    } catch (const std::exception& e) {
        notify(e.what(), true);
    }
}

// =================================================================== menu

void App::drawPluginsMenu() {
    if (!ImGui::BeginMenu("Plugins", book_.has_value())) return;
    if (!pluginsScreen_.scanned) refreshPlugins();
    if (ImGui::MenuItem("Manage Plugins...")) {
        refreshPlugins();
        go(Screen::Plugins);
    }
    bool any = false;
    for (const auto& e : pluginsScreen_.entries) {
        if (e.status != opl::PluginStatus::Enabled || !plugins_->usedWithFile(e.id()) || e.manifest->commands.empty()) continue;
        if (!any) ImGui::Separator();
        any = true;
        if (ImGui::BeginMenu(e.manifest->name.c_str())) {
            for (const auto& cmd : e.manifest->commands)
                if (ImGui::MenuItem(cmd.title.c_str())) runPluginCommand(e.id(), cmd.id, cmd.title);
            ImGui::EndMenu();
        }
    }
    ImGui::EndMenu();
}

// ============================================================ status bar

void App::drawPluginStatus() {
    if (!plugins_) return;
    for (const auto& call : plugins_->activeCalls()) {
        ImGui::SameLine(0, 24.0f);
        std::string label = call.pluginName + ": " + call.title;
        if (!call.progress.empty()) label += " - " + call.progress;
        if (call.fraction >= 0) label += " (" + std::to_string(static_cast<int>(call.fraction * 100)) + "%)";
        ui::Badge(label.c_str(), colorAccent());
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(call.id));
        if (ImGui::SmallButton("Cancel")) plugins_->cancel(call.pluginId, call.id);
        ImGui::PopID();
    }
}

// ================================================================ screen

void App::drawPluginsScreen() {
    if (!pluginsScreen_.scanned) refreshPlugins();
    ui::Heading("Plugins");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshPlugins();
    ui::Muted(("Plugin folder: " + plugins::pluginsDirectory()).c_str());
    if (!pluginsScreen_.registryError.empty()) ui::ErrorText(pluginsScreen_.registryError);
    if (!plugins_->runnerAvailable())
        ui::ErrorText("openplugin-runner isn't installed next to OpenBooks, so Lua plugins can't run.");
    ImGui::Dummy(ImVec2(0, 6));

    if (pluginsScreen_.entries.empty()) {
        ImGui::TextWrapped("No plugins are installed. A plugin is a folder with a plugin.json; put it in the plugin folder above "
                           "and press Refresh.");
        return;
    }

    const float listWidth = ImGui::GetFontSize() * 22.0f;
    ImGui::BeginChild("##pluginList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##plugins", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& e : pluginsScreen_.entries) {
            const std::string key = e.manifest ? e.id() : e.folder.filename().u8string();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = (e.manifest ? e.manifest->name : key) + "##" + key;
            if (ImGui::Selectable(label.c_str(), pluginsScreen_.selected == key, ImGuiSelectableFlags_SpanAllColumns))
                pluginsScreen_.selected = key;
            ImGui::TableNextColumn();
            ImGui::TextColored(statusColor(e.status), "%s", statusLabel(e.status).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##pluginDetail", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (pluginsScreen_.selected.empty() && !pluginsScreen_.entries.empty())
        pluginsScreen_.selected = pluginsScreen_.entries.front().manifest ? pluginsScreen_.entries.front().id()
                                                                          : pluginsScreen_.entries.front().folder.filename().u8string();
    if (const opl::PluginEntry* e = pluginEntry(pluginsScreen_.selected)) drawPluginDetail(*e);
    ImGui::EndChild();
}

void App::drawPluginDetail(const opl::PluginEntry& e) {
    if (!e.manifest) {
        ui::SubHeading(e.folder.filename().u8string().c_str());
        ui::ErrorText(e.detail);
        ui::Muted(e.folder.u8string().c_str());
        return;
    }
    const opl::Manifest& m = *e.manifest;
    const std::string& id = m.id;
    ui::SubHeading((m.name + "  " + m.version).c_str());
    ui::Muted(("by " + m.publisher + "  (" + id + ")").c_str());
    ImGui::TextColored(statusColor(e.status), "%s", statusLabel(e.status).c_str());
    if (!e.detail.empty()) {
        ImGui::SameLine();
        ImGui::TextWrapped("- %s", e.detail.c_str());
    }
    if (!m.description.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextWrapped("%s", m.description.c_str());
    }
    if (!m.homepage.empty()) {
        ImGui::TextUnformatted("Homepage:");
        ImGui::SameLine();
        ImGui::TextUnformatted(m.homepage.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##homepage")) ImGui::SetClipboardText(m.homepage.c_str());
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (m.runtime == opl::Runtime::Lua) {
        ImGui::TextWrapped("Runs as a Lua script in a sandbox: it can't read your files, and it can only reach the servers listed below.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, colorNegative());
        ImGui::TextWrapped("Runs as a native program. It is NOT sandboxed: it can read your files and use the network freely. "
                           "Only enable it if you trust the publisher.");
        ImGui::PopStyleColor();
    }

    // Actions.
    ImGui::Dummy(ImVec2(0, 6));
    switch (e.status) {
        case opl::PluginStatus::NotEnabled:
            if (ui::PrimaryButton("Enable...")) openConsent(e);
            break;
        case opl::PluginStatus::NeedsApproval:
            if (ui::PrimaryButton("Review and approve...")) openConsent(e);
            ImGui::SameLine();
            [[fallthrough]];
        case opl::PluginStatus::Enabled:
            if (ImGui::Button("Disable")) {
                try {
                    plugins_->stop(id);
                    plugins_->registry().disable(id);
                    notify("Disabled " + m.name);
                } catch (const std::exception& ex) {
                    notify(ex.what(), true);
                }
                refreshPlugins();
                return;
            }
            break;
        default: break;
    }
    if (e.status == opl::PluginStatus::Enabled) {
        bool used = plugins_->usedWithFile(id);
        ImGui::SameLine();
        if (ImGui::Checkbox("Use with this file", &used)) {
            try {
                plugins_->setUsedWithFile(id, used);
                ++version_;
            } catch (const std::exception& ex) {
                notify(ex.what(), true);
            }
        }
        if (used && !m.commands.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            for (const auto& cmd : m.commands) {
                ImGui::PushID(cmd.id.c_str());
                if (ImGui::Button(cmd.title.c_str())) runPluginCommand(id, cmd.id, cmd.title);
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        const plugins::RunState state = plugins_->state(id);
        if (state == plugins::RunState::Failed) ui::ErrorText("Stopped: " + plugins_->failure(id));
        if (state == plugins::RunState::Ready || state == plugins::RunState::Starting) {
            ui::Muted(state == plugins::RunState::Ready ? "Running" : "Starting...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) plugins_->stop(id);
        }
    }

    // What it may do.
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Permissions");
    if (m.permissions.empty()) ui::Muted("None");
    for (const std::string& p : m.permissions) {
        const opl::PermissionInfo* info = opl::findPermission(p);
        const bool has = e.grant && std::find(e.grant->permissions.begin(), e.grant->permissions.end(), p) != e.grant->permissions.end();
        ImGui::TextColored(has ? colorPositive() : colorMuted(), has ? "Granted" : "-");
        ImGui::SameLine(ImGui::GetFontSize() * 5.0f);
        if (info->high) ImGui::TextColored(colorWarning(), "%s", info->description);
        else ImGui::TextUnformatted(info->description);
    }
    ImGui::SeparatorText("Network");
    if (m.network.empty()) ui::Muted("No network access");
    for (const std::string& h : m.network) {
        const bool has = e.grant && std::find(e.grant->network.begin(), e.grant->network.end(), h) != e.grant->network.end();
        ImGui::TextColored(has ? colorPositive() : colorMuted(), has ? "Allowed" : "-");
        ImGui::SameLine(ImGui::GetFontSize() * 5.0f);
        ImGui::TextUnformatted(h.c_str());
        if (opl::isDevelopmentHostPattern(h)) {
            ImGui::SameLine();
            ImGui::TextColored(colorWarning(), "(local or test server)");
        }
    }

    // This file.
    if (book_) {
        const auto& data = book_->plugins.entries(id);
        if (!data.empty()) {
            ImGui::SeparatorText("Data in this file");
            std::size_t shown = 0;
            for (const auto& [key, entry] : data)
                if (key.empty() || key[0] != '@') ++shown;
            ImGui::Text("%zu item%s saved by this plugin.", shown, shown == 1 ? "" : "s");
            if (book_->plugins.hasSecrets(id) && !key_) {
                ImGui::PushStyleColor(ImGuiCol_Text, colorWarning());
                ImGui::TextWrapped("It holds a secret (such as an API key) as readable text, because this file has no password. "
                                   "File > Protect with Password encrypts it.");
                ImGui::PopStyleColor();
            }
            if (ui::DangerButton("Remove plugin data...")) {
                confirm("Remove plugin data", "Delete everything " + m.name + " saved in this file? This can't be undone.", "Remove",
                        [this, id] {
                            try {
                                plugins_->removeData(id);
                                ++version_;
                                notify("Removed the plugin's data");
                            } catch (const std::exception& ex) {
                                notify(ex.what(), true);
                            }
                        });
            }
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Show files...")) {
        pluginsScreen_.viewerTitle = m.name;
        pluginsScreen_.files.clear();
        pluginsScreen_.viewerFile = 0;
        try {
            for (opl::PluginFile& f : opl::readPluginFolder(e.folder)) {
                const bool text = json::isValidUtf8(f.content) && f.content.find('\0') == std::string::npos;
                pluginsScreen_.files.emplace_back(f.path, text ? (f.content.size() > 512 * 1024 ? f.content.substr(0, 512 * 1024) + "\n..."
                                                                                               : std::move(f.content))
                                                               : "(binary file, " + std::to_string(f.content.size()) + " bytes)");
            }
        } catch (const std::exception& ex) {
            pluginsScreen_.files.emplace_back("(error)", ex.what());
        }
        ImGui::OpenPopup(kFilesPopup);
    }
    ImGui::SameLine();
    if (ImGui::Button("Show log...")) {
        pluginsScreen_.viewerTitle = m.name;
        pluginsScreen_.logText = plugins_->log(id);
        if (pluginsScreen_.logText.empty()) pluginsScreen_.logText = "(nothing logged since the plugin started)";
        ImGui::OpenPopup(kLogPopup);
    }
    ui::Muted(("Folder: " + e.folder.u8string()).c_str());
    ui::Muted(("Fingerprint: " + e.hash).c_str());

    // The viewers are opened from inside this child window, so they are drawn here too.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 50, ImGui::GetFontSize() * 32), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kFilesPopup)) {
        ui::SubHeading((pluginsScreen_.viewerTitle + " - files (read only)").c_str());
        ImGui::BeginChild("##files", ImVec2(ImGui::GetFontSize() * 12, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(pluginsScreen_.files.size()); ++i)
            if (ImGui::Selectable(pluginsScreen_.files[i].first.c_str(), pluginsScreen_.viewerFile == i)) pluginsScreen_.viewerFile = i;
        ImGui::EndChild();
        ImGui::SameLine();
        if (!pluginsScreen_.files.empty()) {
            std::string& textRef = pluginsScreen_.files[static_cast<std::size_t>(pluginsScreen_.viewerFile)].second;
            ImGui::InputTextMultiline("##content", textRef.data(), textRef.size() + 1, ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()),
                                      ImGuiInputTextFlags_ReadOnly);
        }
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, ImGui::GetFontSize() * 26), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kLogPopup)) {
        ui::SubHeading((pluginsScreen_.viewerTitle + " - log").c_str());
        std::string& log = pluginsScreen_.logText;
        ImGui::InputTextMultiline("##log", log.data(), log.size() + 1, ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()),
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(log.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::openConsent(const opl::PluginEntry& e) {
    pluginsScreen_.consentFolder = e.folder.u8string();
    pluginsScreen_.consentPermissions.clear();
    pluginsScreen_.consentHosts.clear();
    pluginsScreen_.consentError.clear();
    for (const std::string& p : e.manifest->permissions) pluginsScreen_.consentPermissions[p] = true;
    for (const std::string& h : e.manifest->network) pluginsScreen_.consentHosts[h] = true;
    requestPopup(kConsentPopup);
}

// ================================================================= panels

void App::drawPluginPanel() {
    PluginView* view = nullptr;
    for (PluginView& v : pluginViews_)
        if (v.kind == "panel" && v.pluginId + "/" + v.viewId == pluginPanel_) view = &v;
    if (!view) {
        go(Screen::Plugins);
        return;
    }
    ui::Heading(view->title.c_str());
    ImGui::Dummy(ImVec2(0, 6));
    renderPluginView(*view);
}

// ================================================================= modals

void App::drawPluginModals() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    const float wrap = ImGui::GetFontSize() * 34.0f;

    // ---- Approving a plugin
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kConsentPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const opl::PluginEntry* e = nullptr;
        for (const auto& entry : pluginsScreen_.entries)
            if (entry.folder.u8string() == pluginsScreen_.consentFolder) e = &entry;
        if (!e || !e->manifest) {
            ImGui::CloseCurrentPopup();
        } else {
            const opl::Manifest& m = *e->manifest;
            ui::SubHeading(("Enable " + m.name + "?").c_str());
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted(("By " + m.publisher + ", version " + m.version + ".").c_str());
            if (e->status == opl::PluginStatus::NeedsApproval) ImGui::TextColored(colorWarning(), "%s.", e->detail.c_str());
            if (m.runtime == opl::Runtime::Native)
                ImGui::TextColored(colorNegative(), "This is a native program, NOT sandboxed: it can read your files and use the network freely.");
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextUnformatted("It asks to:");
            for (auto& [perm, on] : pluginsScreen_.consentPermissions) {
                const opl::PermissionInfo* info = opl::findPermission(perm);
                const bool isNew = std::find(e->newPermissions.begin(), e->newPermissions.end(), perm) != e->newPermissions.end();
                std::string label = std::string(info->description) + (isNew ? "  (new)" : "") + "##" + perm;
                if (info->high) ImGui::PushStyleColor(ImGuiCol_Text, colorWarning());
                ImGui::Checkbox(label.c_str(), &on);
                if (info->high) ImGui::PopStyleColor();
            }
            if (pluginsScreen_.consentPermissions.empty()) ui::Muted("(nothing)");
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextUnformatted("It may send data to:");
            for (auto& [hostName, on] : pluginsScreen_.consentHosts) {
                const bool isNew = std::find(e->newNetwork.begin(), e->newNetwork.end(), hostName) != e->newNetwork.end();
                std::string label = hostName + (opl::isDevelopmentHostPattern(hostName) ? "  (a local or test server)" : "") +
                                    (isNew ? "  (new)" : "") + "##host";
                ImGui::Checkbox(label.c_str(), &on);
            }
            if (pluginsScreen_.consentHosts.empty()) ui::Muted("(no servers: it can't use the network)");
            ImGui::Dummy(ImVec2(0, 4));
            ui::Muted("You can turn things off; the plugin then works with less. It only sees data in files where you turn it on, "
                      "and every change it proposes is shown to you first. If any of its files change, it needs approval again.");
            ImGui::PopTextWrapPos();
            if (!pluginsScreen_.consentError.empty()) ui::ErrorText(pluginsScreen_.consentError);
            ImGui::Dummy(ImVec2(0, 6));
            if (ui::PrimaryButton("Enable")) {
                std::vector<std::string> perms, hosts;
                for (const auto& [p, on] : pluginsScreen_.consentPermissions)
                    if (on) perms.push_back(p);
                for (const auto& [h, on] : pluginsScreen_.consentHosts)
                    if (on) hosts.push_back(h);
                try {
                    plugins_->stop(m.id);
                    plugins_->registry().enable(*e, perms, hosts);
                    notify("Enabled " + m.name);
                    const std::string id = m.id;
                    refreshPlugins();
                    pluginsScreen_.selected = id;
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception& ex) {
                    pluginsScreen_.consentError = ex.what();
                    refreshPlugins();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Plugin questions open only when nothing else is open, so they never close the user's dialog.
    if (!anyPopupOpen()) {
        if (!secretQuestions_.empty()) ImGui::OpenPopup(kSecretPopup);
        else if (!reviews_.empty()) ImGui::OpenPopup(kReviewPopup);
        else {
            for (PluginView& v : pluginViews_) {
                if (v.kind != "modal" || v.popupOpen) continue;
                ImGui::OpenPopup((v.title + "##pluginview/" + v.pluginId + "/" + v.viewId).c_str());
                v.popupOpen = true;
                break;
            }
        }
    }

    // ---- A secret in a file without a password
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kSecretPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (secretQuestions_.empty()) {
            ImGui::CloseCurrentPopup();
        } else {
            PluginSecretQuestion& q = secretQuestions_.front();
            ui::SubHeading("Save without a password?");
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted((q.pluginName + " wants to save a secret, such as an API key. This file isn't password-protected, so the "
                                    "secret would be saved as readable text in the file and its backups.")
                                       .c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 6));
            int choice = -1;
            if (ui::PrimaryButton("Set a password...")) choice = 2;
            ImGui::SameLine();
            if (ImGui::Button("Save anyway")) choice = 1;
            ImGui::SameLine();
            if (ImGui::Button("Don't save") || ImGui::IsKeyPressed(ImGuiKey_Escape)) choice = 0;
            if (choice >= 0) {
                auto answer = std::move(q.answer);
                secretQuestions_.pop_front();
                ImGui::CloseCurrentPopup();
                answer(choice == 1);
                if (choice == 2) openPasswordForm(0);
            }
        }
        ImGui::EndPopup();
    }

    // ---- Reviewing proposed changes
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 30, 0), ImVec2(ImGui::GetFontSize() * 60, ImGui::GetFontSize() * 40));
    if (ImGui::BeginPopupModal(kReviewPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (reviews_.empty()) {
            ImGui::CloseCurrentPopup();
        } else {
            PluginReview& r = reviews_.front();
            ui::SubHeading((r.proposal.pluginName + " proposes changes").c_str());
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted(r.proposal.summary.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::BeginChild("##changes", ImVec2(wrap, std::min(ImGui::GetFontSize() * 2.2f * r.proposal.changes.size() + 8, ImGui::GetFontSize() * 22)),
                              ImGuiChildFlags_Borders);
            for (std::size_t i = 0; i < r.proposal.changes.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                bool on = r.selected[i] != 0;
                if (ImGui::Checkbox("##on", &on)) r.selected[i] = on ? 1 : 0;
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(r.proposal.changes[i].description.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (!r.proposal.store.empty()) ui::Muted("The plugin will also save its own notes in this file (for example, what it already imported).");
            if (!r.error.empty()) ui::ErrorText("Not applied: " + r.error);
            ImGui::Dummy(ImVec2(0, 6));
            const auto count = std::count(r.selected.begin(), r.selected.end(), 1);
            bool apply = false;
            if (count == 0) {
                ImGui::BeginDisabled();
                ImGui::Button("Apply");
                ImGui::EndDisabled();
            } else {
                apply = ui::PrimaryButton(count == static_cast<long>(r.selected.size()) ? "Apply" : "Apply selected");
            }
            ImGui::SameLine();
            const bool decline = ImGui::Button("Decline");
            if (apply) {
                std::vector<bool> selected(r.selected.begin(), r.selected.end());
                r.error = r.decide(true, selected);
                if (r.error.empty()) {
                    reviews_.pop_front();
                    ImGui::CloseCurrentPopup();
                    notify("Changes applied");
                }
            } else if (decline) {
                r.decide(false, {});
                reviews_.pop_front();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- Windows a plugin described
    for (std::size_t i = 0; i < pluginViews_.size(); ++i) {
        PluginView& v = pluginViews_[i];
        if (v.kind != "modal" || !v.popupOpen) continue;
        const std::string name = v.title + "##pluginview/" + v.pluginId + "/" + v.viewId;
        bool open = true;
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 24, 0), ImVec2(ImGui::GetFontSize() * 56, ImGui::GetFontSize() * 44));
        if (ImGui::BeginPopupModal(name.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (v.closing) {
                ImGui::CloseCurrentPopup();
            } else {
                renderPluginView(v);
            }
            ImGui::EndPopup();
        }
        if (!open || v.closing || !ImGui::IsPopupOpen(name.c_str())) {
            if (!open && !v.closing) sendPluginEvent(v, "", "close");
            pluginViews_.erase(pluginViews_.begin() + static_cast<std::ptrdiff_t>(i));
            break;  // the deque changed; continue next frame
        }
    }
}

// ============================================================ plugin views

PluginView* App::findPluginView(const std::string& pluginId, const std::string& viewId) {
    for (PluginView& v : pluginViews_)
        if (v.pluginId == pluginId && v.viewId == viewId) return &v;
    return nullptr;
}

void App::handlePluginUi(const std::string& pluginId, const std::string& method, const json::Value& params) {
    const std::string id = str(params, "id");
    if (id.empty() || id.size() > 64) throw opl::rpc::RemoteError(opl::rpc::InvalidParams, "a view needs an \"id\"");
    if (method == "ui/close") {
        for (std::size_t i = 0; i < pluginViews_.size(); ++i) {
            PluginView& v = pluginViews_[i];
            if (v.pluginId != pluginId || v.viewId != id) continue;
            if (v.kind == "modal" && v.popupOpen) v.closing = true;  // closed from inside its popup
            else pluginViews_.erase(pluginViews_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        return;
    }
    const json::Value* body = params.find("body");
    if (!body || !body->isArray()) throw opl::rpc::RemoteError(opl::rpc::InvalidParams, "a view needs a \"body\" list");
    if (body->asArray().size() > 200) throw opl::rpc::RemoteError(opl::rpc::InvalidParams, "a view can have at most 200 elements");
    PluginView* v = findPluginView(pluginId, id);
    if (!v) {
        if (method == "ui/update") throw opl::rpc::RemoteError(opl::rpc::InvalidParams, "no view \"" + id + "\" is open");
        if (pluginViews_.size() >= 16) throw opl::rpc::RemoteError(opl::rpc::InvalidParams, "too many plugin views are open");
        pluginViews_.emplace_back();
        v = &pluginViews_.back();
        v->pluginId = pluginId;
        v->viewId = id;
        v->kind = str(params, "kind") == "panel" ? "panel" : "modal";
    }
    v->title = str(params, "title").substr(0, 80);
    if (v->title.empty()) v->title = "Plugin";
    v->body = *body;
    v->error.clear();
    v->waiting = false;
    // Initial values from the plugin (never for secrets).
    for (const json::Value& el : body->asArray()) {
        if (str(el, "type") != "field") continue;
        const std::string fid = str(el, "id");
        const std::string input = str(el, "input");
        const json::Value* value = el.find("value");
        if (fid.empty() || !value || input == "secret") continue;
        if (input == "bool" && value->isBool()) v->flags[fid] = value->asBool();
        else if ((input == "account" || input == "contact" || input == "invoice") && value->isInteger()) v->ids[fid] = static_cast<int>(value->asInt());
        else if (input == "date" && value->isString()) {
            if (auto d = Date::parse(value->asString())) v->dates[fid] = *d;
        } else if (value->isString()) v->text[fid] = value->asString();
    }
    if (v->kind == "panel" && method == "ui/show") {
        pluginPanel_ = v->pluginId + "/" + v->viewId;
        go(Screen::PluginPanel);
    }
}

void App::sendPluginEvent(PluginView& v, const std::string& element, const char* action) {
    json::Object values;
    for (const json::Value& el : v.body.asArray()) {
        if (str(el, "type") != "field") continue;
        const std::string fid = str(el, "id");
        const std::string input = str(el, "input");
        if (fid.empty()) continue;
        if (input == "bool") values.set(fid, v.flags[fid]);
        else if (input == "account" || input == "contact" || input == "invoice")
            values.set(fid, v.ids[fid] ? json::Value(v.ids[fid]) : json::Value());
        else if (input == "date") values.set(fid, v.dates.count(fid) ? json::Value(v.dates[fid].str()) : json::Value());
        else values.set(fid, json::isValidUtf8(v.text[fid]) ? v.text[fid] : std::string());
    }
    // Secrets are sent once and then wiped from the form.
    for (const json::Value& el : v.body.asArray())
        if (str(el, "type") == "field" && str(el, "input") == "secret") crypto::wipe(v.text[str(el, "id")]);

    const std::string pluginId = v.pluginId, viewId = v.viewId;
    v.waiting = true;
    try {
        plugins_->call(pluginId, "ui/event",
                       json::Object{{"viewId", viewId}, {"element", element}, {"action", action}, {"values", json::Value(std::move(values))}},
                       std::chrono::minutes(5),
                       [this, pluginId, viewId](const opl::rpc::Outcome& o) {
                           PluginView* view = findPluginView(pluginId, viewId);
                           if (!view) return;
                           view->waiting = false;
                           if (!o.ok() && o.code != opl::rpc::MethodNotFound) view->error = o.message;
                       },
                       "Working");
    } catch (const std::exception& e) {
        v.waiting = false;
        v.error = e.what();
    }
}

void App::renderPluginView(PluginView& v) {
    const float labelColumn = ImGui::GetFontSize() * 9.0f;
    const float width = ImGui::GetFontSize() * 22.0f;
    ImGui::PushID((v.pluginId + "/" + v.viewId).c_str());
    if (v.waiting) ImGui::BeginDisabled();
    int index = 0;
    for (const json::Value& el : v.body.asArray()) {
        ImGui::PushID(index++);
        const std::string type = str(el, "type");
        const std::string fid = str(el, "id");
        const std::string label = str(el, "label");
        if (type == "text") {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextUnformatted(str(el, "value").c_str());
            ImGui::PopTextWrapPos();
        } else if (type == "heading") {
            ui::SubHeading(str(el, "value").c_str());
        } else if (type == "separator") {
            ImGui::Separator();
        } else if (type == "field" && !fid.empty()) {
            const std::string input = str(el, "input");
            ui::FormLabel(label.c_str(), labelColumn);
            ImGui::SetNextItemWidth(width);
            if (input == "secret") {
                ImGui::InputText("##f", &v.text[fid], ImGuiInputTextFlags_Password);
            } else if (input == "multiline") {
                ui::InputMultiline("##f", v.text[fid], ImVec2(width, ImGui::GetFontSize() * 5));
            } else if (input == "money") {
                ui::MoneyField("##f", v.text[fid], width);
            } else if (input == "decimal") {
                ui::DecimalField("##f", v.text[fid], width);
            } else if (input == "date") {
                if (!v.dates.count(fid)) v.dates[fid] = Date::today();
                ui::DateField("##f", v.dates[fid], width);
            } else if (input == "bool") {
                ImGui::Checkbox("##f", &v.flags[fid]);
            } else if (input == "choice") {
                std::vector<ui::Option> options;
                std::vector<std::string> values;
                if (const json::Value* opts = el.find("options"); opts && opts->isArray()) {
                    for (const json::Value& o : opts->asArray()) {
                        const std::string value = o.isString() ? o.asString() : str(o, "value");
                        const std::string text = o.isString() ? o.asString() : str(o, "label");
                        values.push_back(value);
                        options.push_back(ui::Option{static_cast<int>(values.size()), text.empty() ? value : text});
                    }
                }
                int selected = 0;
                for (std::size_t k = 0; k < values.size(); ++k)
                    if (values[k] == v.text[fid]) selected = static_cast<int>(k + 1);
                const std::string preview = selected ? options[static_cast<std::size_t>(selected - 1)].label : "";
                if (ui::SearchCombo("##f", preview, options, selected, width) && selected > 0)
                    v.text[fid] = values[static_cast<std::size_t>(selected - 1)];
            } else if (input == "account") {
                const std::string filter = str(el, "filter");
                ui::AccountFilter f;
                if (filter == "bank") f = [this](const Account& a) { return ui::isMoneyAccount(*book_, a); };
                else if (filter == "income") f = [](const Account& a) { return a.type == AccountType::Income; };
                else if (filter == "expense") f = [](const Account& a) { return a.type == AccountType::Expense; };
                ui::AccountCombo("##f", *book_, v.ids[fid], f, "(none)", width);
            } else if (input == "contact") {
                ui::ContactCombo("##f", *book_, str(el, "filter") == "vendor" ? ContactKind::Vendor : ContactKind::Customer, v.ids[fid], width);
            } else if (input == "invoice") {
                std::vector<ui::Option> options;
                std::string preview;
                for (const Document& d : book_->documents()) {
                    if (d.kind != DocKind::Invoice || d.voided) continue;
                    std::string text = d.number + "  " + book_->contact(d.contactId).name + "  " + d.total().formatted();
                    if (d.id == v.ids[fid]) preview = text;
                    options.push_back(ui::Option{d.id, std::move(text)});
                }
                ui::SearchCombo("##f", preview, options, v.ids[fid], width);
            } else {
                ui::InputString("##f", v.text[fid]);
            }
            if (const std::string help = str(el, "help"); !help.empty()) {
                ImGui::SameLine();
                ui::Muted(help.c_str());
            }
        } else if (type == "copyable") {
            if (!label.empty()) ui::FormLabel(label.c_str(), labelColumn);
            const std::string value = str(el, "value");
            ImGui::TextUnformatted(value.c_str());  // shown as text: plugins can't open links
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(value.c_str());
                notify("Copied");
            }
        } else if (type == "table") {
            const json::Value* cols = el.find("columns");
            const json::Value* rows = el.find("rows");
            const int n = cols && cols->isArray() ? static_cast<int>(std::min<std::size_t>(cols->asArray().size(), 12)) : 0;
            if (n > 0 && ImGui::BeginTable("##t", n, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                                           ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 12))) {
                for (int c = 0; c < n; ++c)
                    ImGui::TableSetupColumn(cols->asArray()[static_cast<std::size_t>(c)].isString() ? cols->asArray()[static_cast<std::size_t>(c)].asString().c_str() : "");
                ImGui::TableHeadersRow();
                if (rows && rows->isArray()) {
                    for (const json::Value& row : rows->asArray()) {
                        if (!row.isArray()) continue;
                        ImGui::TableNextRow();
                        for (int c = 0; c < n && c < static_cast<int>(row.asArray().size()); ++c) {
                            ImGui::TableSetColumnIndex(c);
                            const json::Value& cell = row.asArray()[static_cast<std::size_t>(c)];
                            ImGui::TextUnformatted(cell.isString() ? cell.asString().c_str() : json::write(cell).c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }
        } else if (type == "progress") {
            const json::Value* f = el.find("fraction");
            const float fraction = f && f->isNumber() ? static_cast<float>(std::clamp(f->asDouble(), 0.0, 1.0)) : 0.0f;
            ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 22, 0), label.empty() ? nullptr : label.c_str());
        } else if (type == "diagnostics") {
            if (const json::Value* items = el.find("items"); items && items->isArray()) {
                for (const json::Value& d : items->asArray()) {
                    const std::string severity = str(d, "severity");
                    const ImVec4 color = severity == "error" ? colorNegative() : severity == "warning" ? colorWarning() : colorMuted();
                    ImGui::TextColored(color, "%s", severity.empty() ? "note" : severity.c_str());
                    ImGui::SameLine();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
                    ImGui::TextUnformatted(str(d, "message").c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        } else if (type == "buttons") {
            if (const json::Value* items = el.find("items"); items && items->isArray()) {
                bool first = true;
                for (const json::Value& b : items->asArray()) {
                    const std::string bid = str(b, "id");
                    const std::string blabel = str(b, "label").empty() ? bid : str(b, "label");
                    if (bid.empty()) continue;
                    if (!first) ImGui::SameLine();
                    first = false;
                    const json::Value* primary = b.find("primary");
                    const bool isPrimary = primary && primary->isBool() && primary->asBool();
                    if (isPrimary ? ui::PrimaryButton(blabel.c_str()) : ImGui::Button(blabel.c_str())) sendPluginEvent(v, bid, "click");
                }
            }
        }
        ImGui::PopID();
    }
    if (v.waiting) {
        ImGui::EndDisabled();
        ui::Muted("Working...");
    }
    if (!v.error.empty()) ui::ErrorText(v.error);
    ImGui::PopID();
}

}  // namespace obgui
