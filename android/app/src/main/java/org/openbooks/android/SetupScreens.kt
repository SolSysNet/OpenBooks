package org.openbooks.android

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import java.io.File
import java.time.LocalDate

private val AccountTypes = listOf("Asset", "Liability", "Equity", "Income", "Expense")

// ---------------------------------------------------------------- chart of accounts

@Composable
fun AccountsScreen() {
    val app = LocalApp.current
    var showInactive by rememberSaveable { mutableStateOf(false) }
    val data = load(showInactive) { Core.arr("accounts", "includeInactive" to showInactive).objects() }
    var editing by remember { mutableStateOf<JSONObject?>(null) }
    var adding by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        Column {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                FilterChip(showInactive, { showInactive = !showInactive }, label = { Text("Show inactive") })
            }
            Loading(data) { list ->
                LazyColumn(contentPadding = ContentPadding) {
                    AccountTypes.forEach { type ->
                        val ofType = list.filter { it.s("type") == type }
                        if (ofType.isNotEmpty()) {
                            item(key = type) {
                                Text(type, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp))
                            }
                            items(ofType, key = { it.i("id") }) { a ->
                                val role = when (a.s("role")) {
                                    "receivables" -> "Accounts receivable"
                                    "payables" -> "Accounts payable"
                                    "salesTax" -> "Sales tax"
                                    "retainedEarnings" -> "Retained earnings"
                                    else -> ""
                                }
                                ListRow(a.s("label"), listOf(role, a.s("description")).filter { it.isNotEmpty() }.joinToString(" · "),
                                    amount = a.s("balance"), dim = !a.b("active")) { editing = a }
                            }
                        }
                    }
                }
            }
        }
        Fab("New account") { adding = true }
    }
    if (adding) AccountDialog(null) { adding = false }
    editing?.let { AccountDialog(it) { editing = null } }
}

@Composable
private fun AccountDialog(existing: JSONObject?, onDismiss: () -> Unit) {
    val app = LocalApp.current
    var name by remember { mutableStateOf(existing?.s("name") ?: "") }
    var number by remember { mutableStateOf(existing?.s("number") ?: "") }
    var type by remember { mutableStateOf(existing?.s("type") ?: "Expense") }
    var description by remember { mutableStateOf(existing?.s("description") ?: "") }
    var active by remember { mutableStateOf(existing?.b("active") ?: true) }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = if (existing == null) "New account" else existing.s("label"),
        confirm = "Save",
        onDismiss = onDismiss,
        onConfirm = {
            if (name.isBlank()) error = "Enter a name."
            else app.run(success = "Account saved", onError = { error = it.message ?: "" }, onDone = { _: Any? -> onDismiss() }) {
                if (existing == null) Core.call("addAccount", "name" to name.trim(), "number" to number.trim(), "type" to type, "description" to description.trim())
                else Core.call("updateAccount", "id" to existing.i("id"), "name" to name.trim(), "number" to number.trim(),
                    "description" to description.trim(), "active" to active)
            }
        },
    ) {
        TextInput("Name", name, { name = it })
        TextInput("Number", number, { number = it }, keyboard = KeyboardType.Number, placeholder = "Optional, e.g. 6100")
        if (existing == null) ChoiceField("Type", AccountTypes, type, { type = it })
        else Muted("Type: ${existing.s("type")} · balance ${MoneyFmt.format(existing.s("balance"))}")
        TextInput("Description", description, { description = it })
        if (existing != null) {
            CheckRow("Active", active) { active = it }
            OutlinedButton({ onDismiss(); app.navigate(Screen.Register(existing.i("id"))) }) { Text("Open register") }
        }
        ErrorText(error)
    }
}

// ---------------------------------------------------------------- items

