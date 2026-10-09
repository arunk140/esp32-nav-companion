package com.navesp32.companion

import kotlin.math.roundToInt

/** What the nav loop computes for one fix. */
data class NavProgress(
    val maneuver: Int,
    val distM: Int,
    val afterManeuver: Int,
    val afterDistM: Int,
    val remainingM: Int,
    val remainingS: Int,
    val street: String,
    val offRoute: Boolean,
    val arrived: Boolean,
    val perpM: Double,
)

/**
 * The part of turn-by-turn that Valhalla does not do for us: tracking a fix
 * along a precomputed route.
 *
 * Pure Kotlin on purpose — no Android types — so the projection and banner math
 * can be unit-tested on the JVM with a synthetic route. [ValhallaSource] owns
 * the network, reroute policy and BLE side.
 */
class ValhallaNavigator {

    companion object {
        const val OFF_ROUTE_M = 50.0    // perpendicular distance that means "left the route"
        const val ARRIVE_M = 25.0       // within this of the end = arrived
        const val MONO_TOLERANCE_M = 3.0
    }

    private var plan: RoutePlan? = null
    private var lastProgressM = 0.0

    val hasRoute: Boolean get() = plan != null

    fun setRoute(p: RoutePlan) {
        plan = p
        lastProgressM = 0.0
    }

    fun clear() {
        plan = null
        lastProgressM = 0.0
    }

    /** Distance along the route at the current progress. */
    fun progressM(): Double = lastProgressM

    fun update(p: GeoPoint): NavProgress? {
        val rp = plan ?: return null
        if (rp.points.size < 2) return null

        val proj = Geo.project(rp.points, p) ?: return null
        val segLen = rp.cumDistM[proj.segIndex + 1] - rp.cumDistM[proj.segIndex]
        var progress = rp.cumDistM[proj.segIndex] + proj.t * segLen

        // GPS noise and a fix snapped behind the previous one must not rewind
        // the banner. Allow a tiny backward step, otherwise hold.
        if (progress < lastProgressM - MONO_TOLERANCE_M) progress = lastProgressM
        if (progress < 0) progress = 0.0
        lastProgressM = progress

        val nextIdx = rp.maneuvers.indexOfFirst { rp.cumDistM[it.beginIdx] > progress + 1.0 }
            .let { if (it < 0) rp.maneuvers.size - 1 else it }
        val next = if (rp.maneuvers.isNotEmpty()) rp.maneuvers[nextIdx] else null
        val after = if (nextIdx + 1 < rp.maneuvers.size) rp.maneuvers[nextIdx + 1] else null

        val dist = if (next != null) {
            (rp.cumDistM[next.beginIdx] - progress).coerceAtLeast(0.0)
        } else {
            (rp.totalDistM - progress).coerceAtLeast(0.0)
        }
        val afterDist = if (next != null && after != null) {
            (rp.cumDistM[after.beginIdx] - rp.cumDistM[next.beginIdx]).coerceAtLeast(0.0)
        } else 0.0

        val remainingM = (rp.totalDistM - progress).coerceAtLeast(0.0)
        val remainingS = (rp.totalTimeS - timeAt(rp, progress)).coerceAtLeast(0.0)

        val arrived = progress >= rp.totalDistM - ARRIVE_M ||
            (next != null && next.nav == Nav.ARRIVE && dist < ARRIVE_M)
        val maneuver = when {
            arrived -> Nav.ARRIVE
            next != null -> next.nav
            else -> Nav.CONTINUE
        }
        val street = next?.let { if (it.street.isNotEmpty()) it.street else it.instruction } ?: ""

        return NavProgress(
            maneuver = maneuver,
            distM = dist.roundToInt(),
            afterManeuver = after?.nav ?: Nav.NONE,
            afterDistM = afterDist.roundToInt(),
            remainingM = remainingM.roundToInt(),
            remainingS = remainingS.roundToInt(),
            street = street,
            offRoute = proj.perpM > OFF_ROUTE_M,
            arrived = arrived,
            perpM = proj.perpM,
        )
    }

    /** Linear interpolation of elapsed time at a distance along the route. */
    private fun timeAt(rp: RoutePlan, distM: Double): Double {
        val a = rp.anchors
        if (a.isEmpty()) return 0.0
        if (distM <= a.first().first) return a.first().second
        if (distM >= a.last().first) return a.last().second
        for (i in 0 until a.size - 1) {
            val (d0, t0) = a[i]
            val (d1, t1) = a[i + 1]
            if (distM in d0..d1) {
                val span = d1 - d0
                return if (span <= 0.0) t1 else t0 + (distM - d0) / span * (t1 - t0)
            }
        }
        return a.last().second
    }
}
