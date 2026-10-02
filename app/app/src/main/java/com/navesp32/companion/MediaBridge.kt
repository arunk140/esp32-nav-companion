package com.navesp32.companion

import android.content.Context
import android.database.ContentObserver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import android.view.KeyEvent
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

// Bridges the phone's active media session to the nav device.
// Contract (firmware_v2/main/ble_service.h, MEDIA_INFO / MEDIA_CMD):
//   STATE 13 B: 0x01, flags, volume %, u32 position ms, u32 duration ms, u16 art id
//   ART: 0x02, u16 art id, u32 offset, u32 total, data (200x200 baseline JPEG)
//   CMD in: 1 play/pause, 2 next, 3 prev, 4 vol+, 5 vol-, 6 set vol (u8 %), 7 sync
class MediaBridge(private val ctx: Context, private val out: Out) {

    interface Out {
        fun mediaWrite(data: ByteArray, tag: String?)
        fun mediaMaxWrite(): Int
        fun mediaBoost(ms: Long)
        fun mediaLinkReady(): Boolean
    }

    companion object {
        private const val TAG = "MediaBridge"
        private const val ART_PX = 200
        private const val ART_MAX = 46 * 1024
        private const val F_PLAYING = 0x01
        private const val F_SESSION = 0x02
        private const val F_NO_ACCESS = 0x04
        private const val ACCESS_RECHECK_MS = 10_000L
    }

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val msm = ctx.getSystemService(MediaSessionManager::class.java)
    private val am = ctx.getSystemService(AudioManager::class.java)

    private var running = false
    private var listening = false
    private var controller: MediaController? = null
    private var artId = 0
    private var artJpeg: ByteArray? = null
    private var artGen = 0

    // ---- lifecycle ----

    fun start() {
        if (running) return
        running = true
        ctx.contentResolver.registerContentObserver(Settings.System.CONTENT_URI, true, volObserver)
        attach()
    }

    fun stop() {
        if (!running) return
        running = false
        main.removeCallbacks(recheck)
        ctx.contentResolver.unregisterContentObserver(volObserver)
        if (listening) {
            msm.removeOnActiveSessionsChangedListener(sessionsListener)
            listening = false
        }
        setController(null)
    }

    private fun attach() {
        if (!running || listening) return
        if (!MediaListenerService.hasAccess(ctx)) {
            main.removeCallbacks(recheck)
            main.postDelayed(recheck, ACCESS_RECHECK_MS)
            return
        }
        try {
            val comp = MediaListenerService.component(ctx)
            msm.addOnActiveSessionsChangedListener(sessionsListener, comp, main)
            listening = true
            pick(msm.getActiveSessions(comp))
        } catch (e: SecurityException) {
            Log.w(TAG, "no media session access: $e")
            main.postDelayed(recheck, ACCESS_RECHECK_MS)
        }
    }

    private val recheck = Runnable {
        attach()
        sendState()
    }

    // ---- sessions ----

    private val sessionsListener =
        MediaSessionManager.OnActiveSessionsChangedListener { list -> pick(list ?: emptyList()) }

    private fun pick(list: List<MediaController>) {
        val playing = list.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
        val next = playing ?: list.firstOrNull()
        if (next?.sessionToken != controller?.sessionToken) setController(next)
        sendState()
    }

    private fun setController(c: MediaController?) {
        controller?.unregisterCallback(ctrlCb)
        controller = c
        c?.registerCallback(ctrlCb, main)
        updateArt(c?.metadata)
    }

    private val ctrlCb = object : MediaController.Callback() {
        override fun onPlaybackStateChanged(state: PlaybackState?) = sendState()

        override fun onMetadataChanged(metadata: MediaMetadata?) {
            updateArt(metadata)
            sendState()
        }

        override fun onSessionDestroyed() {
            setController(null)
            if (listening) pick(msm.getActiveSessions(MediaListenerService.component(ctx)))
        }
    }

    private val volObserver = object : ContentObserver(main) {
        override fun onChange(selfChange: Boolean) = sendState()
    }

    // ---- outgoing ----

    private var lastState: ByteArray? = null