@Composable
fun ItemsScreen() {
    val app = LocalApp.current
    var showInactive by rememberSaveable { mutableStateOf(false) }
    val data = load(showInactive) { Core.arr("items", "includeInactive" to showInactive).objects() }
    var adding by remember { mutableStateOf(false) }
    var toggling by remember { mutableStateOf<JSONObject?>(null) }
    Box(Modifier.fillMaxSize()) {
        Column {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                FilterChip(showInactive, { showInactive = !showInactive }, label = { Text("Show inactive") })
            }
            Loading(data) { list ->
                if (list.isEmpty()) EmptyState("Save the products and services you sell to fill in invoice lines quickly.")
                LazyColumn(contentPadding = ContentPadding) {
                    items(list, key = { it.i("id") }) { i ->
                        ListRow(i.s("name"), listOf(i.s("description"), if (i.b("taxable")) "taxable" else "").filter { it.isNotEmpty() }.joinToString(" · "),
                            amount = i.s("price"), dim = !i.b("active")) { toggling = i }
                    }
                }
            }
        }
        Fab("New item") { adding = true }
    }
    if (adding) ItemDialog { adding = false }
    toggling?.let { i ->
        val active = i.b("active")
        ConfirmDialog(
            i.s("name"),
            if (active) "Make this item inactive? It stays on existing documents but is hidden from new ones." else "Make this item active again?",
            if (active) "Make inactive" else "Make active",
            onConfirm = { app.run { Core.call("setItemActive", "id" to i.i("id"), "active" to !active) } },
            onDismiss = { toggling = null },
        )
    }
}

@Composable
private fun ItemDialog(onDismiss: () -> Unit) {
    val app = LocalApp.current
    val accounts = load { Core.arr("accounts").objects() }
    var name by remember { mutableStateOf("") }
    var description by remember { mutableStateOf("") }
    var price by remember { mutableStateOf("") }
    var income by remember { mutableStateOf(0) }
    var expense by remember { mutableStateOf(0) }
    var taxable by remember { mutableStateOf(true) }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = "New product or service",
        confirm = "Save",
        onDismiss = onDismiss,
        onConfirm = {
            error = when {
                name.isBlank() -> "Enter a name."
                price.isNotBlank() && MoneyFmt.parse(price) == null -> "Check the price."
                income == 0 && expense == 0 -> "Choose an income or expense account."
                else -> ""
            }
            if (error.isEmpty()) app.run(success = "Saved ${name.trim()}", onError = { error = it.message ?: "" }, onDone = { _: Any? -> onDismiss() }) {
                Core.call("addItem", "name" to name.trim(), "description" to description.trim(), "price" to (MoneyFmt.parse(price) ?: ""),
                    "incomeAccountId" to income, "expenseAccountId" to expense, "taxable" to taxable)
            }
        },
    ) {
        TextInput("Name", name, { name = it })
        TextInput("Description", description, { description = it })
        MoneyInput("Price", price, { price = it })
        Loading(accounts) { list ->
            PickerField("Income account (when sold)", accountOptions(list) { it.s("type") == "Income" }, income, { income = it }, noneLabel = "None")
            PickerField("Expense account (when bought)", accountOptions(list) { it.s("type") == "Expense" || it.s("type") == "Asset" && !it.b("money") && !it.b("subledger") },
                expense, { expense = it }, noneLabel = "None")
        }
        CheckRow("Taxable", taxable) { taxable = it }
        ErrorText(error)
    }
}

// ---------------------------------------------------------------- company & security

