package com.navesp32.companion

/**
 * Valhalla maneuver `type` (Odin enum) -> our wire maneuver vocabulary.
 *
 * The mapping is deliberately one-way: Valhalla's numbering is an internal
 * detail, exactly like OsmAnd's TurnType was. Values are documented at
 * https://valhalla.github.io/valhalla/api/route/api-reference/ (kNone=0 ..
 * kBuildingExit=43).
 */
object ManeuverMap {
    fun nav(valhallaType: Int): Int = when (valhallaType) {
        0 -> Nav.NONE
        1, 2, 3 -> Nav.CONTINUE                    // kStart / Right / Left
        4, 5, 6 -> Nav.ARRIVE                      // kDestination / Right / Left
        7, 8, 17, 22, 25 -> Nav.CONTINUE           // becomes/continue/ramp-straight/stay-straight/merge
        9, 18 -> Nav.SLIGHT_RIGHT                  // slight right / ramp right
        10, 20 -> Nav.RIGHT                        // right / exit right
        11 -> Nav.SHARP_RIGHT
        12, 13 -> Nav.UTURN                        // uturn right / uturn left
        14 -> Nav.SHARP_LEFT
        15, 21 -> Nav.LEFT                         // left / exit left
        16, 19 -> Nav.SLIGHT_LEFT                  // slight left / ramp left
        23 -> Nav.KEEP_RIGHT                       // stay right
        24 -> Nav.KEEP_LEFT                        // stay left
        26, 27 -> Nav.ROUNDABOUT                   // roundabout enter / exit
        28, 29 -> Nav.CONTINUE                     // ferry enter / exit
        else -> Nav.NONE                           // transit etc. unsupported
    }
}
