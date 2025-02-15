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
#include "../../module/motion.h"
#include "../../module/probe.h"

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M401: Deploy and activate the Z probe <a href="https://reprap.org/wiki/G-code# "> </a>
 *
 *#### Usage
 *
 *    M401
 */
void GcodeSuite::M401() {
  DEPLOY_PROBE();
  report_current_position();
}

/**
 *### M402: Deactivate and stow the Z probe <a href="https://reprap.org/wiki/G-code# "> </a>
 *
 *#### Usage
 *
 *    M402
 */
void GcodeSuite::M402() {
  STOW_PROBE();
  #ifdef Z_AFTER_PROBING
    move_z_after_probing();
  #endif
  report_current_position();
}

/** @}*/

#endif // HAS_BED_PROBE
