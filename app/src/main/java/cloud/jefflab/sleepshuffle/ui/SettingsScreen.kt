package cloud.jefflab.sleepshuffle.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import cloud.jefflab.sleepshuffle.Playback
import cloud.jefflab.sleepshuffle.Show
import kotlinx.coroutines.launch

@Composable
fun SettingsScreen(onBack: () -> Unit, onSignedOut: () -> Unit) {
    val prefs = Playback.prefs
    val client = Playback.client
    val scope = rememberCoroutineScope()

    var lan by remember { mutableStateOf(prefs.lanUrl) }
    var tail by remember { mutableStateOf(prefs.tailscaleUrl) }
    var serverStatus by remember { mutableStateOf<String?>(null) }

    var selected by remember { mutableStateOf(prefs.shows) }
    var library by remember { mutableStateOf<List<Show>?>(null) }
    var libraryError by remember { mutableStateOf<String?>(null) }

    var window by remember { mutableIntStateOf(prefs.recentWindow) }
    var banned by remember { mutableStateOf(prefs.banned) }

    BackHandler { onBack() }

    LaunchedEffect(Unit) {
        try {
            library = client.series()
        } catch (e: Exception) {
            libraryError = e.message ?: "Couldn't load your library."
        }
    }

    fun toggle(show: Show) {
        val isOn = selected.any { it.id == show.id }
        val next = if (isOn) selected.filterNot { it.id == show.id } else selected + show
        // Keep the library's alphabetical order.
        val order = library?.map { it.id }.orEmpty()
        selected = next.sortedBy { s -> order.indexOf(s.id).let { if (it < 0) Int.MAX_VALUE else it } }
        prefs.shows = selected
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Pal.Bg)
            .systemBarsPadding()
            .imePadding(),
        contentAlignment = Alignment.TopCenter,
    ) {
        Column(
            Modifier
                .widthIn(max = 600.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Settings", color = Pal.Amber, fontSize = 24.sp, modifier = Modifier.weight(1f))
                TextLink("Done", onClick = onBack)
            }

            // ---- Shows
            Spacer(Modifier.height(16.dp))
            Label("Shows to shuffle")
            when {
                libraryError != null -> Text(libraryError!!, color = Pal.AmberDim, fontSize = 16.sp)
                library == null -> Text("Loading your TV library…", color = Pal.Faint, fontSize = 16.sp)
                else -> {
                    // Selected shows first, then the rest of the library.
                    val lib = library!!
                    val ordered = lib.filter { s -> selected.any { it.id == s.id } } +
                        lib.filterNot { s -> selected.any { it.id == s.id } }
                    ordered.forEach { show ->
                        val on = selected.any { it.id == show.id }
                        ShowRow(show.name, on) { toggle(show) }
                    }
                }
            }

            // ---- Repeats
            Spacer(Modifier.height(24.dp))
            Label("Don't repeat the last…")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                BigButton("−10", Modifier.weight(1f), minHeight = 60.dp) {
                    window = (window - 10).coerceAtLeast(0)
                    prefs.recentWindow = window
                }
                Text(
                    "$window episodes",
                    color = Pal.Amber,
                    fontSize = 20.sp,
                    modifier = Modifier.weight(2f).padding(horizontal = 8.dp),
                )
                BigButton("+10", Modifier.weight(1f), minHeight = 60.dp) {
                    window = (window + 10).coerceAtMost(300)
                    prefs.recentWindow = window
                }
            }
            Text(
                "Counted per show. If a show has fewer episodes than this, it just avoids repeats as long as it can.",
                color = Pal.Faint,
                fontSize = 14.sp,
                modifier = Modifier.padding(top = 6.dp),
            )

            // ---- Banned
            Spacer(Modifier.height(24.dp))
            Label("Never shuffled")
            if (banned.isEmpty()) {
                Text("Nothing banned. Hold the button in an episode's Info to add one.", color = Pal.Faint, fontSize = 15.sp)
            } else {
                banned.forEach { b ->
                    Row(
                        Modifier.fillMaxWidth().padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            b.label,
                            color = Pal.AmberDim,
                            fontSize = 16.sp,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f),
                        )
                        Spacer(Modifier.width(8.dp))
                        BigButton("Unban", Modifier.width(110.dp), minHeight = 52.dp, fontSize = 16.sp) {
                            prefs.unban(b.id)
                            banned = prefs.banned
                        }
                    }
                }
            }

            // ---- Server
            Spacer(Modifier.height(24.dp))
            Label("Server")
            Field("Home address", lan, { lan = it }, "http://192.168.1.20:8096", keyboard = KeyboardType.Uri)
            Spacer(Modifier.height(10.dp))
            Field("Tailscale address", tail, { tail = it }, "http://100.x.y.z:8096", keyboard = KeyboardType.Uri)
            Spacer(Modifier.height(10.dp))
            BigButton("Save and test", Modifier.fillMaxWidth(), minHeight = 60.dp) {
                prefs.lanUrl = client.normalize(lan)
                prefs.tailscaleUrl = client.normalize(tail)
                serverStatus = "Testing…"
                scope.launch {
                    val home = client.ping(prefs.lanUrl)
                    val ts = client.ping(prefs.tailscaleUrl)
                    serverStatus = "Home: ${if (home) "reachable" else "no answer"} · Tailscale: ${if (ts) "reachable" else "no answer"}"
                    runCatching { client.resolveBase(force = true) }
                }
            }
            serverStatus?.let {
                Text(it, color = Pal.AmberDim, fontSize = 15.sp, modifier = Modifier.padding(top = 8.dp))
            }
            Text(
                "Signed in as ${prefs.userName.ifEmpty { "?" }}",
                color = Pal.Faint,
                fontSize = 15.sp,
                modifier = Modifier.padding(top = 16.dp),
            )
            Spacer(Modifier.height(8.dp))
            HoldButton("Hold to sign out", Modifier.fillMaxWidth()) {
                Playback.stop()
                prefs.signOut()
                onSignedOut()
            }
            Spacer(Modifier.height(24.dp))
            Text(
                "Sleep Shuffle never reports playback to Jellyfin. Recent plays and bans are kept on this device only.",
                color = Pal.Faint,
                fontSize = 14.sp,
            )
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun ShowRow(name: String, on: Boolean, onClick: () -> Unit) {
    val shape = RoundedCornerShape(16.dp)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
            .heightIn(min = 60.dp)
            .clip(shape)
            .background(if (on) Pal.FillStrong else Pal.Fill)
            .border(if (on) 2.dp else 1.dp, if (on) Pal.LineStrong else Pal.Line, shape)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            name,
            color = if (on) Pal.Amber else Pal.AmberDim,
            fontSize = 18.sp,
            modifier = Modifier.weight(1f),
        )
        Text(if (on) "On" else "Off", color = if (on) Pal.Amber else Pal.Faint, fontSize = 16.sp)
    }
}

