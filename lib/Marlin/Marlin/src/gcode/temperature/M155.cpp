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

#if ENABLED(AUTO_REPORT_TEMPERATURES) && HAS_TEMP_SENSOR

#include "../gcode.h"
#include "../../module/temperature.h"

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M155: Set temperature auto-report interval <a href="https://reprap.org/wiki/G-code#M155:_Automatically_send_temperatures">M155: Automatically send temperatures</a>
 *
 *#### Usage
 *
 *    M155 [ S ]
 *
 *#### Parameters
 *
 * - `S` - Time interval in seconds
 */
void GcodeSuite::M155() {

  if (parser.seenval('S'))
    thermalManager.set_auto_report_interval(parser.value_byte());

}

/** @}*/

#endif // AUTO_REPORT_TEMPERATURES && HAS_TEMP_SENSOR
