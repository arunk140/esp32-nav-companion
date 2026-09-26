package com.navesp32.companion

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.navesp32.companion.databinding.ActivityMainBinding

// Status, Scan QR, Connect, Unpair (confirm), service toggle, brightness slider.
class MainActivity : AppCompatActivity() {

    private lateinit var b: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var brightJob: Runnable? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)
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

    private fun refresh() {
        val bond = Prefs.loadBond(this)
        b.status.text = if (bond == null) {
            "Not paired — tap Scan QR"
        } else {
            "Device: ${bond.name} (${bond.mac})"
        }
        b.serviceToggle.isChecked = Prefs.serviceEnabled(this, bond != null)
        b.btnConnect.isEnabled = bond != null
        b.btnUnpair.isEnabled = bond != null
    }

    private fun hasCam() = ActivityCompat.checkSelfPermission(
        this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

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
        val missing = need.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), 41)
        }
    }

    private fun toast(m: String) {
        Toast.makeText(this, m, Toast.LENGTH_SHORT).show()
    }
}
