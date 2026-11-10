package cloud.jefflab.sleepshuffle

import android.app.PendingIntent
import android.content.Intent
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.datasource.DefaultDataSource
import androidx.media3.datasource.DefaultHttpDataSource
import androidx.media3.datasource.ResolvingDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService

/**
 * Hosts the player so audio keeps going with the screen locked, and gives
 * lock-screen and notification controls for free via MediaSession.
 */
class PlaybackService : MediaSessionService() {

    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        Playback.init(applicationContext)

        // Every stream request carries the Jellyfin token as a header (not just ?api_key=),
        // and waits up to 30 s, because a server-side transcode can be slow to start.
        val http = DefaultHttpDataSource.Factory()
            .setUserAgent("SleepShuffle/1.0")
            .setConnectTimeoutMs(10_000)
            .setReadTimeoutMs(30_000)
            .setAllowCrossProtocolRedirects(true)
        val withAuth = ResolvingDataSource.Factory(DefaultDataSource.Factory(this, http)) { spec ->
            spec.withAdditionalHeaders(mapOf("Authorization" to Playback.client.authHeader()))
        }

        val player = ExoPlayer.Builder(this)
            .setMediaSourceFactory(DefaultMediaSourceFactory(withAuth))
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MOVIE)
                    .build(),
                /* handleAudioFocus = */ true,
            )
            .setHandleAudioBecomingNoisy(true)
            .setWakeMode(C.WAKE_MODE_NETWORK)
            .setSeekBackIncrementMs(10_000)
            .setSeekForwardIncrementMs(10_000)
            .build()

        val openApp = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        session = MediaSession.Builder(this, player)
            .setSessionActivity(openApp)
            .build()

        Playback.attach(player)
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Swiping the app away means "I'm done": stop rather than play on invisibly.
        Playback.stop()
        stopSelf()
    }

    override fun onDestroy() {
        session?.let {
            val p = it.player
            if (p is ExoPlayer) Playback.detach(p)
            p.release()
            it.release()
        }
        session = null
        super.onDestroy()
    }
}
