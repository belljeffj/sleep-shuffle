package cloud.jefflab.sleepshuffle.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextFieldColors
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import cloud.jefflab.sleepshuffle.Playback
import kotlinx.coroutines.launch

/** Shows matched by name on first sign-in. */
private val DEFAULT_SHOWS = listOf("how i met your mother", "new girl")

@Composable
fun fieldColors(): TextFieldColors = OutlinedTextFieldDefaults.colors(
    focusedTextColor = Pal.Amber,
    unfocusedTextColor = Pal.AmberDim,
    focusedBorderColor = Pal.LineStrong,
    unfocusedBorderColor = Pal.Line,
    cursorColor = Pal.Amber,
    focusedLabelColor = Pal.AmberDim,
    unfocusedLabelColor = Pal.Faint,
    focusedPlaceholderColor = Pal.Faint,
    unfocusedPlaceholderColor = Pal.Faint,
    focusedContainerColor = Pal.Bg,
    unfocusedContainerColor = Pal.Bg,
)

@Composable
fun Field(
    label: String,
    value: String,
    onChange: (String) -> Unit,
    placeholder: String = "",
    password: Boolean = false,
    keyboard: KeyboardType = KeyboardType.Text,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        placeholder = { Text(placeholder) },
        singleLine = true,
        visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = if (password) KeyboardType.Password else keyboard),
        colors = fieldColors(),
        textStyle = androidx.compose.ui.text.TextStyle(fontSize = 18.sp),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
fun SetupScreen(onDone: () -> Unit) {
    val prefs = Playback.prefs
    var lan by remember { mutableStateOf(prefs.lanUrl) }
    var tail by remember { mutableStateOf(prefs.tailscaleUrl) }
    var user by remember { mutableStateOf(prefs.userName) }
    var pw by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()

    Box(
        Modifier
            .fillMaxSize()
            .background(Pal.Bg)
            .systemBarsPadding()
            .imePadding(),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier
                .widthIn(max = 520.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
        ) {
            Text("Sleep Shuffle", color = Pal.Amber, fontSize = 28.sp)
            Spacer(Modifier.height(6.dp))
            Text(
                "Connect to your Jellyfin server. The home address is tried first, then Tailscale.",
                color = Pal.Faint,
                fontSize = 16.sp,
            )
            Spacer(Modifier.height(20.dp))
            Field("Home address", lan, { lan = it }, "http://192.168.1.20:8096", keyboard = KeyboardType.Uri)
            Spacer(Modifier.height(10.dp))
            Field("Tailscale address", tail, { tail = it }, "http://100.x.y.z:8096", keyboard = KeyboardType.Uri)
            Spacer(Modifier.height(10.dp))
            Field("Jellyfin username", user, { user = it })
            Spacer(Modifier.height(10.dp))
            Field("Password", pw, { pw = it }, password = true)
            Spacer(Modifier.height(20.dp))
            BigButton(if (busy) "Signing in…" else "Sign in", Modifier.fillMaxWidth(), emphasis = true) {
                if (busy) return@BigButton
                if (lan.isBlank() && tail.isBlank()) {
                    status = "Add at least one address."
                    return@BigButton
                }
                busy = true
                status = null
                scope.launch {
                    try {
                        Playback.client.signIn(lan, tail, user.trim(), pw)
                        // First run: pre-pick How I Met Your Mother and New Girl if the library has them.
                        if (prefs.shows.isEmpty()) {
                            val library = Playback.client.series()
                            prefs.shows = DEFAULT_SHOWS.mapNotNull { wanted ->
                                library.firstOrNull { it.name.lowercase().contains(wanted) }
                            }
                        }
                        onDone()
                    } catch (e: Exception) {
                        status = e.message ?: "Sign-in failed."
                    } finally {
                        busy = false
                    }
                }
            }
            if (status != null) {
                Spacer(Modifier.height(12.dp))
                Text(status!!, color = Pal.AmberDim, fontSize = 16.sp)
            }
            Spacer(Modifier.height(16.dp))
            Text(
                "Your password isn't stored. The app keeps only a Jellyfin access token, and never reports what you watch back to the server.",
                color = Pal.Faint,
                fontSize = 14.sp,
            )
        }
    }
}
