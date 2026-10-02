package com.navesp32.companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

// BLE central for one bonded nav device. Mirrors firmware_v2/main/ble_service.h.
// Phone is GATT client: writes POSITION, CMD and MEDIA_INFO, reads STATUS,
// subscribes to MEDIA_CMD.
//
// Android allows one outstanding GATT operation per connection and silently
// fails the rest, so every operation goes through a serial queue driven by
// the GATT callbacks. All state lives on the main thread.
@SuppressLint("MissingPermission")
class BleManager(private val ctx: Context, private val cb: Cb) {

    interface Cb {
        fun onConnState(connected: Boolean)
        fun onBonded()
        fun onStatus(batteryMv: Int, link: Int)
        fun onMediaReady() {}
        fun onMediaCmd(cmd: ByteArray) {}
    }

    companion object {
        const val SVC_UUID = "e8dffd56-e8b9-4279-9bff-1e0d2220d776"
        const val POSITION_UUID = "26a57cd7-b97d-4414-8e56-d7a5fdb3d5d0"
        const val CMD_UUID = "b18e4af0-21d2-486f-ba3c-85dbedacdf2c"
        const val STATUS_UUID = "d7865290-9778-45bd-8faa-c3f5674b2457"
        const val MEDIA_INFO_UUID = "790d1d86-2191-404d-a0f5-0e960990e190"
        const val MEDIA_CMD_UUID = "f8a580f6-aa84-4898-ad67-a66c12262ac1"
        private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        const val CMD_RECENTER: Byte = 0x01
        const val CMD_SET_ZOOM: Byte = 0x02
        const val CMD_PING: Byte = 0x03
        const val CMD_UNPAIR: Byte = 0x04
        const val CMD_SET_BRIGHTNESS: Byte = 0x05
        const val CMD_PAIR: Byte = 0x06

        // Firmware ATT MTU is 256 and its write buffer 256 B: never exceed.
        const val MAX_WRITE = 253

        private const val TAG = "BleManager"
        private const val OP_TIMEOUT_MS = 3000L
        private const val BUSY_RETRY_MS = 15L

        // 18 B LE: lat/le32 1e-7, lon, speed u16 cm/s, cog u16 0.01deg,
        // sats u8, fix u8, ts u32 ms.
        fun packPosition(latE7: Int, lonE7: Int, speedCms: Int, cogCdeg: Int,
                         sats: Int, fix: Int, tsMs: Long): ByteArray =
            ByteBuffer.allocate(18).order(ByteOrder.LITTLE_ENDIAN).apply {
                putInt(latE7)
                putInt(lonE7)
                putShort(speedCms.toShort())
                putShort(cogCdeg.toShort())
                put(sats.toByte())
                put(fix.toByte())
                putInt((tsMs and 0xFFFFFFFFL).toInt())
            }.array()
    }

    // ---- GATT operation queue ------------------------------------------

    private sealed class Op {
        // tag != null: only the newest queued op with that tag is kept.
        class Write(val chr: UUID, val data: ByteArray, val tag: String?) : Op()
        class Read(val chr: UUID) : Op()
        class Mtu(val mtu: Int) : Op()
        class Subscribe(val chr: UUID) : Op()
    }

    private val handler = Handler(Looper.getMainLooper())
    private val ops = ArrayDeque<Op>()
    private var inflight: Op? = null
    private var busyRetries = 0
    private val opTimeout = Runnable {
        Log.w(TAG, "GATT op timed out: ${inflight?.javaClass?.simpleName}")
        inflight = null
        next()
    }

    private fun enqueue(op: Op) {
        if (op is Op.Write && op.tag != null) {
            ops.removeAll { it is Op.Write && it.tag == op.tag }
        }
        ops.addLast(op)
        next()
    }

    private fun next() {
        if (inflight != null) return
        val g = gatt
        if (g == null) {
            ops.clear()
            return
        }
        val op = ops.removeFirstOrNull() ?: return
        inflight = op
        val started = when (op) {
            is Op.Write -> startWrite(g, op)
            is Op.Read -> chr(op.chr)?.let { g.readCharacteristic(it) } ?: false
            is Op.Mtu -> g.requestMtu(op.mtu)
            is Op.Subscribe -> startSubscribe(g, op.chr)
        }
        if (started) {
            busyRetries = 0
            handler.postDelayed(opTimeout, OP_TIMEOUT_MS)
        } else {
            inflight = null
            // Usually "stack busy" (e.g. a write-no-response buffer full):
            // put it back and try again shortly.
            val target = when (op) {
                is Op.Write -> op.chr
                is Op.Subscribe -> op.chr
                else -> null
            }
            if (target != null && chr(target) != null && busyRetries++ < 200) {
                ops.addFirst(op)
                handler.postDelayed({ next() }, BUSY_RETRY_MS)
            } else {
                Log.w(TAG, "GATT op dropped: ${op.javaClass.simpleName}")
                handler.post { next() }
            }
        }
    }

    private fun opDone() {
        handler.removeCallbacks(opTimeout)
        inflight = null
        next()
    }

