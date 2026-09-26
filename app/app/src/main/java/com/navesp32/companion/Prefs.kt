package com.navesp32.companion

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

// Bonded device + token, encrypted at rest. Keys: mac, name, svc, token.
object Prefs {
    private const val FILE = "nav_bond"

    private fun prefs(ctx: Context) = EncryptedSharedPreferences.create(
        ctx, FILE,
        MasterKey.Builder(ctx).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build(),
        EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
        EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
    )

    data class Bond(val mac: String, val name: String, val svc: String, val token: String)

    fun saveBond(ctx: Context, b: Bond) {
        prefs(ctx).edit()
            .putString("mac", b.mac).putString("name", b.name)
            .putString("svc", b.svc).putString("token", b.token)
            .apply()
    }

    fun loadBond(ctx: Context): Bond? {
        val p = prefs(ctx)
        val mac = p.getString("mac", null) ?: return null
        return Bond(mac, p.getString("name", "?")!!, p.getString("svc", "")!!,
            p.getString("token", "")!!)
    }

    fun clearBond(ctx: Context) {
        prefs(ctx).edit().clear().apply()
    }

    fun serviceEnabled(ctx: Context, def: Boolean = true): Boolean =
        prefs(ctx).getBoolean("service_on", def)

    fun setServiceEnabled(ctx: Context, on: Boolean) {
        prefs(ctx).edit().putBoolean("service_on", on).apply()
    }

    fun brightness(ctx: Context): Int = prefs(ctx).getInt("brightness", 80)

    fun setBrightness(ctx: Context, v: Int) {
        prefs(ctx).edit().putInt("brightness", v.coerceIn(0, 100)).apply()
    }
}
