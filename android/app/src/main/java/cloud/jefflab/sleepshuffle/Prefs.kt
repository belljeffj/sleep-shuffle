package cloud.jefflab.sleepshuffle

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/**
 * Everything the app remembers lives here, on this device only.
 * Nothing about what you watch is written back to Jellyfin.
 */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("sleep_shuffle", Context.MODE_PRIVATE)

    private fun str(key: String): String = sp.getString(key, "") ?: ""
    private fun save(key: String, value: String?) = sp.edit().putString(key, value).apply()

    var lanUrl: String
        get() = str("lanUrl")
        set(v) = save("lanUrl", v)

    var tailscaleUrl: String
        get() = str("tailscaleUrl")
        set(v) = save("tailscaleUrl", v)

    var token: String
        get() = str("token")
        set(v) = save("token", v)

    var userId: String
        get() = str("userId")
        set(v) = save("userId", v)

    var userName: String
        get() = str("userName")
        set(v) = save("userName", v)

    val deviceId: String
        get() {
            val existing = str("deviceId")
            if (existing.isNotEmpty()) return existing
            val fresh = UUID.randomUUID().toString().replace("-", "")
            save("deviceId", fresh)
            return fresh
        }

    val signedIn: Boolean get() = token.isNotEmpty() && userId.isNotEmpty()

    var shows: List<Show>
        get() = parseObjects(str("shows")) { Show(it.getString("id"), it.getString("name")) }
        set(v) {
            val arr = JSONArray()
            v.forEach { arr.put(JSONObject().put("id", it.id).put("name", it.name)) }
            save("shows", arr.toString())
        }

    val history: List<String>
        get() = parseStrings(str("history"))

    fun addHistory(id: String) {
        val updated = (history + id).takeLast(600)
        save("history", JSONArray(updated).toString())
    }

    var banned: List<Banned>
        get() = parseObjects(str("banned")) { Banned(it.getString("id"), it.optString("label")) }
        set(v) {
            val arr = JSONArray()
            v.forEach { arr.put(JSONObject().put("id", it.id).put("label", it.label)) }
            save("banned", arr.toString())
        }

    fun ban(id: String, label: String) {
        if (banned.none { it.id == id }) banned = banned + Banned(id, label)
    }

    fun unban(id: String) {
        banned = banned.filterNot { it.id == id }
    }

    /** How many recent plays (per show) are skipped when picking the next episode. */
    var recentWindow: Int
        get() = sp.getInt("recentWindow", 50)
        set(v) = sp.edit().putInt("recentWindow", v).apply()

    /** Show id, or Playback.ALL for the mix. */
    var lastSelection: String?
        get() = str("lastSelection").ifEmpty { null }
        set(v) = save("lastSelection", v)

    var lastTimer: String
        get() = sp.getString("lastTimer", "min:45") ?: "min:45"
        set(v) = save("lastTimer", v)

    /** 0 = normal, 1..3 = progressively darker. */
    var dimLevel: Int
        get() = sp.getInt("dimLevel", 1)
        set(v) = sp.edit().putInt("dimLevel", v).apply()

    fun signOut() {
        sp.edit().remove("token").remove("userId").remove("userName").apply()
    }

    private fun <T> parseObjects(raw: String, map: (JSONObject) -> T): List<T> = try {
        val arr = JSONArray(raw)
        (0 until arr.length()).map { map(arr.getJSONObject(it)) }
    } catch (e: Exception) {
        emptyList()
    }

    private fun parseStrings(raw: String): List<String> = try {
        val arr = JSONArray(raw)
        (0 until arr.length()).map { arr.getString(it) }
    } catch (e: Exception) {
        emptyList()
    }
}
