package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class NavBridgeRouteTest {

    private class FakeOut : NavBridge.Out {
        val writes = mutableListOf<ByteArray>()
        val tags = mutableListOf<String?>()
        override fun navWrite(data: ByteArray, tag: String?) {
            writes.add(data)
            tags.add(tag)
        }
        override fun navLinkReady() = true
    }

    @Test
    fun chunksWithStartAndEndFlags() {
        val out = FakeOut()
        val bridge = NavBridge(out)
        val pts = (0..69).map { GeoPoint(18.9 + it * 1e-5, 72.8 + it * 1e-5) }
        bridge.sendRoute(pts)

        // 70 points / 31 per chunk = 3 chunks (31, 31, 8).
        assertEquals(3, out.writes.size)
        out.tags.forEach { assertNull(it) }  // never coalesced
        for ((i, w) in out.writes.withIndex()) {
            val bb = ByteBuffer.wrap(w).order(ByteOrder.LITTLE_ENDIAN)
            assertEquals(0x04, bb.get().toInt() and 0xFF)
            val flags = bb.get().toInt() and 0xFF
            assertEquals(i == 0, flags and 0x01 != 0)
            assertEquals(i == out.writes.size - 1, flags and 0x02 != 0)
            assertEquals(i, bb.get().toInt() and 0xFF)
        }
        assertEquals(31, out.writes[0][3].toInt() and 0xFF)
        assertEquals(8, out.writes[2][3].toInt() and 0xFF)
    }

    @Test
    fun clearRouteSendsEmptyStartEnd() {
        val out = FakeOut()
        NavBridge(out).clearRoute()
        assertEquals(1, out.writes.size)
        val w = out.writes[0]
        assertEquals(4, w.size)
        assertEquals(0x04, w[0].toInt() and 0xFF)
        assertEquals(0x03, w[1].toInt() and 0xFF)
        assertEquals(0, w[3].toInt() and 0xFF)
    }

    @Test
    fun tooFewPointsClearsInstead() {
        val out = FakeOut()
        NavBridge(out).sendRoute(listOf(GeoPoint(1.0, 2.0)))
        // The single-point path routes to clearRoute(): one 4-byte frame.
        assertEquals(1, out.writes.size)
        assertEquals(4, out.writes[0].size)
    }

    @Test
    fun notReadyWritesNothing() {
        val out = object : NavBridge.Out {
            var n = 0
            override fun navWrite(data: ByteArray, tag: String?) { n++ }
            override fun navLinkReady() = false
        }
        NavBridge(out).sendRoute((0..40).map { GeoPoint(0.0, it * 1e-4) })
        assertTrue(out.n == 0)
    }
}
