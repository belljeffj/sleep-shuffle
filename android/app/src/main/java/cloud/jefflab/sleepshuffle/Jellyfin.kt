package cloud.jefflab.sleepshuffle

import android.net.Uri
import android.os.Build
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

class JellyfinException(message: String) : Exception(message)

/**
 * Read-only Jellyfin client. It signs in, lists shows and episodes, reads intro markers,
 * and builds stream URLs. It never calls the playback-reporting endpoints, so nothing
 * is marked played and nothing lands in Continue Watching.
 */
class JellyfinClient(private val prefs: Prefs) {

    @Volatile
    private var base: String? = null

    val currentBase: String? get() = base

    fun normalize(url: String): String {
        var u = url.trim().trimEnd('/')
        if (u.isEmpty()) return u
        if (!u.startsWith("http://") && !u.startsWith("https://")) u = "http://$u"
        return u
    }

    fun authHeader(): String {
        val device = (Build.MODEL ?: "Android").replace("\"", "")
        val token = prefs.token
        val tokenPart = if (token.isNotEmpty()) ", Token=\"$token\"" else ""
        return "MediaBrowser Client=\"Sleep Shuffle\", Device=\"$device\", " +
            "DeviceId=\"${prefs.deviceId}\", Version=\"1.0\"$tokenPart"
    }

