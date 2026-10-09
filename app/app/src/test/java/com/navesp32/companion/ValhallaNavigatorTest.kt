package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ValhallaNavigatorTest {

    // Straight ~334 m line east along the equator, with a right turn two
    // segments in and arrival at the end.
    private fun plan(): RoutePlan {
        val pts = listOf(
            GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.001),
            GeoPoint(0.0, 0.002), GeoPoint(0.0, 0.003),
        )
        val cum = DoubleArray(pts.size)
        for (i in 1 until pts.size) cum[i] = cum[i - 1] + Geo.distanceM(pts[i - 1], pts[i])
        val maneuvers = listOf(
            RouteManeuver(Nav.CONTINUE, "Depart", "A", 0, 0, 0.0, 0),
            RouteManeuver(Nav.RIGHT, "Turn right onto B", "B", 2, 2, 10.0, 0),
            RouteManeuver(Nav.ARRIVE, "Arrive", "B", 3, 3, 10.0, 0),
        )
        val anchors = listOf(
            0.0 to 0.0,
            cum[0] to 0.0,
            cum[2] to 10.0,
            cum[3] to 20.0,
        )
        return RoutePlan(pts, maneuvers, cum, anchors, cum.last(), 20.0, pts.last())
    }

    @Test
    fun reportsDistanceToNextManeuverAndRemaining() {
        val nav = ValhallaNavigator()
        nav.setRoute(plan())
        val p = nav.update(GeoPoint(0.0, 0.0005))!!   // halfway along segment 0
        assertEquals(Nav.RIGHT, p.maneuver)
        // 334 m total; ~56 m in, next turn at ~223 m.
        assertTrue("dist=${p.distM}", p.distM in 160..175)
        assertTrue("rem=${p.remainingM}", p.remainingM in 270..285)
        assertFalse(p.offRoute)
        assertFalse(p.arrived)
    }

    @Test
    fun flagsOffRouteBeyondThreshold() {
        val nav = ValhallaNavigator()
        nav.setRoute(plan())
        val p = nav.update(GeoPoint(0.001, 0.0005))!!  // ~111 m off the line
        assertTrue(p.offRoute)
        assertTrue(p.perpM > 100)
    }

    @Test
    fun detectsArrival() {
        val nav = ValhallaNavigator()
        nav.setRoute(plan())
        val p = nav.update(GeoPoint(0.0, 0.003))!!     // at the end
        assertTrue(p.arrived)
        assertEquals(Nav.ARRIVE, p.maneuver)
    }

    @Test
    fun progressDoesNotRewind() {
        val nav = ValhallaNavigator()
        nav.setRoute(plan())
        nav.update(GeoPoint(0.0, 0.002))
        val before = nav.progressM()
        nav.update(GeoPoint(0.0, 0.0005))              // fix snapped well behind
        assertTrue(nav.progressM() >= before)
    }
}
