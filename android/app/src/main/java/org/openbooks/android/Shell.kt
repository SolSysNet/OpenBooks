package org.openbooks.android

import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import java.io.File
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppRoot() {
    val app = LocalApp.current
    if (!app.bookOpen) {
        Scaffold(snackbarHost = { SnackbarHost(app.snackbar) }) { padding ->
            Box(Modifier.fillMaxSize().padding(padding)) { WelcomeScreen() }
        }
        return
    }
    // Composed only while books are open: a drawer first composed with empty content measures
    // zero width and then appears open once its content arrives.
    val drawer = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    BackHandler(enabled = drawer.isOpen || app.stack.size > 1) {
        if (drawer.isOpen) scope.launch { drawer.close() } else app.back()
    }
    ModalNavigationDrawer(
        drawerState = drawer,
        drawerContent = {
            ModalDrawerSheet {
                DrawerContent { screen ->
                    scope.launch { drawer.close() }
                    if (screen == null) app.closeBooks() else app.root(screen)
                }
            }
        },
    ) {
        Scaffold(
            snackbarHost = { SnackbarHost(app.snackbar) },
            topBar = {
                TopAppBar(
                    title = { Text(titleOf(app.current), maxLines = 1) },
                    navigationIcon = {
                        if (app.stack.size > 1) IconButton({ app.back() }) { Text("←", fontSize = 22.sp) }
                        else IconButton({ scope.launch { drawer.open() } }) { Text("☰", fontSize = 22.sp) }
                    },
                )
            },
        ) { padding ->
            Box(Modifier.fillMaxSize().padding(padding)) { CurrentScreen(app.current) }
        }
    }
}

fun titleOf(screen: Screen): String = when (screen) {
    Screen.Dashboard -> "Dashboard"
    Screen.Reports -> "Reports"
    is Screen.Contacts -> if (screen.kind == "Customer") "Customers" else "Vendors"
    is Screen.ContactDetail -> "Contact"
    is Screen.Documents -> DocKinds.plural(screen.kind)
    is Screen.DocumentDetail -> "Document"
    is Screen.DocumentEditor -> when {
        screen.recurringId > 0 -> "Edit recurring invoice"
        screen.recurringId == 0 -> "New recurring invoice"
        else -> "New " + DocKinds.title(screen.kind).lowercase()
    }
    Screen.Recurring -> "Recurring invoices"
    is Screen.PaymentForm -> if (screen.kind == "Received") "Receive payment" else "Pay bills"
    Screen.Payments -> "Payments"
    is Screen.Register -> "Register"
    is Screen.Reconcile -> "Reconcile"
    Screen.Journal -> "Journal entry"
    is Screen.ImportCsv -> "Import statement"
    Screen.Accounts -> "Chart of accounts"
    Screen.Items -> "Products & services"
    Screen.Company -> "Company settings"
}

@Composable
private fun CurrentScreen(screen: Screen) {
    when (screen) {
        Screen.Dashboard -> DashboardScreen()
        Screen.Reports -> ReportsScreen()
        is Screen.Contacts -> ContactsScreen(screen.kind)
        is Screen.ContactDetail -> ContactDetailScreen(screen.id)
        is Screen.Documents -> DocumentsScreen(screen.kind)
        is Screen.DocumentDetail -> DocumentDetailScreen(screen.id)
        is Screen.DocumentEditor -> DocumentEditorScreen(screen)
        Screen.Recurring -> RecurringScreen()
        is Screen.PaymentForm -> PaymentFormScreen(screen)
        Screen.Payments -> PaymentsScreen()
        is Screen.Register -> RegisterScreen(screen.accountId)
        is Screen.Reconcile -> ReconcileScreen(screen.accountId)
        Screen.Journal -> JournalScreen()
        is Screen.ImportCsv -> ImportCsvScreen(screen.accountId)
        Screen.Accounts -> AccountsScreen()
        Screen.Items -> ItemsScreen()
        Screen.Company -> CompanyScreen()
    }
}

