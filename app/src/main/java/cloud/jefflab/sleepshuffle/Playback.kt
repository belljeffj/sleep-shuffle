package cloud.jefflab.sleepshuffle

import android.content.ComponentName
import android.content.Context
import android.net.Uri
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.Tracks
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID

enum class Phase { Idle, Connecting, Playing, Ended, Error }

data class NowState(
    val phase: Phase = Phase.Idle,
    val selectionLabel: String = "",
    val episode: Episode? = null,
    val message: String? = null,
    val timer: TimerSpec = TimerSpec.Off,
    /** Wall-clock stop time for minute and clock timers. */
    val stopAtWallMs: Long? = null,
    /** For episode timers: how many episodes remain, counting the current one. */
    val episodesLeft: Int = 0,
    /** 0 = not fading, 1 = fully faded. Drives volume and screen darkness. */
    val fade: Float = 0f,
    val segments: List<Segment> = emptyList(),
)

/**
 * The app's brain. Owns the shuffle queue and the sleep timer, and drives the
 * ExoPlayer that lives inside [PlaybackService] (same process).
 */
object Playback {
    const val ALL = "ALL"
    const val FADE_MS = 45_000L
    private const val EXTEND_MINUTES = 15

    lateinit var prefs: Prefs
        private set
    lateinit var client: JellyfinClient
        private set
    private lateinit var appContext: Context
    private var initialized = false

    private val scope = MainScope()
    private val _state = MutableStateFlow(NowState())
    val state: StateFlow<NowState> = _state

    private val _player = MutableStateFlow<ExoPlayer?>(null)
    val player: StateFlow<ExoPlayer?> = _player
    private var playerReady = CompletableDeferred<ExoPlayer>()
    private var controllerFuture: ListenableFuture<MediaController>? = null

    private var selection: List<Show> = emptyList()
    private val pools = mutableMapOf<String, List<Episode>>()
    private val episodesById = mutableMapOf<String, Episode>()
    private val hlsItems = mutableSetOf<String>()
    private val playSessions = mutableMapOf<String, String>()
    private var retries = 0
    private var startJob: Job? = null
    private var tickerJob: Job? = null

    fun init(context: Context) {
        if (initialized) return
        appContext = context.applicationContext
        prefs = Prefs(appContext)
        client = JellyfinClient(prefs)
        initialized = true
    }

    // ---- service wiring -------------------------------------------------------------

    fun attach(p: ExoPlayer) {
        p.addListener(listener)
        _player.value = p
        if (!playerReady.isCompleted) playerReady.complete(p)
    }

    fun detach(p: ExoPlayer) {
        p.removeListener(listener)
        if (_player.value === p) _player.value = null
        playerReady = CompletableDeferred()
        tickerJob?.cancel()
        if (_state.value.phase == Phase.Playing || _state.value.phase == Phase.Connecting) {
            _state.value = NowState()
        }
    }

    private fun ensureService() {
        if (controllerFuture != null) return
        val token = SessionToken(appContext, ComponentName(appContext, PlaybackService::class.java))
        controllerFuture = MediaController.Builder(appContext, token).buildAsync()
    }

    private fun releaseController() {
        controllerFuture?.let { MediaController.releaseFuture(it) }
        controllerFuture = null
    }

    // ---- starting and stopping ------------------------------------------------------

    fun labelFor(key: String?): String {
        val shows = prefs.shows
        return when {
            key == null -> ""
            key == ALL -> mixLabel(shows)
            else -> shows.firstOrNull { it.id == key }?.name ?: ""
        }
    }

