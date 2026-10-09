package com.navesp32.companion

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** A place the user shared in, or saved. */
data class Place(
    val id: String,
    val name: String,
    val lat: Double,
    val lon: Double,
    val saved: Boolean,
    val lastUsedMs: Long,
)

/**
 * Recent/saved places, persisted as a small JSON array in SharedPreferences.
 *
 * Deliberately not Room: this is a bounded list (saved + ~10 recent), the app
 * already stores every other preference this way, and it avoids adding KSP/Room
 * to a build that has no annotation processing today. Route geometry and the
 * nav loop are where complexity belongs, not storage of a dozen pins.
 */
object PlaceStore {
    private const val FILE = "nav_places"
    private const val KEY = "places"
    private const val MAX_RECENT = 10

    fun load(ctx: Context): List<Place> {
        val raw = prefs(ctx).getString(KEY, null) ?: return emptyList()
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                Place(
                    id = o.getString("id"),
                    name = o.getString("name"),
                    lat = o.getDouble("lat"),
                    lon = o.getDouble("lon"),
                    saved = o.optBoolean("saved", false),
                    lastUsedMs = o.optLong("lastUsedMs", 0L),
                )
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    /** Most-recently used first, with saved places pinned to the top. */
    fun loadForDisplay(ctx: Context): List<Place> =
        load(ctx).sortedWith(
            compareByDescending<Place> { it.saved }.thenByDescending { it.lastUsedMs }
        )

    /** Adds or refreshes a place, returning the stored entry. */
    fun addRecent(ctx: Context, name: String, lat: Double, lon: Double): Place {
        val id = idFor(lat, lon)
        val all = load(ctx).toMutableList()
        val existing = all.firstOrNull { it.id == id }
        val place = Place(
            id = id,
            name = name.ifBlank { existing?.name ?: "Dropped pin" },
            lat = lat,
            lon = lon,
            saved = existing?.saved ?: false,
            lastUsedMs = System.currentTimeMillis(),
        )
        all.removeAll { it.id == id }
        all.add(0, place)
        save(ctx, trim(all))
        return place
    }

    fun toggleSaved(ctx: Context, id: String) {
        val all = load(ctx).toMutableList()
        val i = all.indexOfFirst { it.id == id }
        if (i < 0) return
        all[i] = all[i].copy(saved = !all[i].saved)
        save(ctx, trim(all))
    }

    fun remove(ctx: Context, id: String) {
        val all = load(ctx).toMutableList().filterNot { it.id == id }
        save(ctx, all)
    }

    private fun trim(all: List<Place>): List<Place> {
        val saved = all.filter { it.saved }
        val recent = all.filterNot { it.saved }
            .sortedByDescending { it.lastUsedMs }
            .take(MAX_RECENT)
        return saved + recent
    }

    private fun save(ctx: Context, all: List<Place>) {
        val arr = JSONArray()
        for (p in all) {
            arr.put(JSONObject().apply {
                put("id", p.id)
                put("name", p.name)
                put("lat", p.lat)
                put("lon", p.lon)
                put("saved", p.saved)
                put("lastUsedMs", p.lastUsedMs)
            })
        }
        prefs(ctx).edit().putString(KEY, arr.toString()).apply()
    }

    private fun idFor(lat: Double, lon: Double) =
        String.format(java.util.Locale.US, "%.7f,%.7f", lat, lon)

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
}
