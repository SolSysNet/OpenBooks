#include "app.hpp"

#include "imgui.h"
#include "imgui_internal.h"  // ErrorRecoveryStoreState / TryToRecoverState
#include "openbooks/cli.hpp"
#include "openbooks/util.hpp"
#include "platform.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace obgui {

namespace fs = std::filesystem;
using namespace ob;

namespace {

constexpr const char* kBooksFilterDescription = "OpenBooks files (*.obk)";
constexpr const char* kBooksFilterPattern = "*.obk";
constexpr float kNoticeSeconds = 6.0f;

fs::path pathFromUtf8(const std::string& s) { return fs::u8path(s); }

// A recent-files entry must be printable UTF-8 that converts to a path; anything else
// (e.g. a corrupted config) is ignored rather than trusted.
bool validRecentPath(const std::string& s) {
    if (s.empty() || s.size() > 4096) return false;
    for (char ch : s) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) return false;
    }
    try {
        (void)fs::u8path(s);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// The same file spelled differently (case on Windows, "..", slashes) counts once in Recent.
bool samePath(const std::string& a, const std::string& b) {
    if (a == b) return true;
    std::string na;
    std::string nb;
    try {
        std::error_code ec;
        if (fs::equivalent(fs::u8path(a), fs::u8path(b), ec)) return true;
        na = fs::u8path(a).lexically_normal().u8string();
        nb = fs::u8path(b).lexically_normal().u8string();
    } catch (const std::exception&) {
        return false;
    }
#ifdef _WIN32
    return iequals(na, nb);
#else
    return na == nb;
#endif
}

std::string documentsDirectory() {
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    if (!home || !*home) return fs::current_path().u8string();
    fs::path docs = fs::u8path(home) / "Documents";
    std::error_code ec;
    return fs::is_directory(docs, ec) ? docs.u8string() : fs::u8path(home).u8string();
}

std::string safeFileName(const std::string& company) {
    std::string out;
    for (char c : company) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') out += c;
    }
    out = trim(out);
    return out.empty() ? "books" : out;
}

}  // namespace

// ------------------------------------------------------------ lifecycle

App::App(std::string initialPath) {
    const std::string dir = configDirectory();
    configPath_ = (fs::u8path(dir) / "openbooks-gui.cfg").u8string();
    iniPath_ = (fs::u8path(dir) / "imgui.ini").u8string();
    ImGui::GetIO().IniFilename = iniPath_.c_str();
    loadConfig();
    applyTheme(darkTheme_);
    newCompany_.path = (fs::u8path(documentsDirectory()) / "My Company.obk").u8string();
    newCompany_.name = "My Company";
    initPlugins();

    if (!initialPath.empty()) {
        openBooks(initialPath);
    } else if (!recent_.empty()) {
        const std::string last = recent_.front();  // a copy: openBooks() reorders recent_
        std::error_code ec;
        if (validRecentPath(last) && fs::exists(pathFromUtf8(last), ec)) openBooks(last);
    }
}

App::~App() {
    resetPluginSession();
    plugins_.reset();  // stops every plugin
    saveConfig();
    clearPreviews();
}

void App::loadConfig() {
    std::ifstream in(pathFromUtf8(configPath_));
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "theme") darkTheme_ = value == "dark";
        if (key == "recent" && validRecentPath(value) && recent_.size() < 8 &&
            std::none_of(recent_.begin(), recent_.end(), [&](const std::string& r) { return samePath(r, value); }))
            recent_.push_back(value);
    }
}

void App::saveConfig() const {
    // Write a temporary file and rename it over the old one, so a crash mid-write can't leave
    // a truncated config behind.
    const fs::path target = pathFromUtf8(configPath_);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "theme=" << (darkTheme_ ? "dark" : "light") << '\n';
        for (const auto& r : recent_) {
            if (validRecentPath(r)) out << "recent=" << r << '\n';
        }
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
    }
}

void App::rememberRecent(std::string path) {  // by value: `path` may alias an element of recent_
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const std::string& r) { return samePath(r, path); }),
                  recent_.end());
    recent_.insert(recent_.begin(), path);
    if (recent_.size() > 8) recent_.resize(8);
    saveConfig();
}

