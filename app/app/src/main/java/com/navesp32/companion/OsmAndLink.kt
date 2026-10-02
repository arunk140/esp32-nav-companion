package com.navesp32.companion

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import net.osmand.aidlapi.IOsmAndAidlCallback
import net.osmand.aidlapi.IOsmAndAidlInterface
import net.osmand.aidlapi.OsmandAidlConstants
import net.osmand.aidlapi.info.AppInfoParams
import net.osmand.aidlapi.navigation.ADirectionInfo
import net.osmand.aidlapi.navigation.ANavigationUpdateParams

/**
 * Reads live turn-by-turn state from OsmAnd over its AIDL API.
 *
 * Two channels, because they are not redundant:
 *  - registerForNavigationUpdates() pushes distanceTo + turnType at the GPS
 *    rate and is our "a turn just changed" wake-up signal.
 *  - getAppInfo() carries everything else (street name, lanes, angle, ETA,
 *    remaining distance/time) and is polled once a second on that wake-up.
 *
 * OsmAnd gates the API per app: the first bind succeeds but calls return
 * null / -1 until the user enables us in OsmAnd -> Plugins. Detecting that
 * -1 is how we know to prompt rather than sit silent.
 */
class OsmAndLink(private val ctx: Context, private val cb: Cb) {

    interface Cb {
        /** navActive == false means navigation stopped. */
        fun onNavState(s: NavState)
        /** True when OsmAnd has us disabled and the user must enable us. */
        fun onConsentNeeded()
        fun onLinkState(bound: Boolean)
    }

    data class NavState(
        val active: Boolean,
        val maneuver: Int,        // our own enum, see NAV_* below
        val distanceM: Int,      // to the maneuver
        val street: String,
        val afterManeuver: Int,
        val afterDistanceM: Int,
        val afterStreet: String,
        val remainingM: Int,
        val remainingS: Int,
    )

    // Our own maneuver vocabulary, deliberately NOT OsmAnd's TurnType numbering:
    // that is an implementation detail of a project we don't control.
    companion object {
        private const val TAG = "OsmAndLink"
        private const val ACTION = "net.osmand.aidl.OsmandAidlServiceV2"
        val PACKAGES = listOf("net.osmand.plus", "net.osmand", "net.osmand.dev")
        private const val POLL_MS = 1000L
        // As you pass a maneuver OsmAnd transiently reports no next-turn info
        // while it recomputes; without this the banner flickers on every turn.
        private const val NONE_DEBOUNCE_MS = 2500

        const val NAV_NONE = 0
        const val NAV_CONTINUE = 1
        const val NAV_SLIGHT_LEFT = 2
        const val NAV_LEFT = 3
        const val NAV_SHARP_LEFT = 4
        const val NAV_SLIGHT_RIGHT = 5
        const val NAV_RIGHT = 6
        const val NAV_SHARP_RIGHT = 7
        const val NAV_KEEP_LEFT = 8
        const val NAV_KEEP_RIGHT = 9
        const val NAV_UTURN = 10
        const val NAV_ROUNDABOUT = 11
        const val NAV_OFF_ROUTE = 12
        const val NAV_ARRIVE = 13

        private fun mapType(v: Int): Int = when (v) {
            OsmandAidlConstants.TURN_TYPE_C -> NAV_CONTINUE
            OsmandAidlConstants.TURN_TYPE_TSLL -> NAV_SLIGHT_LEFT
            OsmandAidlConstants.TURN_TYPE_TL -> NAV_LEFT
            OsmandAidlConstants.TURN_TYPE_TSHL -> NAV_SHARP_LEFT
            OsmandAidlConstants.TURN_TYPE_TSLR -> NAV_SLIGHT_RIGHT
            OsmandAidlConstants.TURN_TYPE_TR -> NAV_RIGHT
            OsmandAidlConstants.TURN_TYPE_TSHR -> NAV_SHARP_RIGHT
            OsmandAidlConstants.TURN_TYPE_KL -> NAV_KEEP_LEFT
            OsmandAidlConstants.TURN_TYPE_KR -> NAV_KEEP_RIGHT
            OsmandAidlConstants.TURN_TYPE_TU, OsmandAidlConstants.TURN_TYPE_TRU -> NAV_UTURN
            OsmandAidlConstants.TURN_TYPE_RNDB, OsmandAidlConstants.TURN_TYPE_RNLB -> NAV_ROUNDABOUT
            OsmandAidlConstants.TURN_TYPE_OFFR -> NAV_OFF_ROUTE
            else -> NAV_NONE
        }
    }

