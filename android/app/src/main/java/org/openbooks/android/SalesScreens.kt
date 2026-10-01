package org.openbooks.android

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import java.io.File
import java.math.BigDecimal
import java.math.RoundingMode

object DocKinds {
    fun title(kind: String) = when (kind) {
        "CreditMemo" -> "Credit memo"
        "SalesReceipt" -> "Sales receipt"
        else -> kind
    }

    fun plural(kind: String) = when (kind) {
        "CreditMemo" -> "Credit memos"
        "SalesReceipt" -> "Sales receipts"
        else -> kind + "s"
    }

    fun party(kind: String) = if (kind == "Bill") "Vendor" else "Customer"
    fun hasDue(kind: String) = kind == "Invoice" || kind == "Bill" || kind == "Estimate"
}

fun dec(text: String): BigDecimal = MoneyFmt.parse(text)?.toBigDecimalOrNull() ?: BigDecimal.ZERO
fun BigDecimal.money(): String = setScale(2, RoundingMode.HALF_UP).toPlainString()

// ---------------------------------------------------------------- dashboard

@Composable
fun DashboardScreen() {
    val app = LocalApp.current
    val data = load { Core.obj("dashboard") }
    var quick by remember { mutableStateOf<String?>(null) }
    Loading(data) { d ->
        ScrollPage {
            Muted("Today is ${d.s("today")}")
            val due = d.i("recurringDue")
            if (due > 0) {
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.primaryContainer)) {
                    Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("$due recurring invoice${if (due == 1) " is" else "s are"} due.", Modifier.weight(1f))
                        Button({
                            app.run(success = null, onDone = { ids: List<Int> -> app.message("Created ${ids.size} invoice(s)") }) {
                                Core.arr("runRecurring").ints()
                            }
                        }) { Text("Create") }
                    }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                SummaryCard("Bank balance", d.s("bank"), Modifier.weight(1f)) { app.navigate(Screen.Register()) }
                SummaryCard(
                    "Customers owe", d.s("receivable"), Modifier.weight(1f),
                    detail = if (d.i("overdueCount") > 0) MoneyFmt.format(d.s("overdue")) + " overdue" else "",
                ) { app.navigate(Screen.Documents("Invoice")) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                SummaryCard(
                    "You owe vendors", d.s("payable"), Modifier.weight(1f),
                    detail = if (!MoneyFmt.isZero(d.s("billsOverdue"))) MoneyFmt.format(d.s("billsOverdue")) + " overdue" else "",
                ) { app.navigate(Screen.Documents("Bill")) }
                SummaryCard(
                    "Net income this year", d.s("netIncome"), Modifier.weight(1f),
                    detail = "In ${MoneyFmt.format(d.s("income"))} · out ${MoneyFmt.format(d.s("expenses"))}",
                ) { app.navigate(Screen.Reports) }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton({ app.navigate(Screen.DocumentEditor("Invoice")) }) { Text("New invoice") }
                OutlinedButton({ app.navigate(Screen.PaymentForm("Received")) }) { Text("Receive payment") }
                OutlinedButton({ app.navigate(Screen.DocumentEditor("Bill")) }) { Text("Enter bill") }
                OutlinedButton({ quick = "Expense" }) { Text("Record expense") }
                OutlinedButton({ quick = "Deposit" }) { Text("Record deposit") }
            }
            val accounts = d.list("accounts")
            if (accounts.isNotEmpty()) {
                SectionTitle("Bank accounts")
                Card {
                    accounts.forEach { a ->
                        ListRow(a.s("label"), amount = a.s("balance")) { app.navigate(Screen.Register(a.i("id"))) }
                    }
                }
            }
            val overdue = d.list("overdueInvoices")
            if (overdue.isNotEmpty()) {
                SectionTitle("Overdue invoices")
                Card {
                    overdue.forEach { o ->
                        ListRow(
                            "${o.s("number")} · ${o.s("contactName")}", "${o.i("daysLate")} days late", amount = o.s("balance"),
                        ) { app.navigate(Screen.DocumentDetail(o.i("id"))) }
                    }
                }
            }
            val recent = d.list("recent")
            if (recent.isNotEmpty()) {
                SectionTitle("Recent activity")
                Card {
                    recent.forEach { t ->
                        ListRow(t.s("payee").ifEmpty { t.s("kind") }, "${t.s("date")} · ${t.s("kind")} ${t.s("ref")}".trim(), amount = t.s("amount"))
                    }
                }
            }
        }
    }
    quick?.let { QuickEntryDialog(it, 0) { quick = null } }
}

// ---------------------------------------------------------------- contacts

@Composable
fun ContactsScreen(kind: String) {
    val app = LocalApp.current
    var showInactive by rememberSaveable { mutableStateOf(false) }
    var query by rememberSaveable { mutableStateOf("") }
    var adding by remember { mutableStateOf(false) }
    val data = load(kind, showInactive) { Core.arr("contacts", "kind" to kind, "includeInactive" to showInactive).objects() }
    Box(Modifier.fillMaxSize()) {
        Column {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                TextInput("Search", query, { query = it }, Modifier.weight(1f))
                Spacer(Modifier.width(8.dp))
                FilterChip(showInactive, { showInactive = !showInactive }, label = { Text("Inactive") })
            }
            Loading(data) { list ->
                val shown = list.filter { query.isBlank() || it.s("name").contains(query.trim(), true) || it.s("email").contains(query.trim(), true) }
                if (shown.isEmpty()) EmptyState(if (list.isEmpty()) "No ${if (kind == "Customer") "customers" else "vendors"} yet." else "Nothing matches.")
                LazyColumn(contentPadding = ContentPadding) {
                    items(shown, key = { it.i("id") }) { c ->
                        ListRow(
                            c.s("name"), listOf(c.s("email"), c.s("phone")).filter { it.isNotEmpty() }.joinToString(" · "),
                            amount = c.s("balance"), dim = !c.b("active"),
                        ) { app.navigate(Screen.ContactDetail(c.i("id"))) }
                    }
                }
            }
        }
        Fab("New ${kind.lowercase()}") { adding = true }
    }
    if (adding) ContactDialog(kind, null) { adding = false }
}