bool App::openBooks(std::string path, std::string password) {  // by value, see rememberRecent()
    lastOpenError_.clear();
    std::unique_ptr<FileKey> key;
    try {
        if (password.empty() && Book::isEncryptedFile(path)) {
            // Ask for the password; the unlock dialog calls back in here with it.
            unlock_ = UnlockState{};
            unlock_.path = path;
            requestPopup("Unlock Books");
            return false;
        }
        Book loaded = Book::load(path, password, &key);
        crypto::wipe(password);
        resetPluginSession();  // plugins never carry over from one file to another
        book_ = std::move(loaded);
        key_ = std::move(key);  // null for unencrypted files
    } catch (const WrongPasswordError& e) {
        crypto::wipe(password);
        lastOpenError_ = e.what();  // shown inside the unlock dialog
        return false;
    } catch (const std::exception& e) {
        crypto::wipe(password);
        lastOpenError_ = e.what();
        notify(std::string("Could not open books: ") + e.what(), true);
        return false;
    }
    path_ = path;
    ++version_;
    screen_ = Screen::Dashboard;
    customers_ = {};
    vendors_ = {};
    invoices_ = {};
    estimates_ = {};
    creditMemos_ = {};
    salesReceipts_ = {};
    recurringList_ = {};
    bills_ = {};
    payments_ = {};
    accountsList_ = {};
    itemsList_ = {};
    register_ = {};
    reconcile_ = {};
    journal_ = {};
    report_ = {};
    company_ = {};
    rememberRecent(path);
    notify("Opened " + book_->company.name);
    return true;
}

void App::closeBooks() {
    resetPluginSession();
    book_.reset();
    key_.reset();  // wipes the key
    path_.clear();
    clearPreviews();
    ++version_;
}

// "Preview PDF" writes unencrypted copies to a temp folder; remove them when they're done with.
void App::clearPreviews() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "OpenBooks";
    if (ec || !fs::is_directory(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".pdf") fs::remove(entry.path(), ec);
    }
}

bool App::commit(const std::function<void(Book&)>& change, std::string* error, const std::string& success) {
    if (!book_) return false;
    try {
        Book draft = *book_;
        change(draft);
        draft.save(path_, key_.get());  // encrypted again when the file has a password
        *book_ = std::move(draft);
        ++version_;
        if (error) error->clear();
        if (!success.empty()) notify(success);
        if (plugins_) plugins_->event("fileSaved", opl::json::Object{});
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        else notify(e.what(), true);
        return false;
    }
}

const Derived& App::derived() {
    if (derived_.version == version_ || !book_) return derived_;
    const Book& b = *book_;
    Derived d;
    d.version = version_;
    // Same rules as Book::documentBalance, computed in one pass instead of per document.
    for (const auto& doc : b.documents()) {
        if (doc.voided) continue;
        if (doc.kind == DocKind::Invoice || doc.kind == DocKind::Bill || doc.kind == DocKind::CreditMemo)
            d.documentBalance[doc.id] = doc.total();
    }
    for (const auto& p : b.payments()) {
        if (p.voided) continue;
        for (const auto& a : p.applications) d.documentBalance[a.documentId] -= a.amount;
    }
    for (const auto& cm : b.documents()) {
        if (cm.kind != DocKind::CreditMemo || cm.voided) continue;
        for (const auto& a : cm.applications) {
            d.documentBalance[a.documentId] -= a.amount;  // the invoice owes less
            d.documentBalance[cm.id] -= a.amount;         // the credit has less left
        }
    }
    for (const auto& doc : b.documents()) {
        if (doc.voided) continue;
        const auto it = d.documentBalance.find(doc.id);
        if (it == d.documentBalance.end()) continue;
        if (doc.kind == DocKind::CreditMemo) d.contactBalance[doc.contactId] -= it->second;
        else d.contactBalance[doc.contactId] += it->second;
    }
    for (const auto& p : b.payments()) {
        if (!p.voided) d.contactBalance[p.contactId] -= p.unapplied();
    }
    d.accountBalance = b.balances(Period{});
    for (const auto& t : b.transactions()) d.transactionsByDate.push_back(t.id);
    std::stable_sort(d.transactionsByDate.begin(), d.transactionsByDate.end(), [&b](int x, int y) {
        const Date dx = b.transaction(x).date;
        const Date dy = b.transaction(y).date;
        return dx != dy ? dx < dy : x < y;
    });
    derived_ = std::move(d);
    return derived_;
}

