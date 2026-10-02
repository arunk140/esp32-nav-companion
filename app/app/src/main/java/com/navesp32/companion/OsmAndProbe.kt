package com.navesp32.companion

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.util.Log

/**
 * Stage-1 probe: can we bind OsmAnd's AIDL service at all, and is it the
 * interface we expect? Deliberately uses no OsmAnd code (their client library
 * is GPL-3.0, so vendoring it is a separate decision) - we only look at the
 * binder's interface descriptor.
 *
 * Binding succeeding is necessary but not sufficient: OsmAnd gates API calls
 * behind a per-app toggle in OsmAnd's Plugins list, and returns null/-1 until
 * the user enables us. That gate can only be tested with the real client.
 */
class OsmAndProbe(private val ctx: Context) {

    companion object {
        private const val TAG = "OsmAndProbe"
        const val ACTION = "net.osmand.aidl.OsmandAidlServiceV2"
        const val EXPECTED_IFACE = "net.osmand.aidlapi.IOsmAndAidlInterface"
        val PACKAGES = listOf("net.osmand.plus", "net.osmand", "net.osmand.dev")
    }

    private var conn: ServiceConnection? = null

    fun bind(onResult: (String) -> Unit) {
        val probe: ServiceConnection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                // getInterfaceDescriptor() is @hide since API 31, so go through
                // reflection; if the greylist blocks it we still learn that the
                // binder is alive, which is the main question.
                val desc: String? = runCatching {
                    IBinder::class.java.getMethod("getInterfaceDescriptor")
                        .invoke(service) as? String
                }.getOrNull()
                val alive = runCatching { service?.pingBinder() }.getOrDefault(false)
                Log.i(
                    TAG,
                    "connected to $name descriptor=$desc ping=$alive " +
                        "expected=$EXPECTED_IFACE match=${desc == EXPECTED_IFACE}"
                )
                onResult(
                    if (desc == EXPECTED_IFACE) "OK: bound, interface matches"
                    else "PARTIAL: bound to $name but descriptor was $desc"
                )
                conn = this
                runCatching { ctx.unbindService(this) }
            }

            override fun onServiceDisconnected(name: ComponentName?) {
                Log.w(TAG, "service disconnected: $name")
                onResult("LOST: $name")
            }

            override fun onBindingDied(name: ComponentName?) {
                Log.w(TAG, "binding died: $name")
                onResult("DIED: $name")
            }

            override fun onNullBinding(name: ComponentName?) {
                Log.w(TAG, "null binding: $name")
                onResult("NULL BINDING: $name")
            }
        }

        var lastErr = "no OsmAnd package found"
        for (pkg in PACKAGES) {
            val intent = Intent(ACTION).setPackage(pkg)
            try {
                val ok = ctx.bindService(intent, probe, Context.BIND_AUTO_CREATE)
                Log.i(TAG, "bindService($pkg) = $ok")
                if (ok) {
                    lastErr = "binding in progress for $pkg"
                    return
                }
            } catch (e: SecurityException) {
                lastErr = "SecurityException on $pkg: ${e.message}"
                Log.w(TAG, lastErr)
            } catch (e: Exception) {
                lastErr = "$pkg: $e"
                Log.w(TAG, lastErr)
            }
        }
        onResult("FAILED: $lastErr")
    }

    fun unbind() {
        conn?.let { runCatching { ctx.unbindService(it) } }
        conn = null
    }
}
