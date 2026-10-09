package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SimplifyTest {

    @Test
    fun removesCollinearPoints() {
        val pts = (0..10).map { GeoPoint(0.0, it * 0.0001) }
        val out = Simplify.rdp(pts, 1.0)
        assertEquals(2, out.size)
        assertEquals(pts.first(), out.first())
        assertEquals(pts.last(), out.last())
    }

    @Test
    fun keepsRealDeviation() {
        // Middle point is ~11 m off the chord; > 5 m so it must survive.
        val pts = listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0001, 0.0005), GeoPoint(0.0, 0.001))
        assertEquals(3, Simplify.rdp(pts, 5.0).size)
        // With a tolerance above the deviation it collapses to the endpoints.
        assertEquals(2, Simplify.rdp(pts, 20.0).size)
    }

    @Test
    fun capsToBudget() {
        val pts = (0..999).map { GeoPoint(0.001 * Math.sin(it * 0.3), it * 0.00001) }
        val out = Simplify.forDisplay(pts, 50)
        assertTrue("size=${out.size}", out.size in 2..50)
        assertEquals(pts.first(), out.first())
        assertEquals(pts.last(), out.last())
    }

    @Test
    fun underBudgetIsUnchanged() {
        val pts = (0..9).map { GeoPoint(0.0, it * 0.001) }
        assertEquals(pts, Simplify.forDisplay(pts, 512))
    }
}