std::string App::windowTitle() const {
    if (!book_) return "OpenBooks";
    return book_->company.name + " - OpenBooks (" + fs::u8path(path_).filename().u8string() + ")";
}

bool App::wantsFrequentRedraw() const {
    if (plugins_ && plugins_->busy()) return true;  // keep polling while plugins are working
    if (notice_.empty()) return false;
    const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - noticeTime_).count();
    return age < kNoticeSeconds + 1.0f;
}

void App::notify(std::string message, bool error) {
    notice_ = std::move(message);
    noticeIsError_ = error;
    noticeTime_ = std::chrono::steady_clock::now();
}

void App::requestPopup(const char* name) { pendingPopup_ = name; }

void App::confirm(std::string title, std::string message, std::string button, std::function<void()> action) {
    confirm_ = ConfirmRequest{std::move(title), std::move(message), std::move(button), std::move(action)};
    requestPopup("Confirm##dialog");
}

void App::go(Screen screen) { screen_ = screen; }

void App::chooseOpenFile() {
    if (nativeFileDialogsAvailable()) {
        if (auto path = openFileDialog("Open books", {kBooksFilterDescription, kBooksFilterPattern})) openBooks(*path);
    } else {
        typedOpenPath_.clear();
        requestPopup("Open Books##typed");
    }
}

// ----------------------------------------------------------------- frame

// Draws one frame. Any exception thrown while drawing (a bug, or data the UI didn't expect)
// is caught here: ImGui's window stack is unwound, the user sees the message, and the app
// keeps running instead of aborting. The books themselves are never left half-changed,
// because every change goes through commit().
void App::frame() {
    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);
    try {
        drawFrame();
    } catch (const std::exception& e) {
        ImGuiIO& io = ImGui::GetIO();
        const bool asserts = io.ConfigErrorRecoveryEnableAssert;
        io.ConfigErrorRecoveryEnableAssert = false;  // we are recovering on purpose
        ImGui::ErrorRecoveryTryToRecoverState(&recovery);
        io.ConfigErrorRecoveryEnableAssert = asserts;
        notify(std::string("Something went wrong: ") + e.what(), true);
        if (book_) screen_ = Screen::Dashboard;  // don't keep re-entering the failing screen
    }
}

