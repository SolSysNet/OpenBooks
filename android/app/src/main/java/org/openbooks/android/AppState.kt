package org.openbooks.android

import android.content.Context
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** Every place the app can show. The drawer replaces the stack; everything else pushes onto it. */
sealed interface Screen {
    data object Dashboard : Screen
    data object Reports : Screen
    data class Contacts(val kind: String) : Screen                 // "Customer" / "Vendor"
    data class ContactDetail(val id: Int) : Screen
    data class Documents(val kind: String) : Screen                // DocKind storage token
    data class DocumentDetail(val id: Int) : Screen
    data class DocumentEditor(val kind: String, val contactId: Int = 0, val recurringId: Int = -1) : Screen
    data object Recurring : Screen
    data class PaymentForm(val kind: String, val contactId: Int = 0, val documentId: Int = 0) : Screen  // "Received"/"Paid"
    data object Payments : Screen
    data class Register(val accountId: Int = 0) : Screen
    data class Reconcile(val accountId: Int = 0) : Screen
    data object Journal : Screen
    data class ImportCsv(val accountId: Int = 0) : Screen
    data object Accounts : Screen
    data object Items : Screen
    data object Company : Screen
}

/** App-wide state: which books are open, navigation, and the change counter screens reload on. */
class AppState(val context: Context, val scope: CoroutineScope) {
    val snackbar = SnackbarHostState()

    var bookOpen by mutableStateOf(false)
        private set
    var companyName by mutableStateOf("")
        private set
    var encrypted by mutableStateOf(false)
        private set
    var bookPath by mutableStateOf("")
        private set
    var cryptoOk by mutableStateOf<Boolean?>(null)
    var cryptoError by mutableStateOf("")

    /** Bumped after every successful change; screens reload whatever they show. */
    var version by mutableIntStateOf(0)
        private set

    var stack by mutableStateOf(listOf<Screen>(Screen.Dashboard))
        private set
    val current: Screen get() = stack.last()

    val booksDir: File get() = File(context.filesDir, "books").apply { mkdirs() }
    val exportDir: File get() = File(context.cacheDir, "exports").apply { mkdirs() }

    fun navigate(screen: Screen) {
        stack = stack + screen
    }

    fun root(screen: Screen) {
        stack = listOf(screen)
    }

    fun back(): Boolean {
        if (stack.size <= 1) return false
        stack = stack.dropLast(1)
        return true
    }

    /** Replaces the current screen, e.g. an editor with the document it just created. */
    fun replace(screen: Screen) {
        stack = stack.dropLast(1) + screen
    }

    fun refreshStatus() {
        scope.launch {
            val status = withContext(Dispatchers.IO) { runCatching { Core.obj("status") }.getOrNull() }
            if (status != null) applyStatus(status)
        }
    }

    fun applyStatus(status: org.json.JSONObject) {
        bookOpen = status.b("open")
        companyName = status.s("company")
        encrypted = status.b("encrypted")
        bookPath = status.s("path")
        version++
    }

    fun message(text: String) {
        scope.launch { snackbar.showSnackbar(text) }
    }

    /**
     * Runs an engine call off the main thread. On success the app reloads (and shows [success]);
     * on failure the error message is shown and [onError] gets it.
     */
    fun <T> run(
        success: String? = null,
        onError: (CoreException) -> Unit = {},
        onDone: (T) -> Unit = {},
        block: () -> T,
    ) {
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                try {
                    Result.success(block())
                } catch (e: CoreException) {
                    Result.failure(e)
                } catch (e: Exception) {
                    Result.failure(CoreException("error", e.message ?: e.toString()))
                }
            }
            result.onSuccess {
                version++
                if (success != null) message(success)
                onDone(it)
            }.onFailure {
                val e = it as CoreException
                message(e.message ?: "Something went wrong")
                onError(e)
            }
        }
    }

    /** Removes exported PDFs/CSVs: they are unencrypted copies. */
    fun clearExports() {
        exportDir.listFiles()?.forEach { it.delete() }
    }

    fun closeBooks() {
        run(onDone = { status: org.json.JSONObject ->
            applyStatus(status)
            clearExports()
            root(Screen.Dashboard)
        }) { Core.obj("close") }
    }
}
