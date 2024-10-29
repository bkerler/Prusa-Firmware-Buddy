#pragma once

#include <WindowItemFanLabel.hpp>
#include <WindowMenuSpin.hpp>

/// Automatically hidden if the extboard is disabled
class MI_XBUDDY_EXTENSION_LIGHTS : public WiSpin {
public:
    MI_XBUDDY_EXTENSION_LIGHTS();
    virtual void OnClick() override;
};

/// Manual control for chamber fans
/// Automatically hidden if the extboard is disabled
class MI_XBUDDY_EXTENSION_COOLING_FANS : public WiSpin {
public:
    MI_XBUDDY_EXTENSION_COOLING_FANS();
    virtual void OnClick() override;
};

/// PWM/RPM info for fan1
/// Automatically hidden if the extboard is disabled
class MI_INFO_XBUDDY_EXTENSION_FAN1 : public WI_FAN_LABEL_t {
public:
    MI_INFO_XBUDDY_EXTENSION_FAN1();
};

/// PWM/RPM info for fan2
/// Automatically hidden if the extboard is disabled
class MI_INFO_XBUDDY_EXTENSION_FAN2 : public WI_FAN_LABEL_t {
public:
    MI_INFO_XBUDDY_EXTENSION_FAN2();
};

/// PWM/RPM info for fan3
/// Automatically hidden if the extboard is disabled
class MI_INFO_XBUDDY_EXTENSION_FAN3 : public WI_FAN_LABEL_t {
public:
    MI_INFO_XBUDDY_EXTENSION_FAN3();
};