@Composable
fun ContactDialog(kind: String, existing: JSONObject?, onDismiss: () -> Unit) {
    val app = LocalApp.current
    var name by remember { mutableStateOf(existing?.s("name") ?: "") }
    var email by remember { mutableStateOf(existing?.s("email") ?: "") }
    var phone by remember { mutableStateOf(existing?.s("phone") ?: "") }
    var address by remember { mutableStateOf(existing?.s("address") ?: "") }
    var terms by remember { mutableStateOf(existing?.i("termsDays")?.takeIf { it >= 0 }?.toString() ?: "") }
    var active by remember { mutableStateOf(existing?.b("active") ?: true) }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = if (existing == null) "New ${kind.lowercase()}" else "Edit ${existing.s("name")}",
        confirm = "Save",
        onDismiss = onDismiss,
        onConfirm = {
            val termsDays = if (terms.isBlank()) -1 else terms.trim().toIntOrNull()
            error = when {
                name.isBlank() -> "Enter a name."
                termsDays == null || termsDays < -1 -> "Terms must be a number of days."
                else -> ""
            }
            if (error.isEmpty()) {
                val fields = arrayOf(
                    "name" to name.trim(), "email" to email.trim(), "phone" to phone.trim(), "address" to address.trim(),
                    "termsDays" to termsDays, "active" to active,
                )
                app.run(success = "Saved ${name.trim()}", onError = { error = it.message ?: "" }, onDone = { _: Any? -> onDismiss() }) {
                    if (existing == null) Core.call("addContact", "kind" to kind, *fields)
                    else Core.call("updateContact", "id" to existing.i("id"), *fields)
                }
            }
        },
    ) {
        TextInput("Name", name, { name = it })
        TextInput("Email", email, { email = it }, keyboard = KeyboardType.Email)
        TextInput("Phone", phone, { phone = it }, keyboard = KeyboardType.Phone)
        TextInput("Address", address, { address = it }, singleLine = false)
        TextInput("Payment terms (days)", terms, { terms = it }, keyboard = KeyboardType.Number, placeholder = "Company default")
        if (existing != null) CheckRow("Active", active) { active = it }
        ErrorText(error)
    }
}

@Composable
fun ContactDetailScreen(id: Int) {
    val app = LocalApp.current
    val data = load(id) { Core.obj("contact", "id" to id) }
    var editing by remember { mutableStateOf(false) }
    Loading(data) { d ->
        val c = d.getJSONObject("contact")
        val kind = c.s("kind")
        ScrollPage {
            Text(c.s("name"), style = MaterialTheme.typography.headlineSmall)
            if (!c.b("active")) StatusChip("Inactive")
            listOf(c.s("email"), c.s("phone"), c.s("address")).filter { it.isNotEmpty() }.forEach { Text(it) }
            Muted("Terms: " + if (c.i("termsDays") < 0) "company default" else "Net ${c.i("termsDays")}")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Open balance  ", color = Palette.muted)
                MoneyText(c.s("balance"), bold = true)
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton({ editing = true }) { Text("Edit") }
                if (kind == "Customer") {
                    Button({ app.navigate(Screen.DocumentEditor("Invoice", id)) }) { Text("New invoice") }
                    OutlinedButton({ app.navigate(Screen.PaymentForm("Received", id)) }) { Text("Receive payment") }
                    OutlinedButton({ app.navigate(Screen.DocumentEditor("Estimate", id)) }) { Text("Estimate") }
                    OutlinedButton({ app.navigate(Screen.DocumentEditor("SalesReceipt", id)) }) { Text("Sales receipt") }
                    OutlinedButton({ app.navigate(Screen.DocumentEditor("CreditMemo", id)) }) { Text("Credit memo") }
                } else {
                    Button({ app.navigate(Screen.DocumentEditor("Bill", id)) }) { Text("Enter bill") }
                    OutlinedButton({ app.navigate(Screen.PaymentForm("Paid", id)) }) { Text("Pay bills") }
                }
            }
            SectionTitle("Transactions")
            val docs = d.list("documents")
            if (docs.isEmpty()) Muted("Nothing yet.")
            Card {
                docs.forEach { doc -> DocumentRow(doc, showContact = false) }
            }
        }
        if (editing) ContactDialog(kind, c) { editing = false }
    }
}

@Composable
fun DocumentRow(doc: JSONObject, showContact: Boolean = true) {
    val app = LocalApp.current
    val label = DocKinds.title(doc.s("kind")) + " " + doc.s("number")
    ListRow(
        title = if (showContact) "${doc.s("number")} · ${doc.s("contactName")}" else label,
        subtitle = doc.s("date") + if (!MoneyFmt.isZero(doc.s("balance")) && doc.s("balance") != doc.s("total")) " · due ${MoneyFmt.format(doc.s("balance"))}" else "",
        amount = doc.s("total"),
        status = doc.s("status"),
        dim = doc.b("voided"),
    ) { app.navigate(Screen.DocumentDetail(doc.i("id"))) }
}

