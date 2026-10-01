package org.openbooks.android

import android.os.Bundle
import android.os.SystemClock
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    private var app: AppState? = null
    private var stoppedAt = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val scope = rememberCoroutineScope()
            val state = remember { AppState(applicationContext, scope).also { app = it } }
            LaunchedEffect(Unit) {
                // Known-answer tests before any password is used on this device's crypto provider.
                val result = withContext(Dispatchers.IO) { runCatching { Core.obj("selfTest") } }
                state.cryptoOk = result.isSuccess
                state.cryptoError = result.exceptionOrNull()?.message ?: ""
                // A killed process can leave exported PDFs/CSVs behind; nothing is open yet, so clear them.
                val open = withContext(Dispatchers.IO) { runCatching { Core.obj("status").b("open") }.getOrDefault(false) }
                if (!open) withContext(Dispatchers.IO) { state.clearExports() }
                state.refreshStatus()
            }
            // Keep password-protected books out of screenshots and the recent-apps thumbnail.
            LaunchedEffect(state.encrypted) {
                if (state.encrypted) window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
            }
            CompositionLocalProvider(LocalApp provides state) {
                OpenBooksTheme { AppRoot() }
            }
        }
    }

    override fun onStop() {
        super.onStop()
        stoppedAt = SystemClock.elapsedRealtime()
    }

    override fun onStart() {
        super.onStart()
        // Password-protected books lock again after a while in the background.
        val state = app ?: return
        if (stoppedAt != 0L && state.encrypted && SystemClock.elapsedRealtime() - stoppedAt > LOCK_AFTER_MS) {
            state.closeBooks()
            state.message("Books locked after 5 minutes in the background")
        }
    }

    private companion object {
        const val LOCK_AFTER_MS = 5 * 60 * 1000L
    }
}