    private fun startWrite(g: BluetoothGatt, op: Op.Write): Boolean {
        val c = chr(op.chr) ?: return false
        val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, op.data, type) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = type
            @Suppress("DEPRECATION")
            c.value = op.data
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    private fun startSubscribe(g: BluetoothGatt, uuid: UUID): Boolean {
        val c = chr(uuid) ?: return false
        if (!g.setCharacteristicNotification(c, true)) return false
        val d = c.getDescriptor(CCCD_UUID) ?: return false
        val v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, v) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            d.value = v
            @Suppress("DEPRECATION")
            g.writeDescriptor(d)
        }
    }

    // ---- connection state ----------------------------------------------

    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private val chars = HashMap<UUID, BluetoothGattCharacteristic>()
    private var token: String = ""
    private var pairSent = false
    private var reconnectTries = 0
    private var cacheRefreshed = false
    private var discovering = false
    private var rediscoverAfter = false
    private var linkUp = false
    var mtu = 23
        private set
    var mediaReady = false
        private set

    private fun chr(uuid: UUID) = chars[uuid]
    private fun chr(uuid: String) = chars[UUID.fromString(uuid)]

    private val adapter: android.bluetooth.BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE)
            as android.bluetooth.BluetoothManager).adapter

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            handler.post {
                Log.i(TAG, "conn state=$newState status=$status")
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    reconnectTries = 0
                    cacheRefreshed = false
                    handler.postDelayed({ gatt?.let { startDiscovery(it) } }, 300)
                } else {
                    close()
                    cb.onConnState(false)
                    scheduleReconnect()
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            handler.post { servicesDiscovered(g, status) }
        }

        // Firmware sends Service Changed after updates that add characteristics.
        override fun onServiceChanged(g: BluetoothGatt) {
            handler.post {
                Log.i(TAG, "service changed: rediscovering")
                rediscover(g)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            handler.post {
                Log.i(TAG, "mtu=$newMtu")
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                if (!linkUp) {
                    linkUp = true
                    cb.onConnState(true)
                }
                opDone()
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic,
                                           status: Int) {
            handler.post { opDone() }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            handler.post {
                if (d.characteristic.uuid == UUID.fromString(MEDIA_CMD_UUID) &&
                    status == BluetoothGatt.GATT_SUCCESS && chr(MEDIA_INFO_UUID) != null
                ) {
                    mediaReady = true
                    Log.i(TAG, "media channel ready")
                    cb.onMediaReady()
                }
                opDone()
            }
        }

        override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic,
                                          value: ByteArray, status: Int) {
            handler.post {
                if (status == BluetoothGatt.GATT_SUCCESS &&
                    ch.uuid == UUID.fromString(STATUS_UUID) && value.size >= 3
                ) {
                    val mv = (value[0].toInt() and 0xFF) or ((value[1].toInt() and 0xFF) shl 8)
                    cb.onStatus(mv, value[2].toInt())
                }
                opDone()
            }
        }

        @Deprecated("compat")
        override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic,
                                          status: Int) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION")
                onCharacteristicRead(g, ch, ch.value ?: ByteArray(0), status)
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic,
                                             value: ByteArray) {
            if (ch.uuid == UUID.fromString(MEDIA_CMD_UUID) && value.isNotEmpty()) {
                val copy = value.copyOf()
                handler.post { cb.onMediaCmd(copy) }
            }
        }

        @Deprecated("compat")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION")
                onCharacteristicChanged(g, ch, ch.value ?: ByteArray(0))
            }
        }
    }

    private fun startDiscovery(g: BluetoothGatt) {
        if (g != gatt) return
        if (discovering) {
            // Results of the running discovery may predate the change.
            rediscoverAfter = true
            return
        }
        discovering = true
        if (!g.discoverServices()) discovering = false
    }

    private fun servicesDiscovered(g: BluetoothGatt, status: Int) {
        if (g != gatt) return
        discovering = false
        if (rediscoverAfter) {
            rediscoverAfter = false
            startDiscovery(g)
            return
        }
        if (status != BluetoothGatt.GATT_SUCCESS) {
            g.disconnect()
            return
        }
        val svc = g.getService(UUID.fromString(SVC_UUID))
        if (svc == null) {
            Log.w(TAG, "nav service missing; disconnect")
            g.disconnect()
            return
        }
        chars.clear()
        for (c in svc.characteristics) chars[c.uuid] = c
        if (chr(POSITION_UUID) == null || chr(CMD_UUID) == null) {
            g.disconnect()
            return
        }
        if (chr(MEDIA_INFO_UUID) == null && !cacheRefreshed) {
            // Stale GATT cache from older firmware: drop it and look again.
            cacheRefreshed = true
            Log.i(TAG, "media characteristics missing: refreshing GATT cache")
            refreshCache(g)
            handler.postDelayed({ rediscover(g) }, 500)
            return
        }
        // Queued ops may reference the previous discovery's objects. The one
        // in flight (if any) is left to complete or time out on its own.
        ops.clear()
        enqueue(Op.Mtu(517))
        if (chr(STATUS_UUID) != null) enqueue(Op.Read(UUID.fromString(STATUS_UUID)))
        if (chr(MEDIA_CMD_UUID) != null) enqueue(Op.Subscribe(UUID.fromString(MEDIA_CMD_UUID)))
        ensureBonded()
    }

    private fun rediscover(g: BluetoothGatt) {
        if (g != gatt) return
        mediaReady = false
        chars.clear()
        ops.clear()
        startDiscovery(g)
    }

    private fun refreshCache(g: BluetoothGatt) {
        try {
            g.javaClass.getMethod("refresh").invoke(g)
        } catch (e: Exception) {
            Log.w(TAG, "gatt refresh: $e")
        }
    }

    // fresh = user just scanned the QR: drop any old system bond first. A
    // bond the device no longer has (reset, reflash) makes Android's
    // automatic encryption fail on every reconnect -> endless "pair again"
    // prompts. Starting from no bond always ends in a clean Just Works pair.
    fun connect(mac: String, token: String, fresh: Boolean = false) {
        if (gatt != null) return
        val dev = try {
            adapter?.getRemoteDevice(mac)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "bad mac $mac")
            return
        } ?: return
        if (fresh && dev.bondState != BluetoothDevice.BOND_NONE) {
            Log.i(TAG, "fresh pairing: removing old bond for $mac")
            removeBondOf(dev)
            handler.postDelayed({ connect(mac, token, false) }, 1500)
            return
        }
        device = dev
        this.token = token
        pairSent = token.isEmpty() // already paired -> skip pair write
        Log.i(TAG, "connect $mac autoConnect=true")
        gatt = dev.connectGatt(ctx, true, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    private fun ensureBonded() {
        val dev = device ?: return
        if (dev.bondState == BluetoothDevice.BOND_BONDED) {
            onBondReady()
        } else {
            Log.i(TAG, "createBond (Just Works)")
            dev.createBond()
            // Bond result arrives via NavService's broadcast receiver -> onBondReady()
        }
    }

    fun onBondReady() {
        cb.onBonded()
        if (!pairSent && token.isNotEmpty()) {
            pairSent = true
            writeCmd(byteArrayOf(CMD_PAIR) + token.toByteArray(Charsets.US_ASCII).take(6).toByteArray())
        }
    }

    // ---- writes (any thread) -------------------------------------------

    private fun write(uuid: String, payload: ByteArray, tag: String?): Boolean {
        if (gatt == null) return false
        handler.post {
            if (chr(uuid) != null) enqueue(Op.Write(UUID.fromString(uuid), payload, tag))
        }
        return true
    }

    // Positions are latest-wins: a fix still queued is replaced by a newer one.
    fun writePosition(payload: ByteArray) = write(POSITION_UUID, payload, "pos")

    fun writeCmd(payload: ByteArray) = write(CMD_UUID, payload, null)

    fun cmdSetBrightness(pct: Int) =
        writeCmd(byteArrayOf(CMD_SET_BRIGHTNESS, pct.coerceIn(0, 100).toByte()))

    // tag != null coalesces (media STATE); art chunks pass null to keep all.
    fun writeMedia(payload: ByteArray, tag: String?) = write(MEDIA_INFO_UUID, payload, tag)

    fun maxWrite(): Int = minOf(mtu - 3, MAX_WRITE)

    // Short bursts (album art): ask for the fastest connection interval.
    fun boost(ms: Long) {
        val g = gatt ?: return
        g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        handler.removeCallbacks(unboost)
        handler.postDelayed(unboost, ms)
    }

    private val unboost = Runnable {
        gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
    }

    // ---- teardown ------------------------------------------------------

    private fun scheduleReconnect() {
        // autoConnect=true already retries in-stack; this is a backstop for
        // hard failures (e.g. 133): full close + reconnect with backoff.
        if (device == null) return
        reconnectTries++
        val delay = minOf(60_000L, 5_000L * reconnectTries)
        handler.postDelayed({
            if (gatt == null && device != null) {
                Log.i(TAG, "backstop reconnect try $reconnectTries")
                val mac = device!!.address
                connect(mac, token)
            }
        }, delay)
    }

    fun disconnect() {
        handler.removeCallbacksAndMessages(null)
        try {
            gatt?.disconnect()
        } catch (e: Exception) {
            Log.w(TAG, "disconnect: $e")
        }
        close()
        device = null
        cb.onConnState(false)
    }

    private fun close() {
        try {
            gatt?.close()
        } catch (e: Exception) {
            Log.w(TAG, "close: $e")
        }
        gatt = null
        chars.clear()
        ops.clear()
        inflight = null
        handler.removeCallbacks(opTimeout)
        discovering = false
        rediscoverAfter = false
        linkUp = false
        mediaReady = false
        mtu = 23
    }

    fun removeBond() {
        device?.let { removeBondOf(it) }
    }

    private fun removeBondOf(dev: BluetoothDevice) {
        try {
            dev.javaClass.getMethod("removeBond").invoke(dev)
        } catch (e: Exception) {
            Log.w(TAG, "removeBond: $e")
        }
    }
}