    private var api: IOsmAndAidlInterface? = null
    private var bound = false            // bindService() succeeded; unbind exactly once
    private var running = false          // between start() and stop()
    private var consentPrompted = false
    private var lastActive = false
    private var lastSeenActiveMs = 0L
    private var callbackToken = -1L      // id returned by registerForNavigationUpdates()

    fun start() {
        if (running) return
        running = true
        bind()
    }

    private fun bind() {
        if (!running || bound) return
        for (pkg in PACKAGES) {
            try {
                if (ctx.bindService(Intent(ACTION).setPackage(pkg), conn, Context.BIND_AUTO_CREATE)) {
                    // bindService() true means the connection is registered and
                    // must be released exactly once, whether or not the service
                    // ever calls back.
                    bound = true
                    Log.i(TAG, "binding to $pkg")
                    return
                }
            } catch (e: Exception) {
                Log.w(TAG, "bind $pkg: $e")
            }
        }
        Log.w(TAG, "no OsmAnd to bind")
        running = false
        cb.onLinkState(false)
    }

    fun stop() {
        running = false
        ctx.mainHandler.removeCallbacks(pollRunnable)
        val a = api
        api = null
        // Best-effort unsubscribe on shutdown: params.subscribeToUpdates = false
        if (a != null && callbackToken >= 0) {
            try {
                a.registerForNavigationUpdates(unsubParams(), null)
            } catch (e: Exception) {
                Log.w(TAG, "unsubscribe: $e")
            }
        }
        callbackToken = -1L
        if (bound) {
            bound = false
            // unbindService throws if the connection was never registered;
            // bound tracks that, and the try/catch covers a died binding.
            try {
                ctx.unbindService(conn)
            } catch (e: Exception) {
                Log.w(TAG, "unbind: $e")
            }
        }
    }

    // Explicit type: the object references conn in its own callbacks, which
    // otherwise makes the inferred type recursive.
    private val conn: ServiceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            if (!running) {
                // stop() raced ahead of the callback: release the binding now.
                if (bound) {
                    bound = false
                    try {
                        ctx.unbindService(conn)
                    } catch (e: Exception) {
                        Log.w(TAG, "unbind after late connect: $e")
                    }
                }
                return
            }
            Log.i(TAG, "connected to $name")
            val i = IOsmAndAidlInterface.Stub.asInterface(service)
            if (i == null) {
                Log.w(TAG, "null interface")
                cb.onLinkState(false)
                return
            }
            api = i
            bound = true
            cb.onLinkState(true)
            val token = register()
            // Any negative result is a rejection: -1 (MIN_UPDATE_TIME_MS_ERROR)
            // is the documented consent case, -2/-5 are the other API errors.
            if (token < 0) {
                Log.w(TAG, "register rejected (consent?) token=$token")
                consentPrompted = true
                cb.onConsentNeeded()
            } else {
                Log.i(TAG, "registered, token=$token")
                callbackToken = token
                consentPrompted = false
                poll()
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            Log.w(TAG, "disconnected: $name")
            // The binding survives a dead service process: Android restarts it
            // and calls onServiceConnected again, so do not rebind here.
            api = null
            callbackToken = -1L
            cb.onLinkState(false)
        }

        override fun onBindingDied(name: ComponentName?) {
            Log.w(TAG, "binding died: $name")
            api = null
            callbackToken = -1L
            if (bound) {
                bound = false
                try {
                    ctx.unbindService(conn)
                } catch (e: Exception) {
                    Log.w(TAG, "unbind after death: $e")
                }
            }
            cb.onLinkState(false)
            // A dead binding must be torn down and recreated from scratch.
            if (running) ctx.mainHandler.postDelayed({ bind() }, 5000)
        }

