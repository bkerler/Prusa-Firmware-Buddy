/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2019 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "../../inc/MarlinConfigPre.h"

#if EXTRUDERS > 1

#include "../gcode.h"
#include "../../module/tool_change.h"

void M217_report(const bool eeprom=false) {

  #if ENABLED(TOOLCHANGE_FILAMENT_SWAP)
    serialprintPGM(eeprom ? PSTR("  M217") : PSTR("Toolchange:"));
    SERIAL_ECHOPAIR(" S", LINEAR_UNIT(toolchange_settings.swap_length));
    SERIAL_ECHOPAIR(" E", LINEAR_UNIT(toolchange_settings.extra_prime));
    SERIAL_ECHOPAIR(" P", LINEAR_UNIT(toolchange_settings.prime_speed));
    SERIAL_ECHOPAIR(" R", LINEAR_UNIT(toolchange_settings.retract_speed));

    #if ENABLED(TOOLCHANGE_PARK)
      SERIAL_ECHOPAIR(" X", LINEAR_UNIT(toolchange_settings.change_point.x));
      SERIAL_ECHOPAIR(" Y", LINEAR_UNIT(toolchange_settings.change_point.y));
    #endif

  #else

    UNUSED(eeprom);

  #endif

  SERIAL_ECHOPAIR(" Z", LINEAR_UNIT(toolchange_settings.z_raise));
  SERIAL_EOL();
}

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M217: Set SINGLENOZZLE toolchange parameters <a href="https://reprap.org/wiki/G-code#M217:_Toolchange_Parameters">M217: Toolchange Parameters</a>
 *
 * Only MK3.5/S, MK3.9/S, MK4/S with MMU and XL
 *
 *#### Usage
 *
 *    M [ Z | X | Y | S | E | P | R |]
 *
 *#### Parameters
 *
 *  - `Z` - Z Raise
 *  - `X` - Park X (not active)(Requires TOOLCHANGE_PARK)
 *  - `Y` - Park Y (not active)(Requires TOOLCHANGE_PARK)
 *  - `S` - Swap length (not active) (Requires TOOLCHANGE_FILAMENT_SWAP)
 *  - `E` - Purge length (not active) (Requires TOOLCHANGE_FILAMENT_SWAP)
 *  - `P` - Prime speed (not active) (Requires TOOLCHANGE_FILAMENT_SWAP)
 *  - `R` - Retract speed (not active) (Requires TOOLCHANGE_FILAMENT_SWAP)
 *
 * Without parameters prints the current Z Raise
 */
void GcodeSuite::M217() {

  #define SPR_PARAM
  #define XY_PARAM

  #if ENABLED(TOOLCHANGE_FILAMENT_SWAP)

    #undef SPR_PARAM
    #define SPR_PARAM "SPRE"

    static constexpr float max_extrude =
      #if ENABLED(PREVENT_LENGTHY_EXTRUDE)
        EXTRUDE_MAXLENGTH
      #else
        500
      #endif
    ;

    if (parser.seenval('S')) { const float v = parser.value_linear_units(); toolchange_settings.swap_length = constrain(v, 0, max_extrude); }
    if (parser.seenval('E')) { const float v = parser.value_linear_units(); toolchange_settings.extra_prime = constrain(v, 0, max_extrude); }
    if (parser.seenval('P')) { const int16_t v = parser.value_linear_units(); toolchange_settings.prime_speed = constrain(v, 10, 5400); }
    if (parser.seenval('R')) { const int16_t v = parser.value_linear_units(); toolchange_settings.retract_speed = constrain(v, 10, 5400); }
  #endif

  #if ENABLED(TOOLCHANGE_PARK)
    #undef XY_PARAM
    #define XY_PARAM "XY"
    if (parser.seenval('X')) { toolchange_settings.change_point.x = parser.value_linear_units(); }
    if (parser.seenval('Y')) { toolchange_settings.change_point.y = parser.value_linear_units(); }
  #endif

  if (parser.seenval('Z')) { toolchange_settings.z_raise = parser.value_linear_units(); }

  if (!parser.seen(SPR_PARAM XY_PARAM "Z")) M217_report();
}

/** @}*/

#endif // EXTRUDERS > 1
