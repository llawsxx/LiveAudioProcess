package com.llawsxx.audioprocess.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val customColorScheme = darkColorScheme(
    primary = SignalTeal,
    secondary = SignalAmber,
    tertiary = SignalTeal,
    background = Charcoal,
    surface = SurfaceCharcoal,
    onPrimary = Charcoal,
    onBackground = Color.White,
    onSurface = Color.White
)

@Composable
fun LiveAudioProcessTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    // Dynamic color is available on Android 12+
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = customColorScheme,
        typography = Typography,
        content = content
    )
}
