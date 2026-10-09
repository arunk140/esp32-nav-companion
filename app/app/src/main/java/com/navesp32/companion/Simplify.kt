package com.navesp32.companion

/**
 * Reduces a route polyline to something the ESP32 can hold and draw.
 *
 * Uses Douglas-Peucker (iteratively, so a long route cannot blow the stack),
 * widening the tolerance until the result fits [forDisplay]'s budget. A stride
 * fallback guarantees the cap if even a large tolerance is not enough.
 */
object Simplify {

    fun forDisplay(points: List<GeoPoint>, maxPts: Int): List<GeoPoint> {
        if (points.size <= maxPts) return points
        if (maxPts < 2) return points.take(1)

        var eps = 5.0
        var out = rdp(points, eps)
        while (out.size > maxPts && eps < 1000.0) {
            eps *= 2.0
            out = rdp(points, eps)
        }
        if (out.size <= maxPts) return out

        // Extremely dense geometry: decimate to the budget, keeping endpoints.
        val stride = (out.size + maxPts - 1) / maxPts
        val reduced = ArrayList<GeoPoint>(maxPts)
        var i = 0
        while (i < out.size) {
            reduced.add(out[i])
            i += stride
        }
        if (reduced.last() != out.last()) reduced.add(out.last())
        return reduced
    }

    /** Douglas-Peucker with an all-pairs distance tolerance in metres. */
    fun rdp(points: List<GeoPoint>, epsM: Double): List<GeoPoint> {
        if (points.size < 3) return points
        val keep = BooleanArray(points.size)
        keep[0] = true
        keep[points.size - 1] = true
        val stack = ArrayDeque<Int>()
        stack.addLast(0)
        stack.addLast(points.size - 1)
        while (stack.isNotEmpty()) {
            val e = stack.removeLast()
            val s = stack.removeLast()
            var maxD = 0.0
            var idx = -1
            for (i in s + 1 until e) {
                val d = Geo.perpDistanceM(points[i], points[s], points[e])
                if (d > maxD) {
                    maxD = d
                    idx = i
                }
            }
            if (idx != -1 && maxD > epsM) {
                keep[idx] = true
                stack.addLast(s)
                stack.addLast(idx)
                stack.addLast(idx)
                stack.addLast(e)
            }
        }
        val out = ArrayList<GeoPoint>(points.size)
        for (i in points.indices) if (keep[i]) out.add(points[i])
        return out
    }
}
