package com.navesp32.companion

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Encodes navigation state for firmware_v2/main/ble_service.h (NAV_STATE).
 *
 * Source-agnostic: both OsmAndLink and ValhallaSource produce [NavState] and
 * hand it here. STATE is fixed 12 bytes (type + 11 body) and goes out on every
 * update (~1 Hz): distances shrink continuously, so the device needs them
 * ticking even when nothing structural changed. TEXT is sent only when the
 * street name changes, and is coalesced by tag so a burst of names cannot build
 * a backlog.
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
        private const val DEST: Byte = 0x03
        // 1 type byte + 11 body bytes. Must match NAV_STATE_LEN in
        // firmware_v2/main/ble_service.h.
        private const val STATE_LEN = 12
        // 1 type + 1 present + i32 lat_e7 + i32 lon_e7. Match NAV_DEST_LEN.
        private const val DEST_LEN = 10
        private const val F_ACTIVE = 0x01
        private const val F_NO_ACCESS = 0x02
        private const val TEXT_MAX = 48
        // Route geometry. Match firmware_v2/main/ble_service.h.
        private const val ROUTE: Byte = 0x04
        private const val ROUTE_HDR = 4
        private const val ROUTE_CHUNK = 31          // NAV_ROUTE_CHUNK_MAX
        private const val ROUTE_F_START = 0x01
        private const val ROUTE_F_END = 0x02
        private const val ROUTE_MAX_PTS = 512       // map_view.c ROUTE_MAX
    }

    private var lastStreet: String? = null
    private var sentState = false
    private var lastDestValid = false
    private var lastDestLat = 0
    private var lastDestLon = 0

    // Sent last, so a stale value never lingers in firmware's sticky field.
    private fun clamp16(v: Int) = v.coerceIn(0, 0xFFFF).toShort()

    fun send(s: NavState, noAccess: Boolean = false) {
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

        // Destination: only on change. lat/lon quantised to 1e7 as on the wire.
        val latE7 = Math.round(s.destLat * 1e7).toInt()
        val lonE7 = Math.round(s.destLon * 1e7).toInt()
        if (s.hasDest != lastDestValid || (s.hasDest &&
                (latE7 != lastDestLat || lonE7 != lastDestLon))) {
            lastDestValid = s.hasDest
            lastDestLat = latE7
            lastDestLon = lonE7
            val d = ByteBuffer.allocate(DEST_LEN).order(ByteOrder.LITTLE_ENDIAN).apply {
                put(DEST)
                put(if (s.hasDest) 1 else 0)
                putInt(if (s.hasDest) latE7 else 0)
                putInt(if (s.hasDest) lonE7 else 0)
            }
            out.navWrite(d.array(), "navdest")
            Log.i(TAG, "dest -> ${if (s.hasDest) "$latE7,$lonE7" else "(none)"}")
        }
    }

    fun sendStopped() {
        lastStreet = null
        send(
            NavState(
                active = false, maneuver = 0, distanceM = 0, street = "",
                afterManeuver = 0, afterDistanceM = 0, afterStreet = "",
                remainingM = 0, remainingS = 0,
                destLat = 0.0, destLon = 0.0, hasDest = false
            )
        )
    }

    /**
     * Sends the route polyline as ordered chunks. tag=null so the BleManager
     * queue does not coalesce them (each chunk is distinct). The firmware
     * assembles reset -> add* -> commit and draws the line.
     */
    fun sendRoute(points: List<GeoPoint>) {
        if (!out.navLinkReady()) return
        val pts = Simplify.forDisplay(points, ROUTE_MAX_PTS)
        if (pts.size < 2) {
            clearRoute()
            return
        }
        var seq = 0
        var i = 0
        while (i < pts.size) {
            val n = minOf(ROUTE_CHUNK, pts.size - i)
            val last = i + n >= pts.size
            val b = ByteBuffer.allocate(ROUTE_HDR + n * 8).order(ByteOrder.LITTLE_ENDIAN)
            b.put(ROUTE)
            var flags = 0
            if (i == 0) flags = flags or ROUTE_F_START
            if (last) flags = flags or ROUTE_F_END
            b.put(flags.toByte())
            b.put((seq++ and 0xFF).toByte())
            b.put(n.toByte())
            for (k in 0 until n) {
                val p = pts[i + k]
                b.putInt(Math.round(p.lat * 1e7).toInt())
                b.putInt(Math.round(p.lon * 1e7).toInt())
            }
            out.navWrite(b.array(), null)
            i += n
        }
        Log.i(TAG, "route sent: ${pts.size} pts, ${seq} chunks")
    }

    /** Empty START|END chunk: the firmware clears the drawn route. */
    fun clearRoute() {
        if (!out.navLinkReady()) return
        val b = ByteBuffer.allocate(ROUTE_HDR).order(ByteOrder.LITTLE_ENDIAN)
        b.put(ROUTE)
        b.put((ROUTE_F_START or ROUTE_F_END).toByte())
        b.put(0)
        b.put(0)
        out.navWrite(b.array(), null)
    }

    // Link went down: forget the sticky street so a reconnect cannot reuse a
    // name from the previous session. No write - there is no link to write to.
    fun reset() {
        lastStreet = null
        sentState = false
        lastDestValid = false
        lastDestLat = 0
        lastDestLon = 0
    }
}
