package com.navesp32.companion

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.ContactsContract
import android.telecom.TelecomManager
import android.telephony.PhoneStateListener
import android.telephony.TelephonyManager
import android.util.Log
import androidx.core.content.ContextCompat
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors

/**
 * Mirrors NAV_CHR_CALL_INFO / NAV_CHR_CALL_CMD in firmware_v2/main/ble_service.h.
 *
 * Phone -> device (CALL_INFO, write-no-rsp):
 *   0x01 RING: 0x01, name, 0x00, number  (UTF-8; either field may be empty)
 *   0x02 END:  0x02
 * Device -> phone (CALL_CMD, notify):
 *   1 answer, 2 reject
 *
 * The device shows a full-screen page on RING and hides it on END.
 *
 * Permissions, and what degrades without them:
 *   READ_PHONE_STATE  - call state. Without it nothing arrives.
 *   READ_CALL_LOG     - incoming number. Without it the number is empty.
 *   READ_CONTACTS     - caller name. Without it the name is empty.
 *   ANSWER_PHONE_CALLS- answer/reject. On API < 28 endCall() does not exist,
 *                       so reject is a no-op there.
 *
 * NOTE: PhoneStateListener is deprecated since API 31 but remains the only
 * public API that hands a normal app the incoming number; the replacement
 * (CallScreeningService) needs the call-screening role, and TelephonyCallback
 * does not expose the number. We keep the deprecated listener on purpose.
 * endCall() is likewise deprecated but still the supported way for a companion
 * app to reject a ringing call.
 */
@Suppress("DEPRECATION")
class CallBridge(private val ctx: Context, private val out: Out) {

    interface Out {
        fun callWrite(data: ByteArray, tag: String?)
        fun callLinkReady(): Boolean
    }

    companion object {
        private const val TAG = "CallBridge"
        private const val RING: Byte = 0x01
        private const val END: Byte = 0x02
        private const val CMD_ANSWER = 1
        private const val CMD_REJECT = 2
        private const val NAME_MAX = 48
        private const val NUMBER_MAX = 32

        // Also the set MainActivity prompts for.
        val PERMISSIONS = arrayOf(
            Manifest.permission.READ_PHONE_STATE,
            Manifest.permission.READ_CALL_LOG,
            Manifest.permission.READ_CONTACTS,
            Manifest.permission.ANSWER_PHONE_CALLS,
        )
    }

    private val tm = ctx.getSystemService(TelephonyManager::class.java)
    private val telecom = ctx.getSystemService(TelecomManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()

    private var running = false
    private var lastState = TelephonyManager.CALL_STATE_IDLE
    // Bumped on every state change; a contacts lookup in flight only sends if
    // it is still the current state (the call may have ended meanwhile).
    private var gen = 0

    @Suppress("DEPRECATION")
    private val listener = object : PhoneStateListener() {
        override fun onCallStateChanged(state: Int, phoneNumber: String?) {
            onState(state, phoneNumber ?: "")
        }
    }

    fun start() {
        if (running) return
        running = true
        try {
            @Suppress("DEPRECATION")
            tm.listen(listener, PhoneStateListener.LISTEN_CALL_STATE)
        } catch (e: Exception) {
            Log.w(TAG, "listen: $e")
            running = false
        }
    }

    fun stop() {
        if (!running) return
        running = false
        try {
            @Suppress("DEPRECATION")
            tm.listen(listener, PhoneStateListener.LISTEN_NONE)
        } catch (e: Exception) {
            Log.w(TAG, "listen stop: $e")
        }
        sendEnd()
        lastState = TelephonyManager.CALL_STATE_IDLE
    }

    private fun onState(state: Int, number: String) {
        if (state == lastState) return
        lastState = state
        if (state == TelephonyManager.CALL_STATE_RINGING) {
            // Contacts lookup is a blocking query: keep it off the main
            // thread. The device shows the number immediately if the name has
            // to wait; see the two-step send below.
            val g = ++gen
            val n = number
            worker.execute {
                val name = if (has(Manifest.permission.READ_CONTACTS)) lookupName(n) else ""
                main.post { if (g == gen) sendRing(name, n) }
            }
        } else {
            // OFFHOOK (answered here or elsewhere) or IDLE (missed/rejected):
            // no longer ringing, so the device hides the page.
            gen++
            sendEnd()
        }
    }

    private fun sendRing(name: String, number: String) {
        if (!out.callLinkReady()) return
        Log.i(TAG, "ringing ${number.ifEmpty { "(hidden)" }} name=${name.ifEmpty { "-" }}")
        val n = name.take(NAME_MAX).toByteArray(Charsets.UTF_8)
        val num = number.take(NUMBER_MAX).toByteArray(Charsets.UTF_8)
        val b = ByteBuffer.allocate(3 + n.size + num.size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(RING)
            put(n)
            put(0)
            put(num)
        }
        out.callWrite(b.array(), "call")
    }

    private fun sendEnd() {
        if (!out.callLinkReady()) return
        out.callWrite(byteArrayOf(END), "call")
    }

    fun handle(cmd: ByteArray) {
        if (cmd.isEmpty()) return
        when (cmd[0].toInt()) {
            CMD_ANSWER -> {
                if (!has(Manifest.permission.ANSWER_PHONE_CALLS)) {
                    Log.w(TAG, "answer: ANSWER_PHONE_CALLS not granted")
                    return
                }
                try {
                    @Suppress("DEPRECATION")
                    telecom.acceptRingingCall()
                } catch (e: Exception) {
                    Log.w(TAG, "answer: $e")
                }
            }
            CMD_REJECT -> {
                if (Build.VERSION.SDK_INT < 28 || !has(Manifest.permission.ANSWER_PHONE_CALLS)) {
                    Log.w(TAG, "reject unsupported on this build/permission")
                    return
                }
                try {
                    telecom.endCall()
                } catch (e: Exception) {
                    Log.w(TAG, "reject: $e")
                }
            }
        }
    }

    private fun has(p: String) =
        ContextCompat.checkSelfPermission(ctx, p) == PackageManager.PERMISSION_GRANTED

    private fun lookupName(number: String): String {
        if (number.isEmpty()) return ""
        return try {
            val uri = Uri.withAppendedPath(
                ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number)
            )
            ctx.contentResolver.query(
                uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null
            )?.use { c -> if (c.moveToFirst()) c.getString(0) ?: "" else "" } ?: ""
        } catch (e: Exception) {
            Log.w(TAG, "lookupName: $e")
            ""
        }
    }
}
