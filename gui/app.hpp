#pragma once

// The OpenBooks desktop application. Platform-independent: the platform main loop
// (main_win32_dx11.cpp / main_glfw_opengl3.cpp) calls frame() once per frame.
//
// Every change goes through App::commit(), which applies it to a copy of the books,
// saves to disk, and only then swaps the copy in. The UI therefore never holds on to
// references into the Book; it remembers ids.

#include "openbooks/book.hpp"
#include "openbooks/crypto.hpp"
#include "openbooks/plugins.hpp"
#include "openbooks/reports.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace obgui {

enum class Screen {
    Dashboard,
    Customers,
    Invoices,
    ReceivePayment,
    Vendors,
    Bills,
    PayBills,
    Payments,
    Register,
    Reconcile,
    Journal,
    Accounts,
    Items,
    Reports,
    Company,
    DocumentEditor,
    Estimates,
    CreditMemos,
    SalesReceipts,
    Recurring,
    Plugins,      // Manage Plugins
    PluginPanel,  // a screen a plugin shows (pluginPanel_)
};

// ------------------------------------------------------------ screen state

struct LineDraft {
    int itemId = 0;
    std::string description;
    int accountId = 0;
    std::string quantity = "1";
    std::string rate;
    bool taxable = true;
};

struct DocumentEditorState {
    ob::DocKind kind = ob::DocKind::Invoice;
    int contactId = 0;
    ob::Date date;
    ob::Date dueDate;
    bool dueDateEdited = false;
    std::string number;
    std::string memo;
    std::string taxRate;
    std::vector<LineDraft> lines;
    std::string error;
    Screen returnTo = Screen::Invoices;
    int depositAccountId = 0;  // sales receipts
    // Recurring-template mode: the same editor builds a RecurringInvoice instead of a document.
    bool templateMode = false;
    int recurringId = 0;  // 0 = new template
    std::string templateName;
    int frequency = 1;  // 0 weekly, 1 monthly, 2 yearly
    int interval = 1;
    bool hasEndDate = false;
    ob::Date endDate;
    bool templateActive = true;
};

struct PaymentFormState {
    int contactId = 0;
    ob::Date date;
    std::string amount;
    int accountId = 0;
    std::string ref;
    std::string memo;
    std::map<int, bool> selected;          // document id -> checked
    std::map<int, std::string> applied;    // document id -> amount text
    std::string error;
};

struct ApplyCreditState {
    int contactId = 0;
    int creditMemoId = 0;
    int invoiceId = 0;
    std::string amount;
    std::string error;
};

struct ListState {
    std::string search;
    int filter = 0;
    int selectedId = 0;
    bool showInactive = false;
};

struct RegisterState {
    int accountId = 0;
    bool limitDates = false;
    ob::Date from;
    ob::Date to;
    int selectedTxn = 0;
};

struct QuickEntryState {
    ob::TxnKind kind = ob::TxnKind::Expense;
    ob::Date date;
    int moneyAccountId = 0;  // bank/card the money leaves or enters
    int otherAccountId = 0;  // category, or destination for transfers
    std::string amount;
    std::string payee;
    std::string memo;
    std::string ref;
    std::string error;
};

struct ImportState {
    std::string path;
    int accountId = 0;
    int dateColumn = 1;
    int descriptionColumn = 2;
    int amountColumn = 3;
    bool hasHeader = true;
    bool negate = false;
    int offsetAccountId = 0;
    std::string loadedPath;
    std::string text;
    std::vector<std::vector<std::string>> rows;
    std::string error;
};

struct RecategorizeState {
    int txnId = 0;
    int fromAccountId = 0;
    int toAccountId = 0;
    std::string error;
};

struct ReconcileState {
    int accountId = 0;
    ob::Date through;
    std::string statementBalance;
    std::string error;
};

struct JournalLineDraft {
    int accountId = 0;
    std::string debit;
    std::string credit;
    std::string memo;
};

struct JournalState {
    ob::Date date;
    std::string ref;
    std::string memo;
    std::vector<JournalLineDraft> lines;
    std::string error;
};

struct AccountForm {
    int editingId = 0;  // 0 = new
    std::string name;
    std::string number;
    std::string description;
    int type = 0;
    bool active = true;
    std::string error;
};

struct ContactForm {
    int editingId = 0;
    ob::ContactKind kind = ob::ContactKind::Customer;
    std::string name;
    std::string email;
    std::string phone;
    std::string address;
    std::string terms;  // blank = company default
    bool active = true;
    std::string error;
};

struct ItemForm {
    std::string name;
    std::string description;
    std::string price;
    int incomeAccountId = 0;
    int expenseAccountId = 0;
    bool taxable = true;
    std::string error;
};

enum class ReportKind { ProfitLoss, BalanceSheet, TrialBalance, ArAging, ApAging, Journal, AccountRegister };
enum class DatePreset { ThisMonth, LastMonth, ThisQuarter, ThisFiscalYear, LastFiscalYear, AllDates, Custom };

