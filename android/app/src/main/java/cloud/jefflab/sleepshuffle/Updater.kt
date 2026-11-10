package cloud.jefflab.sleepshuffle

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Updates from this repo's GitHub Releases. Android releases are tagged `android-v<version>`
 * (the RG SP build uses `nextui-v…` in the same repo, so other tags are ignored).
 * Android always asks the person to tap Install; this only fetches the APK and hands it over.
 */
object Updater {
    private const val REPO = "belljeffj/sleep-shuffle"
    private const val TAG_PREFIX = "android-v"

    data class Release(val version: String, val apkUrl: String, val notes: String)

    sealed class State {
        data object Idle : State()
        data object Checking : State()
        data object UpToDate : State()
        data class Available(val release: Release) : State()
        data class Downloading(val release: Release, val percent: Int) : State()
        data class Failed(val message: String) : State()
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state
    private var checkedThisLaunch = false

    fun installedVersion(context: Context): String = try {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "0"
    } catch (e: Exception) {
        "0"
    }

    /** Once per app start; silent if offline or GitHub can't be reached. */
    fun checkOnLaunch(context: Context) {
        if (checkedThisLaunch) return
        checkedThisLaunch = true
        check(context, quiet = true)
    }

    fun check(context: Context, quiet: Boolean = false) {
        val current = installedVersion(context)
        _state.value = State.Checking
        scope.launch {
            _state.value = try {
                val rel = latest()
                if (rel != null && isNewer(rel.version, current)) State.Available(rel) else State.UpToDate
            } catch (e: Exception) {
                if (quiet) State.Idle else State.Failed("Couldn't reach GitHub to check for updates.")
            }
        }
    }

    fun downloadAndInstall(activity: Activity, release: Release) {
        if (Build.VERSION.SDK_INT >= 26 && !activity.packageManager.canRequestPackageInstalls()) {
            // One-time Android permission: "Allow from this source".
            activity.startActivity(
                Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}")),
            )
            return
        }
        scope.launch {
            _state.value = State.Downloading(release, 0)
            try {
                val file = download(activity, release) { pct -> _state.value = State.Downloading(release, pct) }
                _state.value = State.Available(release)
                val uri = FileProvider.getUriForFile(activity, "${activity.packageName}.updates", file)
                activity.startActivity(
                    Intent(Intent.ACTION_VIEW)
                        .setDataAndType(uri, "application/vnd.android.package-archive")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK),
                )
            } catch (e: Exception) {
                _state.value = State.Failed("The download didn't finish. Try again on Wi-Fi.")
            }
        }
    }

    private suspend fun latest(): Release? = withContext(Dispatchers.IO) {
        val conn = URL("https://api.github.com/repos/$REPO/releases?per_page=30").openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 8000
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        try {
            if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode}")
            val arr = JSONArray(conn.inputStream.bufferedReader().readText())
            var best: Release? = null
            for (i in 0 until arr.length()) {
                val r = arr.getJSONObject(i)
                if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue
                val tag = r.optString("tag_name")
                if (!tag.startsWith(TAG_PREFIX)) continue
                val assets = r.optJSONArray("assets") ?: continue
                var apk: String? = null
                for (j in 0 until assets.length()) {
                    val a = assets.getJSONObject(j)
                    if (a.optString("name").endsWith(".apk")) apk = a.optString("browser_download_url")
                }
                if (apk == null) continue
                val rel = Release(tag.removePrefix(TAG_PREFIX), apk, r.optString("body"))
                if (best == null || isNewer(rel.version, best.version)) best = rel
            }
            best
        } finally {
            conn.disconnect()
        }
    }

    private suspend fun download(context: Context, release: Release, progress: (Int) -> Unit): File =
        withContext(Dispatchers.IO) {
            val dir = File(context.cacheDir, "updates").apply { mkdirs() }
            dir.listFiles()?.forEach { it.delete() }
            val out = File(dir, "sleep-shuffle-${release.version}.apk")
            val conn = URL(release.apkUrl).openConnection() as HttpURLConnection
            conn.connectTimeout = 10000
            conn.readTimeout = 20000
            conn.instanceFollowRedirects = true
            try {
                if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode}")
                val total = conn.contentLengthLong
                var done = 0L
                var lastPct = -1
                conn.inputStream.use { input ->
                    out.outputStream().use { output ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            output.write(buf, 0, n)
                            done += n
                            if (total > 0) {
                                val pct = (done * 100 / total).toInt()
                                if (pct != lastPct) {
                                    lastPct = pct
                                    withContext(Dispatchers.Main) { progress(pct) }
                                }
                            }
                        }
                    }
                }
            } finally {
                conn.disconnect()
            }
            out
        }

    /** Compares dotted versions numerically: 1.10 > 1.9. */
    fun isNewer(candidate: String, current: String): Boolean {
        val a = candidate.split('.').map { it.toIntOrNull() ?: 0 }
        val b = current.split('.').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }
            val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }
}
