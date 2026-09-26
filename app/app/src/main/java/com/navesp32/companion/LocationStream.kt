package com.navesp32.companion

import android.annotation.SuppressLint
import android.content.Context
import android.location.Location
import android.os.Looper
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority

// Fused location -> packed 18 B POSITION payloads. 1 Hz connected, 5 Hz debug.
@SuppressLint("MissingPermission")
class LocationStream(private val ctx: Context, private val onFix: (ByteArray) -> Unit) {

    companion object {
        const val INTERVAL_MS = 1000L
        const val DEBUG_INTERVAL_MS = 200L
    }

    private val client = LocationServices.getFusedLocationProviderClient(ctx)
    private var running = false
    var debug = false

    private val cb = object : LocationCallback() {
        private var lastMs = 0L
        private var n = 0

        override fun onLocationResult(r: LocationResult) {
            val l: Location = r.lastLocation ?: return
            // Never stream null-island: firmware would recenter to empty ocean.
            if (l.latitude == 0.0 && l.longitude == 0.0) return
            val now = System.currentTimeMillis()
            if (lastMs > 0 && now - lastMs > 2000) {
                android.util.Log.i("LocationStream", "fix gap ${now - lastMs}ms")
            }
            lastMs = now
            n++
            if (n % 10 == 0) android.util.Log.i("LocationStream", "fixes=$n")
            val sats = if (l.hasAccuracy()) 0 else 0 // fused API hides SV count; 0 = unknown
            val payload = BleManager.packPosition(
                (l.latitude * 1e7).toInt(),
                (l.longitude * 1e7).toInt(),
                (l.speed * 100).toInt().coerceIn(0, 65535),
                ((l.bearing * 100).toInt().coerceIn(0, 35999)),
                sats,
                if (l.hasAccuracy() && l.accuracy < 50) 1 else 0,
                System.currentTimeMillis()
            )
            onFix(payload)
        }
    }

    fun start() {
        if (running) return
        running = true
        val req = LocationRequest.Builder(
            if (debug) DEBUG_INTERVAL_MS else INTERVAL_MS
        ).setPriority(Priority.PRIORITY_HIGH_ACCURACY)
            // Unbatched: Fused otherwise delivers in ~10 s clumps with the
            // screen off / under Doze, which trips the device stale timeout.
            .setMinUpdateIntervalMillis(500)
            .setMaxUpdateDelayMillis(1000)
            .setMinUpdateDistanceMeters(0f)
            .build()
        client.requestLocationUpdates(req, cb, Looper.getMainLooper())
    }

    fun stop() {
        running = false
        client.removeLocationUpdates(cb)
    }
}
