package org.openbooks.android

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import java.math.BigDecimal

/** Picks the account a banking screen works on; defaults to the first bank account. */
@Composable
private fun AccountHeader(accountId: Int, onChange: (Int) -> Unit, moneyOnly: Boolean): List<JSONObject>? {
    val accounts = load { Core.arr("accounts").objects() }
    val list = accounts.value ?: return null
    val options = if (moneyOnly) moneyAccounts(list) else accountOptions(list)
    if (accountId == 0) options.firstOrNull()?.let { onChange(it.id) }
    PickerField("Account", options, accountId, onChange, Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp))
    return list
}

// ---------------------------------------------------------------- register

@Composable
fun RegisterScreen(initialAccount: Int) {
    var accountId by rememberSaveable { mutableStateOf(initialAccount) }
    var quick by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf<JSONObject?>(null) }
    Box(Modifier.fillMaxSize()) {
        Column {
            val accounts = AccountHeader(accountId, { accountId = it }, moneyOnly = false)
            if (accountId != 0) {
                val data = load(accountId) { Core.obj("register", "accountId" to accountId) }
                Loading(data) { r ->
                    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Muted("Balance")
                            MoneyText(r.s("ending"), big = true)
                        }
                        Column(horizontalAlignment = Alignment.End) {
                            Muted("Cleared")
                            MoneyText(r.s("cleared"))
                        }
                    }
                    HorizontalDivider(Modifier.padding(top = 8.dp))
                    val rows = r.list("rows").reversed()
                    if (rows.isEmpty()) EmptyState("No transactions in this account yet.")
                    LazyColumn(contentPadding = ContentPadding) {
                        items(rows) { row ->
                            RegisterRow(row) { selected = row }
                        }
                    }
                }
            }
            if (accounts == null) Loading(Loaded<Unit>(null, null)) {}
        }
        Fab("New transaction") { quick = true }
    }
    if (quick) QuickEntryDialog("Expense", accountId) { quick = false }
    selected?.let { row -> TransactionDialog(row, accountId) { selected = null } }
}

@Composable
private fun RegisterRow(row: JSONObject, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(row.s("payee").ifEmpty { row.s("kind") })
            Muted(listOf(row.s("date"), row.s("kind"), row.s("ref").let { if (it.isEmpty()) "" else "#$it" }, row.s("offset"))
                .filter { it.isNotEmpty() }.joinToString(" · "))
        }
        Column(horizontalAlignment = Alignment.End) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (row.s("cleared").isNotEmpty()) Text(row.s("cleared") + "  ", color = Palette.positive, fontWeight = FontWeight.Bold)
                MoneyText(row.s("amount"), colorize = true)
            }
            Muted(MoneyFmt.format(row.s("balance")))
        }
    }
    HorizontalDivider()
}

@Composable
private fun TransactionDialog(row: JSONObject, accountId: Int, onDismiss: () -> Unit) {
    val app = LocalApp.current
    val id = row.i("txnId")
    val data = load(id) { Core.obj("transaction", "id" to id) to Core.arr("accounts").objects() }
    var recategorizing by remember { mutableStateOf(false) }
    var newAccount by remember { mutableStateOf(0) }
    var confirmVoid by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(row.s("payee").ifEmpty { row.s("kind") }) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Loading(data) { (t, accounts) ->
                    TableView(t.getJSONObject("table"), firstColumn = 150.dp, column = 90.dp)
                    val from = row.i("offsetAccountId")
                    if (recategorizing && from != 0) {
                        PickerField("Move from ${row.s("offset")} to", postingAccounts(accounts).filter { it.id != from && it.id != accountId }, newAccount, { newAccount = it })
                        Button({
                            app.run(success = "Recategorized", onDone = { _: Any? -> onDismiss() }) {
                                Core.call("recategorize", "id" to id, "fromAccountId" to from, "toAccountId" to newAccount)
                            }
                        }, enabled = newAccount != 0) { Text("Move") }
                    }
                    if (!t.b("manual")) Muted("Created by a ${row.s("kind").lowercase()}: void or change it from that document or payment.")
                }
            }
        },
        confirmButton = { TextButton(onDismiss) { Text("Close") } },
        dismissButton = {
            Row {
                if (row.i("offsetAccountId") != 0 && !recategorizing) TextButton({ recategorizing = true }) { Text("Recategorize") }
                if (data.value?.first?.b("manual") == true) TextButton({ confirmVoid = true }) { Text("Void", color = Palette.negative) }
            }
        },
    )
    if (confirmVoid) ConfirmDialog(
        "Void this transaction?", "Its amounts are reversed out of every account. This can't be undone.", "Void",
        onConfirm = { app.run(success = "Transaction voided", onDone = { _: Any? -> onDismiss() }) { Core.call("voidTransaction", "id" to id) } },
        onDismiss = { confirmVoid = false }, destructive = true,
    )
}

