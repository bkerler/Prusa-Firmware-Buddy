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

#include "../../inc/MarlinConfig.h"

#if HAS_BED_PROBE

#include "../gcode.h"
#include "../../feature/bedlevel/bedlevel.h"
#include "../../module/probe.h"

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M851: Set Z-Probe Offset <a href="https://reprap.org/wiki/G-code#M851:_Set_Z-Probe_Offset">M851: Set Z-Probe Offset</a>
 *
 *#### Usage
 *
 *    M851 [ X ]
 *
 *#### Parameters
 *
 * - `X` - Set offset on X axis
 * - `Y` - Set offset on Y axis
 * - `Z` - Set offset on Z axis
 *
 * Without parameters prints the current Probe Offset
 */
void GcodeSuite::M851() {

  // Show usage with no parameters
  if (!parser.seen("XYZ")) {
    SERIAL_ECHOLNPAIR(MSG_PROBE_OFFSET " X", probe_offset.x, " Y", probe_offset.y, " Z", probe_offset.z);
    return;
  }

  xyz_pos_t offs = probe_offset;

  bool ok = true;

  if (parser.seenval('X')) {
    const float x = parser.value_float();
    if (WITHIN(x, -(X_BED_SIZE), X_BED_SIZE))
      offs.x = x;
    else {
      SERIAL_ECHOLNPAIR("?X out of range (-", int(X_BED_SIZE), " to ", int(X_BED_SIZE), ")");
      ok = false;
    }
  }

  if (parser.seenval('Y')) {
    const float y = parser.value_float();
    if (WITHIN(y, -(Y_BED_SIZE), Y_BED_SIZE))
      offs.y = y;
    else {
      SERIAL_ECHOLNPAIR("?Y out of range (-", int(Y_BED_SIZE), " to ", int(Y_BED_SIZE), ")");
      ok = false;
    }
  }

  if (parser.seenval('Z')) {
    const float z = parser.value_float();
    if (WITHIN(z, Z_PROBE_OFFSET_RANGE_MIN, Z_PROBE_OFFSET_RANGE_MAX))
      offs.z = z;
    else {
      SERIAL_ECHOLNPAIR("?Z out of range (", int(Z_PROBE_OFFSET_RANGE_MIN), " to ", int(Z_PROBE_OFFSET_RANGE_MAX), ")");
      ok = false;
    }
  }

  // Save the new offsets
  if (ok) probe_offset = offs;
}

/** @}*/

#endif // HAS_BED_PROBE
