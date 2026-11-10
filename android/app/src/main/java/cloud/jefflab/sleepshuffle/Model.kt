package cloud.jefflab.sleepshuffle

import java.text.DateFormat
import java.util.Calendar

data class Show(val id: String, val name: String)

data class Banned(val id: String, val label: String)

data class Episode(
    val id: String,
    val seriesId: String,
    val seriesName: String,
    val season: Int,
    val number: Int,
    val name: String,
    val overview: String,
) {
    val code: String get() = "S%02dE%02d".format(season, number)
    val label: String get() = "$seriesName $code · $name"
}

/** An intro or recap range reported by Jellyfin, in milliseconds. */
data class Segment(val type: String, val startMs: Long, val endMs: Long)

/** What the sleep timer is set to. Stored as a short string in preferences. */
sealed class TimerSpec {
    data object Off : TimerSpec()
    data class Minutes(val minutes: Int) : TimerSpec()
    /** 1 = stop at the end of the current episode, 2 = one more after it, and so on. */
    data class Episodes(val count: Int) : TimerSpec()
    data class Clock(val hour: Int, val minute: Int) : TimerSpec()

    fun encode(): String = when (this) {
        Off -> "off"
        is Minutes -> "min:$minutes"
        is Episodes -> "ep:$count"
        is Clock -> "clock:$hour:$minute"
    }

    /** Short form for buttons: "45 min", "1 episode", "11:30 PM", "No timer". */
    fun short(): String = when (this) {
        Off -> "No timer"
        is Minutes -> "$minutes min"
        is Episodes -> if (count == 1) "1 episode" else "$count episodes"
        is Clock -> formatClock(hour, minute)
    }

    fun label(): String = when (this) {
        Off -> "No sleep timer"
        is Minutes -> "Sleep after $minutes min"
        is Episodes -> if (count == 1) "Sleep after 1 episode" else "Sleep after $count episodes"
        is Clock -> "Sleep at ${formatClock(hour, minute)}"
    }

    companion object {
        fun decode(raw: String?): TimerSpec {
            val parts = (raw ?: "").split(":")
            return try {
                when (parts[0]) {
                    "min" -> Minutes(parts[1].toInt())
                    "ep" -> Episodes(parts[1].toInt())
                    "clock" -> Clock(parts[1].toInt(), parts[2].toInt())
                    "off" -> Off
                    else -> Minutes(45)
                }
            } catch (e: Exception) {
                Minutes(45)
            }
        }
    }
}

fun formatClock(hour: Int, minute: Int): String {
    val cal = Calendar.getInstance().apply {
        set(Calendar.HOUR_OF_DAY, hour)
        set(Calendar.MINUTE, minute)
    }
    return DateFormat.getTimeInstance(DateFormat.SHORT).format(cal.time)
}

/** Next wall-clock time (ms) that matches hour:minute, today or tomorrow. */
fun nextClockMs(hour: Int, minute: Int, now: Long = System.currentTimeMillis()): Long {
    val cal = Calendar.getInstance().apply {
        timeInMillis = now
        set(Calendar.HOUR_OF_DAY, hour)
        set(Calendar.MINUTE, minute)
        set(Calendar.SECOND, 0)
        set(Calendar.MILLISECOND, 0)
    }
    if (cal.timeInMillis <= now + 60_000L) cal.add(Calendar.DAY_OF_YEAR, 1)
    return cal.timeInMillis
}

fun formatDuration(ms: Long): String {
    val total = (ms.coerceAtLeast(0L) / 1000L)
    val h = total / 3600
    val m = (total % 3600) / 60
    val s = total % 60
    return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
}

fun mixLabel(shows: List<Show>): String = if (shows.size == 2) "Mix both" else "Mix all ${shows.size}"
