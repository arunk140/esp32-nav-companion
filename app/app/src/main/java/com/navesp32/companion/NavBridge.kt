package com.navesp32.companion

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Encodes navigation state for firmware_v2/main/ble_service.h (NAV_STATE).
 *
 * STATE is fixed 12 bytes (type + 11 body) and goes out on every update (~1 Hz):
 * distances shrink continuously, so the device needs them ticking even when
 * nothing structural changed. TEXT is sent only when the street name changes,
 * and is coalesced by tag so a burst of names cannot build a backlog.
 *
 * Firmware drops anything non-Latin (same rule as media titles), so we send
 * UTF-8 and let the device clamp.
 */
class NavBridge(private val out: Out) {

    interface Out {
        fun navWrite(data: ByteArray, tag: String?)
        fun navLinkReady(): Boolean
    }

    companion object {
        private const val TAG = "NavBridge"
        private const val STATE: Byte = 0x01
        private const val TEXT: Byte = 0x02
        // 1 type byte + 11 body bytes. Must match NAV_STATE_LEN in
        // firmware_v2/main/ble_service.h.
        private const val STATE_LEN = 12
        private const val F_ACTIVE = 0x01
        private const val F_NO_ACCESS = 0x02
        private const val TEXT_MAX = 48
    }

    private var lastStreet: String? = null
    private var sentState = false

    // Sent last, so a stale value never lingers in firmware's sticky field.
    private fun clamp16(v: Int) = v.coerceIn(0, 0xFFFF).toShort()

    fun send(s: OsmAndLink.NavState, noAccess: Boolean = false) {
        if (!out.navLinkReady()) return
        val flags = (if (s.active) F_ACTIVE else 0) or (if (noAccess) F_NO_ACCESS else 0)
        val b = ByteBuffer.allocate(STATE_LEN).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(STATE)
            put(flags.toByte())
            put(s.maneuver.toByte())
            putShort(clamp16(s.distanceM))
            put(s.afterManeuver.toByte())
            putShort(clamp16(s.afterDistanceM))
            putShort(clamp16(s.remainingM))
            putShort(clamp16(s.remainingS))
        }
        if (!sentState) {
            sentState = true
            Log.i(TAG, "first nav state ${STATE_LEN} B: flags=$flags type=${s.maneuver} " +
                "dist=${s.distanceM} rem=${s.remainingM}m link=${out.navLinkReady()}")
        }
        out.navWrite(b.array(), "navst")

        val name = s.street.trim()
        if (name != lastStreet) {
            lastStreet = name
            // Send on every change, including to empty: an empty TEXT lets the
            // device clear a stale name from the previous route (it renders
            // "unnamed road"). Only changes go out, so no backlog can build.
            val bytes = name.take(TEXT_MAX).toByteArray(Charsets.UTF_8)
            val t = ByteBuffer.allocate(2 + bytes.size).order(ByteOrder.LITTLE_ENDIAN).apply {
                put(TEXT)
                put(bytes.size.toByte())
                put(bytes)
            }
            out.navWrite(t.array(), "navtx")
            Log.i(TAG, "street -> ${name.ifEmpty { "(none)" }}")
        }
    }

    fun sendStopped() {
        lastStreet = null
        send(
            OsmAndLink.NavState(
                active = false, maneuver = 0, distanceM = 0, street = "",
                afterManeuver = 0, afterDistanceM = 0, afterStreet = "",
                remainingM = 0, remainingS = 0
            )
        )
    }
}
