package com.navesp32.companion

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min

/** WGS84 point in decimal degrees. */
data class GeoPoint(val lat: Double, val lon: Double)

/** Result of projecting a fix onto the route polyline. */
data class Projection(
    val segIndex: Int,   // segment i spans points[i] .. points[i+1]
    val t: Double,       // 0..1 along that segment
    val perpM: Double,   // perpendicular distance from the fix to the polyline
)

/**
 * Geodesy helpers for the nav loop. Distances here are short (a route across a
 * city), so an equirectangular local frame is well under the wire's 1 m
 * resolution; segment lengths use the haversine formula for the accumulated
 * distances that feed the banner.
 */
object Geo {
    private const val EARTH_R = 6_371_000.0

    fun distanceM(a: GeoPoint, b: GeoPoint): Double {
        val dLat = Math.toRadians(b.lat - a.lat)
        val dLon = Math.toRadians(b.lon - a.lon)
        val la1 = Math.toRadians(a.lat)
        val la2 = Math.toRadians(b.lat)
        val h = Math.sin(dLat / 2).let { it * it } +
            cos(la1) * cos(la2) * Math.sin(dLon / 2).let { it * it }
        return 2 * EARTH_R * Math.asin(min(1.0, Math.sqrt(h)))
    }

    /**
     * Nearest point on [points] to [p], and how far off the line it is.
     * Returns null when there are fewer than two points.
     */
    fun project(points: List<GeoPoint>, p: GeoPoint): Projection? {
        if (points.size < 2) return null
        val latScale = 111_320.0
        val lonScale = 111_320.0 * cos(Math.toRadians(p.lat))
        var bestIdx = 0
        var bestT = 0.0
        var bestD2 = Double.MAX_VALUE
        for (i in 0 until points.size - 1) {
            val a = points[i]
            val b = points[i + 1]
            // Translate so p is the origin.
            val ax = (a.lon - p.lon) * lonScale
            val ay = (a.lat - p.lat) * latScale
            val bx = (b.lon - p.lon) * lonScale
            val by = (b.lat - p.lat) * latScale
            val dx = bx - ax
            val dy = by - ay
            val len2 = dx * dx + dy * dy
            val t = if (len2 <= 1e-9) 0.0 else ((-ax * dx - ay * dy) / len2).coerceIn(0.0, 1.0)
            val cx = ax + t * dx
            val cy = ay + t * dy
            val d2 = cx * cx + cy * cy
            if (d2 < bestD2) {
                bestD2 = d2
                bestIdx = i
                bestT = t
            }
        }
        return Projection(bestIdx, bestT, Math.sqrt(bestD2))
    }

    /** Signed-ish shortest separation, used only to keep progress monotonic. */
    fun approxDistanceM(a: GeoPoint, b: GeoPoint): Double = distanceM(a, b)

    /**
     * Perpendicular distance from [p] to the segment a..b, in metres.
     * Equirectangular local frame; same approximation as [project]. Used by the
     * route simplifier, which runs over every vertex.
     */
    fun perpDistanceM(p: GeoPoint, a: GeoPoint, b: GeoPoint): Double {
        val latScale = 111_320.0
        val lonScale = 111_320.0 * cos(Math.toRadians(p.lat))
        val ax = (a.lon - p.lon) * lonScale
        val ay = (a.lat - p.lat) * latScale
        val bx = (b.lon - p.lon) * lonScale
        val by = (b.lat - p.lat) * latScale
        val dx = bx - ax
        val dy = by - ay
        val len2 = dx * dx + dy * dy
        val t = if (len2 <= 1e-9) 0.0 else ((-ax * dx - ay * dy) / len2).coerceIn(0.0, 1.0)
        val cx = ax + t * dx
        val cy = ay + t * dy
        return Math.sqrt(cx * cx + cy * cy)
    }

    fun clamp(v: Double, lo: Double, hi: Double) = max(lo, min(hi, v))

    fun absDiff(a: Double, b: Double) = abs(a - b)
}
