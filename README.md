# OpenBooks

**Free, open source small-business accounting, in modern C++.**

OpenBooks is a real double-entry accounting system. It handles invoicing, bills, payments,
bank imports, reconciliation and standard financial reports. Your books live in one plain-text
file that you own. It has no subscription, no cloud lock-in and no dependencies beyond a C++17
compiler.

> Status: 0.2. It ships a desktop app (`openbooks-gui`) and a scriptable command line
> (`openbooks`), both working on the same books file. See the [roadmap](#roadmap) for what's next.

## Desktop app

`openbooks-gui` is a native desktop app built with [Dear ImGui](https://github.com/ocornut/imgui).
It runs on Win32 + Direct3D 11 on Windows, and GLFW + OpenGL 3 on Linux and macOS.

- **Dashboard:** bank balance, open and overdue invoices, open bills, fiscal-year profit,
  and recent activity.
- **Sales:** customers, estimates (convert to an invoice in one click), invoices with a
  line-item editor (products, live tax and totals), sales receipts, credit memos you can apply
  to invoices, recurring invoice templates, and receiving payments against specific invoices.
- **Purchases:** vendors, bills and bill payments.
- **Banking:** account registers with running balances, one-click expense, deposit and
  transfer entry, CSV statement import with a preview, recategorizing from the right-click
  menu, and reconciliation with checkboxes.
- **Accounting:** chart of accounts, journal entries (with a live balance check), products
  and services, and company settings, including closing the books.
- **Reports:** Profit & Loss, Balance Sheet, Trial Balance, A/R and A/P Aging, Journal and
  Account Detail. Each has date presets and can be exported to CSV or copied as text.
- **Interface:** light and dark themes, type-to-search pickers, a calendar date picker, native
  file dialogs (Windows; zenity/kdialog on Linux; the system panel on macOS), and recent files.

Every change is saved the moment you make it, and failed changes are never half-applied.
Open a file with `openbooks-gui path\to\books.obk`, or use File → Open.

## Features

| Area | What you get |
|---|---|
| **Double-entry ledger** | Every transaction must balance. Exact integer-cent money math, never floating point. |
| **Chart of accounts** | Assets, liabilities, equity, income and expenses, with a ready-made starter chart. Account numbers are optional. You can deactivate accounts. |
| **Customers & invoicing** | Invoices with products/services, quantities, per-line taxability and sales tax. Auto-numbering, terms/due dates, PDF output. |
| **Estimates** | Quotes with an expiry date that never touch the ledger. Accept, decline, or convert one to an invoice. |
| **Credit memos** | Customer credits for returns, discounts or corrections. They reduce A/R and sales tax and can be applied to any of the customer's invoices. |
| **Sales receipts** | Sales paid on the spot, deposited straight into a bank account without an invoice. |
| **Recurring invoices** | Weekly, monthly or yearly templates (month-end aware). Due invoices are created when you ask, all or nothing. |
| **Vendors & bills** | Enter bills against expense accounts and pay them from any bank or card account. |
| **Payments** | Apply payments to specific invoices/bills, or let OpenBooks pay the oldest first. Partial payments and overpayment credits both work. |
| **Banking** | Record expenses, deposits and transfers. Import bank/card CSVs with duplicate detection, then recategorize. |
| **Reconciliation** | Clear transactions against a statement, see the difference, and lock them as reconciled. |
| **Reports** | Balance Sheet (with retained-earnings rollover by fiscal year), Profit & Loss, Trial Balance, A/R and A/P Aging, Journal, account registers. Every report can be printed as text or exported as `--csv`. |
| **Controls** | You can void but never delete, so the audit trail is kept. Closing the books locks a period. A/R and A/P change only through invoices, bills and payments, so the sub-ledgers always agree with the general ledger. |
| **Safe storage** | Saves are atomic (write, then rename), a `.bak` of the previous save is kept, and integrity is checked on every load. |

## Building

You need CMake 3.16+ and a C++17 compiler (GCC 9+, Clang 10+, or MSVC 2019+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This produces `openbooks` (CLI), `openbooks-gui` (desktop app) and `openbooks_tests`. With
Visual Studio generators they land in `build\Release\`. On MinGW the executables are linked
statically, so they run with no extra DLLs.

The desktop app uses the vendored Dear ImGui in `third_party/imgui`, so Windows needs nothing
extra. On Linux, install GLFW first (e.g. `sudo apt install libglfw3-dev`). On macOS, use
`brew install glfw`. To build only the CLI and engine, add `-DOPENBOOKS_BUILD_GUI=OFF`.
For native file dialogs on Linux, have `zenity` (GNOME and most desktops) or `kdialog` (KDE)
installed. Without either, the app asks for a typed path instead.

## Quick start

```bash
openbooks init --company "Blue Door Design"          # creates ./books.obk with a starter chart
openbooks company set --address "12 Main St, Springfield"

# Opening balance: owner puts $10,000 into the business
openbooks journal --date 2025-01-01 --debit Checking=10000 --credit "Owner's Equity=10000" --memo "Initial investment"

# Customers, products, invoices
openbooks customer add "Acme Corp" --email ap@acme.example --terms 15
openbooks item add "Design hour" --price 95 --income "Service Revenue"
openbooks invoice create --customer Acme --tax 8.25 \
    --line "item=Design hour;qty=12" \
    --line "desc=Printing;amount=140;account=Sales"
openbooks invoice show 1001

# Get paid
openbooks receive-payment --customer Acme --amount 500 --to Checking

# Bills and expenses
openbooks vendor add "City Properties"
openbooks bill create --vendor "City Properties" --line "account=Rent;amount=1800" --number APR-2025
openbooks pay-bill --vendor "City Properties" --amount 1800 --from Checking --ref 1042
openbooks expense --from "Credit Card" --account Software --amount 29.99 --payee "Figma"

# Bank feed
openbooks import-csv Checking statement.csv
openbooks txn recategorize 14 --from "Uncategorized Expense" --to "Office Supplies"

# Reports
openbooks report profit-loss --from 2025-01-01 --to 2025-03-31
openbooks report balance-sheet
openbooks report ar-aging --csv > aging.csv
openbooks register Checking --from 2025-03-01
```

Use `openbooks shell` to run many commands in one session, and `openbooks help <command>` for
every option. Use `-f FILE` or `OPENBOOKS_FILE` to work on a different company's books.

### Referring to things

Accounts, customers, vendors and items can be named by their full name (case-insensitive),
a unique prefix (`Acme` → "Acme Corp"), an account number (`1000`), or `#id`.

### Invoice and bill lines

`--line` takes `key=value` pairs separated by `;`:

| key | meaning |
|---|---|
| `item` | a product/service. Fills in the description, rate, account and taxability. |
| `desc` | description |
| `qty`, `rate` | quantity and unit price. The amount is `qty × rate`, rounded to the cent. |
| `amount` | a flat line amount (use it instead of qty/rate) |
| `account` | the income account (invoices) or expense account (bills) |
| `taxable` | `yes`/`no` (invoices only) |

### Estimates, credit memos, sales receipts and recurring invoices

```bash
openbooks estimate create --customer Acme --line "item=Design hour;qty=40" --expires 2026-11-30
openbooks estimate convert EST-1                      # becomes an invoice
openbooks credit-memo create --customer Acme --line "desc=Returned goods;amount=120;account=Sales" \n    --apply 1004                                     # or later: credit-memo apply CM-1 --invoice 1004
openbooks sales-receipt create --customer "Walk-in" --deposit-to Checking --line "item=T-shirt;qty=3"
openbooks recurring add "Hosting" --customer Acme --frequency monthly --start 2026-01-31 \n    --line "desc=Managed hosting;amount=49;account=Sales"
openbooks recurring run                               # creates every invoice that is due
```

Recurring invoices are never created behind your back. The dashboard shows how many are due,
and one click (or `recurring run`) creates them.

### PDF invoices

```bash
openbooks company set --email billing@bluedoor.example --phone "(555) 010-2030" \
    --invoice-footer "Pay by bank transfer to account 000123. Thank you!" --paper letter
openbooks invoice pdf 1001                    # writes "Invoice 1001 - Acme Corp.pdf"
openbooks invoice pdf 1001 --out acme.pdf --force
```

In the desktop app, open an invoice or bill and choose **Save PDF...** or **Preview PDF**.
PDFs support multiple pages, carry VOID and PAID stamps, and come in Letter or A4. They're
produced by a small built-in writer, so there's no extra dependency and no active content
(see [SECURITY.md](SECURITY.md)).

### Reconciling a bank statement

```bash
openbooks reconcile Checking --through 2025-03-31                      # see uncleared items
openbooks reconcile Checking --clear 3,7,9,12 --statement-balance 8421.17
openbooks reconcile Checking --statement-balance 8421.17 --finish       # locks when difference is 0
```

### Closing a period

```bash
openbooks company set --close-through 2024-12-31
```

After this, nothing dated on or before the close date can be added, voided or changed. Income
and expenses from past fiscal years show on the Balance Sheet as Retained Earnings, so there
are no manual closing entries to make.

## How the accounting works

* Every **transaction** is a list of **splits**. Positive amounts are debits and negative
  amounts are credits, and they must sum to zero.
* An **invoice** debits Accounts Receivable and credits each line's income account plus Sales Tax
  Payable. A **customer payment** debits the bank and credits A/R.
* A **bill** debits each line's expense account and credits Accounts Payable. A **bill payment**
  debits A/P and credits the bank.
* A **void** keeps the record but removes it from every balance. Invoices with payments applied
  must have their payments voided first. Reconciled transactions cannot be voided.

## File format

A `.obk` file is UTF-8 text with one tab-separated record per line (`OPENBOOKS 1` header, then
`COMPANY`, `ACCOUNT`, `CONTACT`, `ITEM`, `TXN`/`SPLIT`, `DOC`/`LINE`, `PAY`/`APPLY`). It's
easy to diff, back up and version-control, and you can recover it by hand if needed. See
[src/storage.cpp](src/storage.cpp) for the exact layout.

## Project layout

```
include/openbooks/   public headers
  money.hpp          exact currency and decimal arithmetic
  date.hpp           calendar dates
  model.hpp          accounts, transactions, contacts, items, invoices/bills, payments
  book.hpp           the ledger engine and all business rules
  reports.hpp        financial reports and text/CSV rendering
  import.hpp         bank/card CSV import
  pdf.hpp            minimal, dependency-free PDF writer
  invoice_pdf.hpp    invoice/bill PDF layout
  cli.hpp            command-line front end
src/                 engine + CLI implementation
gui/                 desktop app
  app.hpp/.cpp       app shell, navigation, dashboard, persistence
  app_sales.cpp      customers, vendors, invoices, bills, payments
  app_banking.cpp    registers, quick entries, CSV import, reconciliation, journal
  app_setup.cpp      chart of accounts, items, company settings
  app_reports.cpp    reports
  widgets.*          date picker, searchable pickers, money fields, report tables
  main_*.cpp         platform main loops (Win32/D3D11, GLFW/OpenGL3)
tests/               self-contained test suite (no framework needed)
third_party/imgui/   Dear ImGui 1.92.9b (MIT)
```

The engine (`openbooks_core`) doesn't depend on either front end. The GUI never edits the
books in place. It applies each change to a copy, saves that copy, and only then swaps it in.

## Roadmap

- [x] Desktop GUI
- [x] Native file dialogs on Linux/macOS
- [x] Estimates, credit memos, sales receipts, recurring invoices
- [ ] Customer refunds of unapplied credit; per-document-type PDF footers
- [x] PDF invoices and bills
- [ ] Company logo on PDFs; email delivery (would need an explicit, opt-in network design)
- [ ] Inventory quantity tracking and COGS
- [ ] Classes/locations, budgets, cash-basis reports
- [ ] Multi-currency
- [ ] Payroll (region-specific plugins)
- [ ] QuickBooks IIF/QBO import
- [ ] SQLite storage backend for large books

## Contributing

Contributions are welcome. Please add tests in `tests/test_main.cpp` for any change to accounting
behavior. The rule to keep: **the Balance Sheet must always balance, and money is never a `double`.**

## License

MIT. See [LICENSE](LICENSE).
