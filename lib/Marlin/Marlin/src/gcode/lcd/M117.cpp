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

#include "../gcode.h"
#include "../../lcd/ultralcd.h"

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M117: Set LCD Status Message <a href="https://reprap.org/wiki/G-code#M117:_Display_Message">M117: Display Message</a>
 *
 *#### Usage
 *
 *    M117 [string]
 *
 *#### Parameters
 *
 * - `[string]` - Message string.
 *
 *#### Examples
 *
 *    M117 Hello World ; Displays on LCD the message "Hello World"
 */
void GcodeSuite::M117() {

  if (parser.string_arg && parser.string_arg[0])
    ui.set_status(parser.string_arg);
  else
    ui.reset_status();

}

/** @}*/
