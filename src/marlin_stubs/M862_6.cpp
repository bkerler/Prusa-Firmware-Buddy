/**
 * @file
 */
#include "PrusaGcodeSuite.hpp"
#include "../../lib/Marlin/Marlin/src/gcode/parser.h"
#include "feature/prusa/MMU2/mmu2_mk4.h"
#include "gcode_info.hpp"

#ifdef PRINT_CHECKING_Q_CMDS

/** \addtogroup G-Codes
 * @{
 */

/**
 *### M862.6: Check supported features <a href="https://reprap.org/wiki/G-code#M862.6:_Firmware_features">M862.6: Firmware features</a>
 *
 *#### Usage
 *
 *    M [ Q | P "<string>" ]
 *
 *#### Parameters
 *
 * - `Q` - Print current supported features
 * - `P "<string>"` - Check for feature
 */

void PrusaGcodeSuite::M862_6() {
    std::vector<std::string_view> supported_features;
    for (auto &feature : GCodeInfo::supported_features) {
        supported_features.push_back(feature);
    }
    #if ENABLED(PRUSA_MMU2)
    if (MMU2::mmu2.Enabled()) {
        supported_features.push_back("MMU3");
    }
    #endif
    if (parser.boolval('Q')) {
        for (auto &feature : supported_features) {
            SERIAL_ECHOLNPAIR(" M862 P\"", feature.data(), "\"");
        }
    }

    if (parser.boolval('P')) {
        char *arg = parser.string_arg + 1;
        while (*arg == ' ' || *arg == '\"') {
            arg++;
        }

        char *arg_end = arg;
        while (isalnum(*arg_end) || *arg_end != '\"') {
            arg_end++;
        }
        *arg_end = 0;

        auto str_arg = std::string_view(arg, arg_end);
        SERIAL_ECHOPAIR("\"", str_arg.data(), "\"");
        if (!count(supported_features.begin(), supported_features.end(), str_arg)) {
            SERIAL_ECHO(" NOT");
        }
        SERIAL_ECHOLN(" supported.");
    }
}

/** @}*/

#endif