    private fun request(
        url: String,
        method: String = "GET",
        body: String? = null,
        connectTimeout: Int = 5000,
        readTimeout: Int = 20000,
    ): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        try {
            conn.requestMethod = method
            conn.connectTimeout = connectTimeout
            conn.readTimeout = readTimeout
            conn.setRequestProperty("Authorization", authHeader())
            conn.setRequestProperty("Accept", "application/json")
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.outputStream.use { it.write(body.toByteArray()) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() } ?: ""
            if (code == 401) throw JellyfinException("Jellyfin turned down the sign-in. Check the username and password.")
            if (code !in 200..299) throw JellyfinException("Jellyfin answered with HTTP $code.")
            return text
        } finally {
            conn.disconnect()
        }
    }

    private suspend fun get(path: String): JSONObject = withContext(Dispatchers.IO) {
        JSONObject(request(baseOrThrow() + path))
    }

    fun baseOrThrow(): String = base ?: throw JellyfinException("Not connected to Jellyfin yet.")

    suspend fun ping(url: String): Boolean = withContext(Dispatchers.IO) {
        if (url.isBlank()) return@withContext false
        try {
            request("${normalize(url)}/System/Info/Public", connectTimeout = 2500, readTimeout = 4000)
            true
        } catch (e: Exception) {
            false
        }
    }

    /** Tries the home address first, then the Tailscale one. */
    suspend fun resolveBase(force: Boolean = false): String {
        if (!force) base?.let { return it }
        val candidates = listOf(prefs.lanUrl, prefs.tailscaleUrl)
            .filter { it.isNotBlank() }
            .map { normalize(it) }
        if (candidates.isEmpty()) throw JellyfinException("No server address set. Add one in Settings.")
        for (candidate in candidates) {
            if (ping(candidate)) {
                base = candidate
                return candidate
            }
        }
        throw JellyfinException("Can't reach Jellyfin at home or over Tailscale. Is Tailscale on?")
    }

    suspend fun signIn(lanUrl: String, tailscaleUrl: String, user: String, password: String) {
        prefs.lanUrl = normalize(lanUrl)
        prefs.tailscaleUrl = normalize(tailscaleUrl)
        prefs.token = ""
        val b = resolveBase(force = true)
        val payload = JSONObject().put("Username", user).put("Pw", password).toString()
        val json = withContext(Dispatchers.IO) {
            JSONObject(request("$b/Users/AuthenticateByName", method = "POST", body = payload))
        }
        val token = json.optString("AccessToken")
        val userObj = json.optJSONObject("User")
        if (token.isEmpty() || userObj == null) throw JellyfinException("Sign-in worked but Jellyfin sent no token.")
        prefs.token = token
        prefs.userId = userObj.optString("Id")
        prefs.userName = userObj.optString("Name", user)
    }

    suspend fun series(): List<Show> {
        resolveBase()
        val json = get(
            "/Items?userId=${prefs.userId}&includeItemTypes=Series&recursive=true" +
                "&sortBy=SortName&sortOrder=Ascending&enableImages=false"
        )
        val items = json.optJSONArray("Items") ?: return emptyList()
        return (0 until items.length()).map {
            val o = items.getJSONObject(it)
            Show(o.getString("Id"), o.optString("Name"))
        }
    }

    suspend fun episodes(seriesId: String, seriesName: String): List<Episode> {
        resolveBase()
        val json = get("/Shows/$seriesId/Episodes?userId=${prefs.userId}&fields=Overview&isMissing=false")
        val items = json.optJSONArray("Items") ?: return emptyList()
        val out = ArrayList<Episode>(items.length())
        for (i in 0 until items.length()) {
            val o = items.getJSONObject(i)
            val season = o.optInt("ParentIndexNumber", 0)
            if (season <= 0) continue // skip specials
            if (o.optString("LocationType") == "Virtual") continue // skip missing files
            out += Episode(
                id = o.getString("Id"),
                seriesId = seriesId,
                seriesName = o.optString("SeriesName", seriesName).ifEmpty { seriesName },
                season = season,
                number = o.optInt("IndexNumber", 0),
                name = o.optString("Name"),
                overview = o.optString("Overview"),
            )
        }
        return out
    }

    /** Intro/recap ranges: Jellyfin 10.10+ media segments first, then the older Intro Skipper API. */
    suspend fun segments(itemId: String): List<Segment> {
        try {
            val json = get("/MediaSegments/$itemId?includeSegmentTypes=Intro&includeSegmentTypes=Recap")
            val items = json.optJSONArray("Items")
            if (items != null && items.length() > 0) {
                return (0 until items.length()).mapNotNull {
                    val o = items.getJSONObject(it)
                    val type = o.optString("Type")
                    if (type != "Intro" && type != "Recap") return@mapNotNull null
                    Segment(type, o.optLong("StartTicks") / 10_000L, o.optLong("EndTicks") / 10_000L)
                }.filter { it.endMs > it.startMs }
            }
        } catch (e: Exception) {
            // fall through to the plugin endpoint
        }
        return try {
            val json = get("/Episode/$itemId/IntroTimestamps/v1")
            if (!json.optBoolean("Valid")) return emptyList()
            val start = (json.optDouble("IntroStart") * 1000).toLong()
            val end = (json.optDouble("IntroEnd") * 1000).toLong()
            if (end > start) listOf(Segment("Intro", start, end)) else emptyList()
        } catch (e: Exception) {
            emptyList()
        }
    }

    /** The original file, untouched. Nothing is transcoded. */
    fun directUrl(ep: Episode): String {
        val b = baseOrThrow()
        return "$b/Videos/${ep.id}/stream?static=true&mediaSourceId=${ep.id}" +
            "&deviceId=${prefs.deviceId}&api_key=${Uri.encode(prefs.token)}"
    }

    /** Fallback when the phone can't decode the original: an H.264/AAC stream made by the server. */
    fun hlsUrl(ep: Episode, playSessionId: String): String {
        val b = baseOrThrow()
        return "$b/Videos/${ep.id}/master.m3u8?mediaSourceId=${ep.id}" +
            "&deviceId=${prefs.deviceId}&playSessionId=$playSessionId" +
            "&videoCodec=h264&audioCodec=aac&maxAudioChannels=2" +
            "&videoBitrate=6000000&audioBitrate=192000&segmentContainer=ts" +
            "&api_key=${Uri.encode(prefs.token)}"
    }

    fun artworkUrl(seriesId: String): String? {
        val b = base ?: return null
        return "$b/Items/$seriesId/Images/Primary?maxHeight=400&api_key=${Uri.encode(prefs.token)}"
    }

    /** Tells the server to stop a fallback transcode we no longer need. */
    suspend fun stopEncoding(playSessionId: String) {
        val b = base ?: return
        withContext(Dispatchers.IO) {
            try {
                request(
                    "$b/Videos/ActiveEncodings?deviceId=${prefs.deviceId}&playSessionId=$playSessionId",
                    method = "DELETE",
                )
            } catch (e: Exception) {
                // best effort
            }
        }
    }
}

object Shuffler {
    /**
     * Picks a random episode from [pool], skipping banned ones, anything in [exclude],
     * and the [window] most recently played episodes from this same pool.
     */
    fun pick(
        pool: List<Episode>,
        banned: Set<String>,
        history: List<String>,
        window: Int,
        exclude: Set<String>,
    ): Episode? {
        val allowed = pool.filter { it.id !in banned && it.id !in exclude }
        if (allowed.isEmpty()) {
            return pool.filter { it.id !in banned }.randomOrNull() ?: pool.randomOrNull()
        }
        val effectiveWindow = window.coerceAtMost(allowed.size - 1).coerceAtLeast(0)
        val poolIds = allowed.mapTo(HashSet()) { it.id }
        val recent = HashSet<String>()
        for (id in history.asReversed()) {
            if (recent.size >= effectiveWindow) break
            if (id in poolIds) recent.add(id)
        }
        val fresh = allowed.filter { it.id !in recent }
        return fresh.ifEmpty { allowed }.random()
    }
}
