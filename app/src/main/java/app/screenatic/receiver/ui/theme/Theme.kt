package app.screenatic.receiver.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.Color
import androidx.tv.material3.LocalContentColor
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.darkColorScheme

private val Colors = darkColorScheme(
    primary = Color(0xFF4FC3F7),
    onPrimary = Color(0xFF00212E),
    background = Color(0xFF0F1A2E),
    onBackground = Color(0xFFE6ECF5),
    surface = Color(0xFF16233D),
    onSurface = Color(0xFFE6ECF5),
)

@Composable
fun ScreenaticTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors) {
        // tv-material Text defaults to LocalContentColor (black) outside a Surface.
        CompositionLocalProvider(LocalContentColor provides Colors.onBackground, content = content)
    }
}
