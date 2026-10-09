package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GeoTest {

    @Test
    fun projectsOntoLine() {
        // Equator, 0.001 degrees of longitude apart (~111 m).
        val line = listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.001))
        val p = GeoPoint(0.0001, 0.0005)  // 0.0001 deg north = ~11.13 m off the line
        val proj = Geo.project(line, p)!!
        assertEquals(0, proj.segIndex)
        assertEquals(0.5, proj.t, 0.02)
        assertEquals(11.13, proj.perpM, 0.5)
    }

    @Test
    fun projectsToNearestOfManySegments() {
        val line = listOf(
            GeoPoint(0.0, 0.0),
            GeoPoint(0.0, 0.001),
            GeoPoint(0.0, 0.002),
        )
        val proj = Geo.project(line, GeoPoint(0.0, 0.0015))!!
        assertEquals(1, proj.segIndex)
        assertEquals(0.5, proj.t, 0.02)
        assertTrue(proj.perpM < 0.5)
    }
}
