package com.navesp32.companion

import java.net.HttpURLConnection
import java.net.URL

/**
 * Follows redirects for Google Maps short links (`maps.app.goo.gl/…`,
 * `goo.gl/maps/…`) so the coordinates hidden behind them can be read.
 *
 * deliberately does not fetch the short URL's HTML body: a plain HEAD/GET with
 * redirects disabled exposes the final URL in the `Location` header, which is
 * all the parser needs. A browser User-Agent avoids Google's consent interstitial.
 */
object LinkResolver {
    private const val UA =
        "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Mobile Safari/537.36"

    fun resolve(url: String): String? {
        var current = url
        repeat(6) {
            val conn = try {
                URL(current).openConnection() as HttpURLConnection
            } catch (e: Exception) {
                return null
            }
            try {
                conn.instanceFollowRedirects = false
                conn.connectTimeout = 8_000
                conn.readTimeout = 8_000
                conn.requestMethod = "GET"
                conn.setRequestProperty("User-Agent", UA)
                val code = conn.responseCode
                if (code in 300..399) {
                    val loc = conn.getHeaderField("Location") ?: return null
                    current = URL(URL(current), loc).toString()
                } else {
                    return current
                }
            } catch (e: Exception) {
                return null
            } finally {
                conn.disconnect()
            }
        }
        return current
    }
}
