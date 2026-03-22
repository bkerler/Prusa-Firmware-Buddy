#include <feature/chamber/chamber_marlin_compat.hpp>

#include <catch2/catch.hpp>

TEST_CASE("Chamber host report is emitted only for real reportable temperatures") {
    using buddy::Chamber;
    using buddy::Temperature;

    Chamber::Capabilities capabilities;

    SECTION("reporting disabled") {
        capabilities.temperature_reporting = false;

        const auto report = buddy::build_chamber_host_report(capabilities, Temperature(35), Temperature(40));
        REQUIRE_FALSE(report.has_value());
    }

    SECTION("current temperature missing") {
        capabilities.temperature_reporting = true;

        const auto report = buddy::build_chamber_host_report(capabilities, std::nullopt, Temperature(40));
        REQUIRE_FALSE(report.has_value());
    }

    SECTION("target temperature missing") {
        capabilities.temperature_reporting = true;

        const auto report = buddy::build_chamber_host_report(capabilities, Temperature(35), std::nullopt);
        REQUIRE(report.has_value());
        REQUIRE(report->actual == Approx(35));
        REQUIRE_FALSE(report->target.has_value());
    }

    SECTION("current and target temperature available") {
        capabilities.temperature_reporting = true;

        const auto report = buddy::build_chamber_host_report(capabilities, Temperature(35), Temperature(40));
        REQUIRE(report.has_value());
        REQUIRE(report->actual == Approx(35));
        REQUIRE(report->target.has_value());
        REQUIRE(*report->target == Approx(40));
    }
}

TEST_CASE("Chamber temperature commands require chamber temperature control") {
    using buddy::Chamber;

    SECTION("reporting without control is rejected") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = false,
            .cooling = false,
        };

        REQUIRE_FALSE(buddy::chamber_temperature_command_supported(capabilities));
    }

    SECTION("cooling support allows commands") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = false,
            .cooling = true,
        };

        REQUIRE(buddy::chamber_temperature_command_supported(capabilities));
    }

    SECTION("heating support allows commands") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = true,
            .cooling = false,
        };

        REQUIRE(buddy::chamber_temperature_command_supported(capabilities));
    }
}

TEST_CASE("Chamber thermal protection capability matches chamber heating risk") {
    using buddy::Chamber;

    SECTION("passive chamber reporting keeps thermal protection compatible") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = false,
            .cooling = false,
        };

        REQUIRE(buddy::chamber_thermal_protection_supported(capabilities));
    }

    SECTION("cooling-only chamber control keeps thermal protection compatible") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = false,
            .cooling = true,
        };

        REQUIRE(buddy::chamber_thermal_protection_supported(capabilities));
    }

    SECTION("heating chamber control needs dedicated thermal protection support") {
        const Chamber::Capabilities capabilities {
            .temperature_reporting = true,
            .heating = true,
            .cooling = false,
        };

        REQUIRE_FALSE(buddy::chamber_thermal_protection_supported(capabilities));
    }
}
