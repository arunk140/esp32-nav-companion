package com.navesp32.companion

import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * Valhalla-backed navigation source. The counterpart to [OsmAndLink]: instead of
 * reading another app's live navigation, it asks Valhalla for a route, then
 * tracks the phone's fixes along it and emits the same [NavState] frames through
 * the same [NavBridge].
 *
 * Reroute policy: when the fix is off the line for [OFF_ROUTE_FIXES] consecutive
 * fixes, request the route again from the current position, but never more often
 * than [REROUTE_MIN_MS] (also keeps us inside the FOSSGIS demo's 1 req/s).
 */
class ValhallaSource(
    private val baseUrl: () -> String,
    private val out: NavBridge,
    private val onStatus: (String) -> Unit = {},
) {
    companion object {
        private const val TAG = "ValhallaSource"
        const val REROUTE_MIN_MS = 10_000L
        const val OFF_ROUTE_FIXES = 3
        const val ARRIVED_DISMISS_MS = 6_000L
    }

    private val main = Handler(Looper.getMainLooper())
    private val client = ValhallaClient(main)
    private val nav = ValhallaNavigator()

    private var dest: GeoPoint? = null
    private var destName = ""
    private var routing = false
    private var lastRouteAtMs = 0L
    private var lastFix: Fix? = null
    private var offRouteCount = 0
    private var arrived = false

    val hasDestination: Boolean get() = dest != null

    /** Called from the UI/service with a chosen place. */
    fun setDestination(name: String, lat: Double, lon: Double) {
        dest = GeoPoint(lat, lon)
        destName = name.ifEmpty { "Destination" }
        nav.clear()
        offRouteCount = 0
        arrived = false
        out.clearRoute()  // drop the previous route line
        // Show the destination marker straight away, before the route arrives.
        out.send(
            NavState(
                active = false, maneuver = Nav.NONE, distanceM = 0, street = "",
                afterManeuver = Nav.NONE, afterDistanceM = 0, afterStreet = "",
                remainingM = 0, remainingS = 0,
                destLat = lat, destLon = lon, hasDest = true,
            )
        )
        onStatus("Routing…")
        lastFix?.let { requestRoute(it) }
    }

    /** Called for every GPS fix from LocationStream. */
    fun onFix(fix: Fix) {
        lastFix = fix
        val d = dest ?: return
        if (routing) return

        if (!nav.hasRoute) {
            requestRoute(fix)
            return
        }

        val p = nav.update(GeoPoint(fix.lat, fix.lon)) ?: return

        if (p.arrived) {
            out.send(stateFrom(p, active = true))
            if (!arrived) {
                arrived = true
                onStatus("Arrived")
                main.postDelayed({ stopGuidance() }, ARRIVED_DISMISS_MS)
            }
            return
        }

        if (p.offRoute) {
            offRouteCount++
            if (offRouteCount >= OFF_ROUTE_FIXES && canReroute()) {
                out.send(stateFrom(p, active = true))  // flips to OFF_ROUTE icon
                requestRoute(fix)
                return
            }
        } else {
            offRouteCount = 0
        }
        out.send(stateFrom(p, active = true))
    }

    /** User pressed Stop, or the link dropped. */
    fun stop() {
        main.removeCallbacksAndMessages(null)
        dest = null
        destName = ""
        nav.clear()
        routing = false
        offRouteCount = 0
        arrived = false
        out.clearRoute()
        out.sendStopped()
    }

    fun stopGuidance() {
        nav.clear()
        dest = null
        destName = ""
        offRouteCount = 0
        arrived = false
        out.clearRoute()
        out.sendStopped()
        onStatus("Navigation ended")
    }

    private fun canReroute() =
        System.currentTimeMillis() - lastRouteAtMs >= REROUTE_MIN_MS

    private fun requestRoute(origin: Fix) {
        val d = dest ?: return
        if (routing) return
        routing = true
        lastRouteAtMs = System.currentTimeMillis()
        val heading = if (origin.bearingDeg >= 0f) origin.bearingDeg.toInt() else null
        client.route(
            baseUrl = baseUrl(),
            origin = GeoPoint(origin.lat, origin.lon),
            headingDeg = heading,
            dest = d,
            destName = destName,
        ) { res ->
            routing = false
            res.onSuccess { plan ->
                nav.setRoute(plan)
                out.sendRoute(plan.points)
                offRouteCount = 0
                arrived = false
                Log.i(TAG, "route: ${plan.maneuvers.size} maneuvers, " +
                    "${plan.totalDistM.round()} m, ${plan.totalTimeS.round()} s")
                lastFix?.let { f ->
                    nav.update(GeoPoint(f.lat, f.lon))?.let { out.send(stateFrom(it, active = true)) }
                }
                onStatus("Navigating")
            }.onFailure { e ->
                Log.w(TAG, "route failed: ${e.message}")
                // A failed reroute keeps the previous route; a failed first
                // route leaves the destination marker up so the user can retry.
                onStatus(if (nav.hasRoute) "Reroute failed" else "Route failed: ${e.message}")
            }
        }
    }

    private fun stateFrom(p: NavProgress, active: Boolean): NavState {
        val d = dest
        return NavState(
            active = active,
            maneuver = if (p.offRoute) Nav.OFF_ROUTE else p.maneuver,
            distanceM = p.distM,
            street = p.street,
            afterManeuver = p.afterManeuver,
            afterDistanceM = p.afterDistM,
            afterStreet = "",
            remainingM = p.remainingM,
            remainingS = p.remainingS,
            destLat = d?.lat ?: 0.0,
            destLon = d?.lon ?: 0.0,
            hasDest = d != null,
        )
    }

    private fun Double.round(): Long = Math.round(this)
}
