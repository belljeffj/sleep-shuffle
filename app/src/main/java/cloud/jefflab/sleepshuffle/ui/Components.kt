package cloud.jefflab.sleepshuffle.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Night palette: black background, dim amber text, no blue light, nothing bright. */
object Pal {
    val Bg = Color.Black
    val Amber = Color(0xFFE8A040)
    val AmberDim = Color(0xFFB07A32)
    val Faint = Color(0xFF6A4A20)
    val Fill = Color(0xFF0E0904)
    val FillStrong = Color(0xFF1C1207)
    val Line = Color(0xFF2E2110)
    val LineStrong = Color(0xFF5A3F18)
}

@Composable
fun SleepTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Pal.Amber,
            onPrimary = Color.Black,
            secondary = Pal.AmberDim,
            background = Pal.Bg,
            onBackground = Pal.AmberDim,
            surface = Pal.Bg,
            onSurface = Pal.AmberDim,
            surfaceVariant = Pal.Fill,
            onSurfaceVariant = Pal.Faint,
            outline = Pal.Line,
        ),
        content = content,
    )
}

@Composable
fun BigButton(
    text: String,
    modifier: Modifier = Modifier,
    sub: String? = null,
    minHeight: Dp = 72.dp,
    fontSize: TextUnit = 22.sp,
    emphasis: Boolean = false,
    selected: Boolean = false,
    onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(20.dp)
    val strong = emphasis || selected
    Box(
        modifier
            .heightIn(min = minHeight)
            .clip(shape)
            .background(if (strong) Pal.FillStrong else Pal.Fill)
            .border(if (selected) 2.dp else 1.dp, if (strong) Pal.LineStrong else Pal.Line, shape)
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text,
                color = if (strong) Pal.Amber else Pal.AmberDim,
                fontSize = fontSize,
                fontWeight = FontWeight.Medium,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            if (sub != null) {
                Text(
                    sub,
                    color = Pal.Faint,
                    fontSize = (fontSize.value * 0.68f).sp,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
fun Label(text: String, modifier: Modifier = Modifier) {
    Text(text, color = Pal.Faint, fontSize = 15.sp, modifier = modifier.padding(bottom = 6.dp, top = 4.dp))
}

/**
 * A button that only fires after being held for [holdMs], with a fill that grows while
 * you hold. Used for anything you wouldn't want to trigger by brushing the screen.
 */
@Composable
fun HoldButton(
    text: String,
    modifier: Modifier = Modifier,
    holdMs: Long = 1200L,
    onConfirmed: () -> Unit,
) {
    var progress by remember { mutableFloatStateOf(0f) }
    val scope = rememberCoroutineScope()
    val confirm by rememberUpdatedState(onConfirmed)
    val shape = RoundedCornerShape(20.dp)
    Box(
        modifier
            .heightIn(min = 72.dp)
            .clip(shape)
            .background(Pal.Fill)
            .drawBehind { drawRect(Pal.FillStrong, size = Size(size.width * progress, size.height)) }
            .border(1.dp, Pal.LineStrong, shape)
            .pointerInput(Unit) {
                detectTapGestures(onPress = {
                    val job = scope.launch {
                        val start = System.currentTimeMillis()
                        while (true) {
                            progress = ((System.currentTimeMillis() - start).toFloat() / holdMs).coerceAtMost(1f)
                            if (progress >= 1f) {
                                confirm()
                                break
                            }
                            delay(16)
                        }
                    }
                    tryAwaitRelease()
                    job.cancel()
                    progress = 0f
                })
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            color = Pal.AmberDim,
            fontSize = 19.sp,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(12.dp),
        )
    }
}

@Composable
fun TextLink(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Text(
        text,
        color = Pal.AmberDim,
        fontSize = 17.sp,
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 12.dp),
    )
}
