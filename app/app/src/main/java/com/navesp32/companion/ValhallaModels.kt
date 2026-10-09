package com.navesp32.companion

/** One decoded turn from a Valhalla leg. */
data class RouteManeuver(
    val nav: Int,              // Nav.*
    val instruction: String,
    val street: String,
    val beginIdx: Int,         // index into RoutePlan.points
    val endIdx: Int,
    val timeS: Double,
    val roundaboutExit: Int,
)

/**
 * A fully decoded, ready-to-navigate route. Geometry and the cumulative
 * distance/time tables are precomputed once so the 1 Hz nav loop only does
 * projection arithmetic.
 */
data class RoutePlan(
    val points: List<GeoPoint>,
    val maneuvers: List<RouteManeuver>,
    val cumDistM: DoubleArray,   // size == points.size
    val anchors: List<Pair<Double, Double>>, // (distanceM, timeS) at maneuver ends
    val totalDistM: Double,
    val totalTimeS: Double,
    val dest: GeoPoint,
)
