// A headless smoke test of the desktop app's plugin screens: the real App and Dear ImGui with no
// window or renderer, driving a real Lua plugin through openplugin-runner. It checks that every
// plugin screen, dialog and element type draws without ImGui errors and that the flows work end to
// end (approve, run, answer the secret warning, review and apply changes).

#include "app.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "openbooks/paths.hpp"
#include "theme.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace obgui {

// The only way the test reaches into App (declared a friend in app.hpp).
struct AppTestAccess {
    App& app;
    bool booksOpen() const { return app.book_.has_value(); }
    const ob::Book& book() const { return *app.book_; }
    void go(Screen s) { app.go(s); }
    Screen screen() const { return app.screen_; }
    PluginsScreenState& plugins() { return app.pluginsScreen_; }
    std::deque<PluginReview>& reviews() { return app.reviews_; }
    std::deque<PluginSecretQuestion>& secrets() { return app.secretQuestions_; }
    std::deque<PluginView>& views() { return app.pluginViews_; }
    void openConsent(const opl::PluginEntry& e) { app.openConsent(e); }
    void run(const std::string& id, const std::string& cmd) { app.runPluginCommand(id, cmd, cmd); }
    void click(PluginView& v, const std::string& element) { app.sendPluginEvent(v, element, "click"); }
    const std::string& notice() const { return app.notice_; }
    ob::plugins::PluginHost& host() { return *app.plugins_; }
};

}  // namespace obgui

namespace {

namespace fs = std::filesystem;
int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
        }                                                                             \
    } while (0)

// No renderer: accept every texture request so ImGui's font atlas keeps working.
void processTextures() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetTexID(static_cast<ImTextureID>(1));
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

void frame(obgui::App& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    processTextures();
}

