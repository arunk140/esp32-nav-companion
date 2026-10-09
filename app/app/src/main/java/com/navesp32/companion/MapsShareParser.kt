package com.navesp32.companion

import java.net.URLDecoder

/**
 * Turns a Google Maps (or generic maps) share into a place with coordinates.
 *
 * Pure string work so it can be unit-tested against real share strings; the
 * network follow for short links lives in [LinkResolver] and happens before
 * [parseUrl] is called on the resolved URL.
 *
 * Coordinate priority matters: for a place share the `!3d/!4d` pair is the POI,
 * while `@lat,lng` is only the viewport centre and can be a street away. So
 * `!3d/!4d` wins, `@` is the last resort.
 */
object MapsShareParser {

    data class Parsed(val name: String, val lat: Double?, val lon: Double?, val url: String?)

    private val URL_RE = Regex("""https?://[^\s]+""")
    private val D3D4D = Regex("""!3d(-?\d+(?:\.\d+)?)!4d(-?\d+(?:\.\d+)?)""")
    private val AT = Regex("""@(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)""")
    private val QUERY = Regex(
        """[?&](?:q|query|ll|destination)=(-?\d+(?:\.\d+)?)(?:,|%2C)(-?\d+(?:\.\d+)?)""",
        RegexOption.IGNORE_CASE
    )
    private val GEO = Regex("""geo:(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)""")
    private val GEO_Q = Regex("""[?&]q=(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)""")
    private val GEO_LABEL = Regex("""[?&]q=[^()]*\(([^)]*)\)""")
    private val PLACE = Regex("""/place/([^/@?]+)""")
    // Last resort: a bare "18.9219,72.8347" (e.g. pasted coordinates).
    private val PLAIN = Regex("""(-?\d{1,3}(?:\.\d+)?),\s*(-?\d{1,3}(?:\.\d+)?)""")

    fun isShortener(url: String): Boolean =
        url.contains("maps.app.goo.gl") || url.contains("goo.gl/maps")

    /** `EXTRA_TEXT` / `EXTRA_SUBJECT` from an ACTION_SEND. */
    fun parseShare(text: String?, subject: String?): Parsed? {
        val src = when {
            !text.isNullOrBlank() -> text
            !subject.isNullOrBlank() -> subject
            else -> return null
        }
        val m = URL_RE.find(src)
        val label = if (m != null) src.removeRange(m.range).trim() else src.trim()
        val url = m?.value?.trimEnd('.', ',', ')', ']', '"')
        if (url != null) {
            return parseUrl(url, label) ?: Parsed(label, null, null, url)
        }
        // No URL: a bare "19.07,72.87".
        parseCoords(src)?.let { return Parsed(label, it.first, it.second, null) }
        return null
    }

    /** `geo:` URIs from ACTION_VIEW. */
    fun parseGeo(uri: String): Parsed? {
        if (!uri.startsWith("geo:")) return null
        val q = GEO_Q.find(uri)
        if (q != null) {
            val la = q.groupValues[1].toDoubleOrNull()
            val lo = q.groupValues[2].toDoubleOrNull()
            if (la != null && lo != null && valid(la, lo)) {
                val name = GEO_LABEL.find(uri)?.groupValues?.get(1)?.let { decode(it) }.orEmpty()
                return Parsed(name.ifEmpty { "Dropped pin" }, la, lo, null)
            }
        }
        val g = GEO.find(uri) ?: return null
        val la = g.groupValues[1].toDoubleOrNull() ?: return null
        val lo = g.groupValues[2].toDoubleOrNull() ?: return null
        if (!valid(la, lo)) return null
        return Parsed("Dropped pin", la, lo, null)
    }

    /**
     * Parse an already-resolved (non-shortened) URL. Returns a Parsed with
     * null coordinates when only a place name/URL could be read — the caller
     * decides whether to try resolving it further.
     */
    fun parseUrl(url: String, fallbackName: String): Parsed? {
        parseGeo(url)?.let { return it }
        val coords = parseCoords(url)
        val name = decode(PLACE.find(url)?.groupValues?.get(1).orEmpty())
            .ifEmpty { fallbackName.trim() }
        if (coords == null) {
            return if (name.isEmpty()) null else Parsed(name, null, null, url)
        }
        return Parsed(name.ifEmpty { "Dropped pin" }, coords.first, coords.second, url)
    }

    private fun parseCoords(s: String): Pair<Double, Double>? {
        val patterns = listOf(D3D4D, QUERY, AT, PLAIN)
        for (re in patterns) {
            val m = re.find(s) ?: continue
            val la = m.groupValues[1].toDoubleOrNull() ?: continue
            val lo = m.groupValues[2].toDoubleOrNull() ?: continue
            if (valid(la, lo)) return la to lo
        }
        return null
    }

    private fun valid(lat: Double, lon: Double) =
        lat in -90.0..90.0 && lon in -180.0..180.0 && !(lat == 0.0 && lon == 0.0)

    private fun decode(s: String): String =
        try { URLDecoder.decode(s, "UTF-8").trim() } catch (e: Exception) { s.trim() }
}
