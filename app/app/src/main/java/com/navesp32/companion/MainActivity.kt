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
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.navesp32.companion.databinding.ActivityMainBinding

// Status, Scan QR, Connect, Unpair (confirm), service toggle, brightness,
// routing source, destination list (fed by Google Maps shares).
class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "MainActivity"
        private const val SRC_OSMAND = "osmAnd"
        private const val SRC_VALHALLA = "valhalla"
    }

    private lateinit var b: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var brightJob: Runnable? = null

    // Set while refresh()/startNav() writes controls, so listeners do not fire
    // commands straight back at the device.
    private var updating = false
    private var osmand: OsmAndLink? = null

    private val stateRx = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context?, intent: Intent?) = refresh()
    }

    private val navStatusRx = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context?, intent: Intent?) {
            intent?.getStringExtra(NavService.EXTRA_NAV_STATUS)?.let { b.navStatus.text = it }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)
        ContextCompat.registerReceiver(
            this, stateRx, IntentFilter(NavService.ACTION_STATE),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        ContextCompat.registerReceiver(
            this, navStatusRx, IntentFilter(NavService.ACTION_NAV_STATUS),
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
                    override fun onNavState(s: NavState) {
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
        b.sourceGroup.setOnCheckedChangeListener { _, checkedId ->
            if (updating) return@setOnCheckedChangeListener
            val src = if (checkedId == R.id.radioValhalla) SRC_VALHALLA else SRC_OSMAND
            Prefs.setNavSource(this, src)
            // Switching source ends whatever the previous one was doing.
            startService(Intent(this, NavService::class.java).setAction(NavService.ACTION_NAV_STOP))
            b.navStatus.text = ""
        }
        b.valhallaUrl.setText(Prefs.valhallaUrl(this))
        b.valhallaUrl.setOnFocusChangeListener { _, focused -> if (!focused) saveUrl() }
        b.btnStopNav.setOnClickListener {
            startService(Intent(this, NavService::class.java).setAction(NavService.ACTION_NAV_STOP))
            b.navStatus.text = "Navigation stopped"
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

        handleIntent(intent)
        refreshDestinations()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        refresh()
        refreshDestinations()
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(stateRx)
            unregisterReceiver(navStatusRx)
        } catch (e: Exception) {
            Log.w(TAG, "unregister: $e")
        }
        osmand?.stop()
        osmand = null
        super.onDestroy()
    }

    // ---- destination ingest (Google Maps share) -------------------------

    private fun handleIntent(intent: Intent?) {
        when (intent?.action) {
            Intent.ACTION_SEND -> ingestShare(
                intent.getStringExtra(Intent.EXTRA_TEXT),
                intent.getStringExtra(Intent.EXTRA_SUBJECT),
            )
            Intent.ACTION_VIEW -> {
                val data = intent.dataString ?: return
                if (data.startsWith("geo:")) {
                    MapsShareParser.parseGeo(data)?.let { addParsed(it) }
                } else {
                    ingestShare(data, null)
                }
            }
        }
    }

    private fun ingestShare(text: String?, subject: String?) {
        val parsed = MapsShareParser.parseShare(text, subject)
        if (parsed == null) {
            toast("Couldn't read that share")
            return
        }
        if (parsed.lat != null && parsed.lon != null) {
            addParsed(parsed)
            return
        }
        val url = parsed.url
        if (url != null && MapsShareParser.isShortener(url)) {
            b.navStatus.text = "Resolving link…"
            Thread {
                val resolved = LinkResolver.resolve(url)
                val full = resolved?.let { MapsShareParser.parseUrl(it, parsed.name) }
                runOnUiThread {
                    if (full?.lat != null && full.lon != null) {
                        addParsed(full)
                    } else {
                        b.navStatus.text = ""
                        toast("Couldn't read coordinates from that link")
                    }
                }
            }.start()
        } else {
            toast("No coordinates in that share")
        }
    }

    private fun addParsed(p: MapsShareParser.Parsed) {
        val lat = p.lat ?: return
        val lon = p.lon ?: return
        val place = PlaceStore.addRecent(this, p.name, lat, lon)
        refreshDestinations()
        toast("Added ${place.name}")
    }

    // ---- destination list ------------------------------------------------

    private fun refreshDestinations() {
        b.destList.removeAllViews()
        val places = PlaceStore.loadForDisplay(this)
        if (places.isEmpty()) {
            val t = android.widget.TextView(this)
            t.text = "No places yet — share one from Google Maps."
            t.textSize = 12f
            b.destList.addView(t)
            return
        }
        for (p in places) {
            val btn = Button(this)
            btn.text = (if (p.saved) "★ " else "") + p.name
            btn.isAllCaps = false
            btn.setOnClickListener { startNav(p) }
            btn.setOnLongClickListener {
                PlaceStore.toggleSaved(this, p.id)
                refreshDestinations()
                toast(if (p.saved) "Unsaved" else "Saved")
                true
            }
            b.destList.addView(
                btn,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            )
        }
    }

    private fun startNav(p: Place) {
        if (Prefs.loadBond(this) == null) {
            toast("Pair a device first")
            return
        }
        updating = true
        b.radioValhalla.isChecked = true
        updating = false
        Prefs.setNavSource(this, SRC_VALHALLA)
        PlaceStore.addRecent(this, p.name, p.lat, p.lon)
        refreshDestinations()
        Prefs.setServiceEnabled(this, true)
        b.serviceToggle.isChecked = true
        // Only (re)start the service when it is not already connected; a
        // redundant ACTION_START resets the visible link state to "Connecting…"
        // even though the link is up.
        if (NavService.linkState != NavService.CONNECTED) NavService.start(this)
        startService(
            Intent(this, NavService::class.java)
                .setAction(NavService.ACTION_NAV_START)
                .putExtra(NavService.EXTRA_DEST_NAME, p.name)
                .putExtra(NavService.EXTRA_DEST_LAT_E7, (p.lat * 1e7).toInt())
                .putExtra(NavService.EXTRA_DEST_LON_E7, (p.lon * 1e7).toInt())
        )
        b.navStatus.text = "Routing to ${p.name}…"
    }

    private fun saveUrl() {
        Prefs.setValhallaUrl(this, b.valhallaUrl.text.toString())
        toast("Server URL saved")
    }

    private fun refresh() {
        val bond = Prefs.loadBond(this)
        val st = NavService.linkState
        updating = true
        b.darkMapToggle.isChecked = Prefs.darkMap(this)
        b.radioValhalla.isChecked = Prefs.navSource(this) == SRC_VALHALLA
        b.radioOsmand.isChecked = Prefs.navSource(this) != SRC_VALHALLA
        if (!b.valhallaUrl.hasFocus()) b.valhallaUrl.setText(Prefs.valhallaUrl(this))
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