void App::drawFrame() {
    if (plugins_) plugins_->poll();  // plugin callbacks run here, on the UI thread
    drawMenuBar();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##host", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    if (!book_) {
        drawWelcome();
    } else {
        const float statusHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
        const float sidebarWidth = ImGui::GetFontSize() * 13.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 12));
        ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, -statusHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
        drawSidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
        ImGui::BeginChild("##content", ImVec2(0, -statusHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
        switch (screen_) {
            case Screen::Dashboard: drawDashboard(); break;
            case Screen::Customers: drawContacts(ContactKind::Customer); break;
            case Screen::Vendors: drawContacts(ContactKind::Vendor); break;
            case Screen::Invoices: drawDocuments(DocKind::Invoice); break;
            case Screen::Bills: drawDocuments(DocKind::Bill); break;
            case Screen::ReceivePayment: drawPaymentForm(PaymentKind::Received); break;
            case Screen::PayBills: drawPaymentForm(PaymentKind::Paid); break;
            case Screen::Payments: drawPayments(); break;
            case Screen::Register: drawRegister(); break;
            case Screen::Reconcile: drawReconcile(); break;
            case Screen::Journal: drawJournal(); break;
            case Screen::Accounts: drawAccounts(); break;
            case Screen::Items: drawItems(); break;
            case Screen::Reports: drawReports(); break;
            case Screen::Company: drawCompany(); break;
            case Screen::DocumentEditor: drawDocumentEditor(); break;
            case Screen::Estimates: drawDocuments(DocKind::Estimate); break;
            case Screen::CreditMemos: drawDocuments(DocKind::CreditMemo); break;
            case Screen::SalesReceipts: drawDocuments(DocKind::SalesReceipt); break;
            case Screen::Recurring: drawRecurring(); break;
            case Screen::Plugins: drawPluginsScreen(); break;
            case Screen::PluginPanel: drawPluginPanel(); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        drawStatusBar();
    }
    ImGui::End();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) chooseOpenFile();

    if (!pendingPopup_.empty()) {
        ImGui::OpenPopup(pendingPopup_.c_str());
        pendingPopup_.clear();
    }
    drawModals();
}

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool open = book_.has_value();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Company...")) closeBooks();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) chooseOpenFile();
        if (ImGui::BeginMenu("Open Recent", !recent_.empty())) {
            for (const auto& r : std::vector<std::string>(recent_)) {
                if (ImGui::MenuItem(r.c_str())) openBooks(r);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close Books", nullptr, false, open)) closeBooks();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create", open)) {
        if (ImGui::MenuItem("Estimate")) startDocument(DocKind::Estimate, 0, Screen::Estimates);
        if (ImGui::MenuItem("Invoice")) startDocument(DocKind::Invoice, 0, Screen::Invoices);
        if (ImGui::MenuItem("Sales Receipt")) startDocument(DocKind::SalesReceipt, 0, Screen::SalesReceipts);
        if (ImGui::MenuItem("Credit Memo")) startDocument(DocKind::CreditMemo, 0, Screen::CreditMemos);
        if (ImGui::MenuItem("Recurring Invoice")) startRecurringEditor(0);
        if (ImGui::MenuItem("Receive Payment")) startPayment(PaymentKind::Received, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Bill")) startDocument(DocKind::Bill, 0, Screen::Bills);
        if (ImGui::MenuItem("Pay Bills")) startPayment(PaymentKind::Paid, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Expense")) openQuickEntry(TxnKind::Expense);
        if (ImGui::MenuItem("Deposit")) openQuickEntry(TxnKind::Deposit);
        if (ImGui::MenuItem("Transfer")) openQuickEntry(TxnKind::Transfer);
        if (ImGui::MenuItem("Journal Entry")) go(Screen::Journal);
        ImGui::Separator();
        if (ImGui::MenuItem("Import Bank Statement (CSV)...")) {
            import_.error.clear();
            requestPopup("Import Bank Statement");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Dark Theme", nullptr, darkTheme_)) {
            darkTheme_ = !darkTheme_;
            applyTheme(darkTheme_);
            saveConfig();
        }
        ImGui::EndMenu();
    }
    drawPluginsMenu();
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About OpenBooks")) requestPopup("About OpenBooks");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::drawSidebar() {
    struct Entry {
        const char* label;
        Screen screen;
    };
    struct Group {
        const char* title;
        std::vector<Entry> entries;
    };
    static const std::vector<Group> groups = {
        {"OVERVIEW", {{"Dashboard", Screen::Dashboard}, {"Reports", Screen::Reports}}},
        {"SALES",
         {{"Customers", Screen::Customers},
          {"Estimates", Screen::Estimates},
          {"Invoices", Screen::Invoices},
          {"Sales Receipts", Screen::SalesReceipts},
          {"Credit Memos", Screen::CreditMemos},
          {"Recurring Invoices", Screen::Recurring},
          {"Receive Payment", Screen::ReceivePayment}}},
        {"PURCHASES", {{"Vendors", Screen::Vendors}, {"Bills", Screen::Bills}, {"Pay Bills", Screen::PayBills}}},
        {"BANKING", {{"Registers", Screen::Register}, {"Reconcile", Screen::Reconcile}, {"Payments", Screen::Payments}}},
        {"ACCOUNTING", {{"Chart of Accounts", Screen::Accounts}, {"Journal Entry", Screen::Journal},
                        {"Products & Services", Screen::Items}, {"Company Settings", Screen::Company}}},
    };

    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 1.15f);
    ImGui::PushStyleColor(ImGuiCol_Text, colorAccent());
    ImGui::TextUnformatted("OpenBooks");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ui::Muted(book_->company.name.c_str());
    ImGui::PopTextWrapPos();

    for (const auto& g : groups) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushFont(nullptr, kBaseFontSize * 0.8f);
        ui::Muted(g.title);
        ImGui::PopFont();
        for (const auto& e : g.entries) {
            const bool selected = screen_ == e.screen ||
                                  (screen_ == Screen::DocumentEditor && e.screen == editor_.returnTo);
            if (ImGui::Selectable(e.label, selected)) go(e.screen);
        }
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushFont(nullptr, kBaseFontSize * 0.8f);
    ui::Muted("PLUGINS");
    ImGui::PopFont();
    if (ImGui::Selectable("Manage Plugins", screen_ == Screen::Plugins)) go(Screen::Plugins);
    for (const PluginView& v : pluginViews_) {
        if (v.kind != "panel") continue;
        const std::string key = v.pluginId + "/" + v.viewId;
        ImGui::PushID(key.c_str());
        if (ImGui::Selectable(v.title.c_str(), screen_ == Screen::PluginPanel && pluginPanel_ == key)) {
            pluginPanel_ = key;
            go(Screen::PluginPanel);
        }
        ImGui::PopID();
    }
}

