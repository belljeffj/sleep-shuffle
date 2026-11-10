package cloud.jefflab.sleepshuffle.ui

import android.content.pm.ActivityInfo
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.media3.common.C
import androidx.media3.ui.AspectRatioFrameLayout
import androidx.media3.ui.PlayerView
import cloud.jefflab.sleepshuffle.NowState
import cloud.jefflab.sleepshuffle.Phase
import cloud.jefflab.sleepshuffle.Playback
import cloud.jefflab.sleepshuffle.TimerSpec
import cloud.jefflab.sleepshuffle.formatClock
import cloud.jefflab.sleepshuffle.formatDuration
import kotlinx.coroutines.delay

/** Overlay darkness per dim level (0 = off). Also lowers the screen backlight below the system minimum. */
private val DIM_OVERLAY = listOf(0f, 0.3f, 0.55f, 0.75f)
private val DIM_BRIGHTNESS = listOf(WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE, 0.15f, 0.04f, 0.01f)

@Composable
fun PlayerScreen(activity: ComponentActivity, onExit: () -> Unit) {
    val s by Playback.state.collectAsStateWithLifecycle()
    val exo by Playback.player.collectAsStateWithLifecycle()

    var controlsVisible by remember { mutableStateOf(true) }
    var poke by remember { mutableLongStateOf(0L) }
    var locked by remember { mutableStateOf(false) }
    var unlockHint by remember { mutableStateOf(false) }
    var timerSheet by remember { mutableStateOf(false) }
    var infoSheet by remember { mutableStateOf(false) }
    var dimLevel by remember { mutableIntStateOf(Playback.prefs.dimLevel.coerceIn(0, 3)) }
    var rotationLocked by remember {
        mutableStateOf(activity.requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_LOCKED)
    }
    var position by remember { mutableLongStateOf(0L) }
    var duration by remember { mutableLongStateOf(0L) }
    var playing by remember { mutableStateOf(false) }
    var remaining by remember { mutableStateOf<Long?>(null) }
    var endedReveal by remember { mutableStateOf(false) }

    fun touched() {
        poke = System.currentTimeMillis()
    }

    // Poll the player twice a second for the clock and progress bar.
    LaunchedEffect(exo) {
        while (true) {
            val p = exo
            position = p?.currentPosition ?: 0L
            duration = p?.duration?.takeIf { it != C.TIME_UNSET && it > 0 } ?: 0L
            playing = p?.isPlaying ?: false
            remaining = Playback.remainingMs()
            delay(500)
        }
    }

    // Controls fade away on their own so the screen goes dark.
    LaunchedEffect(controlsVisible, poke, timerSheet, infoSheet) {
        if (controlsVisible && !timerSheet && !infoSheet) {
            delay(6000)
            controlsVisible = false
        }
    }
    LaunchedEffect(unlockHint, poke) {
        if (unlockHint) {
            delay(4000)
            unlockHint = false
        }
    }
    LaunchedEffect(endedReveal) {
        if (endedReveal) {
            delay(10_000)
            endedReveal = false
        }
    }

    BackHandler {
        when {
            timerSheet -> timerSheet = false
            infoSheet -> infoSheet = false
            locked -> unlockHint = true
            else -> onExit()
        }
    }

    val window = activity.window

    // Keep the screen awake only while something is actually on it.
    val keepOn = s.phase == Phase.Playing || s.phase == Phase.Connecting
    DisposableEffect(keepOn) {
        if (keepOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        onDispose { window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }
    }

    // Backlight: the dim level, dropping to minimum during the fade and after the timer ends.
    val brightness = when {
        s.phase == Phase.Ended -> 0.01f
        s.fade > 0.05f -> 0.01f
        else -> DIM_BRIGHTNESS[dimLevel]
    }
    LaunchedEffect(brightness) {
        val lp = window.attributes
        lp.screenBrightness = brightness
        window.attributes = lp
    }

    // Full screen: hide the status and navigation bars.
    DisposableEffect(Unit) {
        val insets = WindowCompat.getInsetsController(window, window.decorView)
        insets.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        insets.hide(WindowInsetsCompat.Type.systemBars())
        onDispose {
            insets.show(WindowInsetsCompat.Type.systemBars())
            val lp = window.attributes
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            window.attributes = lp
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }

    val overlayAlpha = when (s.phase) {
        Phase.Ended -> 1f
        else -> maxOf(DIM_OVERLAY[dimLevel], s.fade)
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        val p = exo
        if (p != null) {
            AndroidView(
                factory = { ctx ->
                    PlayerView(ctx).apply {
                        useController = false
                        setShutterBackgroundColor(android.graphics.Color.BLACK)
                        setBackgroundColor(android.graphics.Color.BLACK)
                        setKeepContentOnPlayerReset(true)
                        resizeMode = AspectRatioFrameLayout.RESIZE_MODE_FIT
                        player = p
                    }
                },
                update = { view -> if (view.player !== p) view.player = p },
                onRelease = { view -> view.player = null },
                modifier = Modifier.fillMaxSize(),
            )
        }

        // Dimmer
        Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = overlayAlpha)))

        // Tap anywhere: show or hide controls. When touch-locked, taps only reveal the unlock button.
        Box(
            Modifier
                .fillMaxSize()
                .pointerInput(locked, s.phase) {
                    detectTapGestures(onTap = {
                        when {
                            s.phase == Phase.Ended -> endedReveal = true
                            locked -> {
                                unlockHint = true
                                touched()
                            }
                            else -> {
                                controlsVisible = !controlsVisible
                                touched()
                            }
                        }
                    })
                },
        )

        when (s.phase) {
            Phase.Idle, Phase.Connecting -> CenterMessage(
                if (s.selectionLabel.isNotEmpty()) "Shuffling ${s.selectionLabel}…" else "Connecting…",
            )

            Phase.Error -> Column(
                Modifier
                    .align(Alignment.Center)
                    .widthIn(max = 480.dp)
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    s.message ?: "Something went wrong.",
                    color = Pal.AmberDim,
                    fontSize = 19.sp,
                    textAlign = TextAlign.Center,
                )
                Spacer(Modifier.height(20.dp))
                BigButton("Try again", Modifier.fillMaxWidth(), emphasis = true) { Playback.retryLast() }
                Spacer(Modifier.height(10.dp))
                BigButton("Home", Modifier.fillMaxWidth()) { onExit() }
            }

            Phase.Ended -> if (endedReveal) {
                Column(
                    Modifier
                        .align(Alignment.Center)
                        .widthIn(max = 420.dp)
                        .padding(24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("Stopped by the sleep timer", color = Pal.Faint, fontSize = 17.sp)
                    Spacer(Modifier.height(16.dp))
                    BigButton("Keep watching", Modifier.fillMaxWidth(), sub = "15 more minutes", emphasis = true) {
                        endedReveal = false
                        Playback.keepWatching()
                    }
                    Spacer(Modifier.height(10.dp))
                    BigButton("Home", Modifier.fillMaxWidth()) { onExit() }
                }
            }

            Phase.Playing -> {
                val ep = s.episode
                val segment = s.segments.firstOrNull { position >= it.startMs && position < it.endMs - 1500 }

                if (controlsVisible && !locked) {
                    Controls(
                        modifier = Modifier.align(Alignment.BottomCenter),
                        s = s,
                        title = ep?.name ?: "",
                        subtitle = ep?.let { "${it.seriesName} · ${it.code}" } ?: "",
                        position = position,
                        duration = duration,
                        playing = playing,
                        timerText = timerStatus(s, remaining),
                        dimLevel = dimLevel,
                        rotationLocked = rotationLocked,
                        onAny = { touched() },
                        onBack10 = { Playback.seekBy(-10_000) },
                        onPlayPause = { Playback.togglePlay() },
                        onFwd10 = { Playback.seekBy(10_000) },
                        onNext = { Playback.nextRandom() },
                        onTimer = { timerSheet = true },
                        onDim = {
                            dimLevel = (dimLevel + 1) % 4
                            Playback.prefs.dimLevel = dimLevel
                        },
                        onRotate = {
                            rotationLocked = !rotationLocked
                            activity.requestedOrientation = if (rotationLocked) {
                                ActivityInfo.SCREEN_ORIENTATION_LOCKED
                            } else {
                                ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
                            }
                        },
                        onLock = {
                            locked = true
                            controlsVisible = false
                        },
                        onInfo = { infoSheet = true },
                    )
                }

                if (segment != null && !locked && s.fade == 0f) {
                    BigButton(
                        if (segment.type == "Recap") "Skip recap" else "Skip intro",
                        Modifier
                            .align(Alignment.TopEnd)
                            .systemBarsPadding()
                            .padding(16.dp)
                            .width(180.dp),
                        minHeight = 64.dp,
                        fontSize = 19.sp,
                    ) {
                        Playback.skipTo(segment.endMs)
                        touched()
                    }
                }

                if (locked && unlockHint) {
                    Column(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .systemBarsPadding()
                            .padding(24.dp)
                            .widthIn(max = 420.dp)
                            .fillMaxWidth(),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text("Touch is locked", color = Pal.Faint, fontSize = 15.sp)
                        Spacer(Modifier.height(8.dp))
                        HoldButton("Hold to unlock", Modifier.fillMaxWidth()) {
                            locked = false
                            unlockHint = false
                            controlsVisible = true
                            touched()
                        }
                    }
                }

                if (s.fade > 0f) {
                    Column(
                        Modifier.align(Alignment.Center),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text("Fading out", color = Pal.Faint, fontSize = 16.sp)
                        Spacer(Modifier.height(12.dp))
                        BigButton(
                            "+15 min",
                            Modifier.width(260.dp),
                            minHeight = 120.dp,
                            fontSize = 30.sp,
                            emphasis = true,
                        ) { Playback.extend() }
                    }
                }
            }
        }

        if (timerSheet) {
            TimerSheet(
                current = s.timer,
                onPick = {
                    Playback.applyTimer(it)
                    timerSheet = false
                    touched()
                },
                onClose = { timerSheet = false },
            )
        }

        if (infoSheet) {
            InfoSheet(s, onClose = { infoSheet = false })
        }
    }
}