// ---------------------------------------------------------------- documents

@Composable
fun DocumentsScreen(kind: String) {
    val app = LocalApp.current
    var openOnly by rememberSaveable(kind) { mutableStateOf(false) }
    var query by rememberSaveable(kind) { mutableStateOf("") }
    val data = load(kind, openOnly) { Core.arr("documents", "kind" to kind, "openOnly" to openOnly).objects() }
    Box(Modifier.fillMaxSize()) {
        Column {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                TextInput("Search", query, { query = it }, Modifier.weight(1f))
                if (kind != "SalesReceipt") {
                    Spacer(Modifier.width(8.dp))
                    FilterChip(openOnly, { openOnly = !openOnly }, label = { Text("Open") })
                }
            }
            Loading(data) { list ->
                val q = query.trim()
                val shown = list.filter { q.isEmpty() || it.s("number").contains(q, true) || it.s("contactName").contains(q, true) }
                if (shown.isEmpty()) EmptyState(if (list.isEmpty()) "No ${DocKinds.plural(kind).lowercase()} yet." else "Nothing matches.")
                LazyColumn(contentPadding = ContentPadding) {
                    items(shown, key = { it.i("id") }) { DocumentRow(it) }
                }
            }
        }
        Fab("New ${DocKinds.title(kind).lowercase()}") { app.navigate(Screen.DocumentEditor(kind)) }
    }
}

@Composable
fun DocumentDetailScreen(id: Int) {
    val app = LocalApp.current
    val context = app.context
    val data = load(id) { Core.obj("document", "id" to id) }
    var confirmVoid by remember { mutableStateOf(false) }
    var applying by remember { mutableStateOf(false) }
    var showText by remember { mutableStateOf(false) }
    var pendingPdf by remember { mutableStateOf<File?>(null) }

    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/pdf")) { uri ->
        val file = pendingPdf
        if (uri != null && file != null) app.run(success = "PDF saved") { Files.copyToUri(context, file, uri) }
    }

    fun makePdf(d: JSONObject, then: (File) -> Unit) {
        val file = File(app.exportDir, d.s("pdfName").ifEmpty { "document.pdf" })
        app.run(onDone = { _: Any? -> then(file) }) { Core.call("pdf", "id" to id, "path" to file.absolutePath) }
    }

    Loading(data) { d ->
        val kind = d.s("kind")
        val voided = d.b("voided")
        val balance = d.s("balance")
        ScrollPage {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("${d.s("title")} ${d.s("number")}", style = MaterialTheme.typography.headlineSmall)
                    Text(d.s("contactName"), color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = 2.dp))
                }
                StatusChip(d.s("status"))
            }
            Muted(buildString {
                append("Date ${d.s("date")}")
                if (d.b("hasDueDate")) append(if (kind == "Estimate") " · expires ${d.s("due")}" else " · due ${d.s("due")}")
                if (d.s("depositAccount").isNotEmpty()) append(" · deposited to ${d.s("depositAccount")}")
            })
            if (d.i("linkedId") != 0) {
                TextButton({ app.navigate(Screen.DocumentDetail(d.i("linkedId"))) }) {
                    Text(if (kind == "Estimate") "Converted to invoice ${d.s("linkedNumber")}" else "From estimate ${d.s("linkedNumber")}")
                }
            }
            if (d.i("recurringId") != 0) Muted("Created from a recurring invoice")
            if (d.s("memo").isNotEmpty()) Text(d.s("memo"))

            Card {
                d.list("lines").forEach { l ->
                    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
                        Column(Modifier.weight(1f)) {
                            Text(l.s("description").ifEmpty { l.s("accountName") })
                            Muted("${l.s("quantity").trimEnd('0').trimEnd('.')} × ${MoneyFmt.format(l.s("rate"))} · ${l.s("accountName")}" +
                                if (l.b("taxable") && kind != "Bill") " · taxable" else "")
                        }
                        MoneyText(l.s("amount"))
                    }
                    HorizontalDivider()
                }
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    TotalLine("Subtotal", d.s("subtotal"))
                    if (!MoneyFmt.isZero(d.s("tax"))) TotalLine("Tax (${d.s("taxRate")}%)", d.s("tax"))
                    TotalLine("Total", d.s("total"), bold = true)
                    if (!MoneyFmt.isZero(d.s("paid"))) TotalLine("Paid", "-" + d.s("paid"))
                    if (!MoneyFmt.isZero(d.s("credited"))) TotalLine("Credits applied", "-" + d.s("credited"))
                    if (!MoneyFmt.isZero(d.s("applied"))) TotalLine("Applied to invoices", "-" + d.s("applied"))
                    if (kind == "Invoice" || kind == "Bill" || kind == "CreditMemo") TotalLine(if (kind == "CreditMemo") "Remaining credit" else "Balance due", balance, bold = true)
                }
            }

            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!voided && kind == "Invoice" && !MoneyFmt.isZero(balance))
                    Button({ app.navigate(Screen.PaymentForm("Received", d.i("contactId"), id)) }) { Text("Receive payment") }
                if (!voided && kind == "Bill" && !MoneyFmt.isZero(balance))
                    Button({ app.navigate(Screen.PaymentForm("Paid", d.i("contactId"), id)) }) { Text("Pay bill") }
                if (!voided && kind == "CreditMemo" && !MoneyFmt.isZero(balance))
                    Button({ applying = true }) { Text("Apply to invoice") }
                if (!voided && kind == "Estimate" && d.s("estimateStatus") != "Converted") {
                    Button({
                        app.run(success = "Converted to an invoice", onDone = { newId: Int -> app.replace(Screen.DocumentDetail(newId)) }) {
                            Core.int("convertEstimate", "id" to id, "date" to today())
                        }
                    }) { Text("Convert to invoice") }
                }
                if (kind != "Bill") {
                    OutlinedButton({ makePdf(d) { Files.share(context, it, "application/pdf") } }) { Text("Share PDF") }
                    OutlinedButton({ makePdf(d) { if (!Files.view(context, it, "application/pdf")) app.message("No PDF viewer installed") } }) { Text("View PDF") }
                    OutlinedButton({ makePdf(d) { pendingPdf = it; saver.launch(it.name) } }) { Text("Save PDF…") }
                }
                OutlinedButton({ showText = !showText }) { Text(if (showText) "Hide text" else "Plain text") }
                if (!voided) OutlinedButton({ confirmVoid = true }) { Text("Void", color = Palette.negative) }
            }

            if (kind == "Estimate" && !voided && d.s("estimateStatus") != "Converted") {
                SectionTitle("Estimate status")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf("Pending", "Accepted", "Declined").forEach { s ->
                        FilterChip(d.s("estimateStatus") == s, {
                            app.run { Core.call("setEstimateStatus", "id" to id, "status" to s) }
                        }, label = { Text(s) })
                    }
                }
            }

            val payments = d.list("payments")
            if (payments.isNotEmpty()) {
                SectionTitle("Payments")
                Card { payments.forEach { p -> ListRow("Payment", p.s("date"), amount = p.s("amount")) } }
            }
            val credits = d.list("credits")
            if (credits.isNotEmpty()) {
                SectionTitle("Credits applied")
                Card {
                    credits.forEach { c ->
                        CreditRow("Credit memo ${c.s("number")}", c.s("amount")) {
                            app.run(success = "Credit removed") { Core.call("unapplyCredit", "creditMemoId" to c.i("creditMemoId"), "invoiceId" to id) }
                        }
                    }
                }
            }
            val applied = d.list("appliedTo")
            if (applied.isNotEmpty()) {
                SectionTitle("Applied to")
                Card {
                    applied.forEach { a ->
                        CreditRow("Invoice ${a.s("number")}", a.s("amount")) {
                            app.run(success = "Credit removed") { Core.call("unapplyCredit", "creditMemoId" to id, "invoiceId" to a.i("invoiceId")) }
                        }
                    }
                }
            }
            if (showText) {
                Card {
                    SelectionContainer {
                        Text(d.s("text"), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(12.dp))
                    }
                }
            }
        }
        if (confirmVoid) ConfirmDialog(
            title = "Void ${d.s("title").lowercase()} ${d.s("number")}?",
            text = "Voiding keeps the record but zeroes its amounts and reverses its effect on your books. This can't be undone.",
            confirm = "Void", destructive = true,
            onConfirm = { app.run(success = "Voided") { Core.call("voidDocument", "id" to id) } },
            onDismiss = { confirmVoid = false },
        )
        if (applying) ApplyCreditDialog(d) { applying = false }
    }
}

