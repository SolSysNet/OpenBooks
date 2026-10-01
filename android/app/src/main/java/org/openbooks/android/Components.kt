package org.openbooks.android

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneOffset

val LocalApp = staticCompositionLocalOf<AppState> { error("no AppState") }

fun today(): String = LocalDate.now().toString()

// ---------------------------------------------------------------- loading

class Loaded<T>(val value: T?, val error: String?)

/** Loads engine data off the main thread; reloads when [keys] change or the books change. */
@Composable
fun <T> load(vararg keys: Any?, block: () -> T): Loaded<T> {
    val app = LocalApp.current
    val state = produceState(Loaded<T>(null, null), app.version, *keys) {
        value = withContext(Dispatchers.IO) {
            try {
                Loaded(block(), null)
            } catch (e: Exception) {
                Loaded(null, e.message ?: e.toString())
            }
        }
    }
    return state.value
}

/** Shows a spinner, the error, or [content] with the loaded value. */
@Composable
fun <T> Loading(loaded: Loaded<T>, content: @Composable (T) -> Unit) {
    val v = loaded.value
    when {
        v != null -> content(v)
        loaded.error != null -> Text(loaded.error, color = Palette.negative, modifier = Modifier.padding(16.dp))
        else -> Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
    }
}

// ---------------------------------------------------------------- layout

@Composable
fun ScrollPage(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(text, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold, modifier = modifier.padding(top = 4.dp))
}

@Composable
fun Muted(text: String, modifier: Modifier = Modifier) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = Palette.muted, modifier = modifier)
}

@Composable
fun EmptyState(text: String) {
    Text(text, color = Palette.muted, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth().padding(32.dp))
}

@Composable
fun ErrorText(text: String) {
    if (text.isNotEmpty()) Text(text, color = Palette.negative, style = MaterialTheme.typography.bodyMedium)
}

/** A tappable list row: title and subtitle on the left, an amount (and status) on the right. */
@Composable
fun ListRow(
    title: String,
    subtitle: String = "",
    amount: String? = null,
    status: String? = null,
    dim: Boolean = false,
    onClick: (() -> Unit)? = null,
) {
    Row(
        Modifier.fillMaxWidth()
            .let { if (onClick != null) it.clickable(onClick = onClick) else it }
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis, color = if (dim) Palette.muted else Color.Unspecified)
            if (subtitle.isNotEmpty()) Muted(subtitle)
        }
        Column(horizontalAlignment = Alignment.End) {
            if (amount != null) MoneyText(amount, dim = dim)
            if (!status.isNullOrEmpty()) StatusChip(status)
        }
    }
    HorizontalDivider()
}

// ---------------------------------------------------------------- money and status

@Composable
fun MoneyText(
    amount: String,
    modifier: Modifier = Modifier,
    colorize: Boolean = false,
    dim: Boolean = false,
    bold: Boolean = false,
    big: Boolean = false,
) {
    val color = when {
        dim -> Palette.muted
        colorize && MoneyFmt.isNegative(amount) -> Palette.negative
        colorize && !MoneyFmt.isZero(amount) -> Palette.positive
        else -> Color.Unspecified
    }
    Text(
        MoneyFmt.format(amount),
        modifier = modifier,
        color = color,
        fontWeight = if (bold) FontWeight.SemiBold else null,
        style = if (big) MaterialTheme.typography.headlineSmall else MaterialTheme.typography.bodyLarge,
        textAlign = TextAlign.End,
    )
}

@Composable
fun StatusChip(status: String) {
    val color = Palette.status(status)
    Surface(color = color.copy(alpha = 0.15f), shape = RoundedCornerShape(50)) {
        Text(status, color = color, style = MaterialTheme.typography.labelSmall, modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp))
    }
}

@Composable
fun SummaryCard(label: String, amount: String, modifier: Modifier = Modifier, detail: String = "", onClick: (() -> Unit)? = null) {
    Card(modifier.let { if (onClick != null) it.clickable(onClick = onClick) else it }) {
        Column(Modifier.padding(12.dp)) {
            Muted(label)
            MoneyText(amount, big = true, colorize = false)
            if (detail.isNotEmpty()) Muted(detail)
        }
    }
}

// ---------------------------------------------------------------- fields

@Composable
fun TextInput(
    label: String,
    value: String,
    onChange: (String) -> Unit,
    modifier: Modifier = Modifier.fillMaxWidth(),
    singleLine: Boolean = true,
    keyboard: KeyboardType = KeyboardType.Text,
    placeholder: String = "",
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        modifier = modifier,
        singleLine = singleLine,
        minLines = if (singleLine) 1 else 3,
        placeholder = if (placeholder.isEmpty()) null else ({ Text(placeholder) }),
        keyboardOptions = KeyboardOptions(keyboardType = keyboard),
    )
}

