/**
 * @file screen_menu_sensor_info_XL.cpp
 */

#include "screen_menu_sensor_info.hpp"
#include "ScreenHandler.hpp"
#include "MItem_tools.hpp"
#include "DialogMoveZ.hpp"

#include "i_window_menu_container.hpp"
#include <tuple>

#include "marlin_client.hpp"
#include "cmath_ext.h"
#include "../Marlin/src/module/configuration_store.h"

#include <filament_sensor.hpp>
#include <filament_sensors_handler.hpp>

void ScreenMenuSensorInfo::windowEvent(window_t *sender, GUI_event_t event, void *param) {
    if (event == GUI_event_t::LOOP) {
        Item<MI_INFO_PRINTER_FILL_SENSOR>().UpdateValue(GetExtruderFSensor(marlin_vars().active_extruder.get()));
        Item<MI_INFO_SIDE_FILL_SENSOR>().UpdateValue(GetSideFSensor(marlin_vars().active_extruder.get()));
        Item<MI_INFO_PRINT_FAN>().UpdateValue(
            marlin_vars().print_fan_speed,
            marlin_vars().active_hotend().print_fan_rpm);
        Item<MI_INFO_HBR_FAN>().UpdateValue(
            sensor_data().hbrFan,
            marlin_vars().active_hotend().heatbreak_fan_rpm);
        Item<MI_INFO_INPUT_VOLTAGE>().UpdateValue(sensor_data().inputVoltage);
        Item<MI_INFO_5V_VOLTAGE>().UpdateValue(sensor_data().sandwich5VVoltage);
        Item<MI_INFO_SANDWICH_5V_CURRENT>().UpdateValue(sensor_data().sandwich5VCurrent);
        Item<MI_INFO_BUDDY_5V_CURRENT>().UpdateValue(sensor_data().buddy5VCurrent);
    }

    if (event == GUI_event_t::HELD_RELEASED) {
        DialogMoveZ::Show();
        return;
    }

    ScreenMenu::windowEvent(sender, event, param);
}
