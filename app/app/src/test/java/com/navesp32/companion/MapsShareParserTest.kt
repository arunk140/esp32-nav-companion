package com.navesp32.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MapsShareParserTest {

    @Test
    fun prefersPoiCoordinatesOverViewportCentre() {
        // The `@` is the viewport centre; `!3d/!4d` is the actual place.
        val text = "Gateway of India\n" +
            "https://www.google.com/maps/place/Gateway+of+India/" +
            "@18.9000,72.8000,17z/data=!3m1!4b1!4m5!3m4!1s0x0:0x0!8m2!3d18.9219!4d72.8347"
        val p = MapsShareParser.parseShare(text, null)!!
        assertEquals("Gateway of India", p.name)
        assertEquals(18.9219, p.lat!!, 1e-6)
        assertEquals(72.8347, p.lon!!, 1e-6)
    }

    @Test
    fun shortLinkHasNoCoordinatesButIsResolvable() {
        val p = MapsShareParser.parseShare("https://maps.app.goo.gl/abc123", null)!!
        assertNull(p.lat)
        assertTrue(MapsShareParser.isShortener(p.url!!))
    }

    @Test
    fun parsesGeoUri() {
        val p = MapsShareParser.parseGeo("geo:19.07,72.87?q=19.07,72.87(Gateway)")!!
        assertEquals(19.07, p.lat!!, 1e-6)
        assertEquals(72.87, p.lon!!, 1e-6)
        assertEquals("Gateway", p.name)
    }

    @Test
    fun parsesBareCoordinates() {
        val p = MapsShareParser.parseShare("18.9219,72.8347", null)!!
        assertEquals(18.9219, p.lat!!, 1e-6)
        assertEquals(72.8347, p.lon!!, 1e-6)
    }

    @Test
    fun resolvedQueryUrlCarriesCoordinates() {
        val p = MapsShareParser.parseUrl(
            "https://www.google.com/maps/search/?api=1&query=19.1197,72.8464",
            "Location"
        )!!
        assertEquals(19.1197, p.lat!!, 1e-6)
        assertEquals(72.8464, p.lon!!, 1e-6)
    }

    @Test
    fun nullIslandIsRejected() {
        assertNull(MapsShareParser.parseShare("0,0", null)?.takeIf { it.lat != null })
    }
}