    /** [rememberTimer] = false for one-off timers like "keep watching 15 min". */
    fun start(selectionKey: String, timer: TimerSpec, rememberTimer: Boolean = true) {
        startJob?.cancel()
        tickerJob?.cancel()
        val shows = prefs.shows
        selection = if (selectionKey == ALL) shows else shows.filter { it.id == selectionKey }
        if (selection.isEmpty()) {
            _state.value = NowState(phase = Phase.Error, message = "Pick some shows in Settings first.")
            return
        }
        prefs.lastSelection = selectionKey
        if (rememberTimer) prefs.lastTimer = timer.encode()
        _state.value = NowState(phase = Phase.Connecting, selectionLabel = labelFor(selectionKey))
        ensureService()

        startJob = scope.launch {
            try {
                client.resolveBase(force = true)
                for (show in selection) {
                    val eps = client.episodes(show.id, show.name)
                    pools[show.id] = eps
                    eps.forEach { episodesById[it.id] = it }
                }
                if (selection.all { pools[it.id].isNullOrEmpty() }) {
                    throw JellyfinException("Jellyfin has no episodes for ${labelFor(selectionKey)}.")
                }
                val p = withTimeoutOrNull(10_000L) { playerReady.await() }
                    ?: throw JellyfinException("The player didn't start. Try again.")
                val first = pickNext(emptySet()) ?: throw JellyfinException("Nothing left to shuffle.")
                val second = pickNext(setOf(first.id))
                hlsItems.clear()
                retries = 0
                p.volume = 1f
                p.setMediaItems(listOfNotNull(first, second).map { buildItem(it) }, 0, 0L)
                p.prepare()
                p.play()
                _state.update { it.copy(phase = Phase.Playing, message = null) }
                onBecameCurrent(first)
                applyTimer(timer, remember = false)
                startTicker()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _state.update { it.copy(phase = Phase.Error, message = e.message ?: "Something went wrong.") }
            }
        }
    }

    fun retryLast() {
        start(prefs.lastSelection ?: ALL, TimerSpec.decode(prefs.lastTimer), rememberTimer = false)
    }

    /** After the timer stopped playback: carry on for 15 minutes without changing the usual timer. */
    fun keepWatching() {
        start(prefs.lastSelection ?: ALL, TimerSpec.Minutes(EXTEND_MINUTES), rememberTimer = false)
    }

    /** User left the player. */
    fun stop() {
        startJob?.cancel()
        halt()
        _state.value = NowState()
    }

    /** Sleep timer ran out. */
    private fun finish() {
        halt()
        _state.update { it.copy(phase = Phase.Ended, fade = 1f) }
    }

    private fun halt() {
        tickerJob?.cancel()
        _player.value?.let { p ->
            p.pause()
            p.stop()
            p.clearMediaItems()
            p.volume = 1f
        }
        val sessions = playSessions.values.toList()
        playSessions.clear()
        hlsItems.clear()
        scope.launch { sessions.forEach { client.stopEncoding(it) } }
        releaseController()
    }

    // ---- shuffle queue --------------------------------------------------------------

    private fun queuedIds(): Set<String> {
        val p = _player.value ?: return emptySet()
        return (0 until p.mediaItemCount).mapTo(HashSet()) { p.getMediaItemAt(it).mediaId }
    }

    private fun pickNext(extraExclude: Set<String>): Episode? {
        val banned = prefs.banned.mapTo(HashSet()) { it.id }
        val candidates = selection.filter { !pools[it.id].isNullOrEmpty() }
        if (candidates.isEmpty()) return null
        // "Mix" mode: choose the show first, evenly, then an episode from it.
        val show = candidates.random()
        return Shuffler.pick(
            pool = pools[show.id].orEmpty(),
            banned = banned,
            history = prefs.history,
            window = prefs.recentWindow,
            exclude = queuedIds() + extraExclude,
        )
    }

    private fun buildItem(ep: Episode): MediaItem {
        val hls = ep.id in hlsItems
        val uri = if (hls) {
            val session = playSessions.getOrPut(ep.id) { UUID.randomUUID().toString().replace("-", "") }
            client.hlsUrl(ep, session)
        } else {
            client.directUrl(ep)
        }
        val meta = MediaMetadata.Builder()
            .setTitle(ep.name)
            .setDisplayTitle(ep.name)
            .setArtist("${ep.seriesName} · ${ep.code}")
            .setArtworkUri(client.artworkUrl(ep.seriesId)?.let { Uri.parse(it) })
            .build()
        val builder = MediaItem.Builder()
            .setMediaId(ep.id)
            .setUri(uri)
            .setMediaMetadata(meta)
        if (hls) builder.setMimeType(MimeTypes.APPLICATION_M3U8)
        return builder.build()
    }

