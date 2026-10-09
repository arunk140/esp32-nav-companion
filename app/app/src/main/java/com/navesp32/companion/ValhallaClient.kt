package com.navesp32.companion

import android.os.Handler
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * Minimal Valhalla `/route` client.
 *
 * No third-party HTTP/JSON dependency: the app already targets the Android SDK,
 * where `HttpURLConnection` and `org.json` are bundled. The request runs off the
 * main thread and the callback is delivered back on it.
 *
 * Endpoint defaults to the FOSSGIS public demo. That service is for development
 * and testing only, rate-limits to 1 request/user/second, and asks clients to
 * identify themselves — hence X-Client-Id. The base URL is user-configurable so
 * it can point at a self-hosted instance without a rebuild.
 */
class ValhallaClient(private val main: Handler) {

    companion object {
        private const val TAG = "ValhallaClient"
        const val DEFAULT_BASE_URL = "https://valhalla1.openstreetmap.de"
        private const val CLIENT_ID = "nav-esp32"
        const val CONNECT_TIMEOUT_MS = 15_000
        const val READ_TIMEOUT_MS = 20_000
    }

    /**
     * Requests a route. [headingDeg] is optional (0 is a valid north heading, so
     * it is passed as a nullable rather than a sentinel).
     */
    fun route(
        baseUrl: String,
        origin: GeoPoint,
        headingDeg: Int?,
        dest: GeoPoint,
        destName: String,
        callback: (Result<RoutePlan>) -> Unit,
    ) {
        Thread {
            val result = try {
                Result.success(request(baseUrl, origin, headingDeg, dest, destName))
            } catch (e: Exception) {
                Log.w(TAG, "route failed: $e")
                Result.failure(e)
            }
            main.post { callback(result) }
        }.start()
    }

    private fun request(
        baseUrl: String,
        origin: GeoPoint,
        headingDeg: Int?,
        dest: GeoPoint,
        destName: String,
    ): RoutePlan {
        val body = JSONObject().apply {
            put("locations", JSONArray().apply {
                put(JSONObject().apply {
                    put("lat", origin.lat)
                    put("lon", origin.lon)
                    if (headingDeg != null) put("heading", headingDeg)
                })
                put(JSONObject().apply {
                    put("lat", dest.lat)
                    put("lon", dest.lon)
                    if (destName.isNotEmpty()) put("name", destName)
                })
            })
            put("costing", "auto")
            put("units", "kilometers")
            put("directions_type", "instructions")
            put("language", "en-US")
            put("id", "nav-companion")
        }.toString()

        val endpoint = if (baseUrl.endsWith("/route")) baseUrl
        else baseUrl.trimEnd('/') + "/route"

        val conn = (URL(endpoint).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
            doOutput = true
            setRequestProperty("Content-Type", "application/json")
            setRequestProperty("Accept", "application/json")
            setRequestProperty("X-Client-Id", CLIENT_ID)
        }
        try {
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            val code = conn.responseCode
            val text = (if (code in 200..299) conn.inputStream else conn.errorStream)
                ?.let { BufferedReader(InputStreamReader(it, Charsets.UTF_8)).readText() }
                .orEmpty()
            if (code !in 200..299) {
                val msg = try {
                    JSONObject(text).optString("error").ifEmpty { "HTTP $code" }
                } catch (e: Exception) {
                    "HTTP $code"
                }
                throw RuntimeException(msg)
            }
            return parseRoute(text)
        } finally {
            conn.disconnect()
        }
    }

    private fun parseRoute(json: String): RoutePlan {
        val trip = JSONObject(json).getJSONObject("trip")
        val legs = trip.getJSONArray("legs")

        val points = ArrayList<GeoPoint>()
        val maneuvers = ArrayList<RouteManeuver>()
        var totalTime = 0.0

        for (li in 0 until legs.length()) {
            val leg = legs.getJSONObject(li)
            val legPoints = Polyline.decode(leg.optString("shape"), 6)
            // Drop the duplicate joint when stitching legs (multi-waypoint routes).
            val start = if (points.isNotEmpty() && legPoints.isNotEmpty() &&
                near(points.last(), legPoints.first())
            ) 1 else 0
            val offset = points.size
            for (i in start until legPoints.size) points.add(legPoints[i])

            val ms = leg.optJSONArray("maneuvers") ?: JSONArray()
            for (mi in 0 until ms.length()) {
                val m = ms.getJSONObject(mi)
                val begin = (m.optInt("begin_shape_index", 0) - start).coerceIn(0, points.size - 1) + offset
                val end = (m.optInt("end_shape_index", 0) - start).coerceIn(0, points.size - 1) + offset
                val name = streetName(m)
                val time = m.optDouble("time", 0.0)
                totalTime += time
                maneuvers.add(
                    RouteManeuver(
                        nav = ManeuverMap.nav(m.optInt("type", 0)),
                        instruction = m.optString("instruction"),
                        street = name,
                        beginIdx = begin.coerceAtMost(end),
                        endIdx = end,
                        timeS = time,
                        roundaboutExit = m.optInt("roundabout_exit_count", 0),
                    )
                )
            }
        }
        if (points.size < 2) throw RuntimeException("route has no geometry")

        val n = points.size
        val cum = DoubleArray(n)
        for (i in 1 until n) cum[i] = cum[i - 1] + Geo.distanceM(points[i - 1], points[i])

        // Cumulative distance -> elapsed time anchors. Each maneuver's time is
        // spread from the previous anchor to its end index, so the banner's
        // remaining-time shrinks smoothly between turns.
        val anchors = ArrayList<Pair<Double, Double>>(maneuvers.size + 1)
        anchors.add(0.0 to 0.0)
        var acc = 0.0
        for (m in maneuvers.sortedBy { it.endIdx }) {
            acc += m.timeS
            val d = cum[m.endIdx.coerceIn(0, n - 1)]
            val prev = anchors.last()
            anchors.add(maxOf(prev.first, d) to acc)
        }
        if (totalTime <= 0.0) totalTime = trip.optJSONObject("summary")?.optDouble("time", 0.0) ?: 0.0

        return RoutePlan(
            points = points,
            maneuvers = maneuvers,
            cumDistM = cum,
            anchors = anchors,
            totalDistM = cum[n - 1],
            totalTimeS = totalTime,
            dest = points.last(),
        )
    }

    private fun streetName(m: JSONObject): String {
        val names = m.optJSONArray("street_names") ?: return ""
        // Instructions/text are ASCII on the wire to the device anyway.
        return if (names.length() > 0) names.optString(0) else ""
    }

    private fun near(a: GeoPoint, b: GeoPoint): Boolean =
        Math.abs(a.lat - b.lat) < 1e-7 && Math.abs(a.lon - b.lon) < 1e-7
}
