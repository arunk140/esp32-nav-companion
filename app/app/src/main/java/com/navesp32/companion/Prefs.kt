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

    fun darkMap(ctx: Context): Boolean = prefs(ctx).getBoolean("darkmap", true)

    fun setDarkMap(ctx: Context, dark: Boolean) {
        prefs(ctx).edit().putBoolean("darkmap", dark).apply()
    }

    fun brightness(ctx: Context): Int = prefs(ctx).getInt("brightness", 80)

    fun setBrightness(ctx: Context, v: Int) {
        prefs(ctx).edit().putInt("brightness", v.coerceIn(0, 100)).apply()
    }

    // ---- navigation source -------------------------------------------------

    /** "osmAnd" (default, reads OsmAnd) or "valhalla" (companion-app routing). */
    fun navSource(ctx: Context): String = prefs(ctx).getString("navsource", "osmAnd") ?: "osmAnd"

    fun setNavSource(ctx: Context, source: String) {
        prefs(ctx).edit().putString("navsource", source).apply()
    }

    /** Valhalla base URL; defaults to the FOSSGIS public demo (dev/testing only). */
    fun valhallaUrl(ctx: Context): String =
        prefs(ctx).getString("valhalla_url", ValhallaClient.DEFAULT_BASE_URL)
            ?: ValhallaClient.DEFAULT_BASE_URL

    fun setValhallaUrl(ctx: Context, url: String) {
        prefs(ctx).edit().putString("valhalla_url", url.trim().ifEmpty {
            ValhallaClient.DEFAULT_BASE_URL
        }).apply()
    }
}