/** Expense, deposit or transfer in one dialog, like the desktop's quick entry. */
@Composable
fun QuickEntryDialog(initialKind: String, moneyAccountId: Int, onDismiss: () -> Unit) {
    val app = LocalApp.current
    val accounts = load { Core.arr("accounts").objects() }
    var kind by remember { mutableStateOf(initialKind) }
    var date by remember { mutableStateOf(today()) }
    var money by remember { mutableStateOf(moneyAccountId) }
    var other by remember { mutableStateOf(0) }
    var amount by remember { mutableStateOf("") }
    var payee by remember { mutableStateOf("") }
    var memo by remember { mutableStateOf("") }
    var ref by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = "Record ${kind.lowercase()}",
        confirm = "Save",
        onDismiss = onDismiss,
        onConfirm = {
            val value = MoneyFmt.parse(amount)
            error = when {
                value == null || dec(value).signum() <= 0 -> "Enter an amount greater than zero."
                money == 0 || other == 0 -> "Choose both accounts."
                money == other -> "Choose two different accounts."
                else -> ""
            }
            if (error.isEmpty()) {
                // Expense: money leaves the bank. Deposit: money enters it. Transfer: from -> to.
                val debit = if (kind == "Deposit") money else other
                val credit = if (kind == "Deposit") other else money
                app.run(success = "Recorded ${kind.lowercase()} of ${MoneyFmt.format(value!!)}", onError = { error = it.message ?: "" }, onDone = { _: Int -> onDismiss() }) {
                    Core.int(
                        "postTransaction", "kind" to kind, "date" to date, "payee" to payee.trim(), "memo" to memo.trim(), "ref" to ref.trim(),
                        "splits" to listOf(mapOf("accountId" to debit, "amount" to value), mapOf("accountId" to credit, "amount" to "-$value")),
                    )
                }
            }
        },
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("Expense", "Deposit", "Transfer").forEach { k -> FilterChip(kind == k, { kind = k; other = 0 }, label = { Text(k) }) }
        }
        Loading(accounts) { list ->
            val moneyOptions = moneyAccounts(list)
            if (money == 0) moneyOptions.firstOrNull()?.let { money = it.id }
            DateField("Date", date, { date = it })
            when (kind) {
                "Expense" -> {
                    PickerField("Paid from", moneyOptions, money, { money = it })
                    PickerField("Category", postingAccounts(list).sortedBy { if (it.detail == "Expense") 0 else 1 }, other, { other = it })
                    TextInput("Payee", payee, { payee = it })
                }
                "Deposit" -> {
                    PickerField("Deposit to", moneyOptions, money, { money = it })
                    PickerField("Category", postingAccounts(list).sortedBy { if (it.detail == "Income") 0 else 1 }, other, { other = it })
                    TextInput("Received from", payee, { payee = it })
                }
                else -> {
                    PickerField("From", moneyOptions, money, { money = it })
                    PickerField("To", moneyOptions, other, { other = it })
                }
            }
        }
        MoneyInput("Amount", amount, { amount = it })
        TextInput("Memo", memo, { memo = it })
        TextInput("Ref / check #", ref, { ref = it })
        ErrorText(error)
    }
}

// ---------------------------------------------------------------- reconcile