// Draws frames until `done` or 15 seconds.
bool frameUntil(obgui::App& app, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        frame(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    frame(app);
    return true;
}

bool windowActive(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w && w->WasActive;
}

void closeAllPopups(obgui::App& app) {
    ImGui::NewFrame();
    if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
    app.frame();
    ImGui::Render();
    processTextures();
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

const char* kMain = R"LUA(
op.command("demo", function()
  op.ui.show{ id = "demo", title = "Demo window", kind = "modal", body = {
    { type = "heading", value = "Everything a plugin can draw" },
    { type = "text", value = "Plain text, wrapped to the window." },
    { type = "separator" },
    { type = "field", id = "name", label = "Name", input = "text", value = "Acme" },
    { type = "field", id = "key", label = "API key", input = "secret" },
    { type = "field", id = "notes", label = "Notes", input = "multiline" },
    { type = "field", id = "amount", label = "Amount", input = "money", value = "12.50" },
    { type = "field", id = "rate", label = "Rate", input = "decimal" },
    { type = "field", id = "when", label = "Date", input = "date", value = "2026-09-30" },
    { type = "field", id = "live", label = "Live mode", input = "bool", value = true },
    { type = "field", id = "mode", label = "Mode", input = "choice", options = { "test", { value = "live", label = "Live" } } },
    { type = "field", id = "deposit", label = "Deposit to", input = "account", filter = "bank" },
    { type = "field", id = "customer", label = "Customer", input = "contact" },
    { type = "field", id = "invoice", label = "Invoice", input = "invoice" },
    { type = "copyable", label = "Dashboard", value = "https://dashboard.example.com/keys" },
    { type = "table", columns = { "Date", "Amount" }, rows = { { "2026-09-30", "12.50" }, { "2026-10-01", 7 } } },
    { type = "progress", label = "Half way", fraction = 0.5 },
    { type = "diagnostics", items = { { severity = "error", message = "Something to fix" }, { severity = "warning", message = "A note" } } },
    { type = "buttons", items = { { id = "save", label = "Save", primary = true }, { id = "cancel", label = "Cancel" } } },
  } }
  op.ui.show{ id = "status", title = "Lab status", kind = "panel", body = { { type = "text", value = "All good" } } }
end)

op.ui.on("demo", function(ev)
  if ev.element == "save" then
    assert(ev.values.name == "Acme" and ev.values.amount == "12.50" and ev.values.live == true and ev.values.when == "2026-09-30")
    op.ui.notify("info", "saved " .. ev.values.name)
  end
  op.ui.close("demo")
end)

op.command("sync", function(ctx)
  op.store.set("apiKey", "rk_test", { secret = true })
  local inv = op.read("invoices", { status = "open" })[1]
  local accounts = op.read("accounts")
  local checking
  for _, a in ipairs(accounts) do if a.name == "Checking" then checking = a.id end end
  local r = op.propose{ summary = "Import 1 payment", changes = {
    { op = "recordPayment", contact = inv.contact, date = "2026-09-30", account = checking, amount = inv.balance,
      applications = { { document = inv.id, amount = inv.balance } } } } }
  return { message = r.applied and "imported" or "declined" }
end)
)LUA";

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "openbooks-gui-smoke";
    fs::remove_all(dir);
    const fs::path config = dir / "config";
#ifdef _WIN32
    _putenv_s("OPENBOOKS_CONFIG_DIR", config.string().c_str());
#else
    setenv("OPENBOOKS_CONFIG_DIR", config.string().c_str(), 1);
#endif
    const fs::path pluginDir = config / "plugins" / "lab";
    write(pluginDir / "plugin.json",
          R"({"schema":1,"id":"org.example.lab","name":"Lab","version":"1.0.0","publisher":"Tests","apps":{"openbooks":">=0.5"},)"
          R"("protocol":1,"runtime":"lua","main":"main.lua","permissions":["read:invoices","read:ledger","propose:payments","store"],)"
          R"("network":["api.example.com"],"commands":[{"id":"demo","title":"Demo"},{"id":"sync","title":"Sync"}]})");
    write(pluginDir / "main.lua", kMain);

    // Books with one open invoice.
    const std::string books = (dir / "books.obk").u8string();
    {
        ob::Book b;
        b.createDefaultChart();
        ob::Contact c;
        c.name = "Acme";
        const int acme = b.addContact(c);
        ob::Document d;
        d.kind = ob::DocKind::Invoice;
        d.contactId = acme;
        d.date = *ob::Date::parse("2026-09-01");
        d.dueDate = *ob::Date::parse("2026-10-01");
        ob::DocLine l;
        l.accountId = b.findAccount("Service Revenue").id;
        l.rate = *ob::Money::parse("300");
        d.lines.push_back(l);
        b.createDocument(d);
        b.save(books);
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1400, 900);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    obgui::loadFonts();
    {
        obgui::App app(books);
        obgui::AppTestAccess t{app};
        frame(app);
        CHECK(t.booksOpen());

        // ---- Manage Plugins: listed, not enabled.
        t.go(obgui::Screen::Plugins);
        frameUntil(app, [&] { return t.plugins().scanned; });
        CHECK(t.plugins().entries.size() == 1);
        CHECK(t.plugins().entries[0].status == opl::PluginStatus::NotEnabled);

        // ---- Approve it through the consent dialog's own logic.
        t.openConsent(t.plugins().entries[0]);
        frame(app);
        frame(app);
        CHECK(windowActive("Enable Plugin##plugin"));
        t.plugins().consentHosts["api.example.com"] = false;  // the user unticks the network host
        closeAllPopups(app);
        std::vector<std::string> perms;
        for (const auto& [p, on] : t.plugins().consentPermissions)
            if (on) perms.push_back(p);
        t.host().registry().enable(t.plugins().entries[0], perms, {});
        t.host().setUsedWithFile("org.example.lab", true);
        t.plugins().scanned = false;
        frameUntil(app, [&] { return t.plugins().scanned; });
        CHECK(t.plugins().entries[0].status == opl::PluginStatus::Enabled);
        CHECK(t.plugins().entries[0].grant->network.empty());

        // ---- A plugin window with every element type, and a panel.
        t.run("org.example.lab", "demo");
        CHECK(frameUntil(app, [&] { return t.views().size() == 2; }));
        frame(app);
        frame(app);
        CHECK(windowActive("Demo window##pluginview/org.example.lab/demo"));
        CHECK(t.screen() == obgui::Screen::PluginPanel);  // the panel opened
        for (int i = 0; i < 5; ++i) frame(app);
        obgui::PluginView* demo = nullptr;
        for (auto& v : t.views())
            if (v.viewId == "demo") demo = &v;
        CHECK(demo != nullptr);
        if (demo) {
            demo->text["key"] = "sk_secret";
            t.click(*demo, "save");
            CHECK(demo->text["key"].empty() || demo->text["key"].find_first_not_of('\0') == std::string::npos);  // wiped
        }
        CHECK(frameUntil(app, [&] { return t.views().size() == 1; }));  // the plugin closed it
        CHECK(t.notice().find("saved Acme") != std::string::npos);
        t.go(obgui::Screen::Plugins);
        for (int i = 0; i < 3; ++i) frame(app);

        // ---- Secret warning, then the review dialog.
        t.run("org.example.lab", "sync");
        CHECK(frameUntil(app, [&] { return !t.secrets().empty(); }));
        frame(app);
        CHECK(windowActive("Save Without a Password?##plugin"));
        {
            auto answer = std::move(t.secrets().front().answer);  // "Save anyway"
            t.secrets().pop_front();
            closeAllPopups(app);
            answer(true);
        }
        CHECK(frameUntil(app, [&] { return !t.reviews().empty(); }));
        frame(app);
        CHECK(windowActive("Review Changes##plugin"));
        CHECK(t.reviews().front().proposal.changes.size() == 1);
        {
            obgui::PluginReview& r = t.reviews().front();
            const std::string error = r.decide(true, {true});  // "Apply"
            CHECK(error.empty());
            t.reviews().pop_front();
            closeAllPopups(app);
        }
        CHECK(frameUntil(app, [&] { return t.notice().find("imported") != std::string::npos; }));
        CHECK(t.book().documentBalance(t.book().documents()[0].id).isZero());
        CHECK(t.book().plugins.get("org.example.lab", "apiKey") != nullptr);

        // ---- The detail screen with plugin data and a secret in an unencrypted file.
        t.plugins().selected = "org.example.lab";
        for (int i = 0; i < 3; ++i) frame(app);
        CHECK(t.notice().find("Something went wrong") == std::string::npos);
    }
    ImGui::DestroyContext();
    fs::remove_all(dir);
    std::cout << (g_failures ? "FAILED" : "gui smoke test passed") << " (" << g_failures << " failures)\n";
    return g_failures ? 1 : 0;
}