@Composable
fun MoneyInput(label: String, value: String, onChange: (String) -> Unit, modifier: Modifier = Modifier.fillMaxWidth()) {
    val invalid = value.isNotBlank() && MoneyFmt.parse(value) == null
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        modifier = modifier,
        singleLine = true,
        isError = invalid,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
    )
}

/** Password field. Passwords are never remembered across process death (no rememberSaveable). */
@Composable
fun PasswordInput(label: String, value: String, onChange: (String) -> Unit, modifier: Modifier = Modifier.fillMaxWidth()) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        modifier = modifier,
        singleLine = true,
        visualTransformation = PasswordVisualTransformation(),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false),
    )
}

/** Looks like a text field; opens something when tapped. */
@Composable
fun ClickField(label: String, text: String, modifier: Modifier = Modifier.fillMaxWidth(), onClick: () -> Unit) {
    Box(modifier) {
        OutlinedTextField(
            value = text,
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            trailingIcon = { Text("▾") },
        )
        Box(Modifier.matchParentSize().semantics { contentDescription = "$label: $text" }.clickable(onClick = onClick))
    }
}

private fun toMillis(date: String): Long? =
    runCatching { LocalDate.parse(date).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli() }.getOrNull()

private fun fromMillis(millis: Long): String = Instant.ofEpochMilli(millis).atOffset(ZoneOffset.UTC).toLocalDate().toString()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DateField(
    label: String,
    value: String,
    onChange: (String) -> Unit,
    modifier: Modifier = Modifier.fillMaxWidth(),
    allowEmpty: Boolean = false,
) {
    var open by remember { mutableStateOf(false) }
    ClickField(label, value.ifEmpty { "—" }, modifier) { open = true }
    if (open) {
        val state = rememberDatePickerState(initialSelectedDateMillis = toMillis(value) ?: toMillis(today()))
        DatePickerDialog(
            onDismissRequest = { open = false },
            confirmButton = {
                TextButton({
                    state.selectedDateMillis?.let { onChange(fromMillis(it)) }
                    open = false
                }) { Text("OK") }
            },
            dismissButton = {
                Row {
                    if (allowEmpty) TextButton({ onChange(""); open = false }) { Text("Clear") }
                    TextButton({ open = false }) { Text("Cancel") }
                }
            },
        ) { DatePicker(state) }
    }
}

data class Option(val id: Int, val label: String, val detail: String = "")

/** A searchable picker for accounts, customers, vendors and items. */
@Composable
fun PickerField(
    label: String,
    options: List<Option>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier.fillMaxWidth(),
    noneLabel: String? = null,
) {
    var open by remember { mutableStateOf(false) }
    val current = options.firstOrNull { it.id == selected }?.label ?: noneLabel ?: "Choose…"
    ClickField(label, current, modifier) { open = true }
    if (open) {
        var query by remember { mutableStateOf("") }
        val shown = options.filter { query.isBlank() || it.label.contains(query.trim(), ignoreCase = true) }
        AlertDialog(
            onDismissRequest = { open = false },
            title = { Text(label) },
            text = {
                Column {
                    if (options.size > 8) TextInput("Search", query, { query = it })
                    LazyColumn(Modifier.heightIn(max = 420.dp)) {
                        if (noneLabel != null) item {
                            Text(noneLabel, color = Palette.muted, modifier = Modifier.fillMaxWidth()
                                .clickable { onSelect(0); open = false }.padding(vertical = 12.dp))
                        }
                        items(shown, key = { it.id }) { o ->
                            Column(Modifier.fillMaxWidth().clickable { onSelect(o.id); open = false }.padding(vertical = 10.dp)) {
                                Text(o.label, fontWeight = if (o.id == selected) FontWeight.Bold else null)
                                if (o.detail.isNotEmpty()) Muted(o.detail)
                            }
                        }
                        if (shown.isEmpty()) item { Muted("Nothing matches.", Modifier.padding(vertical = 12.dp)) }
                    }
                }
            },
            confirmButton = { TextButton({ open = false }) { Text("Close") } },
        )
    }
}

/** A fixed set of text choices, e.g. account type or frequency. */
@Composable
fun ChoiceField(label: String, choices: List<String>, selected: String, onSelect: (String) -> Unit, modifier: Modifier = Modifier.fillMaxWidth()) {
    val options = choices.mapIndexed { i, c -> Option(i + 1, c) }
    PickerField(label, options, options.firstOrNull { it.label == selected }?.id ?: 0, { id -> onSelect(choices[id - 1]) }, modifier)
}