    private fun ensureNextQueued() {
        val p = _player.value ?: return
        if (p.mediaItemCount - p.currentMediaItemIndex >= 2) return
        val next = pickNext(emptySet()) ?: return
        p.addMediaItem(buildItem(next))
    }

    private fun onBecameCurrent(ep: Episode) {
        if (_state.value.episode?.id == ep.id) return
        prefs.addHistory(ep.id)
        _state.update { it.copy(episode = ep, segments = emptyList()) }
        scope.launch {
            val segs = try {
                client.segments(ep.id)
            } catch (e: Exception) {
                emptyList()
            }
            if (_state.value.episode?.id == ep.id) _state.update { it.copy(segments = segs) }
        }
    }

    private val listener = object : Player.Listener {
        override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
            val p = _player.value ?: return
            val ep = mediaItem?.let { episodesById[it.mediaId] } ?: return
            if (reason == Player.MEDIA_ITEM_TRANSITION_REASON_AUTO) {
                val s = _state.value
                if (s.timer is TimerSpec.Episodes) {
                    if (s.episodesLeft <= 1) {
                        finish()
                        return
                    }
                    _state.update { it.copy(episodesLeft = it.episodesLeft - 1) }
                }
            }
            // Drop what's already been played, and stop any transcode it used.
            val idx = p.currentMediaItemIndex
            if (idx > 0) {
                for (i in 0 until idx) {
                    playSessions.remove(p.getMediaItemAt(i).mediaId)?.let { s ->
                        scope.launch { client.stopEncoding(s) }
                    }
                }
                p.removeMediaItems(0, idx)
            }
            retries = 0
            onBecameCurrent(ep)
            ensureNextQueued()
        }

        override fun onTracksChanged(tracks: Tracks) {
            val p = _player.value ?: return
            val id = p.currentMediaItem?.mediaId ?: return
            if (id in hlsItems) return
            val badAudio = tracks.containsType(C.TRACK_TYPE_AUDIO) && !tracks.isTypeSupported(C.TRACK_TYPE_AUDIO)
            val badVideo = tracks.containsType(C.TRACK_TYPE_VIDEO) && !tracks.isTypeSupported(C.TRACK_TYPE_VIDEO)
            if (badAudio || badVideo) reloadCurrent(useHls = true)
        }