@Composable
private fun TotalLine(label: String, amount: String, bold: Boolean = false) {
    Row(Modifier.fillMaxWidth()) {
        Text(label, Modifier.weight(1f), fontWeight = if (bold) FontWeight.SemiBold else null)
        MoneyText(amount, bold = bold)
    }
}

@Composable
private fun CreditRow(label: String, amount: String, onRemove: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f))
        MoneyText(amount)
        TextButton(onRemove) { Text("Remove") }
    }
}

@Composable
private fun ApplyCreditDialog(memo: JSONObject, onDismiss: () -> Unit) {
    val app = LocalApp.current
    val invoices = load(memo.i("contactId")) {
        Core.arr("documents", "kind" to "Invoice", "contactId" to memo.i("contactId"), "openOnly" to true).objects()
    }
    var invoiceId by remember { mutableStateOf(0) }
    var amount by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = "Apply credit ${memo.s("number")}",
        confirm = "Apply",
        onDismiss = onDismiss,
        confirmEnabled = invoiceId != 0,
        onConfirm = {
            val value = MoneyFmt.parse(amount)
            if (value == null) error = "Enter an amount."
            else app.run(success = "Credit applied", onError = { error = it.message ?: "" }, onDone = { _: Any? -> onDismiss() }) {
                Core.call("applyCredit", "creditMemoId" to memo.i("id"), "invoiceId" to invoiceId, "amount" to value)
            }
        },
    ) {
        Text("Available credit: ${MoneyFmt.format(memo.s("balance"))}")
        Loading(invoices) { list ->
            if (list.isEmpty()) Muted("${memo.s("contactName")} has no open invoices.")
            else PickerField(
                "Invoice",
                list.map { Option(it.i("id"), "${it.s("number")} · ${it.s("date")}", "Balance ${MoneyFmt.format(it.s("balance"))}") },
                invoiceId,
                { picked ->
                    invoiceId = picked
                    val inv = list.first { it.i("id") == picked }
                    amount = dec(memo.s("balance")).min(dec(inv.s("balance"))).money()
                },
            )
        }
        MoneyInput("Amount", amount, { amount = it })
        ErrorText(error)
    }
}