@Composable
fun ReconcileScreen(initialAccount: Int) {
    val app = LocalApp.current
    var accountId by rememberSaveable { mutableStateOf(initialAccount) }
    var through by rememberSaveable { mutableStateOf(today()) }
    var statement by rememberSaveable { mutableStateOf("") }
    Column {
        AccountHeader(accountId, { accountId = it }, moneyOnly = true)
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            DateField("Statement date", through, { through = it }, Modifier.weight(1f))
            MoneyInput("Ending balance", statement, { statement = it }, Modifier.weight(1f))
        }
        if (accountId == 0) return@Column
        val data = load(accountId, through) { Core.obj("reconcile", "accountId" to accountId, "through" to through) }
        Loading(data) { r ->
            val cleared = dec(r.s("clearedBalance"))
            val difference = if (MoneyFmt.parse(statement) == null) null else dec(statement) - cleared
            Card(Modifier.fillMaxWidth().padding(16.dp)) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Row { Text("Previously reconciled", Modifier.weight(1f)); MoneyText(r.s("reconciled")) }
                    Row { Text("Cleared balance", Modifier.weight(1f)); MoneyText(cleared.money()) }
                    Row {
                        Text("Difference", Modifier.weight(1f), fontWeight = FontWeight.SemiBold)
                        if (difference == null) Muted("enter the statement balance") else MoneyText(difference.money(), bold = true, colorize = difference.signum() != 0)
                    }
                    Button(
                        enabled = difference != null && difference.signum() == 0,
                        modifier = Modifier.fillMaxWidth(),
                        onClick = {
                            app.run(onDone = { n: Int -> app.message("Reconciled $n transaction(s)"); statement = "" }) {
                                Core.int("finishReconciliation", "accountId" to accountId)
                            }
                        },
                    ) { Text("Finish reconciliation") }
                }
            }
            val items = r.list("items")
            if (items.isEmpty()) EmptyState("Nothing left to reconcile through $through.")
            LazyColumn(contentPadding = ContentPadding) {
                items(items, key = { it.i("txnId") }) { t ->
                    Row(
                        Modifier.fillMaxWidth().clickable {
                            app.run { Core.call("setCleared", "txnId" to t.i("txnId"), "accountId" to accountId, "cleared" to !t.b("cleared")) }
                        }.padding(horizontal = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Checkbox(t.b("cleared"), { c -> app.run { Core.call("setCleared", "txnId" to t.i("txnId"), "accountId" to accountId, "cleared" to c) } })
                        Column(Modifier.weight(1f)) {
                            Text(t.s("payee").ifEmpty { "—" })
                            Muted(t.s("date"))
                        }
                        MoneyText(t.s("amount"), colorize = true, modifier = Modifier.padding(end = 8.dp))
                    }
                    HorizontalDivider()
                }
            }
        }
    }
}

// ---------------------------------------------------------------- journal entry

private class JournalLine {
    var accountId by mutableStateOf(0)
    var debit by mutableStateOf("")
    var credit by mutableStateOf("")
    var memo by mutableStateOf("")
}

@Composable
fun JournalScreen() {
    val app = LocalApp.current
    val accounts = load { Core.arr("accounts").objects() }
    var date by remember { mutableStateOf(today()) }
    var ref by remember { mutableStateOf("") }
    var memo by remember { mutableStateOf("") }
    val lines = remember { mutableStateListOf(JournalLine(), JournalLine()) }
    var error by remember { mutableStateOf("") }
    Loading(accounts) { list ->
        val options = accountOptions(list)
        val debits = lines.fold(BigDecimal.ZERO) { s, l -> s + dec(l.debit) }
        val credits = lines.fold(BigDecimal.ZERO) { s, l -> s + dec(l.credit) }
        ScrollPage {
            Muted("For adjustments, depreciation, opening balances and corrections. Debits must equal credits.")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DateField("Date", date, { date = it }, Modifier.weight(1f))
                TextInput("Ref", ref, { ref = it }, Modifier.weight(1f))
            }
            TextInput("Memo", memo, { memo = it })
            lines.forEachIndexed { index, line ->
                Card {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        PickerField("Account", options, line.accountId, { line.accountId = it })
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                            MoneyInput("Debit", line.debit, { line.debit = it; if (it.isNotBlank()) line.credit = "" }, Modifier.weight(1f))
                            MoneyInput("Credit", line.credit, { line.credit = it; if (it.isNotBlank()) line.debit = "" }, Modifier.weight(1f))
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            TextInput("Line memo", line.memo, { line.memo = it }, Modifier.weight(1f))
                            if (lines.size > 2) TextButton({ lines.removeAt(index) }) { Text("Remove") }
                        }
                    }
                }
            }
            OutlinedButton({ lines.add(JournalLine()) }) { Text("+ Add line") }
            Row {
                Text("Debits ${debits.money()}   Credits ${credits.money()}", Modifier.weight(1f))
                if (debits.compareTo(credits) != 0) Text("Off by ${(debits - credits).abs().money()}", color = Palette.negative)
            }
            ErrorText(error)
            Button(
                modifier = Modifier.fillMaxWidth(),
                enabled = debits.signum() > 0 && debits.compareTo(credits) == 0,
                onClick = {
                    val used = lines.filter { it.debit.isNotBlank() || it.credit.isNotBlank() }
                    error = when {
                        used.any { it.accountId == 0 } -> "Choose an account on every line with an amount."
                        used.any { (it.debit.isNotBlank() && MoneyFmt.parse(it.debit) == null) || (it.credit.isNotBlank() && MoneyFmt.parse(it.credit) == null) } -> "Check the amounts."
                        else -> ""
                    }
                    if (error.isNotEmpty()) return@Button
                    val splits = used.map { l ->
                        mapOf("accountId" to l.accountId, "amount" to (dec(l.debit) - dec(l.credit)).money(), "memo" to l.memo.trim())
                    }
                    app.run(success = "Journal entry posted", onError = { error = it.message ?: "" }, onDone = { _: Int ->
                        lines.clear(); lines.add(JournalLine()); lines.add(JournalLine()); ref = ""; memo = ""
                    }) {
                        Core.int("postTransaction", "kind" to "Journal", "date" to date, "ref" to ref.trim(), "memo" to memo.trim(), "splits" to splits)
                    }
                },
            ) { Text("Post journal entry") }
        }
    }
}

