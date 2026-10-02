package com.navesp32.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat

// Foreground service: BLE auto-connect + location stream.
// START = connect (bonded device) + stream. STOP = disconnect + stop stream.
// UNPAIR = CMD 0x04 + removeBond + clear prefs + stop.
class NavService : Service() {

    companion object {
        const val ACTION_START = "com.navesp32.companion.START"
        const val ACTION_STOP = "com.navesp32.companion.STOP"
        const val ACTION_UNPAIR = "com.navesp32.companion.UNPAIR"
        const val ACTION_BRIGHTNESS = "com.navesp32.companion.BRIGHTNESS"
        const val ACTION_SLEEP = "com.navesp32.companion.SLEEP"
        const val ACTION_THEME = "com.navesp32.companion.THEME"
        const val ACTION_THEME_STATE = "com.navesp32.companion.THEME_STATE"
        const val EXTRA_DARK_MAP = "darkMap"
        const val EXTRA_BRIGHTNESS = "brightness"
        const val EXTRA_FRESH = "fresh"
        private const val CH = "nav"
        private const val NOTIF_ID = 41
        private const val TAG = "NavService"

        const val ACTION_STATE = "com.navesp32.companion.STATE"
        const val EXTRA_STATE = "state"

        // Live link state, readable by the UI even when the service is not
        // running. Broadcast as ACTION_STATE as well, so a resumed screen
        // picks up changes.
        const val OFFLINE = "offline"
        const val CONNECTING = "connecting"
        const val CONNECTED = "connected"
        const val ASLEEP = "asleep"

        @Volatile
        var linkState: String = OFFLINE
            private set

        fun stateLabel(st: String): String = when (st) {
            CONNECTED -> "Connected"
            CONNECTING -> "Connecting..."
            ASLEEP -> "Asleep - touch the device to wake it"
            else -> "Searching for the device..."
        }

        fun start(ctx: Context, fresh: Boolean = false) {
            ctx.startForegroundService(
                Intent(ctx, NavService::class.java).setAction(ACTION_START)
                    .putExtra(EXTRA_FRESH, fresh)
            )
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, NavService::class.java).setAction(ACTION_STOP))
        }
    }

    private var ble: BleManager? = null
    private var loc: LocationStream? = null
    private var media: MediaBridge? = null
    private var nav: NavBridge? = null
    private var osmand: OsmAndLink? = null
    private var call: CallBridge? = null
    private var connected = false
    private val main = Handler(Looper.getMainLooper())

    private val bondRx = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            val dev: BluetoothDevice? =
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
            Log.i(TAG, "bond ${dev?.address} -> $state")
            if (state == BluetoothDevice.BOND_BONDED) ble?.onBondReady()
        }
    }

    // A Bluetooth off/on (toggle, airplane mode) kills the GATT client and
    // autoConnect never recovers: rebuild the connection once the adapter is on.
    private val adapterRx = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_TURNING_OFF -> {
                    Log.i(TAG, "bluetooth turning off")
                    loc?.stop()
                    call?.stop()
                    ble?.disconnect()
                }
                BluetoothAdapter.STATE_ON -> {
                    val bond = Prefs.loadBond(this@NavService) ?: return
                    if (!Prefs.serviceEnabled(this@NavService)) return
                    Log.i(TAG, "bluetooth on: reconnecting")
                    ble?.disconnect()
                    main.postDelayed({ ble?.connect(bond.mac, bond.token) }, 1000)
                    setState(CONNECTING)
                }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH, "Nav link", NotificationManager.IMPORTANCE_LOW)
        )
        registerReceiver(bondRx, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
        registerReceiver(adapterRx, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
        nav = NavBridge(object : NavBridge.Out {
            override fun navWrite(data: ByteArray, tag: String?) {
                ble?.writeNav(data, tag)
            }

            override fun navLinkReady() = ble?.mediaReady == true
        })
        osmand = OsmAndLink(this, object : OsmAndLink.Cb {
            override fun onNavState(s: OsmAndLink.NavState) {
                nav?.send(s)
            }

            override fun onConsentNeeded() {
                nav?.send(
                    OsmAndLink.NavState(false, 0, 0, "", 0, 0, "", 0, 0, 0.0, 0.0, false),
                    noAccess = true
                )
                updateNotif("Enable Nav Companion in OsmAnd -> Plugins")
            }

            override fun onLinkState(bound: Boolean) {
                if (!bound) nav?.sendStopped()
            }
        })
        media = MediaBridge(this, object : MediaBridge.Out {
            override fun mediaWrite(data: ByteArray, tag: String?) {
                ble?.writeMedia(data, tag)
            }
            override fun mediaMaxWrite() = ble?.maxWrite() ?: 20
            override fun mediaBoost(ms: Long) {
                ble?.boost(ms)
            }
            override fun mediaLinkReady() = ble?.mediaReady == true
        })
        call = CallBridge(this, object : CallBridge.Out {
            override fun callWrite(data: ByteArray, tag: String?) {
                ble?.writeCall(data, tag)
            }
            override fun callLinkReady() = ble?.callReady == true
        })
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(bondRx)
            unregisterReceiver(adapterRx)
        } catch (e: Exception) {
            Log.w(TAG, "$e")
        }
        loc?.stop()
        media?.stop()
        osmand?.stop()
        call?.stop()
        ble?.disconnect()
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIF_ID, notif("Starting…"))
        when (intent?.action) {
            ACTION_STOP -> {
                Prefs.setServiceEnabled(this, false)
                asleep = false
                loc?.stop()
                media?.stop()
                osmand?.stop()
                call?.stop()
                ble?.disconnect()
                setState(OFFLINE)
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_UNPAIR -> {
                // Writes are queued: give UNPAIR time to reach the device
                // before tearing the link down.
                ble?.writeCmd(byteArrayOf(BleManager.CMD_UNPAIR))
                Prefs.clearBond(this)
                loc?.stop()
                media?.stop()
                call?.stop()
                main.postDelayed({
                    ble?.removeBond()
                    ble?.disconnect()
                }, 800)
                asleep = false
                setState(OFFLINE)
                return START_STICKY
            }
            ACTION_SLEEP -> {
                if (!connected) {
                    setState(linkState)
                    return START_STICKY
                }
                // The device is about to power off: say so instead of letting
                // the dropped link look like a connection problem. Do NOT
                // disconnect here - that would cancel this very write and the
                // reconnect backoff. The link drops on its own when the board
                // sleeps, and BleManager keeps retrying (5 s -> 60 s) so the
                // board is picked up again as soon as it wakes on touch.
                asleep = true
                setState(ASLEEP)
                ble?.writeCmd(byteArrayOf(BleManager.CMD_SLEEP))
                loc?.stop()
                call?.stop()
                return START_STICKY
            }
            ACTION_THEME -> {
                val dark = intent.getBooleanExtra(EXTRA_DARK_MAP, false)
                Prefs.setDarkMap(this, dark)
                ble?.cmdSetTheme(dark)
                return START_STICKY
            }
            ACTION_BRIGHTNESS -> {
                val v = intent.getIntExtra(EXTRA_BRIGHTNESS, -1)
                if (v >= 0) {
                    Prefs.setBrightness(this, v)
                    ble?.cmdSetBrightness(v)
                }
                return START_STICKY
            }
            else -> { /* START below */ }
        }
        Prefs.setServiceEnabled(this, true)
        val bond = Prefs.loadBond(this)
        if (bond == null) {
            asleep = false
            setState(OFFLINE)
            return START_STICKY
        }
        if (ble == null) {
            ble = BleManager(this, object : BleManager.Cb {
                override fun onConnState(c: Boolean) {
                    connected = c
                    if (c) asleep = false
                    setState(if (c) CONNECTED else if (asleep) ASLEEP else CONNECTING)
                    if (c) {
                        loc?.start()
                    } else {
                        loc?.stop()
                        // No link, no guidance: forget the bridge's sticky
                        // street so a reconnect can't reuse a stale name, and
                        // stop polling OsmAnd while the board cannot use it.
                        nav?.reset()
                        osmand?.stop()
                        call?.stop()
                    }
                }

                override fun onBonded() {
                    updateNotif("Paired")
                }

                override fun onStatus(batteryMv: Int, link: Int, darkMap: Boolean) {
                    Log.i(TAG, "status link=$link darkMap=$darkMap")
                    // The device is the truth: show what it is actually using.
                    if (Prefs.darkMap(this@NavService) != darkMap) {
                        Prefs.setDarkMap(this@NavService, darkMap)
                        sendBroadcast(Intent(ACTION_THEME_STATE)
                            .setPackage(packageName).putExtra(EXTRA_DARK_MAP, darkMap))
                    }
                }

                override fun onMediaReady() {
                    media?.start()
                    media?.sendAll()
                    // Turn-by-turn only makes sense over an up link, and
                    // binding OsmAnd while asleep is wasteful.
                    osmand?.start()
                }

                override fun onMediaCmd(cmd: ByteArray) {
                    media?.handle(cmd)
                }

                override fun onCallReady() {
                    call?.start()
                }

                override fun onCallCmd(cmd: ByteArray) {
                    call?.handle(cmd)
                }
            })
        }
        if (loc == null) {
            loc = LocationStream(this) { payload ->
                if (connected) ble?.writePosition(payload)
            }
        }
        val fresh = intent?.getBooleanExtra(EXTRA_FRESH, false) == true
        if (fresh) ble?.disconnect()  // drop any attempt using old credentials
        ble?.connect(bond.mac, bond.token, fresh)
        ble?.cmdSetTheme(Prefs.darkMap(this))
        setState(CONNECTING)
        return START_STICKY
    }

    private fun notif(text: String): Notification {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CH)
            .setContentTitle("Nav Companion")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    private var asleep = false

    // Single place that publishes the link state: notification + broadcast.
    private fun setState(st: String) {
        if (linkState == st) return
        linkState = st
        val bond = Prefs.loadBond(this)
        updateNotif(
            when (st) {
                CONNECTED -> "Connected to ${bond?.name ?: "device"}"
                ASLEEP -> "Asleep - touch the device to wake it"
                CONNECTING -> "Connecting..."
                else -> "Searching for ${bond?.name ?: "device"}..."
            }
        )
        sendBroadcast(
            Intent(ACTION_STATE).setPackage(packageName).putExtra(EXTRA_STATE, st)
        )
    }

    private fun updateNotif(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, notif(text))
    }
}