// ---------------------------------------------------------------- document editor (also recurring templates)

private class LineState(
    itemId: Int = 0, accountId: Int = 0, description: String = "", quantity: String = "1", rate: String = "", taxable: Boolean = true,
) {
    var itemId by mutableStateOf(itemId)
    var accountId by mutableStateOf(accountId)
    var description by mutableStateOf(description)
    var quantity by mutableStateOf(quantity)
    var rate by mutableStateOf(rate)
    var taxable by mutableStateOf(taxable)

    fun amount(): BigDecimal =
        (quantity.trim().toBigDecimalOrNull() ?: BigDecimal.ONE).multiply(dec(rate)).setScale(2, RoundingMode.HALF_UP)

    fun isBlank() = itemId == 0 && description.isBlank() && rate.isBlank()
}

@Composable
fun DocumentEditorScreen(screen: Screen.DocumentEditor) {
    val app = LocalApp.current
    val recurring = screen.recurringId >= 0
    val kind = if (recurring) "Invoice" else screen.kind
    val sales = kind != "Bill"
    val refs = load { Triple(Core.arr("accounts").objects(), Core.arr("contacts", "kind" to DocKinds.party(kind)).objects(), Core.arr("items").objects()) }

    var contactId by remember { mutableStateOf(screen.contactId) }
    var date by remember { mutableStateOf(today()) }
    var due by remember { mutableStateOf("") }
    var number by remember { mutableStateOf("") }
    var memo by remember { mutableStateOf("") }
    var taxRate by remember { mutableStateOf("") }
    var depositId by remember { mutableStateOf(0) }
    val lines = remember { mutableStateListOf(LineState()) }
    // recurring template fields
    var name by remember { mutableStateOf("") }
    var frequency by remember { mutableStateOf("Monthly") }
    var interval by remember { mutableStateOf("1") }
    var start by remember { mutableStateOf(today()) }
    var end by remember { mutableStateOf("") }
    var active by remember { mutableStateOf(true) }
    var loaded by remember { mutableStateOf(screen.recurringId <= 0) }
    var error by remember { mutableStateOf("") }
    var saving by remember { mutableStateOf(false) }

    if (screen.recurringId > 0) {
        LaunchedEffect(screen.recurringId) {
            app.run(onDone = { r: JSONObject ->
                name = r.s("name"); contactId = r.i("contactId"); frequency = r.s("frequency"); interval = r.i("interval").toString()
                start = r.s("start"); end = r.s("end"); active = r.b("active"); memo = r.s("memo")
                taxRate = r.s("taxRate").let { if (MoneyFmt.isZero(it)) "" else it }
                lines.clear()
                r.list("lines").forEach { l ->
                    lines.add(LineState(l.i("itemId"), l.i("accountId"), l.s("description"), l.s("quantity"), l.s("rate"), l.b("taxable")))
                }
                if (lines.isEmpty()) lines.add(LineState())
                loaded = true
            }) { Core.obj("recurring", "id" to screen.recurringId) }
        }
    }

    Loading(refs) { (accounts, contacts, items) ->
        if (!loaded) return@Loading
        val lineAccounts = accountOptions(accounts) { !it.b("subledger") && it.s("role") != "salesTax" }
            .sortedBy { o -> if (o.detail == (if (sales) "Income" else "Expense")) 0 else 1 }
        val itemOptions = items.map { Option(it.i("id"), it.s("name"), MoneyFmt.format(it.s("price"))) }
        if (depositId == 0 && kind == "SalesReceipt") moneyAccounts(accounts).firstOrNull()?.let { depositId = it.id }

        val subtotal = lines.filter { !it.isBlank() }.fold(BigDecimal.ZERO) { s, l -> s + l.amount() }
        val taxable = lines.filter { !it.isBlank() && it.taxable }.fold(BigDecimal.ZERO) { s, l -> s + l.amount() }
        val tax = if (sales) taxable.multiply(taxRate.trim().removeSuffix("%").toBigDecimalOrNull() ?: BigDecimal.ZERO)
            .divide(BigDecimal(100)).setScale(2, RoundingMode.HALF_UP) else BigDecimal.ZERO

        ScrollPage {
            if (recurring) {
                TextInput("Template name", name, { name = it }, placeholder = "e.g. Monthly retainer")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextInput("Every", interval, { interval = it }, Modifier.weight(1f), keyboard = KeyboardType.Number)
                    ChoiceField("Period", listOf("Weekly", "Monthly", "Yearly"), frequency, { frequency = it }, Modifier.weight(2f))
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DateField("First invoice", start, { start = it }, Modifier.weight(1f))
                    DateField("End (optional)", end, { end = it }, Modifier.weight(1f), allowEmpty = true)
                }
                if (screen.recurringId > 0) CheckRow("Active", active) { active = it }
            }
            PickerField(DocKinds.party(kind), contactOptions(contacts), contactId, { contactId = it })
            if (!recurring) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    DateField("Date", date, { date = it }, Modifier.weight(1f))
                    if (DocKinds.hasDue(kind)) DateField(if (kind == "Estimate") "Expires" else "Due", due, { due = it }, Modifier.weight(1f), allowEmpty = true)
                }
                if (DocKinds.hasDue(kind) && due.isEmpty()) Muted("Leave the due date empty to use the ${DocKinds.party(kind).lowercase()}'s payment terms.")
                TextInput(if (kind == "Bill") "Vendor's bill number" else "Number", number, { number = it }, placeholder = "Automatic")
                if (kind == "SalesReceipt") PickerField("Deposit to", moneyAccounts(accounts), depositId, { depositId = it })
            }

            SectionTitle("Lines")
            lines.forEachIndexed { index, line ->
                Card {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (itemOptions.isNotEmpty()) PickerField("Product / service", itemOptions, line.itemId, { picked ->
                            line.itemId = picked
                            items.firstOrNull { it.i("id") == picked }?.let { item ->
                                if (line.description.isBlank()) line.description = item.s("description").ifEmpty { item.s("name") }
                                if (line.rate.isBlank() || MoneyFmt.isZero(line.rate)) line.rate = item.s("price")
                                val account = if (sales) item.i("incomeAccountId").takeIf { it != 0 } ?: item.i("expenseAccountId")
                                else item.i("expenseAccountId").takeIf { it != 0 } ?: item.i("incomeAccountId")
                                if (account != 0) line.accountId = account
                                line.taxable = item.b("taxable")
                            }
                        }, noneLabel = "None")
                        TextInput("Description", line.description, { line.description = it })
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextInput("Qty", line.quantity, { line.quantity = it }, Modifier.weight(1f), keyboard = KeyboardType.Decimal)
                            MoneyInput("Rate", line.rate, { line.rate = it }, Modifier.weight(2f))
                        }
                        PickerField(if (sales) "Income account" else "Expense account", lineAccounts, line.accountId, { line.accountId = it })
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (sales) Box(Modifier.weight(1f)) { CheckRow("Taxable", line.taxable) { line.taxable = it } }
                            else Spacer(Modifier.weight(1f))
                            MoneyText(line.amount().money(), bold = true)
                            if (lines.size > 1) TextButton({ lines.removeAt(index) }) { Text("Remove") }
                        }
                    }
                }
            }
            OutlinedButton({ lines.add(LineState()) }) { Text("+ Add line") }

            if (sales) TextInput("Sales tax rate (%)", taxRate, { taxRate = it }, keyboard = KeyboardType.Decimal, placeholder = "0")
            TextInput(if (sales) "Message / memo" else "Memo", memo, { memo = it }, singleLine = false)
            Card {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    TotalLine("Subtotal", subtotal.money())
                    if (tax.signum() != 0) TotalLine("Tax", tax.money())
                    TotalLine("Total", (subtotal + tax).money(), bold = true)
                }
            }
            ErrorText(error)
            Button(
                enabled = !saving,
                modifier = Modifier.fillMaxWidth(),
                onClick = {
                    val used = lines.filter { !it.isBlank() }
                    error = when {
                        contactId == 0 -> "Choose a ${DocKinds.party(kind).lowercase()}."
                        used.isEmpty() -> "Add at least one line."
                        used.any { it.accountId == 0 } -> "Choose an account for every line."
                        used.any { MoneyFmt.parse(it.rate) == null } -> "Enter a rate for every line."
                        used.any { it.quantity.isNotBlank() && it.quantity.trim().toBigDecimalOrNull() == null } -> "Quantities must be numbers."
                        recurring && name.isBlank() -> "Name this recurring invoice."
                        recurring && (interval.trim().toIntOrNull() ?: 0) < 1 -> "Repeat at least every 1 period."
                        else -> ""
                    }
                    if (error.isNotEmpty()) return@Button
                    val lineArgs = used.map { l ->
                        mapOf(
                            "itemId" to l.itemId, "accountId" to l.accountId, "description" to l.description.trim(),
                            "quantity" to l.quantity.trim().ifEmpty { "1" }, "rate" to MoneyFmt.parse(l.rate), "taxable" to l.taxable,
                        )
                    }
                    saving = true
                    if (recurring) {
                        val fields = arrayOf(
                            "name" to name.trim(), "contactId" to contactId, "frequency" to frequency, "interval" to interval.trim().toInt(),
                            "start" to start, "end" to end, "taxRate" to taxRate.trim(), "memo" to memo, "active" to active, "lines" to lineArgs,
                        )
                        app.run(success = "Recurring invoice saved", onError = { saving = false; error = it.message ?: "" }, onDone = { _: Any? -> app.back() }) {
                            if (screen.recurringId > 0) Core.call("updateRecurring", "id" to screen.recurringId, *fields)
                            else Core.call("addRecurring", *fields)
                        }
                    } else {
                        app.run(
                            success = "${DocKinds.title(kind)} saved",
                            onError = { saving = false; error = it.message ?: "" },
                            onDone = { id: Int -> app.replace(Screen.DocumentDetail(id)) },
                        ) {
                            Core.int(
                                "createDocument", "kind" to kind, "contactId" to contactId, "date" to date, "due" to due,
                                "number" to number.trim(), "memo" to memo, "taxRate" to (if (sales) taxRate.trim() else ""),
                                "depositAccountId" to depositId, "lines" to lineArgs,
                            )
                        }
                    }
                },
            ) { Text(if (recurring) "Save recurring invoice" else "Save ${DocKinds.title(kind).lowercase()}") }
        }
    }
}