@Composable
fun CheckRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onChange(!checked) }, verticalAlignment = Alignment.CenterVertically) {
        Checkbox(checked, onChange)
        Text(label)
    }
}

// ---------------------------------------------------------------- option builders

fun accountOptions(accounts: List<JSONObject>, filter: (JSONObject) -> Boolean = { true }): List<Option> =
    accounts.filter { it.b("active") && filter(it) }.map { Option(it.i("id"), it.s("label"), it.s("type")) }

/** Accounts money moves through: bank, cash, credit cards, loans. */
fun moneyAccounts(accounts: List<JSONObject>): List<Option> = accountOptions(accounts) { it.b("money") }

/** Accounts a transaction can be categorized to (anything but A/R and A/P, which need documents). */
fun postingAccounts(accounts: List<JSONObject>): List<Option> = accountOptions(accounts) { !it.b("subledger") }

fun contactOptions(contacts: List<JSONObject>): List<Option> =
    contacts.map { Option(it.i("id"), it.s("name"), it.s("email")) }

// ---------------------------------------------------------------- dialogs

@Composable
fun ConfirmDialog(
    title: String,
    text: String,
    confirm: String,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
    destructive: Boolean = false,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = {
            Button(
                onClick = { onConfirm(); onDismiss() },
                colors = if (destructive) ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error) else ButtonDefaults.buttonColors(),
            ) { Text(confirm) }
        },
        dismissButton = { TextButton(onDismiss) { Text("Cancel") } },
    )
}

/** A dialog holding a scrollable form. */
@Composable
fun FormDialog(
    title: String,
    confirm: String,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
    confirmEnabled: Boolean = true,
    content: @Composable ColumnScope.() -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
        },
        confirmButton = { Button(onConfirm, enabled = confirmEnabled) { Text(confirm) } },
        dismissButton = { TextButton(onDismiss) { Text("Cancel") } },
    )
}

// ---------------------------------------------------------------- report tables

/** Renders a report table from the engine (title, headers, rows with style/indent/cells). */
@Composable
fun TableView(table: JSONObject, modifier: Modifier = Modifier, firstColumn: Dp = 200.dp, column: Dp = 110.dp) {
    val headers = table.optJSONArray("headers")?.strings() ?: emptyList()
    val numeric = table.optJSONArray("numeric")?.let { a -> List(a.length()) { a.optBoolean(it) } } ?: emptyList()
    val rows = table.list("rows")
    Column(modifier) {
        Text(table.s("title"), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        table.optJSONArray("subtitles")?.strings()?.forEach { Muted(it) }
        Spacer(Modifier.size(8.dp))
        Column(Modifier.horizontalScroll(rememberScrollState())) {
            Row {
                headers.forEachIndexed { i, h ->
                    Text(
                        h, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelLarge,
                        textAlign = if (numeric.getOrElse(i) { false }) TextAlign.End else TextAlign.Start,
                        modifier = Modifier.width(if (i == 0) firstColumn else column).padding(horizontal = 4.dp, vertical = 6.dp),
                    )
                }
            }
            HorizontalDivider()
            rows.forEach { row ->
                val style = row.s("style")
                if (style == "spacer") {
                    Spacer(Modifier.size(10.dp))
                    return@forEach
                }
                val cells = row.optJSONArray("cells")?.strings() ?: emptyList()
                val weight = if (style == "section" || style == "total") FontWeight.SemiBold else null
                if (style == "total") HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                Row {
                    cells.forEachIndexed { i, c ->
                        val isNumber = numeric.getOrElse(i) { false }
                        Text(
                            c, fontWeight = weight, maxLines = 2, overflow = TextOverflow.Ellipsis,
                            fontFamily = if (isNumber) FontFamily.Monospace else null,
                            style = MaterialTheme.typography.bodyMedium,
                            textAlign = if (isNumber) TextAlign.End else TextAlign.Start,
                            modifier = Modifier.width(if (i == 0) firstColumn else column)
                                .padding(start = if (i == 0) (4 + 12 * row.i("indent")).dp else 4.dp, end = 4.dp, top = 3.dp, bottom = 3.dp),
                        )
                    }
                }
            }
        }
    }
}

val ContentPadding = PaddingValues(bottom = 88.dp)  // room for a floating action button

@Composable
fun rememberText(initial: String = ""): androidx.compose.runtime.MutableState<String> = rememberSaveable { mutableStateOf(initial) }
