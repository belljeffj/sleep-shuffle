package cloud.jefflab.sleepshuffle.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import cloud.jefflab.sleepshuffle.TimerSpec
import cloud.jefflab.sleepshuffle.formatClock
import java.util.Calendar

val PRESET_MINUTES = listOf(15, 30, 45, 60, 90)

/** Full-screen, bottom-anchored timer picker with big targets. */
@Composable
fun TimerSheet(current: TimerSpec, onPick: (TimerSpec) -> Unit, onClose: () -> Unit) {
    var custom by remember { mutableIntStateOf((current as? TimerSpec.Minutes)?.minutes ?: 30) }
    // Clock time kept as minutes after midnight, in 15-minute steps.
    var clockMinutes by remember {
        mutableIntStateOf(
            (current as? TimerSpec.Clock)?.let { it.hour * 60 + it.minute } ?: run {
                val cal = Calendar.getInstance()
                val now = cal.get(Calendar.HOUR_OF_DAY) * 60 + cal.get(Calendar.MINUTE)
                (((now + 60) / 15 + 1) * 15) % (24 * 60)
            }
        )
    }
    fun shiftClock(delta: Int) {
        clockMinutes = ((clockMinutes + delta) % 1440 + 1440) % 1440
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.97f))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { }
            .systemBarsPadding(),
        contentAlignment = Alignment.BottomCenter,
    ) {
        Column(
            Modifier
                .widthIn(max = 600.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
        ) {
            Text("Sleep timer", color = Pal.Amber, fontSize = 22.sp, modifier = Modifier.padding(bottom = 10.dp))

            Label("Minutes")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                PRESET_MINUTES.forEach { m ->
                    BigButton(
                        "$m",
                        Modifier.weight(1f),
                        minHeight = 64.dp,
                        selected = current == TimerSpec.Minutes(m),
                    ) { onPick(TimerSpec.Minutes(m)) }
                }
            }

            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                BigButton("−5", Modifier.weight(1f), minHeight = 64.dp) { custom = (custom - 5).coerceAtLeast(5) }
                Text(
                    "$custom min",
                    color = Pal.Amber,
                    fontSize = 22.sp,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.weight(1.4f),
                )
                BigButton("+5", Modifier.weight(1f), minHeight = 64.dp) { custom = (custom + 5).coerceAtMost(600) }
                BigButton("Set", Modifier.weight(1.2f), minHeight = 64.dp, emphasis = true) {
                    onPick(TimerSpec.Minutes(custom))
                }
            }

            Spacer(Modifier.height(8.dp))
            Label("Episodes")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(1, 2, 3).forEach { n ->
                    BigButton(
                        if (n == 1) "1 ep" else "$n eps",
                        Modifier.weight(1f),
                        minHeight = 64.dp,
                        selected = current == TimerSpec.Episodes(n),
                    ) { onPick(TimerSpec.Episodes(n)) }
                }
            }

            Spacer(Modifier.height(8.dp))
            Label("At a time")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                BigButton("−1h", Modifier.weight(1f), minHeight = 64.dp, fontSize = 18.sp) { shiftClock(-60) }
                BigButton("−15", Modifier.weight(1f), minHeight = 64.dp, fontSize = 18.sp) { shiftClock(-15) }
                Text(
                    formatClock(clockMinutes / 60, clockMinutes % 60),
                    color = Pal.Amber,
                    fontSize = 21.sp,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.weight(1.8f),
                )
                BigButton("+15", Modifier.weight(1f), minHeight = 64.dp, fontSize = 18.sp) { shiftClock(15) }
                BigButton("+1h", Modifier.weight(1f), minHeight = 64.dp, fontSize = 18.sp) { shiftClock(60) }
            }
            Spacer(Modifier.height(8.dp))
            BigButton(
                "Stop at ${formatClock(clockMinutes / 60, clockMinutes % 60)}",
                Modifier.fillMaxWidth(),
                minHeight = 64.dp,
                emphasis = true,
            ) { onPick(TimerSpec.Clock(clockMinutes / 60, clockMinutes % 60)) }

            Spacer(Modifier.height(16.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                BigButton("No timer", Modifier.weight(1f), selected = current == TimerSpec.Off) {
                    onPick(TimerSpec.Off)
                }
                BigButton("Close", Modifier.weight(1f)) { onClose() }
            }
        }
    }
}