// ---------------------------------------------------------------- recurring

@Composable
fun RecurringScreen() {
    val app = LocalApp.current
    val data = load { Core.arr("recurringList").objects() }
    var selected by remember { mutableStateOf<JSONObject?>(null) }
    var deleting by remember { mutableStateOf<JSONObject?>(null) }
    Box(Modifier.fillMaxSize()) {
        Loading(data) { list ->
            LazyColumn(contentPadding = ContentPadding) {
                val dueCount = list.count { it.s("status") == "Due" }
                if (dueCount > 0) item {
                    Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("$dueCount due now", Modifier.weight(1f))
                        Button({
                            app.run(onDone = { ids: List<Int> -> app.message("Created ${ids.size} invoice(s)") }) { Core.arr("runRecurring").ints() }
                        }) { Text("Create invoices") }
                    }
                }
                if (list.isEmpty()) item { EmptyState("No recurring invoices. Use them for retainers, rent, subscriptions…") }
                items(list, key = { it.i("id") }) { r ->
                    ListRow(
                        "${r.s("name")} · ${r.s("contactName")}",
                        r.s("schedule") + (if (r.s("next").isNotEmpty()) " · next ${r.s("next")}" else "") + " · ${r.i("created")} created",
                        amount = r.s("total"), status = r.s("status"), dim = !r.b("active"),
                    ) { selected = r }
                }
            }
        }
        Fab("New recurring") { app.navigate(Screen.DocumentEditor("Invoice", recurringId = 0)) }
    }
    selected?.let { r ->
        AlertDialog(
            onDismissRequest = { selected = null },
            title = { Text(r.s("name")) },
            text = { Text("${r.s("contactName")}\n${r.s("schedule")} from ${r.s("start")}${if (r.s("end").isNotEmpty()) " until " + r.s("end") else ""}\n${r.i("created")} invoice(s) created so far") },
            confirmButton = {
                Column(horizontalAlignment = Alignment.End) {
                    TextButton({ selected = null; app.navigate(Screen.DocumentEditor("Invoice", recurringId = r.i("id"))) }) { Text("Edit") }
                    TextButton({
                        selected = null
                        app.run(success = if (r.b("active")) "Paused" else "Resumed") { Core.call("setRecurringActive", "id" to r.i("id"), "active" to !r.b("active")) }
                    }) { Text(if (r.b("active")) "Pause" else "Resume") }
                    TextButton({ selected = null; deleting = r }) { Text("Delete", color = Palette.negative) }
                    TextButton({ selected = null }) { Text("Close") }
                }
            },
        )
    }
    deleting?.let { r ->
        ConfirmDialog(
            "Delete ${r.s("name")}?", "Invoices it already created are kept.", "Delete",
            onConfirm = { app.run(success = "Deleted") { Core.call("deleteRecurring", "id" to r.i("id")) } },
            onDismiss = { deleting = null }, destructive = true,
        )
    }
}