// ---------------------------------------------------------------- CSV import

@Composable
fun ImportCsvScreen(initialAccount: Int) {
    val app = LocalApp.current
    val context = app.context
    var accountId by rememberSaveable { mutableStateOf(initialAccount) }
    var fileName by remember { mutableStateOf("") }
    var text by remember { mutableStateOf("") }
    var dateCol by remember { mutableStateOf("1") }
    var descCol by remember { mutableStateOf("2") }
    var amountCol by remember { mutableStateOf("3") }
    var header by remember { mutableStateOf(true) }
    var negate by remember { mutableStateOf(false) }
    var offset by remember { mutableStateOf(0) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) app.run(onDone = { t: String -> text = t; fileName = Files.displayName(context, uri) ?: "statement.csv" }) {
            Files.readText(context, uri)
        }
    }
    fun options() = arrayOf(
        "dateColumn" to (dateCol.toIntOrNull() ?: 1), "descriptionColumn" to (descCol.toIntOrNull() ?: 2),
        "amountColumn" to (amountCol.toIntOrNull() ?: 3), "hasHeader" to header, "negate" to negate, "offsetAccountId" to offset,
    )
    Column {
        val accounts = AccountHeader(accountId, { accountId = it }, moneyOnly = true)
        ScrollPage {
            Muted("Import a statement exported from your bank or card as CSV. Each row becomes a transaction you can recategorize later. Rows already imported are skipped.")
            OutlinedButton({ picker.launch(arrayOf("text/*", "application/csv", "application/vnd.ms-excel", "*/*")) }) {
                Text(if (fileName.isEmpty()) "Choose CSV file…" else fileName)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextInput("Date col", dateCol, { dateCol = it }, Modifier.weight(1f), keyboard = KeyboardType.Number)
                TextInput("Description", descCol, { descCol = it }, Modifier.weight(1f), keyboard = KeyboardType.Number)
                TextInput("Amount col", amountCol, { amountCol = it }, Modifier.weight(1f), keyboard = KeyboardType.Number)
            }
            CheckRow("First row is a header", header) { header = it }
            CheckRow("Flip signs (card statements that show charges as positive)", negate) { negate = it }
            if (accounts != null) PickerField("Categorize to", postingAccounts(accounts), offset, { offset = it }, noneLabel = "Uncategorized")
            if (text.isNotEmpty()) {
                val preview = load(text, dateCol, descCol, amountCol, header, negate) { Core.arr("csvPreview", "text" to text, *options()).objects() }
                Loading(preview) { rows ->
                    val bad = rows.count { !it.b("dateValid") || !it.b("amountValid") }
                    SectionTitle("Preview (${rows.size} rows${if (bad > 0) ", $bad with problems" else ""})")
                    Card {
                        rows.take(50).forEach { r ->
                            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp)) {
                                Text(r.s("date"), Modifier.width(96.dp), color = if (r.b("dateValid")) MaterialTheme.colorScheme.onSurface else Palette.negative)
                                Text(r.s("description"), Modifier.weight(1f), maxLines = 1)
                                if (r.b("amountValid")) MoneyText(r.s("amount"), colorize = true) else Text(r.s("amount"), color = Palette.negative)
                            }
                        }
                    }
                    Button(
                        enabled = accountId != 0 && rows.isNotEmpty() && bad == 0,
                        modifier = Modifier.fillMaxWidth(),
                        onClick = {
                            app.run(onDone = { r: JSONObject ->
                                app.message("Imported ${r.i("imported")}, skipped ${r.i("skipped")} already imported")
                                text = ""; fileName = ""
                            }) { Core.obj("importCsv", "accountId" to accountId, "text" to text, *options()) }
                        },
                    ) { Text("Import") }
                    if (bad > 0) Muted("Fix the column numbers so every row has a valid date and amount.")
                }
            }
        }
    }
}