        override fun onNullBinding(name: ComponentName?) {
            Log.w(TAG, "null binding: $name")
        }
    }

    private fun unsubParams(): ANavigationUpdateParams =
        ANavigationUpdateParams().apply {
            setSubscribeToUpdates(false)
            setCallbackId(callbackToken)
        }

    private fun register(): Long = try {
        api?.registerForNavigationUpdates(ANavigationUpdateParams(), navCallback) ?: -1L
    } catch (e: Exception) {
        Log.w(TAG, "register threw: $e")
        -1L
    }

    // The push payload has only distanceTo + turnType: no street name. It is a
    // wake-up signal, not the data itself. AIDL requires every callback to be
    // implemented; the rest are unused by this app.
    private val navCallback = object : IOsmAndAidlCallback.Stub() {
        override fun updateNavigationInfo(info: ADirectionInfo?) {
            Log.i(TAG, "nav update dist=${info?.distanceTo} type=${info?.turnType}")
            ctx.mainHandler.post { poll() }
        }

        override fun onUpdate() {}
        override fun onAppInitialized() {}
        override fun onSearchComplete(resultSet: MutableList<net.osmand.aidlapi.search.SearchResult>?) {}
        override fun onGpxBitmapCreated(bitmap: net.osmand.aidlapi.gpx.AGpxBitmap?) {}
        override fun onContextMenuButtonClicked(buttonId: Int, pointId: String?, layerId: String?) {}
        override fun onVoiceRouterNotify(params: net.osmand.aidlapi.navigation.OnVoiceNavigationParams?) {}
        override fun onKeyEvent(params: android.view.KeyEvent?) {}
        override fun onLogcatMessage(params: net.osmand.aidlapi.logcat.OnLogcatMessageParams?) {}
    }

    // One-shot read for the debug button.
    fun pollNow(cb: (String) -> Unit) {
        Thread {
            val text = try {
                val info = api?.appInfo
                if (info == null) "appInfo() == null (consent not granted yet?)"
                else "turnInfo=" + (info.turnInfo?.keySet()?.joinToString().orEmpty()) +
                    "\nleft=${info.leftDistance}m ${info.leftTime}s\n" +
                    "bound=${info.lastKnownLocation != null}"
            } catch (e: Exception) {
                "error: $e"
            }
            ctx.mainHandler.post { cb(text) }
        }.start()
    }

    // A single perpetual poll loop. Nav pushes only nudge it: removeCallbacks +
    // post reads immediately without creating a second loop. The previous
    // version cleared its guard *inside* run(), so every push spawned another
    // 1 Hz loop (one per GPS update, forever).
    private val pollRunnable = object : Runnable {
        override fun run() {
            if (!running) return
            val a = api
            if (a != null) {
                val info = try {
                    a.appInfo
                } catch (e: Exception) {
                    Log.w(TAG, "poll: $e")
                    null
                }
                emit(info)
            }
            // Keep polling: navigation state is time-dependent (distances
            // shrink even when no event fires). A null api just idles until
            // onServiceConnected nudges us again.
            if (running) ctx.mainHandler.postDelayed(this, POLL_MS)
        }
    }

    private fun poll() {
        if (!running) return
        ctx.mainHandler.removeCallbacks(pollRunnable)
        ctx.mainHandler.post(pollRunnable)
    }

    // OsmAnd does not type the turnInfo bundle consistently: some builds
    // hand back "TL" as a String where others hand back an Int, and a typed
    // Bundle.getInt() throws ClassCastException and loses the whole frame.
    // Read defensively and never let one field break the rest.
    private fun bInt(b: Bundle?, key: String, def: Int): Int {
        if (b == null) return def
        return try {
            when (val v = b.get(key)) {
                is Int -> v
                is Long -> v.toInt()
                is String -> v.trim().substringBefore('.').toIntOrNull() ?: def
                else -> def
            }
        } catch (e: Exception) {
            def
        }
    }

    private fun bStr(b: Bundle?, key: String): String {
        if (b == null) return ""
        return try {
            b.get(key)?.toString().orEmpty()
        } catch (e: Exception) {
            ""
        }
    }

    private fun emit(info: AppInfoParams?) {
        if (info == null) {
            if (!consentPrompted) {
                cb.onConsentNeeded()
                consentPrompted = true
            }
            return
        }
        val t = info.turnInfo
        if (t == null) {
            // Debounced: OsmAnd blanks the next-turn data briefly at each turn.
            val now = android.os.SystemClock.elapsedRealtime()
            if (lastActive && now - lastSeenActiveMs < NONE_DEBOUNCE_MS) return
            if (lastActive) {
                lastActive = false
                cb.onNavState(NavState(false, NAV_NONE, 0, "", NAV_NONE, 0, "", 0, 0))
            }
            return
        }
        lastActive = true
        lastSeenActiveMs = android.os.SystemClock.elapsedRealtime()
        val s = NavState(
            active = true,
            maneuver = mapType(bInt(t, "next_turn_type", OsmandAidlConstants.TURN_TYPE_C)),
            distanceM = bInt(t, "next_turn_distance", 0),
            street = bStr(t, "next_turn_name"),
            afterManeuver = mapType(bInt(t, "after_nextturn_type", OsmandAidlConstants.TURN_TYPE_C)),
            afterDistanceM = bInt(t, "after_nextturn_distance", 0),
            afterStreet = bStr(t, "after_nextturn_name"),
            remainingM = info.leftDistance,
            remainingS = info.leftTime,
        )
        Log.i(TAG, "nav: $s")
        cb.onNavState(s)
    }
}

/** mainLooper handler, kept here so the link doesn't reach for a Context API. */
private val Context.mainHandler: android.os.Handler
    get() = android.os.Handler(android.os.Looper.getMainLooper())