// ---------------------------------------------------------------- payments

@Composable
fun PaymentFormScreen(screen: Screen.PaymentForm) {
    val app = LocalApp.current
    val received = screen.kind == "Received"
    val party = if (received) "Customer" else "Vendor"
    var contactId by remember { mutableStateOf(screen.contactId) }
    var date by remember { mutableStateOf(today()) }
    var accountId by remember { mutableStateOf(0) }
    var amount by remember { mutableStateOf("") }
    var ref by remember { mutableStateOf("") }
    var memo by remember { mutableStateOf("") }
    val applied = remember { mutableStateMapOf<Int, String>() }
    var error by remember { mutableStateOf("") }
    var saving by remember { mutableStateOf(false) }

    val refs = load { Core.arr("accounts").objects() to Core.arr("contacts", "kind" to party).objects() }
    val docs = load(contactId) {
        if (contactId == 0) emptyList()
        else Core.arr("documents", "kind" to (if (received) "Invoice" else "Bill"), "contactId" to contactId, "openOnly" to true).objects().reversed()
    }
    // Pre-fill when opened from a specific invoice or bill.
    LaunchedEffect(docs.value) {
        val list = docs.value ?: return@LaunchedEffect
        if (screen.documentId != 0 && applied.isEmpty()) {
            list.firstOrNull { it.i("id") == screen.documentId }?.let { applied[it.i("id")] = it.s("balance"); amount = it.s("balance") }
        }
    }

    Loading(refs) { (accounts, contacts) ->
        val money = moneyAccounts(accounts)
        if (accountId == 0) money.firstOrNull()?.let { accountId = it.id }
        val appliedTotal = applied.values.fold(BigDecimal.ZERO) { s, v -> s + dec(v) }
        ScrollPage {
            PickerField(party, contactOptions(contacts), contactId, { contactId = it; applied.clear() })
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DateField("Date", date, { date = it }, Modifier.weight(1f))
                MoneyInput("Amount", amount, { amount = it }, Modifier.weight(1f))
            }
            PickerField(if (received) "Deposit to" else "Pay from", money, accountId, { accountId = it })
            TextInput(if (received) "Reference (check #)" else "Check # / reference", ref, { ref = it })
            TextInput("Memo", memo, { memo = it })

            if (contactId != 0) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    SectionTitle(if (received) "Open invoices" else "Open bills", Modifier.weight(1f))
                    TextButton({
                        // Oldest first, until the amount runs out (or everything, when no amount is entered).
                        var left = if (amount.isBlank()) null else dec(amount)
                        applied.clear()
                        docs.value?.forEach { d ->
                            val bal = dec(d.s("balance"))
                            val take = left?.min(bal) ?: bal
                            if (take.signum() > 0) {
                                applied[d.i("id")] = take.money()
                                left = left?.subtract(take)
                            }
                        }
                        if (amount.isBlank()) amount = applied.values.fold(BigDecimal.ZERO) { s, v -> s + dec(v) }.money()
                    }) { Text("Auto-apply") }
                }
                Loading(docs) { list ->
                    if (list.isEmpty()) Muted("Nothing open — the payment will be kept as a credit.")
                    Card {
                        list.forEach { d ->
                            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text("${d.s("number")} · due ${d.s("due")}")
                                    Muted("Balance ${MoneyFmt.format(d.s("balance"))}")
                                }
                                MoneyInput("Apply", applied[d.i("id")] ?: "", { applied[d.i("id")] = it }, Modifier.width(130.dp))
                            }
                        }
                    }
                }
                val total = dec(amount)
                Muted("Applied ${MoneyFmt.format(appliedTotal.money())} of ${MoneyFmt.format(total.money())}" +
                    if (total > appliedTotal) " · ${MoneyFmt.format((total - appliedTotal).money())} left as credit" else "")
            }
            ErrorText(error)
            Button(
                enabled = !saving,
                modifier = Modifier.fillMaxWidth(),
                onClick = {
                    val value = if (amount.isBlank()) appliedTotal.money() else MoneyFmt.parse(amount)
                    error = when {
                        contactId == 0 -> "Choose a ${party.lowercase()}."
                        accountId == 0 -> "Choose an account."
                        value == null || dec(value).signum() <= 0 -> "Enter an amount greater than zero."
                        applied.values.any { it.isNotBlank() && MoneyFmt.parse(it) == null } -> "Check the applied amounts."
                        appliedTotal > dec(value) -> "You applied more than the payment amount."
                        else -> ""
                    }
                    if (error.isNotEmpty()) return@Button
                    saving = true
                    val apps = applied.filter { dec(it.value).signum() != 0 }.map { mapOf("documentId" to it.key, "amount" to MoneyFmt.parse(it.value)) }
                    app.run(
                        success = if (received) "Payment recorded" else "Bill payment recorded",
                        onError = { saving = false; error = it.message ?: "" },
                        onDone = { _: Int -> app.back() },
                    ) {
                        Core.int(
                            "recordPayment", "kind" to screen.kind, "contactId" to contactId, "date" to date, "accountId" to accountId,
                            "amount" to value, "ref" to ref.trim(), "memo" to memo, "applications" to apps,
                        )
                    }
                },
            ) { Text(if (received) "Record payment" else "Record bill payment") }
        }
    }
}

