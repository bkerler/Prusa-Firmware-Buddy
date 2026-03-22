#pragma once

#include "chamber.hpp"

namespace buddy {

struct ChamberHostReport {
    Temperature actual;
    std::optional<Temperature> target;
};

inline std::optional<ChamberHostReport> build_chamber_host_report(const Chamber::Capabilities &capabilities, std::optional<Temperature> current, std::optional<Temperature> target) {
    if (!capabilities.temperature_reporting || !current.has_value()) {
        return std::nullopt;
    }

    return ChamberHostReport {
        .actual = *current,
        .target = target,
    };
}

inline bool chamber_temperature_command_supported(const Chamber::Capabilities &capabilities) {
    return capabilities.temperature_control();
}

inline bool chamber_thermal_protection_supported(const Chamber::Capabilities &capabilities) {
    // Cooling-only and passive chamber implementations don't need Marlin-side thermal runaway protection.
    return !capabilities.heating;
}

} // namespace buddy
