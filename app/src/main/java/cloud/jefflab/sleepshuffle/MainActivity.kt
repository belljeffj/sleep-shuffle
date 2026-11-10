package cloud.jefflab.sleepshuffle

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import cloud.jefflab.sleepshuffle.ui.HomeScreen
import cloud.jefflab.sleepshuffle.ui.PlayerScreen
import cloud.jefflab.sleepshuffle.ui.SettingsScreen
import cloud.jefflab.sleepshuffle.ui.SetupScreen
import cloud.jefflab.sleepshuffle.ui.SleepTheme

private enum class Screen { Setup, Home, Player, Settings }

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Playback.init(applicationContext)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.BLACK),
            navigationBarStyle = SystemBarStyle.dark(Color.BLACK),
        )
        setContent {
            SleepTheme { App(this) }
        }
    }
}

@Composable
private fun App(activity: ComponentActivity) {
    val prefs = Playback.prefs
    var screen by remember {
        val phase = Playback.state.value.phase
        mutableStateOf(
            when {
                !prefs.signedIn -> Screen.Setup
                phase == Phase.Playing || phase == Phase.Connecting -> Screen.Player
                else -> Screen.Home
            },
        )
    }

    when (screen) {
        Screen.Setup -> SetupScreen(onDone = { screen = Screen.Home })
        Screen.Home -> HomeScreen(
            onPlay = { key, timer ->
                Playback.start(key, timer)
                screen = Screen.Player
            },
            onSettings = { screen = Screen.Settings },
        )
        Screen.Player -> PlayerScreen(
            activity = activity,
            onExit = {
                Playback.stop()
                screen = Screen.Home
            },
        )
        Screen.Settings -> SettingsScreen(
            onBack = { screen = Screen.Home },
            onSignedOut = { screen = Screen.Setup },
        )
    }
}