private fun timerStatus(s: NowState, remaining: Long?): String = when (val t = s.timer) {
    TimerSpec.Off -> "No timer"
    is TimerSpec.Episodes -> if (s.episodesLeft <= 1) "Sleep after this episode" else "Sleep after ${s.episodesLeft} episodes"
    is TimerSpec.Clock -> {
        val r = remaining ?: 0L
        "Sleep at ${formatClock(t.hour, t.minute)} (${minutesText(r)})"
    }
    is TimerSpec.Minutes -> "Sleep in ${minutesText(remaining ?: 0L)}"
}

private fun minutesText(ms: Long): String {
    if (ms < 60_000L) return "${(ms / 1000L).coerceAtLeast(0L)} s"
    val total = (ms + 59_999L) / 60_000L
    return if (total >= 60) "${total / 60} h ${total % 60} min" else "$total min"
}

@Composable
private fun CenterMessage(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, color = Pal.Faint, fontSize = 19.sp, textAlign = TextAlign.Center, modifier = Modifier.padding(24.dp))
    }
}

@Composable
private fun Controls(
    modifier: Modifier,
    s: NowState,
    title: String,
    subtitle: String,
    position: Long,
    duration: Long,
    playing: Boolean,
    timerText: String,
    dimLevel: Int,
    rotationLocked: Boolean,
    onAny: () -> Unit,
    onBack10: () -> Unit,
    onPlayPause: () -> Unit,
    onFwd10: () -> Unit,
    onNext: () -> Unit,
    onTimer: () -> Unit,
    onDim: () -> Unit,
    onRotate: () -> Unit,
    onLock: () -> Unit,
    onInfo: () -> Unit,
) {
    fun act(f: () -> Unit): () -> Unit = {
        onAny()
        f()
    }
    Box(
        modifier
            .fillMaxWidth()
            .background(Color.Black.copy(alpha = 0.6f))
            .systemBarsPadding(),
        contentAlignment = Alignment.BottomCenter,
    ) {
        Column(
            Modifier
                .widthIn(max = 640.dp)
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 14.dp),
        ) {
            Text(subtitle, color = Pal.Faint, fontSize = 15.sp, maxLines = 1)
            Text(
                title,
                color = Pal.AmberDim,
                fontSize = 20.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "${formatDuration(position)} / ${formatDuration(duration)}",
                    color = Pal.Faint,
                    fontSize = 14.sp,
                    modifier = Modifier.weight(1f),
                )
                Text(timerText, color = Pal.AmberDim, fontSize = 15.sp)
            }
            Spacer(Modifier.height(6.dp))
            ProgressLine(if (duration > 0) position.toFloat() / duration else 0f)
            Spacer(Modifier.height(14.dp))

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                BigButton("−10s", Modifier.weight(1f), fontSize = 19.sp, onClick = act(onBack10))
                PlayPauseButton(playing, Modifier.weight(1.3f), act(onPlayPause))
                BigButton("+10s", Modifier.weight(1f), fontSize = 19.sp, onClick = act(onFwd10))
                BigButton("Next", Modifier.weight(1.2f), sub = "random", fontSize = 19.sp, onClick = act(onNext))
            }
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val small = 60.dp
                BigButton("Timer", Modifier.weight(1f), sub = s.timer.short(), minHeight = small, fontSize = 16.sp, onClick = act(onTimer))
                BigButton(
                    "Dim",
                    Modifier.weight(1f),
                    sub = if (dimLevel == 0) "off" else "$dimLevel of 3",
                    minHeight = small,
                    fontSize = 16.sp,
                    onClick = act(onDim),
                )
                BigButton(
                    "Rotate",
                    Modifier.weight(1f),
                    sub = if (rotationLocked) "locked" else "auto",
                    minHeight = small,
                    fontSize = 16.sp,
                    selected = rotationLocked,
                    onClick = act(onRotate),
                )
                BigButton("Lock", Modifier.weight(1f), sub = "touch", minHeight = small, fontSize = 16.sp, onClick = act(onLock))
                BigButton("Info", Modifier.weight(1f), sub = "episode", minHeight = small, fontSize = 16.sp, onClick = act(onInfo))
            }
        }
    }
}