void App::drawStatusBar() {
    ImGui::Separator();
    ImGui::SetCursorPosX(10.0f);
    ImGui::AlignTextToFramePadding();
    ui::Muted(path_.c_str());
    if (key_) {
        ImGui::SameLine();
        ui::Badge("Password protected", colorPositive());
    }
    drawPluginStatus();
    if (notice_.empty()) return;
    const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - noticeTime_).count();
    if (age > kNoticeSeconds) return;
    const float width = ImGui::CalcTextSize(notice_.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - width - 16.0f, ImGui::GetCursorPosX() + 20.0f));
    ImVec4 color = noticeIsError_ ? colorNegative() : colorPositive();
    color.w = std::min(1.0f, (kNoticeSeconds - age) / 1.0f);
    ImGui::TextColored(color, "%s", notice_.c_str());
}

// --------------------------------------------------------------- welcome

void App::drawWelcome() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::min(avail.x - 40.0f, ImGui::GetFontSize() * 36.0f);
    ImGui::SetCursorPos(ImVec2((avail.x - width) * 0.5f, std::max(20.0f, avail.y * 0.08f)));
    ImGui::BeginChild("##welcome", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);

    ImGui::PushFont(g_fonts.bold, kBaseFontSize * 2.4f);
    ImGui::PushStyleColor(ImGuiCol_Text, colorAccent());
    ImGui::TextUnformatted("OpenBooks");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ui::Muted("Open source accounting for small business.");
    ImGui::Dummy(ImVec2(0, 14));

    ui::SubHeading("Create a new company");
    ImGui::Separator();
    const float labelColumn = ImGui::GetFontSize() * 8.0f;
    ui::FormLabel("Company name", labelColumn);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ui::InputString("##name", newCompany_.name)) {
        newCompany_.path = (fs::u8path(newCompany_.path).parent_path() / fs::u8path(safeFileName(newCompany_.name) + ".obk")).u8string();
    }
    ui::FormLabel("Save books to", labelColumn);
    const float browseWidth = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(browseWidth + ImGui::GetStyle().ItemSpacing.x));
    ui::InputString("##path", newCompany_.path);
    if (nativeFileDialogsAvailable()) {
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            if (auto p = saveFileDialog("Create books", {kBooksFilterDescription, kBooksFilterPattern}, "obk",
                                        safeFileName(newCompany_.name) + ".obk"))
                newCompany_.path = *p;
        }
    }
    ImGui::SetCursorPosX(labelColumn);
    ImGui::Checkbox("Start with a standard chart of accounts", &newCompany_.starterChart);
    ImGui::SetCursorPosX(labelColumn);
    ImGui::Checkbox("Protect with a password (encrypted with AES-256)", &newCompany_.protect);
    if (newCompany_.protect) {
        ui::FormLabel("Password", labelColumn);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ui::InputString("##newpw", newCompany_.password, ImGuiInputTextFlags_Password);
        ui::FormLabel("Repeat password", labelColumn);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ui::InputString("##newpw2", newCompany_.repeat, ImGuiInputTextFlags_Password);
        ImGui::SetCursorPosX(labelColumn);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ui::Muted("There is no way to recover a forgotten password. Keep it somewhere safe.");
        ImGui::PopTextWrapPos();
    }
    ImGui::SetCursorPosX(labelColumn);
    if (ui::PrimaryButton("Create Company")) {
        newCompany_.error.clear();
        const std::string name = trim(newCompany_.name);
        const std::string path = trim(newCompany_.path);
        std::error_code ec;
        if (name.empty()) newCompany_.error = "Enter a company name.";
        else if (path.empty()) newCompany_.error = "Choose where to save the books.";
        else if (fs::exists(pathFromUtf8(path), ec)) newCompany_.error = "That file already exists. Open it instead, or pick another name.";
        else if (newCompany_.protect && newCompany_.password != newCompany_.repeat) newCompany_.error = "The passwords don't match.";
        else {
            try {
                std::unique_ptr<FileKey> key;
                if (newCompany_.protect) key = FileKey::fromNewPassword(newCompany_.password);
                Book b;
                b.company.name = name;
                if (newCompany_.starterChart) b.createDefaultChart();
                fs::create_directories(pathFromUtf8(path).parent_path(), ec);
                b.save(path, key.get());
                std::string password = newCompany_.password;
                crypto::wipe(newCompany_.password);
                crypto::wipe(newCompany_.repeat);
                if (openBooks(path, std::move(password))) notify("Created " + name);
            } catch (const std::exception& e) {
                newCompany_.error = e.what();
            }
        }
    }
    ui::ErrorText(newCompany_.error);

    ImGui::Dummy(ImVec2(0, 18));
    ui::SubHeading("Open existing books");
    ImGui::Separator();
    if (ImGui::Button("Open...")) chooseOpenFile();
    if (!recent_.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ui::Muted("Recent");
        for (const auto& r : std::vector<std::string>(recent_)) {
            ImGui::PushID(r.c_str());
            if (ImGui::TextLink(fs::u8path(r).filename().u8string().c_str())) openBooks(r);
            ImGui::SameLine();
            ui::Muted(r.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (!notice_.empty() && noticeIsError_) {
        ImGui::SetCursorPosX((avail.x - width) * 0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextColored(colorNegative(), "%s", notice_.c_str());
        ImGui::PopTextWrapPos();
    }
}

// ---------------------------------------------------------------- modals

void App::drawModals() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm##dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ui::SubHeading(confirm_.title.c_str());
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(confirm_.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6));
        if (ui::DangerButton(confirm_.button.c_str())) {
            auto action = confirm_.action;
            ImGui::CloseCurrentPopup();
            if (action) action();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Books##typed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to an OpenBooks file (.obk):");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 28.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ui::InputString("##openpath", typedOpenPath_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ui::PrimaryButton("Open") || enter) {
            if (openBooks(trim(typedOpenPath_))) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About OpenBooks", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ui::Heading("OpenBooks");
        ImGui::Text("Version %s", kVersion);
        ImGui::TextUnformatted("Open source double-entry accounting for small business.");
        ui::Muted("MIT License. Built with Dear ImGui.");
        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    drawUnlockModal();  // needed before any books are open
    if (!book_) return;
    drawContactModal();
    drawAccountModal();
    drawItemModal();
    drawQuickEntryModal();
    drawImportModal();
    drawRecategorizeModal();
    drawTransactionModal();
    drawApplyCreditModal();
    drawPasswordModal();
    drawPluginModals();
}

// ------------------------------------------------------------- dashboard

void App::drawDashboard() {
    const Book& b = *book_;
    const Derived& d = derived();
    const Date today = Date::today();

    ui::Heading(b.company.name.c_str());

    // Quick actions, right-aligned on the heading line.
    {
        const char* labels[] = {"New Invoice", "Receive Payment", "Enter Bill", "Record Expense"};
        float total = 0;
        for (const char* l : labels) total += ImGui::CalcTextSize(l).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > total) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - total + ImGui::GetStyle().ItemSpacing.x);
        if (ui::PrimaryButton(labels[0])) startDocument(DocKind::Invoice, 0, Screen::Invoices);
        ImGui::SameLine();
        if (ImGui::Button(labels[1])) startPayment(PaymentKind::Received, 0);
        ImGui::SameLine();
        if (ImGui::Button(labels[2])) startDocument(DocKind::Bill, 0, Screen::Bills);
        ImGui::SameLine();
        if (ImGui::Button(labels[3])) openQuickEntry(TxnKind::Expense);
    }
    ui::Muted(("Today is " + today.str()).c_str());

    // Recurring invoices waiting to be created.
    if (const int due = b.dueRecurringCount(today); due > 0) {
        ImGui::Dummy(ImVec2(0, 4));
        ImVec4 bg = colorWarning();
        bg.w = 0.14f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::BeginChild("##recurringDue", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d recurring invoice%s due.", due, due == 1 ? " is" : "s are");
        ImGui::SameLine();
        bool create = ui::PrimaryButton("Create now");
        ImGui::SameLine();
        if (ImGui::Button("Review")) go(Screen::Recurring);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (create) {
            createDueRecurring();
            return;
        }
    }
    ImGui::Dummy(ImVec2(0, 10));

    // Figures.
    // Bank accounts: asset accounts that money actually moves through, plus obviously named ones.
    std::set<int> bankAccounts;
    for (const auto& t : b.transactions()) {
        if (t.voided) continue;
        for (const auto& s : t.splits) {
            bool moneySide = false;
            switch (t.kind) {
                case TxnKind::CustomerPayment:
                case TxnKind::VendorPayment:
                case TxnKind::Import:
                case TxnKind::Transfer: moneySide = true; break;
                case TxnKind::Expense: moneySide = s.amount < Money(); break;
                case TxnKind::Deposit: moneySide = s.amount > Money(); break;
                default: break;
            }
            const Account& a = b.account(s.accountId);
            if (moneySide && a.type == AccountType::Asset && ui::isMoneyAccount(b, a)) bankAccounts.insert(a.id);
        }
    }
    for (const auto& a : b.accounts()) {
        const std::string n = toLower(a.name);
        if (a.type == AccountType::Asset && (n.find("checking") != std::string::npos || n.find("saving") != std::string::npos ||
                                             n.find("cash") != std::string::npos || n.find("bank") != std::string::npos))
            bankAccounts.insert(a.id);
    }
    Money cash;
    for (int id : bankAccounts) {
        auto it = d.accountBalance.find(id);
        if (it != d.accountBalance.end()) cash += it->second;
    }
    Money receivable, overdue, payable, billsOverdue;
    int overdueCount = 0;
    for (const auto& doc : b.documents()) {
        if (doc.voided) continue;
        const auto it = d.documentBalance.find(doc.id);
        const Money bal = it == d.documentBalance.end() ? Money() : it->second;
        if (bal.isZero()) continue;
        if (doc.kind == DocKind::Invoice) {
            receivable += bal;
            if (doc.dueDate < today) {
                overdue += bal;
                ++overdueCount;
            }
        } else if (doc.kind == DocKind::Bill) {
            payable += bal;
            if (doc.dueDate < today) billsOverdue += bal;
        } else if (doc.kind == DocKind::CreditMemo) {
            receivable -= bal;  // unapplied customer credit
        }
    }
    const Date fyStart = b.fiscalYearStart(today);
    const auto ytd = b.balances(Period{fyStart, today});
    Money income, expenses;
    for (const auto& a : b.accounts()) {
        const auto it = ytd.find(a.id);
        if (it == ytd.end()) continue;
        if (a.type == AccountType::Income) income -= it->second;
        if (a.type == AccountType::Expense) expenses += it->second;
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float cardWidth = (ImGui::GetContentRegionAvail().x - spacing * 3) / 4.0f;
    ui::StatCard("##cash", "Bank balance", cash.formatted(), cash < Money() ? colorNegative() : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                 std::to_string(bankAccounts.size()) + " bank & cash account(s)", cardWidth);
    ImGui::SameLine();
    ui::StatCard("##ar", "Open invoices", receivable.formatted(), ImGui::GetStyleColorVec4(ImGuiCol_Text),
                 overdueCount ? std::to_string(overdueCount) + " overdue: " + overdue.formatted() : "Nothing overdue",
                 cardWidth);
    ImGui::SameLine();
    ui::StatCard("##ap", "Open bills", payable.formatted(), ImGui::GetStyleColorVec4(ImGuiCol_Text),
                 billsOverdue.isZero() ? "Nothing overdue" : "Overdue: " + billsOverdue.formatted(), cardWidth);
    ImGui::SameLine();
    const Money net = income - expenses;
    ui::StatCard("##ni", "Net income, fiscal year to date", net.formatted(), net < Money() ? colorNegative() : colorPositive(),
                 "Income " + income.formatted() + "  |  Expenses " + expenses.formatted(), cardWidth);

    ImGui::Dummy(ImVec2(0, 10));
    const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    const float panelHeight = ImGui::GetFontSize() * 14.0f;
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    ImGui::BeginGroup();
    ui::SubHeading("Bank & credit card accounts");
    if (ImGui::BeginTable("##banks", 2, tableFlags, ImVec2(half, panelHeight))) {
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
        ImGui::TableHeadersRow();
        for (const auto& a : b.accounts()) {
            if (!ui::isMoneyAccount(b, a) || !a.active) continue;
            const auto it = d.accountBalance.find(a.id);
            const Money bal = it == d.accountBalance.end() ? Money() : naturalSign(a.type, it->second);
            if (bal.isZero() && !bankAccounts.count(a.id)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(a.id);
            if (ImGui::Selectable(accountLabel(a).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                register_.accountId = a.id;
                go(Screen::Register);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ui::MoneyText(bal);
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ui::SubHeading("Overdue invoices");
    if (ImGui::BeginTable("##overdue", 4, tableFlags, ImVec2(half, panelHeight))) {
        ImGui::TableSetupColumn("Invoice", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        ImGui::TableSetupColumn("Customer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Days late", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableHeadersRow();
        for (const auto& doc : b.documents()) {
            if (doc.kind != DocKind::Invoice || doc.voided || doc.dueDate >= today) continue;
            const auto it = d.documentBalance.find(doc.id);
            if (it == d.documentBalance.end() || it->second.isZero()) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(doc.id);
            if (ImGui::Selectable(doc.number.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                invoices_.selectedId = doc.id;
                go(Screen::Invoices);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(b.contact(doc.contactId).name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(colorNegative(), "%d", today - doc.dueDate);
            ImGui::TableSetColumnIndex(3);
            ui::MoneyText(it->second);
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 10));
    ui::SubHeading("Recent activity");
    if (ImGui::BeginTable("##recent", 5, tableFlags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
        ImGui::TableSetupColumn("Payee / Memo", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (auto it = d.transactionsByDate.rbegin(); it != d.transactionsByDate.rend() && shown < 12; ++it) {
            const Transaction& t = b.transaction(*it);
            if (t.voided) continue;
            ++shown;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(t.id);
            if (ImGui::Selectable(t.date.str().c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                detailTxn_ = t.id;
                requestPopup("Transaction");
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(toString(t.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(t.ref.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(t.payee.empty() ? t.memo.c_str() : t.payee.c_str());
            ImGui::TableSetColumnIndex(4);
            ui::MoneyText(t.debitTotal(), true, false);
        }
        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(3);
            ui::Muted("No transactions yet. Create an invoice or record an expense to get started.");
        }
        ImGui::EndTable();
    }
}

}  // namespace obgui