@Composable
private fun DrawerContent(go: (Screen?) -> Unit) {
    val app = LocalApp.current
    Column(Modifier.verticalScroll(rememberScrollState()).padding(vertical = 12.dp)) {
        Text(app.companyName, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp))
        if (app.encrypted) Muted("🔒 Password protected", Modifier.padding(horizontal = 24.dp))
        Spacer(Modifier.height(8.dp))
        @Composable
        fun item(label: String, screen: Screen?) = NavigationDrawerItem(
            label = { Text(label) },
            selected = screen != null && app.stack.firstOrNull() == screen,
            onClick = { go(screen) },
            modifier = Modifier.padding(horizontal = 12.dp),
        )
        @Composable
        fun header(text: String) = Text(
            text, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(start = 28.dp, top = 16.dp, bottom = 4.dp),
        )
        item("Dashboard", Screen.Dashboard)
        header("Sales")
        item("Customers", Screen.Contacts("Customer"))
        item("Invoices", Screen.Documents("Invoice"))
        item("Estimates", Screen.Documents("Estimate"))
        item("Sales receipts", Screen.Documents("SalesReceipt"))
        item("Credit memos", Screen.Documents("CreditMemo"))
        item("Recurring invoices", Screen.Recurring)
        item("Payments", Screen.Payments)
        header("Expenses")
        item("Vendors", Screen.Contacts("Vendor"))
        item("Bills", Screen.Documents("Bill"))
        header("Banking")
        item("Registers", Screen.Register())
        item("Reconcile", Screen.Reconcile())
        item("Journal entry", Screen.Journal)
        item("Import statement (CSV)", Screen.ImportCsv())
        header("Reports & setup")
        item("Reports", Screen.Reports)
        item("Chart of accounts", Screen.Accounts)
        item("Products & services", Screen.Items)
        item("Company settings", Screen.Company)
        HorizontalDivider(Modifier.padding(vertical = 8.dp))
        item(if (app.encrypted) "Lock & close books" else "Close books", null)
    }
}

/** Floating action button for list screens. */
@Composable
fun BoxScope.Fab(text: String, onClick: () -> Unit) {
    ExtendedFloatingActionButton(
        onClick = onClick,
        modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
    ) { Text(text) }
}

// ---------------------------------------------------------------- welcome / open / create

@Composable
private fun WelcomeScreen() {
    val app = LocalApp.current
    val context = app.context
    var refresh by remember { mutableIntStateOf(0) }
    val files = remember(refresh, app.version) { Files.books(app.booksDir) }
    var unlocking by remember { mutableStateOf<File?>(null) }
    var creating by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<File?>(null) }

    fun open(file: File) {
        if (Files.isEncrypted(file)) {
            if (app.cryptoOk == false) app.message("Encryption is unavailable on this device")
            else unlocking = file
            return
        }
        app.run(onDone = { status: org.json.JSONObject -> app.applyStatus(status); app.root(Screen.Dashboard) }) {
            Core.obj("open", "path" to file.absolutePath)
        }
    }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) app.run(success = "Books imported", onDone = { f: File -> refresh++; open(f) }) {
            Files.importBooks(context, uri, app.booksDir)
        }
    }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        Text("OpenBooks", style = MaterialTheme.typography.displaySmall, fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
        Text("Open source bookkeeping. Your books stay on this device — the app has no internet access.", color = Palette.muted)
        if (app.cryptoOk == false) {
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
                Column(Modifier.padding(12.dp)) {
                    Text("Encryption is unavailable", fontWeight = FontWeight.SemiBold)
                    Text("This device's crypto provider failed OpenBooks' self-test, so password-protected books can't be opened or created here. ${app.cryptoError}")
                }
            }
        }
        Spacer(Modifier.height(8.dp))
        SectionTitle("Your books")
        if (files.isEmpty()) Muted("No books yet. Create a company to get started, or import a .obk file from your computer.")
        Card {
            files.forEach { f ->
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.weight(1f)) {
                        ListRow(
                            title = (if (Files.isEncrypted(f)) "🔒 " else "") + f.name.removeSuffix(Files.EXTENSION),
                            subtitle = "Modified " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(f.lastModified())),
                            onClick = { open(f) },
                        )
                    }
                    TextButton({ deleting = f }) { Text("Delete", color = Palette.negative) }
                }
            }
        }
        Button({ creating = true }, Modifier.fillMaxWidth()) { Text("New company") }
        OutlinedButton({ importer.launch(arrayOf("*/*")) }, Modifier.fillMaxWidth()) { Text("Import a books file (.obk)") }
        Muted("Books files are compatible with OpenBooks on Windows, macOS and Linux, including password-protected ones.")
    }

    unlocking?.let { file ->
        UnlockDialog(file, onDismiss = { unlocking = null })
    }
    if (creating) CreateBooksDialog(onDismiss = { creating = false })
    deleting?.let { f ->
        ConfirmDialog(
            title = "Delete ${f.name.removeSuffix(Files.EXTENSION)}?",
            text = "This permanently deletes these books from this device. Export a copy first if you might need them.",
            confirm = "Delete",
            destructive = true,
            onConfirm = { f.delete(); refresh++ },
            onDismiss = { deleting = null },
        )
    }
}