@Composable
fun CompanyScreen() {
    val app = LocalApp.current
    val context = app.context
    val data = load { Core.obj("company") to Core.arr("accounts").objects() }
    var passwordDialog by remember { mutableStateOf<String?>(null) }  // "set" / "change" / "remove"
    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) app.run(success = "Books exported") { Files.copyToUri(context, File(app.bookPath), uri) }
    }
    Loading(data) { (c, accounts) ->
        var name by remember(c) { mutableStateOf(c.s("name")) }
        var address by remember(c) { mutableStateOf(c.s("address")) }
        var email by remember(c) { mutableStateOf(c.s("email")) }
        var phone by remember(c) { mutableStateOf(c.s("phone")) }
        var footer by remember(c) { mutableStateOf(c.s("invoiceFooter")) }
        var paper by remember(c) { mutableStateOf(c.s("paperSize")) }
        var fyMonth by remember(c) { mutableStateOf(c.i("fiscalYearStartMonth")) }
        var terms by remember(c) { mutableStateOf(c.i("defaultTermsDays").toString()) }
        var next by remember(c) { mutableStateOf(c.i("nextInvoiceNumber").toString()) }
        var closed by remember(c) { mutableStateOf(c.s("closedThrough")) }
        var ar by remember(c) { mutableStateOf(c.i("receivablesAccountId")) }
        var ap by remember(c) { mutableStateOf(c.i("payablesAccountId")) }
        var tax by remember(c) { mutableStateOf(c.i("salesTaxAccountId")) }
        var re by remember(c) { mutableStateOf(c.i("retainedEarningsAccountId")) }
        var error by remember(c) { mutableStateOf("") }
        val months = listOf("January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December")

        ScrollPage {
            SectionTitle("Security")
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (c.b("encrypted")) {
                        Text("🔒 Password protected", fontWeight = FontWeight.SemiBold)
                        Muted("AES-256-GCM, key from PBKDF2-HMAC-SHA256 (${c.i("iterations")} iterations) · ${c.s("cryptoBackend")}")
                        Muted("Locks automatically after 5 minutes in the background. Screenshots are blocked while open.")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton({ passwordDialog = "change" }) { Text("Change password") }
                            OutlinedButton({ passwordDialog = "remove" }) { Text("Remove") }
                        }
                    } else {
                        Text("Not password protected")
                        Muted("Anyone with this file can read it. Android already encrypts app storage, but a password also protects exported copies.")
                        Button({ passwordDialog = "set" }, enabled = app.cryptoOk == true) { Text("Set a password") }
                        if (app.cryptoOk == false) ErrorText("Encryption is unavailable on this device: ${app.cryptoError}")
                    }
                }
            }
            SectionTitle("Backup")
            Card {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Muted("Books are never uploaded anywhere. Export a copy to keep a backup or to open on a computer." +
                        if (c.b("encrypted")) " The copy stays password protected." else "")
                    OutlinedButton({ exporter.launch(File(app.bookPath).name) }) { Text("Export books file…") }
                }
            }

            SectionTitle("Company")
            TextInput("Company name", name, { name = it })
            TextInput("Address", address, { address = it }, singleLine = false)
            TextInput("Email", email, { email = it }, keyboard = KeyboardType.Email)
            TextInput("Phone", phone, { phone = it }, keyboard = KeyboardType.Phone)
            TextInput("Invoice footer", footer, { footer = it }, singleLine = false, placeholder = "e.g. Thank you for your business!")
            ChoiceField("Paper size", listOf("Letter", "A4"), paper, { paper = it })

            SectionTitle("Accounting")
            ChoiceField("Fiscal year starts", months, months[(fyMonth - 1).coerceIn(0, 11)], { fyMonth = months.indexOf(it) + 1 })
            TextInput("Default payment terms (days)", terms, { terms = it }, keyboard = KeyboardType.Number)
            TextInput("Next invoice number", next, { next = it }, keyboard = KeyboardType.Number)
            DateField("Books closed through", closed, { closed = it }, allowEmpty = true)
            Muted("Nothing dated on or before the closing date can be added, changed or voided.")
            PickerField("Accounts receivable", accountOptions(accounts) { it.s("type") == "Asset" }, ar, { ar = it })
            PickerField("Accounts payable", accountOptions(accounts) { it.s("type") == "Liability" }, ap, { ap = it })
            PickerField("Sales tax payable", accountOptions(accounts) { it.s("type") == "Liability" }, tax, { tax = it })
            PickerField("Retained earnings", accountOptions(accounts) { it.s("type") == "Equity" }, re, { re = it })
            ErrorText(error)
            Button(
                modifier = Modifier.fillMaxWidth(),
                onClick = {
                    error = when {
                        name.isBlank() -> "Enter a company name."
                        terms.trim().toIntOrNull() == null -> "Terms must be a number of days."
                        next.trim().toIntOrNull() == null -> "The next invoice number must be a number."
                        else -> ""
                    }
                    if (error.isEmpty()) app.run(success = "Settings saved", onError = { error = it.message ?: "" }, onDone = { _: Any? -> app.refreshStatus() }) {
                        Core.call(
                            "setCompany", "name" to name.trim(), "address" to address, "email" to email.trim(), "phone" to phone.trim(),
                            "invoiceFooter" to footer, "paperSize" to paper, "fiscalYearStartMonth" to fyMonth,
                            "defaultTermsDays" to terms.trim().toInt(), "nextInvoiceNumber" to next.trim().toInt(), "closedThrough" to closed,
                            "receivablesAccountId" to ar, "payablesAccountId" to ap, "salesTaxAccountId" to tax, "retainedEarningsAccountId" to re,
                        )
                    }
                },
            ) { Text("Save settings") }
        }
    }
    passwordDialog?.let { PasswordDialog(it) { passwordDialog = null } }
}