        override fun onPlayerError(error: PlaybackException) {
            handleError(error)
        }
    }

    private val formatErrors = setOf(
        PlaybackException.ERROR_CODE_DECODER_INIT_FAILED,
        PlaybackException.ERROR_CODE_DECODER_QUERY_FAILED,
        PlaybackException.ERROR_CODE_DECODING_FAILED,
        PlaybackException.ERROR_CODE_DECODING_FORMAT_EXCEEDS_CAPABILITIES,
        PlaybackException.ERROR_CODE_DECODING_FORMAT_UNSUPPORTED,
        PlaybackException.ERROR_CODE_PARSING_CONTAINER_UNSUPPORTED,
        PlaybackException.ERROR_CODE_PARSING_CONTAINER_MALFORMED,
        PlaybackException.ERROR_CODE_AUDIO_TRACK_INIT_FAILED,
    )

    private fun handleError(error: PlaybackException) {
        if (_state.value.phase != Phase.Playing) return
        retries++
        if (retries > 3) {
            halt()
            _state.update {
                it.copy(phase = Phase.Error, message = "Playback keeps failing (${error.errorCodeName}).")
            }
            return
        }
        if (error.errorCode in formatErrors) {
            reloadCurrent(useHls = true)
        } else {
            // Probably the network: maybe we left home Wi-Fi. Re-pick LAN vs Tailscale and resume.
            scope.launch {
                try {
                    client.resolveBase(force = true)
                } catch (e: Exception) {
                    _state.update { it.copy(message = e.message) }
                    delay(4000)
                }
                reloadCurrent(useHls = false)
            }
        }
    }

    private fun reloadCurrent(useHls: Boolean) {
        val p = _player.value ?: return
        val currentId = p.currentMediaItem?.mediaId ?: return
        if (useHls) hlsItems.add(currentId)
        val idx = p.currentMediaItemIndex
        val pos = p.currentPosition
        val rebuilt = (0 until p.mediaItemCount).mapNotNull { i ->
            episodesById[p.getMediaItemAt(i).mediaId]?.let { buildItem(it) }
        }
        if (rebuilt.isEmpty()) return
        p.setMediaItems(rebuilt, idx.coerceAtMost(rebuilt.size - 1), pos)
        p.prepare()
        p.play()
    }

    // ---- controls -------------------------------------------------------------------

    fun togglePlay() {
        val p = _player.value ?: return
        if (p.isPlaying) p.pause() else p.play()
    }

    fun seekBy(deltaMs: Long) {
        val p = _player.value ?: return
        val dur = p.duration.takeIf { it != C.TIME_UNSET } ?: Long.MAX_VALUE
        p.seekTo((p.currentPosition + deltaMs).coerceIn(0L, dur))
    }

    fun skipTo(ms: Long) {
        _player.value?.seekTo(ms)
    }

    fun nextRandom() {
        val p = _player.value ?: return
        ensureNextQueued()
        if (p.hasNextMediaItem()) p.seekToNextMediaItem()
    }

    fun banCurrent() {
        val ep = _state.value.episode ?: return
        prefs.ban(ep.id, ep.label)
        nextRandom()
    }

    // ---- sleep timer ----------------------------------------------------------------

    fun applyTimer(spec: TimerSpec, remember: Boolean = true) {
        if (remember) prefs.lastTimer = spec.encode()
        val now = System.currentTimeMillis()
        val stopAt = when (spec) {
            is TimerSpec.Minutes -> now + spec.minutes * 60_000L
            is TimerSpec.Clock -> nextClockMs(spec.hour, spec.minute, now)
            else -> null
        }
        val left = (spec as? TimerSpec.Episodes)?.count ?: 0
        _player.value?.volume = 1f
        _state.update { it.copy(timer = spec, stopAtWallMs = stopAt, episodesLeft = left, fade = 0f) }
    }

    /** The big "+15 min" button during the fade. */
    fun extend() {
        applyTimer(TimerSpec.Minutes(EXTEND_MINUTES), remember = false)
    }

    /** Milliseconds until the timer stops playback, or null if no stop is pending. */
    fun remainingMs(s: NowState = _state.value): Long? {
        val p = _player.value ?: return null
        return when (s.timer) {
            is TimerSpec.Minutes, is TimerSpec.Clock -> s.stopAtWallMs?.let { it - System.currentTimeMillis() }
            is TimerSpec.Episodes -> {
                val dur = p.duration
                if (s.episodesLeft <= 1 && dur != C.TIME_UNSET && dur > 0) dur - p.currentPosition else null
            }
            TimerSpec.Off -> null
        }
    }

    private fun startTicker() {
        tickerJob?.cancel()
        tickerJob = scope.launch {
            while (isActive) {
                tick()
                delay(250)
            }
        }
    }

    private fun tick() {
        val p = _player.value ?: return
        val s = _state.value
        if (s.phase != Phase.Playing) return
        val remaining = remainingMs(s)
        if (remaining == null) {
            if (s.fade != 0f) {
                p.volume = 1f
                _state.update { it.copy(fade = 0f) }
            }
            return
        }
        if (remaining <= 0L) {
            finish()
            return
        }
        val fade = if (remaining < FADE_MS) 1f - remaining.toFloat() / FADE_MS else 0f
        val v = 1f - fade
        p.volume = v * v
        if (kotlin.math.abs(fade - s.fade) > 0.004f || (fade == 0f && s.fade != 0f)) {
            _state.update { it.copy(fade = fade) }
        }
    }
}
