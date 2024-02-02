#include "touchscreen_common.hpp"

LOG_COMPONENT_DEF(Touch, LOG_SEVERITY_INFO);

bool Touchscreen_Base::is_enabled() const {
    return config_store().touch_enabled.get();
}

void Touchscreen_Base::set_enabled(bool set) {
    if (set == is_enabled()) {
        return;
    }

    config_store().touch_enabled.set(set);
    log_info(Touch, "set enabled %i", set);
}

TouchscreenEvent Touchscreen_Base::get_event() {
    if (is_last_event_consumed_) {
        return TouchscreenEvent();
    }

    is_last_event_consumed_ = true;
    return last_event_;
}

TouchscreenEvent Touchscreen_Base::get_last_event() const {
    return last_event_;
}

void Touchscreen_Base::update() {
    if (!is_hw_ok_) {
        return;
    }

    TouchState touch_state = last_touch_state_;
    update_impl(touch_state);

    // Touch start -> set up gesture recognition
    if (touch_state.multitouch_point_count == 1 && gesture_recognition_state_ == GestureRecognitionState::idle) {
        gesture_recognition_state_ = GestureRecognitionState::active;
        gesture_start_pos_ = touch_state.multitouch_points[0].position;

    } else if (touch_state.multitouch_point_count > 1) {
        // Multiple touch points -> invalid gesture state
        gesture_recognition_state_ = GestureRecognitionState::invalid;

    } else if (touch_state.multitouch_point_count == 0 && gesture_recognition_state_ != GestureRecognitionState::idle) {
        recognize_gesture();

        // Touch end - recognize gesture
        gesture_recognition_state_ = GestureRecognitionState::idle;
    }

    last_touch_state_ = touch_state;
}

void Touchscreen_Base::recognize_gesture() {
    if (gesture_recognition_state_ == GestureRecognitionState::invalid) {
        return;
    }

    assert(last_touch_state_.multitouch_point_count > 0);
    const point_ui16_t last_touch_pos = last_touch_state_.multitouch_points[0].position;

    const point_i16_t touch_pos_diff = point_i16_t::from_point(last_touch_pos) - point_i16_t::from_point(gesture_start_pos_);
    const point_i16_t touch_pos_diff_abs(abs(touch_pos_diff.x), abs(touch_pos_diff.y));

    /// Distance (manhattan) from the gesture_start_pos that is still considered to be a click
    static constexpr int16_t click_tolerance = 10;

    /// Tangens of max angle that considers as a swipe gesture
    static constexpr float swipe_max_angle_tan = 0.5;

    TouchscreenEvent event {
        .pos_x = gesture_start_pos_.x,
        .pos_y = gesture_start_pos_.y,
    };

    log_info(Touch, "abs diff %i %i", touch_pos_diff_abs.x, touch_pos_diff_abs.y);

    if (touch_pos_diff_abs.x <= click_tolerance && touch_pos_diff_abs.y <= click_tolerance) {
        event.type = GUI_event_t::TOUCH_CLICK;

    } else if (static_cast<float>(touch_pos_diff_abs.x) / static_cast<float>(touch_pos_diff_abs.y) <= swipe_max_angle_tan) {
        event.type = touch_pos_diff.y < 0 ? GUI_event_t::TOUCH_SWIPE_UP : GUI_event_t::TOUCH_SWIPE_DOWN;

    } else if (static_cast<float>(touch_pos_diff_abs.y) / static_cast<float>(touch_pos_diff_abs.x) <= swipe_max_angle_tan) {
        event.type = touch_pos_diff.x < 0 ? GUI_event_t::TOUCH_SWIPE_LEFT : GUI_event_t::TOUCH_SWIPE_RIGHT;
    }

    if (event) {
        last_event_ = event;
        is_last_event_consumed_ = false;
    }
}
