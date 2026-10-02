package com.navesp32.companion

import android.Manifest
import android.content.Context
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.navesp32.companion.databinding.ActivityMainBinding

// Status, Scan QR, Connect, Unpair (confirm), service toggle, brightness slider.
class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "MainActivity"
    }

    private lateinit var b: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var brightJob: Runnable? = null

    // Set while refresh() writes the switch, so its listener does not fire a
    // command straight back at the device.
    private var updating = false
    private var osmand: OsmAndLink? = null

    private val stateRx = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context?, intent: Intent?) = refresh()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)
        ContextCompat.registerReceiver(
            this, stateRx, IntentFilter(NavService.ACTION_STATE),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        requestPerms()
        refresh()

        b.btnScan.setOnClickListener {
            if (!hasCam()) {
                requestPerms()
                return@setOnClickListener
            }
            startActivity(Intent(this, QrScannerActivity::class.java))
        }
        b.btnConnect.setOnClickListener {
            if (Prefs.loadBond(this) == null) {
                toast("Scan QR first")
                return@setOnClickListener
            }
            Prefs.setServiceEnabled(this, true)
            b.serviceToggle.isChecked = true
            NavService.start(this)
            refresh()
        }
        b.btnUnpair.setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("Unpair?")
                .setMessage("Disconnects and forgets the nav device on both ends.")
                .setPositiveButton("Unpair") { _, _ ->
                    startService(Intent(this, NavService::class.java)
                        .setAction(NavService.ACTION_UNPAIR))
                    refresh()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        b.btnProbeOsmand.setOnClickListener {
            if (osmand != null) {
                toast("Reading OsmAnd...")
                b.navDump.text = "reading..."
                osmand!!.pollNow { text -> b.navDump.text = text }
            } else {
                toast("Connecting to OsmAnd...")
                osmand = OsmAndLink(this, object : OsmAndLink.Cb {
                    override fun onNavState(s: OsmAndLink.NavState) {
                        b.navDump.text =
                            "active=${s.active}\nturn=${s.maneuver} in ${s.distanceM} m\n" +
                                "into: ${s.street}\nthen ${s.afterManeuver} in ${s.afterDistanceM} m\n" +
                                "${s.remainingM} m / ${s.remainingS} s left"
                    }

                    override fun onConsentNeeded() {
                        b.navDump.text = "OsmAnd has us disabled:\nenable this app in\nOsmAnd -> Plugins"
                    }

                    override fun onLinkState(bound: Boolean) {
                        if (!bound) b.navDump.text = "OsmAnd not reachable"
                    }
                }).also { it.start() }
            }
        }
        b.darkMapToggle.setOnCheckedChangeListener { _, on ->
            if (updating) return@setOnCheckedChangeListener
            Prefs.setDarkMap(this, on)
            startService(Intent(this, NavService::class.java)
                .setAction(NavService.ACTION_THEME)
                .putExtra(NavService.EXTRA_DARK_MAP, on))
        }
        b.btnSleep.setOnClickListener {
            if (Prefs.loadBond(this) == null) {
                toast("Scan QR first")
                return@setOnClickListener
            }
            startService(Intent(this, NavService::class.java)
                .setAction(NavService.ACTION_SLEEP))
            toast("Device sleeping - touch its screen to wake")
        }
        b.btnMedia.setOnClickListener {
            // Media sessions of other apps are only readable with
            // Notification access; Android has no runtime prompt for it.
            startActivity(Intent(android.provider.Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        b.btnCalls.setOnClickListener {
            val missing = CallBridge.PERMISSIONS.filter { !hasPerm(it) }
            if (missing.isNotEmpty()) {
                ActivityCompat.requestPermissions(this, missing.toTypedArray(), 42)
            } else {
                toast("Call controls enabled")
            }
        }
        b.serviceToggle.setOnCheckedChangeListener { _, on ->
            if (on) {
                Prefs.setServiceEnabled(this, true)
                NavService.start(this)
            } else {
                NavService.stop(this)
            }
            refresh()
        }
        b.brightness.progress = Prefs.brightness(this)
        b.brightness.setOnSeekBarChangeListener(object :
            android.widget.SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(s: android.widget.SeekBar, v: Int, fromUser: Boolean) {
                if (!fromUser) return
                brightJob?.let { handler.removeCallbacks(it) }
                brightJob = Runnable {
                    Prefs.setBrightness(this@MainActivity, v)
                    startService(Intent(this@MainActivity, NavService::class.java)
                        .setAction(NavService.ACTION_BRIGHTNESS)
                        .putExtra(NavService.EXTRA_BRIGHTNESS, v))
                }
                handler.postDelayed(brightJob!!, 200)
            }

            override fun onStartTrackingTouch(s: android.widget.SeekBar) {}
            override fun onStopTrackingTouch(s: android.widget.SeekBar) {}
        })
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(stateRx)
        } catch (e: Exception) {
            Log.w(TAG, "stateRx: $e")
        }
        osmand?.stop()
        osmand = null
        super.onDestroy()
    }

    private fun refresh() {
        val bond = Prefs.loadBond(this)
        val st = NavService.linkState
        updating = true
        b.darkMapToggle.isChecked = Prefs.darkMap(this)
        updating = false
        b.status.text = if (bond == null) {
            "Not paired — tap Scan QR"
        } else {
            "${bond.name} (${bond.mac})\n${NavService.stateLabel(st)}"
        }
        b.linkDot.setBackgroundResource(
            when (st) {
                NavService.CONNECTED -> R.drawable.dot_green
                NavService.CONNECTING -> R.drawable.dot_amber
                NavService.ASLEEP -> R.drawable.dot_blue
                else -> R.drawable.dot_gray
            }
        )
        b.serviceToggle.isChecked = Prefs.serviceEnabled(this, bond != null)
        b.btnConnect.isEnabled = bond != null
        b.btnUnpair.isEnabled = bond != null
        b.btnSleep.isEnabled = bond != null && st == NavService.CONNECTED
        val media = MediaListenerService.hasAccess(this)
        b.btnMedia.text = if (media) "Media controls: enabled" else "Enable media controls"
        val calls = CallBridge.PERMISSIONS.all { hasPerm(it) }
        b.btnCalls.text = if (calls) "Call controls: enabled" else "Enable call controls"
    }

    private fun hasCam() = ActivityCompat.checkSelfPermission(
        this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun hasPerm(p: String) = ActivityCompat.checkSelfPermission(
        this, p) == PackageManager.PERMISSION_GRANTED

    private fun requestPerms() {
        val need = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= 31) {
            need += Manifest.permission.BLUETOOTH_SCAN
            need += Manifest.permission.BLUETOOTH_CONNECT
        }
        need += Manifest.permission.CAMERA
        if (Build.VERSION.SDK_INT >= 33) {
            need += Manifest.permission.POST_NOTIFICATIONS
        }
        need += CallBridge.PERMISSIONS
        val missing = need.filter { !hasPerm(it) }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), 41)
        }
    }

    private fun toast(m: String) {
        Toast.makeText(this, m, Toast.LENGTH_SHORT).show()
    }
}
