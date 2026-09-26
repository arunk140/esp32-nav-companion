package com.navesp32.companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

// BLE central for one bonded nav device. Mirrors firmware/main/ble/ble_service.h.
// Phone is GATT client: writes POSITION + CMD, reads STATUS once per connect.
@SuppressLint("MissingPermission")
class BleManager(private val ctx: Context, private val cb: Cb) {

    interface Cb {
        fun onConnState(connected: Boolean)
        fun onBonded()
        fun onStatus(batteryMv: Int, link: Int)
    }

    companion object {
        const val SVC_UUID = "e8dffd56-e8b9-4279-9bff-1e0d2220d776"
        const val POSITION_UUID = "26a57cd7-b97d-4414-8e56-d7a5fdb3d5d0"
        const val CMD_UUID = "b18e4af0-21d2-486f-ba3c-85dbedacdf2c"
        const val STATUS_UUID = "d7865290-9778-45bd-8faa-c3f5674b2457"

        const val CMD_RECENTER: Byte = 0x01
        const val CMD_SET_ZOOM: Byte = 0x02
        const val CMD_PING: Byte = 0x03
        const val CMD_UNPAIR: Byte = 0x04
        const val CMD_SET_BRIGHTNESS: Byte = 0x05
        const val CMD_PAIR: Byte = 0x06

        private const val TAG = "BleManager"

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

    private val handler = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var chrPos: BluetoothGattCharacteristic? = null
    private var chrCmd: BluetoothGattCharacteristic? = null
    private var token: String = ""
    private var pairSent = false
    private var reconnectTries = 0

    private val adapter: android.bluetooth.BluetoothAdapter? =
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE)
            as android.bluetooth.BluetoothManager).adapter

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "conn state=$newState status=$status")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                reconnectTries = 0
                handler.postDelayed({ g.discoverServices() }, 300)
            } else {
                close()
                cb.onConnState(false)
                scheduleReconnect()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
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
            chrPos = svc.getCharacteristic(UUID.fromString(POSITION_UUID))
            chrCmd = svc.getCharacteristic(UUID.fromString(CMD_UUID))
            val chrStatus = svc.getCharacteristic(UUID.fromString(STATUS_UUID))
            if (chrPos == null || chrCmd == null) {
                g.disconnect()
                return
            }
            g.requestMtu(517)
            if (chrStatus != null) g.readCharacteristic(chrStatus)
            ensureBonded()
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            Log.i(TAG, "mtu=$mtu")
            cb.onConnState(true)
        }

        override fun onCharacteristicRead(g: BluetoothGatt,
                                          ch: BluetoothGattCharacteristic,
                                          value: ByteArray, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS &&
                ch.uuid == UUID.fromString(STATUS_UUID) && value.size >= 3
            ) {
                val mv = (value[0].toInt() and 0xFF) or ((value[1].toInt() and 0xFF) shl 8)
                cb.onStatus(mv, value[2].toInt())
            }
        }

        @Deprecated("compat")
        override fun onCharacteristicRead(g: BluetoothGatt,
                                          ch: BluetoothGattCharacteristic, status: Int) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION")
                onCharacteristicRead(g, ch, ch.value ?: ByteArray(0), status)
            }
        }
    }

    fun connect(mac: String, token: String) {
        if (gatt != null) return
        val dev = try {
            adapter?.getRemoteDevice(mac)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "bad mac $mac")
            return
        } ?: return
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

    fun writePosition(payload: ByteArray): Boolean {
        val g = gatt ?: return false
        val c = chrPos ?: return false
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, payload, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) ==
                BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            c.value = payload
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    fun writeCmd(payload: ByteArray): Boolean {
        val g = gatt ?: return false
        val c = chrCmd ?: return false
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, payload, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) ==
                BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            c.value = payload
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    fun cmdSetBrightness(pct: Int) =
        writeCmd(byteArrayOf(CMD_SET_BRIGHTNESS, pct.coerceIn(0, 100).toByte()))

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
        chrPos = null
        chrCmd = null
    }

    fun removeBond() {
        try {
            device?.javaClass?.getMethod("removeBond")?.invoke(device)
        } catch (e: Exception) {
            Log.w(TAG, "removeBond: $e")
        }
    }
}
