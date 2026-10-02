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
        const val EXTRA_BRIGHTNESS = "brightness"
        const val EXTRA_FRESH = "fresh"
        private const val CH = "nav"
        private const val NOTIF_ID = 41
        private const val TAG = "NavService"

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
                    ble?.disconnect()
                }
                BluetoothAdapter.STATE_ON -> {
                    val bond = Prefs.loadBond(this@NavService) ?: return
                    if (!Prefs.serviceEnabled(this@NavService)) return
                    Log.i(TAG, "bluetooth on: reconnecting")
                    ble?.disconnect()
                    main.postDelayed({ ble?.connect(bond.mac, bond.token) }, 1000)
                    updateNotif("Connecting…")
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
        ble?.disconnect()
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIF_ID, notif("Starting…"))
        when (intent?.action) {
            ACTION_STOP -> {
                Prefs.setServiceEnabled(this, false)
                loc?.stop()
                media?.stop()
                ble?.disconnect()
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
                main.postDelayed({
                    ble?.removeBond()
                    ble?.disconnect()
                }, 800)
                updateNotif("Unpaired")
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
            updateNotif("Not paired — scan QR")
            return START_STICKY
        }
        if (ble == null) {
            ble = BleManager(this, object : BleManager.Cb {
                override fun onConnState(c: Boolean) {
                    connected = c
                    updateNotif(if (c) "Connected to ${bond.name}" else "Connecting…")
                    if (c) loc?.start() else loc?.stop()
                }

                override fun onBonded() {
                    updateNotif("Paired")
                }

                override fun onStatus(batteryMv: Int, link: Int) {
                    Log.i(TAG, "status link=$link")
                }

                override fun onMediaReady() {
                    media?.start()
                    media?.sendAll()
                }

                override fun onMediaCmd(cmd: ByteArray) {
                    media?.handle(cmd)
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
        updateNotif("Connecting…")
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

    private fun updateNotif(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIF_ID, notif(text))
    }
}
