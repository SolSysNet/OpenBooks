package org.openbooks.android

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.ui.graphics.Color

private val Green = Color(0xFF2CA01C)
private val GreenDark = Color(0xFF4CAF50)

private val Light = lightColorScheme(
    primary = Green,
    onPrimary = Color.White,
    secondary = Color(0xFF3D6B45),
    primaryContainer = Color(0xFFD5EFD0),
    onPrimaryContainer = Color(0xFF0B3D06),
    secondaryContainer = Color(0xFFDDEBDD),
    onSecondaryContainer = Color(0xFF14311A),
    background = Color(0xFFF7F8FA),
    surface = Color(0xFFF7F8FA),
    surfaceVariant = Color(0xFFE6E9EC),
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color.White,
    surfaceContainer = Color(0xFFF1F3F5),
    surfaceContainerHigh = Color(0xFFECEEF1),
    surfaceContainerHighest = Color.White,
    error = Color(0xFFC82D28),
)

private val Dark = darkColorScheme(
    primary = GreenDark,
    onPrimary = Color.Black,
    secondary = Color(0xFF8BC48F),
    primaryContainer = Color(0xFF1F4D1C),
    onPrimaryContainer = Color(0xFFCDEBC7),
    secondaryContainer = Color(0xFF2A3B2C),
    onSecondaryContainer = Color(0xFFD5E8D6),
    background = Color(0xFF1E2024),
    surface = Color(0xFF1E2024),
    surfaceVariant = Color(0xFF33373D),
    surfaceContainerLowest = Color(0xFF191B1F),
    surfaceContainerLow = Color(0xFF232529),
    surfaceContainer = Color(0xFF26292E),
    surfaceContainerHigh = Color(0xFF2C2F34),
    surfaceContainerHighest = Color(0xFF2F3238),
    error = Color(0xFFF06E64),
)

@Composable
fun OpenBooksTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) Dark else Light, content = content)
}

/** Semantic colors matching the desktop app. */
object Palette {
    val positive: Color @Composable @ReadOnlyComposable get() = if (isSystemInDarkTheme()) Color(0xFF6EC878) else Color(0xFF1E8228)
    val negative: Color @Composable @ReadOnlyComposable get() = MaterialTheme.colorScheme.error
    val warning: Color @Composable @ReadOnlyComposable get() = if (isSystemInDarkTheme()) Color(0xFFEBB450) else Color(0xFFBE7800)
    val muted: Color @Composable @ReadOnlyComposable get() = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f)

    @Composable
    @ReadOnlyComposable
    fun status(s: String): Color = when (s) {
        "Paid", "Applied", "Converted", "Accepted" -> positive
        "Overdue", "Declined", "Expired" -> negative
        "Partial", "Partly applied", "Due" -> warning
        "Void", "Paused", "Finished" -> muted
        else -> MaterialTheme.colorScheme.primary
    }
}