    fun sendState(force: Boolean = false) {
        if (!out.mediaLinkReady()) return
        val c = controller
        val st = c?.playbackState
        val isPlaying = st?.state == PlaybackState.STATE_PLAYING
        var flags = 0
        if (isPlaying) flags = flags or F_PLAYING
        if (c != null) flags = flags or F_SESSION
        if (!listening) flags = flags or F_NO_ACCESS
        val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val vol = am.getStreamVolume(AudioManager.STREAM_MUSIC) * 100 / max
        var pos = st?.position ?: 0L
        if (isPlaying && st != null && st.lastPositionUpdateTime > 0) {
            pos += ((SystemClock.elapsedRealtime() - st.lastPositionUpdateTime) * st.playbackSpeed).toLong()
        }
        val dur = c?.metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val p = ByteBuffer.allocate(13).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(0x01)
            put(flags.toByte())
            put(vol.coerceIn(0, 100).toByte())
            putInt(pos.coerceIn(0, 0xFFFFFFFFL).toInt())
            putInt(dur.coerceIn(0, 0xFFFFFFFFL).toInt())
            putShort(artId.toShort())
        }.array()
        // Position always moves while playing, so compare without it.
        val sig = p.copyOf().also { for (i in 3..6) it[i] = 0 }
        if (!force && !isPlaying && lastState?.contentEquals(sig) == true) return
        lastState = sig
        out.mediaWrite(p, "mstate")
    }

    private fun sendArt() {
        val jpeg = artJpeg ?: return
        if (!out.mediaLinkReady() || artId == 0) return
        val chunk = out.mediaMaxWrite() - 11
        if (chunk < 8) return
        out.mediaBoost(4000)
        var off = 0
        while (off < jpeg.size) {
            val n = minOf(chunk, jpeg.size - off)
            val b = ByteBuffer.allocate(11 + n).order(ByteOrder.LITTLE_ENDIAN)
            b.put(0x02)
            b.putShort(artId.toShort())
            b.putInt(off)
            b.putInt(jpeg.size)
            b.put(jpeg, off, n)
            out.mediaWrite(b.array(), null)
            off += n
        }
        Log.i(TAG, "art ${"%04x".format(artId)} sent: ${jpeg.size} B")
    }

    fun sendAll() {
        sendState(force = true)
        sendArt()
    }

    // ---- album art ----

    private var artTrack: String? = null   // track the current art belongs to
    private var artSrc: String? = null     // art source last tried for it

    private fun updateArt(md: MediaMetadata?) {
        if (md == null) {
            artTrack = null
            artSrc = null
            artGen++
            artId = 0
            artJpeg = null
            return
        }
        val title = md.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = md.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val album = md.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""
        val bmp = md.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: md.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: md.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
        val uri = md.getString(MediaMetadata.METADATA_KEY_ALBUM_ART_URI)
            ?: md.getString(MediaMetadata.METADATA_KEY_ART_URI)
            ?: md.getString(MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI)
        val track = "$title|$artist|$album"
        val src = when {
            bmp != null -> "bmp:${bmp.generationId}:${bmp.width}x${bmp.height}"
            uri != null -> "uri:$uri"
            else -> null
        }
        if (track != artTrack) {
            // New track: drop the old art right away (the device shows a
            // placeholder rather than the previous song's cover).
            artTrack = track
            artSrc = null
            artGen++
            artId = 0
            artJpeg = null
        } else if (artId != 0 || src == null || src == artSrc) {
            // Already have art for this track (players re-send metadata,
            // often with a re-scaled bitmap), or nothing new to try.
            return
        }
        if (src == null) return
        // Players commonly publish the title + an https URL first and the
        // downloaded bitmap a moment later: each new source gets a try until
        // one of them produces art.
        artSrc = src
        val gen = ++artGen
        val id = (track.hashCode() and 0xFFFF).let { if (it == 0) 1 else it }
        worker.execute {
            val img = bmp ?: loadUri(uri)
            val jpeg = img?.let { encode(it) }
            main.post {
                if (gen != artGen) return@post  // superseded by a newer track/source
                if (jpeg == null) {
                    Log.i(TAG, "no art from $src")
                    return@post
                }
                artId = id
                artJpeg = jpeg
                sendState(force = true)
                sendArt()
            }
        }
    }

    private fun loadUri(s: String?): Bitmap? {
        if (s == null) return null
        val uri = Uri.parse(s)
        return try {
            when (uri.scheme) {
                "content", "android.resource", "file" ->
                    ctx.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
                "http", "https" -> {
                    val c = java.net.URL(s).openConnection() as java.net.HttpURLConnection
                    c.connectTimeout = 4000
                    c.readTimeout = 4000
                    try {
                        c.inputStream.use { BitmapFactory.decodeStream(it) }
                    } finally {
                        c.disconnect()
                    }
                }
                else -> null
            }
        } catch (e: Exception) {
            Log.w(TAG, "art uri $s: $e")
            null
        }
    }

    // Centre-crop to a square, scale to 200x200, baseline JPEG under ART_MAX.
    private fun encode(src0: Bitmap): ByteArray? {
        return try {
            val src = if (src0.config == Bitmap.Config.HARDWARE) {
                src0.copy(Bitmap.Config.ARGB_8888, false)
            } else {
                src0
            }
            val side = minOf(src.width, src.height)
            val sq = Bitmap.createBitmap(src, (src.width - side) / 2, (src.height - side) / 2, side, side)
            val scaled = Bitmap.createScaledBitmap(sq, ART_PX, ART_PX, true)
            var q = 85
            var bytes: ByteArray
            do {
                val bos = ByteArrayOutputStream()
                scaled.compress(Bitmap.CompressFormat.JPEG, q, bos)
                bytes = bos.toByteArray()
                q -= 15
            } while (bytes.size > ART_MAX && q >= 40)
            bytes
        } catch (e: Exception) {
            Log.w(TAG, "art encode: $e")
            null
        }
    }

    // ---- incoming commands ----

    fun handle(cmd: ByteArray) {
        if (cmd.isEmpty()) return
        val c = controller
        when (cmd[0].toInt()) {
            1 -> if (c != null) {
                if (c.playbackState?.state == PlaybackState.STATE_PLAYING) {
                    c.transportControls.pause()
                } else {
                    c.transportControls.play()
                }
            } else {
                mediaKey(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
            }
            2 -> c?.transportControls?.skipToNext() ?: mediaKey(KeyEvent.KEYCODE_MEDIA_NEXT)
            3 -> c?.transportControls?.skipToPrevious() ?: mediaKey(KeyEvent.KEYCODE_MEDIA_PREVIOUS)
            4 -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0)
            5 -> am.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0)
            6 -> if (cmd.size >= 2) {
                val pct = (cmd[1].toInt() and 0xFF).coerceIn(0, 100)
                val max = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                am.setStreamVolume(AudioManager.STREAM_MUSIC, (pct * max + 50) / 100, 0)
            }
            7 -> sendAll()
        }
        // Echo the result (volume steps, play state) without waiting for callbacks.
        if (cmd[0].toInt() in 4..6) main.postDelayed({ sendState(force = true) }, 150)
    }

    private fun mediaKey(code: Int) {
        am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
        am.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
    }
}
