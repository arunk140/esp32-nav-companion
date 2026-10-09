package com.navesp32.companion

/**
 * Shared navigation frame. Produced by any routing source ([OsmAndLink],
 * [ValhallaSource]) and encoded for the device by [NavBridge]. Field order
 * mirrors NAV_STATE in firmware_v2/main/ble_service.h.
 *
 * This used to live inside OsmAndLink; it moved out when Valhalla became a
 * second producer, so "OsmAnd" no longer leaks into the shared type name.
 */
data class NavState(
    val active: Boolean,
    val maneuver: Int,        // see Nav.*
    val distanceM: Int,       // to the maneuver
    val street: String,
    val afterManeuver: Int,
    val afterDistanceM: Int,
    val afterStreet: String,
    val remainingM: Int,
    val remainingS: Int,
    val destLat: Double,      // valid only when hasDest
    val destLon: Double,
    val hasDest: Boolean,
)

/**
 * Maneuver vocabulary shared by every source. Values are the wire contract,
 * mirroring NAV_* in firmware_v2/main/ble_service.h and app/.../NavBridge.kt.
 * Never reorder: these are bytes on the BLE link.
 */
object Nav {
    const val NONE = 0
    const val CONTINUE = 1
    const val SLIGHT_LEFT = 2
    const val LEFT = 3
    const val SHARP_LEFT = 4
    const val SLIGHT_RIGHT = 5
    const val RIGHT = 6
    const val SHARP_RIGHT = 7
    const val KEEP_LEFT = 8
    const val KEEP_RIGHT = 9
    const val UTURN = 10
    const val ROUNDABOUT = 11
    const val OFF_ROUTE = 12
    const val ARRIVE = 13
}