@Composable
private fun UnlockDialog(file: File, onDismiss: () -> Unit) {
    val app = LocalApp.current
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    FormDialog(
        title = "Unlock ${file.name.removeSuffix(Files.EXTENSION)}",
        confirm = if (busy) "Unlocking…" else "Unlock",
        confirmEnabled = password.isNotEmpty() && !busy,
        onDismiss = onDismiss,
        onConfirm = {
            busy = true
            app.run(
                onError = { e -> busy = false; error = if (e.code == "wrong_password") "That password is not correct." else (e.message ?: "") },
                onDone = { status: org.json.JSONObject ->
                    password = ""
                    app.applyStatus(status)
                    app.root(Screen.Dashboard)
                    onDismiss()
                },
            ) { Core.obj("open", "path" to file.absolutePath, "password" to password) }
        },
    ) {
        Text("These books are password protected.")
        PasswordInput("Password", password, { password = it; error = "" })
        ErrorText(error)
    }
}

@Composable
private fun CreateBooksDialog(onDismiss: () -> Unit) {
    val app = LocalApp.current
    var company by remember { mutableStateOf("") }
    var protect by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var repeat by remember { mutableStateOf("") }
    var starter by remember { mutableStateOf(true) }
    var error by remember { mutableStateOf("") }
    FormDialog(
        title = "New company",
        confirm = "Create",
        onDismiss = onDismiss,
        onConfirm = {
            error = when {
                company.isBlank() -> "Enter a company name."
                protect && password.length < 8 -> "Use a password of at least 8 characters."
                protect && password != repeat -> "The passwords don't match."
                else -> ""
            }
            if (error.isEmpty()) {
                val file = Files.uniqueFile(app.booksDir, Files.safeName(company), Files.EXTENSION)
                app.run(
                    success = "Created ${company.trim()}",
                    onError = { error = it.message ?: "" },
                    onDone = { status: org.json.JSONObject ->
                        app.applyStatus(status)
                        app.root(Screen.Dashboard)
                        onDismiss()
                    },
                ) {
                    Core.obj(
                        "create", "path" to file.absolutePath, "company" to company.trim(),
                        "password" to (if (protect) password else null), "starterChart" to starter,
                    )
                }
            }
        },
    ) {
        TextInput("Company name", company, { company = it })
        CheckRow("Start with a standard chart of accounts", starter) { starter = it }
        if (app.cryptoOk != false) {
            CheckRow("Protect with a password", protect) { protect = it }
            if (protect) {
                PasswordInput("Password (8+ characters)", password, { password = it })
                PasswordInput("Repeat password", repeat, { repeat = it })
                Muted("AES-256 encryption. If you forget the password, nobody can recover these books.")
            }
        }
        ErrorText(error)
    }
}