@Composable
private fun ProgressLine(fraction: Float) {
    Canvas(
        Modifier
            .fillMaxWidth()
            .height(3.dp),
    ) {
        drawRect(Pal.Line)
        drawRect(Pal.AmberDim, size = Size(size.width * fraction.coerceIn(0f, 1f), size.height))
    }
}

@Composable
private fun PlayPauseButton(playing: Boolean, modifier: Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(20.dp)
    Box(
        modifier
            .heightIn(min = 80.dp)
            .clip(shape)
            .background(Pal.FillStrong)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(34.dp)) {
            val w = size.width
            val h = size.height
            if (playing) {
                val bar = w * 0.3f
                drawRect(Pal.Amber, topLeft = Offset(w * 0.1f, 0f), size = Size(bar, h))
                drawRect(Pal.Amber, topLeft = Offset(w * 0.6f, 0f), size = Size(bar, h))
            } else {
                val path = Path().apply {
                    moveTo(w * 0.15f, 0f)
                    lineTo(w * 0.95f, h / 2f)
                    lineTo(w * 0.15f, h)
                    close()
                }
                drawPath(path, Pal.Amber)
            }
        }
    }
}

@Composable
private fun InfoSheet(s: NowState, onClose: () -> Unit) {
    val ep = s.episode
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.96f))
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
            if (ep != null) {
                Text("${ep.seriesName} · Season ${ep.season}, Episode ${ep.number}", color = Pal.Faint, fontSize = 16.sp)
                Spacer(Modifier.height(4.dp))
                Text(ep.name, color = Pal.Amber, fontSize = 24.sp)
                Spacer(Modifier.height(12.dp))
                Text(
                    ep.overview.ifBlank { "No summary on the server for this one." },
                    color = Pal.AmberDim,
                    fontSize = 18.sp,
                    lineHeight = 26.sp,
                )
                Spacer(Modifier.height(24.dp))
                HoldButton("Hold to never shuffle this one again", Modifier.fillMaxWidth()) {
                    Playback.banCurrent()
                    onClose()
                }
                Spacer(Modifier.height(10.dp))
            }
            BigButton("Close", Modifier.fillMaxWidth()) { onClose() }
        }
    }
}