struct ReportState {
    ReportKind kind = ReportKind::ProfitLoss;
    DatePreset preset = DatePreset::ThisFiscalYear;
    ob::Date from;
    ob::Date to;
    int accountId = 0;
    bool initialized = false;
    std::uint64_t builtVersion = 0;
    std::string builtKey;
    ob::Table table;
};

struct CompanyForm {
    bool loaded = false;
    std::string name;
    std::string address;
    std::string email;
    std::string phone;
    std::string invoiceFooter;
    int paperSize = 0;  // 0 Letter, 1 A4
    int fiscalYearStartMonth = 1;
    std::string terms;
    std::string nextInvoice;
    bool closed = false;
    ob::Date closedThrough;
    int receivables = 0;
    int payables = 0;
    int salesTax = 0;
    int retainedEarnings = 0;
    std::string error;
};

struct NewCompanyForm {
    std::string name;
    std::string path;
    bool starterChart = true;
    bool protect = false;  // create the file encrypted
    std::string password;
    std::string repeat;
    std::string error;
};

// Unlocking an encrypted file, and setting / changing / removing its password.
struct UnlockState {
    std::string path;
    std::string password;
    std::string error;
};

struct PasswordForm {
    int mode = 0;  // 0 set, 1 change, 2 remove
    std::string current;
    std::string next;
    std::string repeat;
    std::string error;
};

struct ConfirmRequest {
    std::string title;
    std::string message;
    std::string button;
    std::function<void()> action;
};

// ------------------------------------------------------------- plugins (app_plugins.cpp)

// A window or screen a plugin described (docs/design.md, section 10), with what the user typed.
struct PluginView {
    std::string pluginId;
    std::string viewId;
    std::string title;
    std::string kind;          // "modal" or "panel"
    opl::json::Value body;     // the element list
    std::map<std::string, std::string> text;  // field id -> text (text, secret, money, choice, ...)
    std::map<std::string, bool> flags;        // bool fields
    std::map<std::string, int> ids;           // account / contact / invoice pickers
    std::map<std::string, ob::Date> dates;
    std::string error;
    bool waiting = false;  // an event is with the plugin
    bool popupOpen = false;
    bool closing = false;  // the plugin asked to close it
};

struct PluginReview {
    ob::plugins::Proposal proposal;
    ob::plugins::Delegate::Decide decide;
    std::vector<char> selected;
    std::string error;
};

struct PluginSecretQuestion {
    std::string pluginName;
    std::function<void(bool)> answer;
};

struct PluginsScreenState {
    std::vector<opl::PluginEntry> entries;  // cached: scanning hashes every plugin file
    bool scanned = false;
    std::string selected;  // plugin id, or the folder name of an invalid plugin
    std::string registryError;
    // The approval dialog.
    std::string consentFolder;
    std::map<std::string, bool> consentPermissions;
    std::map<std::string, bool> consentHosts;
    std::string consentError;
    // The read-only file and log viewers.
    std::string viewerTitle;
    std::vector<std::pair<std::string, std::string>> files;  // path, text
    int viewerFile = 0;
    std::string logText;
};

class App;

// What the plugin host asks of the desktop app.
class GuiPluginDelegate : public ob::plugins::Delegate {
public:
    explicit GuiPluginDelegate(App& app) : app_(app) {}
    const ob::Book* books() override;
    bool encrypted() override;
    void commit(const std::function<void(ob::Book&)>& change) override;
    void review(ob::plugins::Proposal proposal, Decide decide) override;
    void confirmUnencryptedSecret(const std::string& pluginName, std::function<void(bool)> answer) override;
    bool pluginUi(const std::string& pluginId, const std::string& method, const opl::json::Value& params) override;
    void notify(const std::string& pluginId, const std::string& level, const std::string& message) override;
    std::optional<std::string> saveFile(const std::string& suggestedName, const std::string& content) override;

private:
    App& app_;
};

// Values derived from the books, rebuilt only when the books change.
struct Derived {
    std::uint64_t version = 0;
    std::unordered_map<int, ob::Money> documentBalance;
    std::unordered_map<int, ob::Money> contactBalance;
    std::map<int, ob::Money> accountBalance;  // raw, all dates
    std::vector<int> transactionsByDate;      // ids, oldest first
};

// ------------------------------------------------------------------- app

class App {
public:
    explicit App(std::string initialPath);
    ~App();

    void frame();
    bool quitRequested() const { return quit_; }
    std::string windowTitle() const;
    bool wantsFrequentRedraw() const;  // true while a notification is fading

private:
    // ---- books & persistence
    bool openBooks(std::string path, std::string password = {});
    void closeBooks();
    bool commit(const std::function<void(ob::Book&)>& change, std::string* error = nullptr,
                const std::string& success = {});
    const Derived& derived();
    void loadConfig();
    void saveConfig() const;
    void rememberRecent(std::string path);

