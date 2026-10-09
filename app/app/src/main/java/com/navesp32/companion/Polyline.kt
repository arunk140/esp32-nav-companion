package com.navesp32.companion

/**
 * Decoder for Google/Valhalla encoded polylines.
 *
 * Valhalla's JSON `shape` uses 6-decimal precision (polyline6), unlike the
 * classic 5-decimal Google polyline. Getting the precision wrong scales the
 * route by a factor of ten — the reason this is a named parameter.
 */
object Polyline {
    fun decode(encoded: String, precision: Int = 6): List<GeoPoint> {
        val scale = Math.pow(10.0, precision.toDouble())
        val out = ArrayList<GeoPoint>(encoded.length / 4)
        var index = 0
        var lat = 0
        var lon = 0
        val n = encoded.length
        while (index < n) {
            var result = 0
            var shift = 0
            var b: Int
            do {
                if (index >= n) return out
                b = encoded[index++].code - 63
                result = result or ((b and 0x1f) shl shift)
                shift += 5
            } while (b and 0x20 != 0)
            lat += if (result and 1 != 0) (result shr 1).inv() else (result shr 1)

            result = 0
            shift = 0
            do {
                if (index >= n) return out
                b = encoded[index++].code - 63
                result = result or ((b and 0x1f) shl shift)
                shift += 5
            } while (b and 0x20 != 0)
            lon += if (result and 1 != 0) (result shr 1).inv() else (result shr 1)

            out.add(GeoPoint(lat / scale, lon / scale))
        }
        return out
    }
}
