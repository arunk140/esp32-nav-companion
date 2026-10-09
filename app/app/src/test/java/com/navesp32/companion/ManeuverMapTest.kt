package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Test

class ManeuverMapTest {
    @Test
    fun mapsValhallaTypes() {
        assertEquals(Nav.NONE, ManeuverMap.nav(0))
        assertEquals(Nav.CONTINUE, ManeuverMap.nav(1))   // kStart
        assertEquals(Nav.ARRIVE, ManeuverMap.nav(4))     // kDestination
        assertEquals(Nav.SLIGHT_RIGHT, ManeuverMap.nav(9))
        assertEquals(Nav.RIGHT, ManeuverMap.nav(10))
        assertEquals(Nav.LEFT, ManeuverMap.nav(15))
        assertEquals(Nav.SLIGHT_LEFT, ManeuverMap.nav(16))
        assertEquals(Nav.SHARP_LEFT, ManeuverMap.nav(14))
        assertEquals(Nav.KEEP_RIGHT, ManeuverMap.nav(23))
        assertEquals(Nav.KEEP_LEFT, ManeuverMap.nav(24))
        assertEquals(Nav.ROUNDABOUT, ManeuverMap.nav(27))
        assertEquals(Nav.NONE, ManeuverMap.nav(99))
    }
}