    // ---- chrome
    void drawFrame();
    void drawMenuBar();
    void drawSidebar();
    void drawStatusBar();
    void drawWelcome();
    void drawModals();
    void go(Screen screen);
    void notify(std::string message, bool error = false);
    void requestPopup(const char* name);
    void confirm(std::string title, std::string message, std::string button, std::function<void()> action);
    void chooseOpenFile();

    // ---- screens (app.cpp)
    void drawDashboard();
    // app_plugins.cpp
    friend class GuiPluginDelegate;
    friend struct AppTestAccess;  // tests/gui_smoke.cpp
    void initPlugins();
    void resetPluginSession();  // stops plugins and drops their windows and pending questions
    void refreshPlugins();
    const opl::PluginEntry* pluginEntry(const std::string& selected) const;
    void drawPluginsMenu();
    void drawPluginsScreen();
    void drawPluginDetail(const opl::PluginEntry& e);
    void drawPluginPanel();
    void drawPluginModals();
    void drawPluginStatus();
    void openConsent(const opl::PluginEntry& e);
    void runPluginCommand(const std::string& pluginId, const std::string& commandId, const std::string& title);
    void handlePluginUi(const std::string& pluginId, const std::string& method, const opl::json::Value& params);
    void renderPluginView(PluginView& v);
    void sendPluginEvent(PluginView& v, const std::string& element, const char* action);
    PluginView* findPluginView(const std::string& pluginId, const std::string& viewId);
    // app_sales.cpp
    void drawContacts(ob::ContactKind kind);
    void drawDocuments(ob::DocKind kind);
    void drawDocumentDetail(int documentId);
    void drawDocumentEditor();
    void startDocument(ob::DocKind kind, int contactId, Screen returnTo);
    void startRecurringEditor(int recurringId);  // 0 = new
    ListState& listFor(ob::DocKind kind);
    Screen screenFor(ob::DocKind kind) const;
    void openDocument(int documentId);  // navigate to its list and select it
    void openApplyCredit(int contactId, int creditMemoId, int invoiceId);
    void drawApplyCreditModal();
    void drawUnlockModal();
    void drawPasswordModal();
    void openPasswordForm(int mode);
    void clearPreviews();
    void drawRecurring();
    void createDueRecurring();
    void drawPaymentForm(ob::PaymentKind kind);
    void startPayment(ob::PaymentKind kind, int contactId);
    void drawPayments();
    void openContactForm(ob::ContactKind kind, int editingId);
    void drawContactModal();
    // app_banking.cpp
    void drawRegister();
    void drawReconcile();
    void drawJournal();
    void openQuickEntry(ob::TxnKind kind);
    void drawQuickEntryModal();
    void drawImportModal();
    void drawRecategorizeModal();
    void drawTransactionModal();
    void transactionContextMenu(int txnId, int accountId);
    // app_setup.cpp
    void drawAccounts();
    void drawItems();
    void drawCompany();
    void openAccountForm(int editingId);
    void drawAccountModal();
    void drawItemModal();
    // app_reports.cpp
    void drawReports();

    // ---- state
    std::optional<ob::Book> book_;
    std::unique_ptr<ob::FileKey> key_;  // set while an encrypted file is open
    std::string lastOpenError_;
    UnlockState unlock_;
    PasswordForm passwordForm_;
    std::string path_;
    std::uint64_t version_ = 1;
    Derived derived_;
    Screen screen_ = Screen::Dashboard;
    bool quit_ = false;
    bool darkTheme_ = false;
    std::vector<std::string> recent_;
    std::string configPath_;
    std::string iniPath_;

    std::string notice_;
    bool noticeIsError_ = false;
    std::chrono::steady_clock::time_point noticeTime_;
    std::string pendingPopup_;
    ConfirmRequest confirm_;
    std::string typedOpenPath_;

    NewCompanyForm newCompany_;
    ListState customers_;
    ListState vendors_;
    ListState invoices_;
    ListState bills_;
    ListState estimates_;
    ListState creditMemos_;
    ListState salesReceipts_;
    ListState recurringList_;
    ApplyCreditState applyCredit_;
    ListState payments_;
    ListState accountsList_;
    ListState itemsList_;
    DocumentEditorState editor_;
    PaymentFormState receiveForm_;
    PaymentFormState payForm_;
    RegisterState register_;
    QuickEntryState quick_;
    ImportState import_;
    RecategorizeState recategorize_;
    int detailTxn_ = 0;
    ReconcileState reconcile_;
    JournalState journal_;
    AccountForm accountForm_;
    ContactForm contactForm_;
    ItemForm itemForm_;
    ReportState report_;
    CompanyForm company_;

    std::unique_ptr<GuiPluginDelegate> pluginDelegate_;
    std::unique_ptr<ob::plugins::PluginHost> plugins_;
    PluginsScreenState pluginsScreen_;
    std::deque<PluginReview> reviews_;
    std::deque<PluginSecretQuestion> secretQuestions_;
    std::deque<PluginView> pluginViews_;  // deque: views are referenced while new ones are added
    std::string pluginPanel_;              // "<plugin id>/<view id>" shown on Screen::PluginPanel
};

}  // namespace obgui
