package com.navesp32.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.IBinder
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
        private const val CH = "nav"
        private const val NOTIF_ID = 41
        private const val TAG = "NavService"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, NavService::class.java).setAction(ACTION_START))
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, NavService::class.java).setAction(ACTION_STOP))
        }
    }

    private var ble: BleManager? = null
    private var loc: LocationStream? = null
    private var connected = false

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

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CH, "Nav link", NotificationManager.IMPORTANCE_LOW)
        )
        registerReceiver(bondRx, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(bondRx)
        } catch (e: Exception) {
            Log.w(TAG, "$e")
        }
        loc?.stop()
        ble?.disconnect()
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIF_ID, notif("Starting…"))
        when (intent?.action) {
            ACTION_STOP -> {
                Prefs.setServiceEnabled(this, false)
                loc?.stop()
                ble?.disconnect()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_UNPAIR -> {
                ble?.writeCmd(byteArrayOf(BleManager.CMD_UNPAIR))
                ble?.removeBond()
                Prefs.clearBond(this)
                loc?.stop()
                ble?.disconnect()
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
            })
        }
        if (loc == null) {
            loc = LocationStream(this) { payload ->
                if (connected) ble?.writePosition(payload)
            }
        }
        ble?.connect(bond.mac, bond.token)
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
