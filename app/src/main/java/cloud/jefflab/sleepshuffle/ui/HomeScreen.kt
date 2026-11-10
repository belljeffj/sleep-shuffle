package cloud.jefflab.sleepshuffle.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import cloud.jefflab.sleepshuffle.Playback
import cloud.jefflab.sleepshuffle.TimerSpec
import cloud.jefflab.sleepshuffle.mixLabel

/**
 * Bedtime home screen. Everything you'd tap sits in the bottom half,
 * with "Same as last night" at the very bottom, under your thumb.
 */
@Composable
fun HomeScreen(onPlay: (String, TimerSpec) -> Unit, onSettings: () -> Unit) {
    val prefs = Playback.prefs
    val shows = remember { prefs.shows }
    var timer by remember { mutableStateOf(TimerSpec.decode(prefs.lastTimer)) }
    var timerSheet by remember { mutableStateOf(false) }
    val lastKey = remember { prefs.lastSelection }
    val lastTimer = remember { TimerSpec.decode(prefs.lastTimer) }
    val lastValid = lastKey != null && (lastKey == Playback.ALL || shows.any { it.id == lastKey })

    BoxWithConstraints(
        Modifier
            .fillMaxSize()
            .background(Pal.Bg)
            .systemBarsPadding(),
    ) {
        val viewport = maxHeight
        Column(
            Modifier
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .heightIn(min = viewport),
            verticalArrangement = Arrangement.SpaceBetween,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(
                Modifier
                    .widthIn(max = 560.dp)
                    .fillMaxWidth()
                    .padding(start = 20.dp, end = 8.dp, top = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Sleep Shuffle", color = Pal.Faint, fontSize = 16.sp, modifier = Modifier.weight(1f))
                TextLink("Settings", onClick = onSettings)
            }

            Column(
                Modifier
                    .widthIn(max = 560.dp)
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp, vertical = 16.dp),
            ) {
                if (shows.isEmpty()) {
                    Text(
                        "No shows picked yet. Open Settings and choose some.",
                        color = Pal.AmberDim,
                        fontSize = 19.sp,
                    )
                    return@Column
                }

                Label("Sleep timer · ${timer.short()}")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    PRESET_MINUTES.forEach { m ->
                        BigButton(
                            "$m",
                            Modifier.weight(1f),
                            minHeight = 60.dp,
                            fontSize = 19.sp,
                            selected = timer == TimerSpec.Minutes(m),
                        ) { timer = TimerSpec.Minutes(m) }
                    }
                    val custom = timer.let { it !is TimerSpec.Minutes || it.minutes !in PRESET_MINUTES }
                    BigButton(
                        "More",
                        Modifier.weight(1.2f),
                        minHeight = 60.dp,
                        fontSize = 17.sp,
                        selected = custom,
                    ) { timerSheet = true }
                }

                Spacer(Modifier.height(22.dp))
                Label("Shuffle")
                shows.forEach { show ->
                    BigButton(show.name, Modifier.fillMaxWidth()) { onPlay(show.id, timer) }
                    Spacer(Modifier.height(10.dp))
                }
                if (shows.size > 1) {
                    BigButton(mixLabel(shows), Modifier.fillMaxWidth()) { onPlay(Playback.ALL, timer) }
                    Spacer(Modifier.height(10.dp))
                }

                if (lastValid) {
                    Spacer(Modifier.height(10.dp))
                    BigButton(
                        "Same as last night",
                        Modifier.fillMaxWidth(),
                        sub = "${Playback.labelFor(lastKey)} · ${lastTimer.short()}",
                        minHeight = 100.dp,
                        fontSize = 24.sp,
                        emphasis = true,
                    ) { onPlay(lastKey!!, lastTimer) }
                }
            }
        }

        if (timerSheet) {
            TimerSheet(
                current = timer,
                onPick = {
                    timer = it
                    timerSheet = false
                },
                onClose = { timerSheet = false },
            )
        }
    }
}