@Composable
private fun PasswordDialog(mode: String, onDismiss: () -> Unit) {
    val app = LocalApp.current
    var current by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var repeat by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    FormDialog(
        title = when (mode) { "set" -> "Set a password"; "change" -> "Change password"; else -> "Remove password" },
        confirm = if (busy) "Working…" else if (mode == "remove") "Remove password" else "Save password",
        confirmEnabled = !busy,
        onDismiss = onDismiss,
        onConfirm = {
            error = when {
                mode != "set" && current.isEmpty() -> "Enter the current password."
                mode != "remove" && password.length < 8 -> "Use at least 8 characters."
                mode != "remove" && password != repeat -> "The new passwords don't match."
                else -> ""
            }
            if (error.isNotEmpty()) return@FormDialog
            busy = true
            app.run(
                success = if (mode == "remove") "Password removed" else "Password saved",
                onError = { busy = false; error = if (it.code == "wrong_password") "The current password is not correct." else it.message ?: "" },
                onDone = { status: JSONObject -> app.applyStatus(status); onDismiss() },
            ) {
                if (mode == "remove") Core.obj("removePassword", "current" to current)
                else Core.obj("setPassword", "current" to current, "password" to password)
            }
        },
    ) {
        if (mode != "set") PasswordInput("Current password", current, { current = it })
        if (mode != "remove") {
            PasswordInput("New password (8+ characters)", password, { password = it })
            PasswordInput("Repeat new password", repeat, { repeat = it })
            Muted("There is no way to recover books if the password is lost.")
        } else {
            Muted("The books file will be saved unencrypted.")
        }
        ErrorText(error)
    }
}

// ---------------------------------------------------------------- reports

private data class ReportDef(val id: String, val title: String, val period: Boolean)

private val Reports = listOf(
    ReportDef("profit-loss", "Profit & loss", true),
    ReportDef("balance-sheet", "Balance sheet", false),
    ReportDef("ar-aging", "A/R aging", false),
    ReportDef("ap-aging", "A/P aging", false),
    ReportDef("trial-balance", "Trial balance", false),
    ReportDef("journal", "Journal", true),
    ReportDef("account", "Account detail", true),
)

@Composable
fun ReportsScreen() {
    val app = LocalApp.current
    val context = app.context
    var report by rememberSaveable { mutableStateOf("profit-loss") }
    var from by rememberSaveable { mutableStateOf(LocalDate.now().withDayOfYear(1).toString()) }
    var to by rememberSaveable { mutableStateOf(today()) }
    var asOf by rememberSaveable { mutableStateOf(today()) }
    var accountId by rememberSaveable { mutableStateOf(0) }
    var pendingCsv by remember { mutableStateOf<File?>(null) }
    val def = Reports.first { it.id == report }
    val accounts = load { Core.arr("accounts").objects() }
    LaunchedEffect(Unit) {
        // Start the default period at the fiscal year start, like the desktop.
        app.run(onDone = { c: JSONObject ->
            val month = c.i("fiscalYearStartMonth").coerceIn(1, 12)
            val now = LocalDate.now()
            var start = LocalDate.of(now.year, month, 1)
            if (start.isAfter(now)) start = start.minusYears(1)
            from = start.toString()
        }) { Core.obj("company") }
    }
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri ->
        val file = pendingCsv
        if (uri != null && file != null) app.run(success = "CSV saved") { Files.copyToUri(context, file, uri) }
    }
    val data = load(report, from, to, asOf, accountId) {
        if (report == "account" && accountId == 0) null
        else Core.obj("report", "name" to report, "from" to from, "to" to to, "asOf" to asOf, "accountId" to accountId)
    }
    fun csvFile(r: JSONObject): File = File(app.exportDir, "${def.title.replace(Regex("[^A-Za-z0-9]+"), "-")}-${if (def.period) to else asOf}.csv")
        .also { it.writeText(r.s("csv")) }

    ScrollPage {
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Reports.forEach { r -> FilterChip(report == r.id, { report = r.id }, label = { Text(r.title) }) }
        }
        if (def.period) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DateField("From", from, { from = it }, Modifier.weight(1f))
                DateField("To", to, { to = it }, Modifier.weight(1f))
            }
        } else {
            DateField("As of", asOf, { asOf = it })
        }
        if (report == "account") accounts.value?.let { PickerField("Account", accountOptions(it), accountId, { id -> accountId = id }) }
        if (report == "account" && accountId == 0) Muted("Choose an account.")
        else Loading(data) { loaded ->
            if (loaded == null) return@Loading
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton({ Files.share(context, csvFile(loaded), "text/csv") }) { Text("Share CSV") }
                OutlinedButton({ val f = csvFile(loaded); pendingCsv = f; saver.launch(f.name) }) { Text("Save CSV…") }
            }
            Card { TableView(loaded.getJSONObject("table"), Modifier.padding(12.dp)) }
        }
    }
}
