package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PolylineTest {

    @Test
    fun decodesPolyline6() {
        // Hand-encoded [(0,0), (0.000001,0.000001)] at precision 6.
        val pts = Polyline.decode("??AA", 6)
        assertEquals(2, pts.size)
        assertEquals(0.0, pts[0].lat, 1e-9)
        assertEquals(0.0, pts[0].lon, 1e-9)
        assertEquals(1e-6, pts[1].lat, 1e-12)
        assertEquals(1e-6, pts[1].lon, 1e-12)
    }

    @Test
    fun emptyInputYieldsNoPoints() {
        assertTrue(Polyline.decode("", 6).isEmpty())
    }
}