@Composable
fun PaymentsScreen() {
    val app = LocalApp.current
    var kind by rememberSaveable { mutableStateOf("") }
    val data = load(kind) { Core.arr("payments", "kind" to kind).objects() }
    var selected by remember { mutableStateOf<JSONObject?>(null) }
    Box(Modifier.fillMaxSize()) {
        Column {
            Row(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(kind == "", { kind = "" }, label = { Text("All") })
                FilterChip(kind == "Received", { kind = "Received" }, label = { Text("Received") })
                FilterChip(kind == "Paid", { kind = "Paid" }, label = { Text("Paid") })
            }
            Loading(data) { list ->
                if (list.isEmpty()) EmptyState("No payments yet.")
                LazyColumn(contentPadding = ContentPadding) {
                    items(list, key = { it.i("id") }) { p ->
                        ListRow(
                            "${p.s("contactName")}${if (p.s("ref").isNotEmpty()) " · #" + p.s("ref") else ""}",
                            "${p.s("date")} · ${if (p.s("kind") == "Received") "received into" else "paid from"} ${p.s("accountName")}",
                            amount = if (p.s("kind") == "Paid") "-" + p.s("amount") else p.s("amount"),
                            status = if (p.b("voided")) "Void" else if (!MoneyFmt.isZero(p.s("unapplied"))) "Unapplied ${MoneyFmt.format(p.s("unapplied"))}" else null,
                            dim = p.b("voided"),
                        ) { selected = p }
                    }
                }
            }
        }
        Fab("Receive payment") { app.navigate(Screen.PaymentForm("Received")) }
    }
    selected?.let { p ->
        var confirm by remember { mutableStateOf(false) }
        AlertDialog(
            onDismissRequest = { selected = null },
            title = { Text("Payment ${if (p.s("kind") == "Received") "from" else "to"} ${p.s("contactName")}") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("${p.s("date")} · ${MoneyFmt.format(p.s("amount"))}")
                    Text("Account: ${p.s("accountName")}")
                    if (p.s("ref").isNotEmpty()) Text("Reference: ${p.s("ref")}")
                    val to = p.optJSONArray("appliedTo")?.strings() ?: emptyList()
                    if (to.isNotEmpty()) Text("Applied to: " + to.joinToString(", "))
                    if (!MoneyFmt.isZero(p.s("unapplied"))) Text("Unapplied credit: ${MoneyFmt.format(p.s("unapplied"))}")
                    if (p.b("voided")) StatusChip("Void")
                }
            },
            confirmButton = { TextButton({ selected = null }) { Text("Close") } },
            dismissButton = { if (!p.b("voided")) TextButton({ confirm = true }) { Text("Void", color = Palette.negative) } },
        )
        if (confirm) ConfirmDialog(
            "Void this payment?", "The invoices or bills it paid become open again. This can't be undone.", "Void",
            onConfirm = { selected = null; app.run(success = "Payment voided") { Core.call("voidPayment", "id" to p.i("id")) } },
            onDismiss = { confirm = false }, destructive = true,
        )
    }
}
