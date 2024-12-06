/*
 * CoreXY precise homing refinement - implementation
 * TODO: @wavexx: Add some documentation
 */

#include "homing_corexy.hpp"

// sanity checks
#ifdef PRECISE_HOMING
    #error "PRECISE_HOMING_COREXY is mutually exclusive with PRECISE_HOMING"
#endif
#ifdef HAS_TMC_WAVETABLE
    // Wavetable restoration needs to happen after refinement succeeds, and
    // not per-axis as currently done. Ensure the setting is not enabled by mistake.
    #error "PRECISE_HOMING_COREXY is not compatible with HAS_TMC_WAVETABLE"
#endif

#include "../planner.h"
#include "../stepper.h"
#include "../endstops.h"

#if ENABLED(CRASH_RECOVERY)
    #include "feature/prusa/crash_recovery.hpp"
#endif

#include <bsod.h>
#include <scope_guard.hpp>
#include <feature/phase_stepping/phase_stepping.hpp>
#include <feature/input_shaper/input_shaper_config.hpp>
#include <config_store/store_instance.hpp>

#pragma GCC diagnostic warning "-Wdouble-promotion"

static bool COREXY_HOME_UNSTABLE = false;

// convert raw AB steps to XY mm
void corexy_ab_to_xy(const xy_long_t &steps, xy_pos_t &mm) {
    float x = static_cast<float>(steps.a + steps.b) / 2.f;
    float y = static_cast<float>(CORESIGN(steps.a - steps.b)) / 2.f;
    mm.x = x * planner.mm_per_step[X_AXIS];
    mm.y = y * planner.mm_per_step[Y_AXIS];
}

// convert raw AB steps to XY mm and position in mini-steps
static void corexy_ab_to_xy(const xy_long_t &steps, xy_pos_t &mm, xy_long_t &pos_msteps) {
    float x = static_cast<float>(steps.a + steps.b) / 2.f;
    float y = static_cast<float>(CORESIGN(steps.a - steps.b)) / 2.f;
    mm.x = x * planner.mm_per_step[X_AXIS];
    mm.y = y * planner.mm_per_step[Y_AXIS];
    pos_msteps.x = LROUND(x * PLANNER_STEPS_MULTIPLIER);
    pos_msteps.y = LROUND(y * PLANNER_STEPS_MULTIPLIER);
}

// convert raw AB steps to XY mm, filling others from current state
void corexy_ab_to_xyze(const xy_long_t &steps, xyze_pos_t &mm) {
    corexy_ab_to_xy(steps, mm);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
        mm[i] = planner.get_axis_position_mm((AxisEnum)i);
    }
}

// convert raw AB steps to XY mm and position in mini-steps, filling others from current state
static void corexy_ab_to_xyze(const xy_long_t &steps, xyze_pos_t &mm, xyze_long_t &pos_msteps) {
    pos_msteps = planner.get_position_msteps();
    corexy_ab_to_xy(steps, mm, pos_msteps);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
        mm[i] = planner.get_axis_position_mm((AxisEnum)i);
    }
}

static void plan_raw_move(const xyze_pos_t target_mm, const xyze_long_t target_pos, const feedRate_t fr_mm_s) {
    planner._buffer_msteps_raw(target_pos, target_mm, fr_mm_s, active_extruder);
    planner.synchronize();
}

static void plan_corexy_raw_move(const xy_long_t &target_steps_ab, const feedRate_t fr_mm_s) {
    // reconstruct full final position
    xyze_pos_t target_mm;
    xyze_long_t target_pos_msteps;
    corexy_ab_to_xyze(target_steps_ab, target_mm, target_pos_msteps);

    plan_raw_move(target_mm, target_pos_msteps, fr_mm_s);
}

// TMC µsteps(phase) per Marlin µsteps
static constexpr int16_t phase_per_ustep(const AxisEnum axis) {
    // Originally, we read the microstep configuration from the driver; this no
    // longer make sense with 256 microsteps.
    // Thus, we use the printer defaults instead of stepper_axis(axis).microsteps();
    assert(axis <= AxisEnum::Z_AXIS);
    static const int MICROSTEPS[] = { X_MICROSTEPS, Y_MICROSTEPS, Z_MICROSTEPS };
    return 256 / MICROSTEPS[axis];
};

// TMC full cycle µsteps per Marlin µsteps
static constexpr int16_t phase_cycle_steps(const AxisEnum axis) {
    return 1024 / phase_per_ustep(axis);
}

static int16_t axis_mscnt(const AxisEnum axis) {
#if HAS_PHASE_STEPPING()
    return phase_stepping::logical_ustep(axis);
#else
    return stepper_axis(axis).MSCNT();
#endif
}

static int16_t phase_backoff_steps(const AxisEnum axis) {
    int16_t effectorBackoutDir; // Direction in which the effector mm coordinates move away from endstop.
    int16_t stepperCountDir; // Direction in which the TMC µstep count(phase) increases.
    switch (axis) {
    case X_AXIS:
        effectorBackoutDir = -X_HOME_DIR;
        stepperCountDir = INVERT_X_DIR ? -1 : 1;
        break;
    case Y_AXIS:
        effectorBackoutDir = -Y_HOME_DIR;
        stepperCountDir = INVERT_Y_DIR ? -1 : 1;
        break;
    default:
        bsod("invalid backoff axis");
    }

    int16_t phaseCurrent = axis_mscnt(axis); // The TMC µsteps(phase) count of the current position
    int16_t phaseDelta = ((stepperCountDir < 0) == (effectorBackoutDir < 0) ? phaseCurrent : 1024 - phaseCurrent);
    int16_t phasePerStep = phase_per_ustep(axis);
    return int16_t((phaseDelta + phasePerStep / 2) / phasePerStep) * effectorBackoutDir;
}

static bool phase_aligned(AxisEnum axis) {
    int16_t phase_cur = axis_mscnt(axis);
    int16_t ustep_max = phase_per_ustep(axis) / 2;
    return (phase_cur <= ustep_max || phase_cur >= (1024 - ustep_max));
}

/**
 * @brief Part of precise homing.
 * @param origin_steps
 * @param dist
 * @param m_steps
 * @param m_dist
 * @return True on success
 */
static bool measure_axis_distance(AxisEnum axis, xy_long_t origin_steps, int32_t dist, int32_t &m_steps, float &m_dist) {
    // full initial position
    xyze_long_t initial_steps = { origin_steps.a, origin_steps.b, stepper.position(C_AXIS), stepper.position(E_AXIS) };
    xyze_pos_t initial_mm;
    corexy_ab_to_xyze(initial_steps, initial_mm);

    // full target position
    xyze_long_t target_steps = initial_steps;
    target_steps[axis] += dist;

    xyze_pos_t target_mm;
    xyze_long_t target_pos_msteps;
    corexy_ab_to_xy(target_steps, target_mm, target_pos_msteps);
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
        target_mm[i] = initial_mm[i];
    }
    xyze_long_t initial_pos_msteps = planner.get_position_msteps();
    LOOP_S_L_N(i, C_AXIS, XYZE_N) {
        target_pos_msteps[i] = initial_pos_msteps[i];
    }

    // move towards the endstop
    sensorless_t stealth_states = start_sensorless_homing_per_axis(axis);
#ifdef XY_HOMING_MEASURE_SENS
    // this will be reset to default implicitly by end_sensorless_homing_per_axis()
    stepper_axis(axis).sgt(XY_HOMING_MEASURE_SENS);
#endif
    endstops.enable(true);
#ifdef XY_HOMING_MEASURE_FR
    float measure_fr = XY_HOMING_MEASURE_FR;
#else
    float measure_fr = homing_feedrate(axis);
#endif
    plan_raw_move(target_mm, target_pos_msteps, measure_fr);
    uint8_t hit = endstops.trigger_state();
    endstops.not_homing();

    xyze_long_t hit_steps;
    xyze_pos_t hit_mm;
    if (hit) {
        // resync position from steppers to get hit position
        endstops.hit_on_purpose();
        planner.reset_position();
        hit_steps = { stepper.position(A_AXIS), stepper.position(B_AXIS), stepper.position(C_AXIS), stepper.position(E_AXIS) };
        corexy_ab_to_xyze(hit_steps, hit_mm);
    } else {
        hit_steps = target_steps;
        hit_mm = target_mm;
    }
    end_sensorless_homing_per_axis(axis, stealth_states);

    // move back to starting point
    plan_raw_move(initial_mm, initial_pos_msteps, homing_feedrate(axis));
    if (planner.draining()) {
        return false;
    }

    // sanity checks
    AxisEnum fixed_axis = (axis == B_AXIS ? A_AXIS : B_AXIS);
    if (hit_steps[fixed_axis] != initial_steps[fixed_axis] || initial_steps[fixed_axis] != stepper.position(fixed_axis)) {
        bsod("fixed axis moved unexpectedly");
    }
    if (initial_steps[axis] != stepper.position(axis)) {
        bsod("measured axis didn't return");
    }

    // result values
    m_steps = hit_steps[axis] - initial_steps[axis];
    m_dist = hypotf(hit_mm[X_AXIS] - initial_mm[X_AXIS], hit_mm[Y_AXIS] - initial_mm[Y_AXIS]);
    return hit;
}

/**
 * @brief Part of precise homing.
 * @param axis Physical axis to measure
 * @param c_dist AB cycle distance from the endstop
 * @param m_dist 1/2 distance from the endstop (mm)
 * @return True on success
 */
static bool measure_phase_cycles(AxisEnum axis, xy_pos_t &c_dist, xy_pos_t &m_dist) {
    // adjust current of the holding motor
    AxisEnum other_axis = (axis == B_AXIS ? A_AXIS : B_AXIS);
    auto &other_stepper = stepper_axis(other_axis);

    int32_t other_orig_cur = other_stepper.rms_current();
    float other_orig_hold = other_stepper.hold_multiplier();
    other_stepper.rms_current(XY_HOMING_HOLDING_CURRENT, 1.);

    // adjust current of the measured motor
    auto &axis_stepper = stepper_axis(axis);
    int32_t axis_orig_cur = axis_stepper.rms_current();
    float axis_orig_hold = axis_stepper.hold_multiplier();
#ifdef XY_HOMING_MEASURE_CURRENT
    axis_stepper.rms_current(XY_HOMING_MEASURE_CURRENT, 1.);
#endif

    // disable IS on AB axes to ensure _only_ the measured axis is being moved
    // (cartesian IS mixing can cause both to move, triggering an invalid endstop)
    std::optional<input_shaper::AxisConfig> is_config_orig[2] = {
        input_shaper::get_axis_config(A_AXIS),
        input_shaper::get_axis_config(B_AXIS)
    };
    input_shaper::set_axis_config(A_AXIS, std::nullopt);
    input_shaper::set_axis_config(B_AXIS, std::nullopt);

    ScopeGuard state_restorer([&]() {
        other_stepper.rms_current(other_orig_cur, other_orig_hold);
        axis_stepper.rms_current(axis_orig_cur, axis_orig_hold);
        input_shaper::set_axis_config(A_AXIS, is_config_orig[A_AXIS]);
        input_shaper::set_axis_config(B_AXIS, is_config_orig[B_AXIS]);
    });

    const int32_t measure_max_dist = (XY_HOMING_ORIGIN_OFFSET * 4) / planner.mm_per_step[axis];
    const int32_t measure_dir = (axis == B_AXIS ? -X_HOME_DIR : -Y_HOME_DIR);
    xy_long_t origin_steps = { stepper.position(A_AXIS), stepper.position(B_AXIS) };
    constexpr int probe_n = 2; // note the following code assumes always two probes per retry
    xy_long_t p_steps[probe_n];
    xy_pos_t p_dist[probe_n] = { -XY_HOMING_ORIGIN_BUMP_MAX_ERR, -XY_HOMING_ORIGIN_BUMP_MAX_ERR };

    uint8_t retry;
    for (retry = 0; retry != XY_HOMING_ORIGIN_BUMP_RETRIES; ++retry) {
        uint8_t slot0 = retry % probe_n;
        uint8_t slot1 = (retry + 1) % probe_n;

        // measure distance B+/B-
        if (!measure_axis_distance(axis, origin_steps, measure_max_dist * measure_dir, p_steps[slot1][1], p_dist[slot1][1])
            || !measure_axis_distance(axis, origin_steps, measure_max_dist * -measure_dir, p_steps[slot1][0], p_dist[slot1][0])) {
            if (!planner.draining()) {
                ui.status_printf_P(0, "Endstop not reached");
            }
            return false;
        }

        // keep signs positive
        p_steps[slot1][0] = abs(p_steps[slot1][0]);
        p_dist[slot1][0] = abs(p_dist[slot1][0]);
        p_steps[slot1][1] = abs(p_steps[slot1][1]);
        p_dist[slot1][1] = abs(p_dist[slot1][1]);

        if (abs(p_dist[slot0][0] - p_dist[slot1][0]) < float(XY_HOMING_ORIGIN_BUMP_MAX_ERR)
            && abs(p_dist[slot0][1] - p_dist[slot1][1]) < float(XY_HOMING_ORIGIN_BUMP_MAX_ERR)) {
            break;
        }
    }
    if (retry == XY_HOMING_ORIGIN_BUMP_RETRIES) {
        ui.status_printf_P(0, "Axis measurement failed");
        return false;
    }

    // calculate the absolute cycle coordinates
    float d1 = (p_steps[0][0] + p_steps[1][0]) / 2.f;
    float d2 = (p_steps[0][1] + p_steps[1][1]) / 2.f;
    float d = d1 + d2;
    float a = d / 2.f;
    float b = d1 - a;

    c_dist[0] = a / float(phase_cycle_steps(other_axis));
    c_dist[1] = b / float(phase_cycle_steps(axis));

    m_dist[0] = (p_dist[0][0] + p_dist[1][0]) / 2.f;
    m_dist[1] = (p_dist[0][1] + p_dist[1][1]) / 2.f;

    if (DEBUGGING(LEVELING)) {
        // measured distance and cycle
        SERIAL_ECHOLNPAIR("home ", physical_axis_codes[axis], "+ steps 0:", p_steps[0][1], " 1:", p_steps[1][1],
            " cycle A:", c_dist[0], " mm:", m_dist[1]);
        SERIAL_ECHOLNPAIR("home ", physical_axis_codes[axis], "- steps 0:", p_steps[0][0], " 1:", p_steps[1][0],
            " cycle B:", c_dist[1], " mm:", m_dist[0]);
    }
    return true;
}

// return true if the point is too close to the phase grid halfway point
static bool point_is_unstable(const xy_pos_t &c_dist, const xy_pos_t &origin) {
    static constexpr float threshold = 1. / 4;
    LOOP_XY(axis) {
        if (abs(fmod(c_dist[axis] - origin[axis], 1.f) - 0.5f) < threshold) {
            return true;
        }
    }
    return false;
}

// translate fractional cycle distance by origin and round to final AB grid
static xy_long_t cdist_translate(const xy_pos_t &c_dist, const xy_pos_t &origin) {
    xy_long_t c_ab;
    LOOP_XY(axis) {
        long o_int = long(roundf(origin[axis]));
        c_ab[axis] = long(roundf(c_dist[axis] - origin[axis])) + o_int;
    }
    return c_ab;
}

/**
 * @brief plan a relative move by full AB cycles around origin_steps
 * @param ab_off full AB cycles away from homing corner
 */
static void plan_corexy_abgrid_move(const xy_long_t &origin_steps, const xy_long_t &ab_off, const float fr_mm_s) {
    long a = ab_off[X_HOME_DIR == Y_HOME_DIR ? A_AXIS : B_AXIS] * -Y_HOME_DIR;
    long b = ab_off[X_HOME_DIR == Y_HOME_DIR ? B_AXIS : A_AXIS] * -X_HOME_DIR;

    xy_long_t point_steps = {
        origin_steps[A_AXIS] + phase_cycle_steps(A_AXIS) * a,
        origin_steps[B_AXIS] + phase_cycle_steps(B_AXIS) * b
    };

    plan_corexy_raw_move(point_steps, fr_mm_s);
}

static bool measure_origin_multipoint(AxisEnum axis, const xy_long_t &origin_steps,
    xy_pos_t &origin, xy_pos_t &distance, const float fr_mm_s) {
    // scramble probing sequence to improve belt redistribution when estimating the centroid
    // unit is full AB cycles away from homing corner as given to plan_corexy_abgrid_move()
    static constexpr xy_long_t point_sequence[] = {
        { 1, 0 },
        { -1, 0 },
        { 0, 1 },
        { 0, -1 },
        { -1, -1 },
        { 1, 1 },
        { 1, -1 },
        { -1, 1 },
        { 0, 0 },
    };

    struct point_data {
        xy_pos_t c_dist;
        xy_pos_t m_dist;
        bool revalidate;
    };

    point_data points[std::size(point_sequence)];

    // allow single-point revalidation on instability to speed-up retries
    // start by forcing whole-grid revalidation
    for (size_t i = 0; i != std::size(point_sequence); ++i) {
        points[i].revalidate = true;
    }

    // keep track of points to revalidate
    size_t rev_cnt = std::size(point_sequence);
    for (size_t revcount = 0; revcount < std::size(point_sequence) / 2; ++revcount) {
        xy_pos_t c_acc = { 0, 0 };
        xy_pos_t m_acc = { 0, 0 };
        size_t new_rev_cnt = 0;

        // cycle through grid points and calculate centroid
        for (size_t i = 0; i != std::size(point_sequence); ++i) {
            const auto &seq = point_sequence[i];
            auto &data = points[i];

            if (data.revalidate) {
                plan_corexy_abgrid_move(origin_steps, seq, fr_mm_s);
                if (planner.draining()) {
                    return false;
                }

                if (!measure_phase_cycles(axis, data.c_dist, data.m_dist)) {
                    return false;
                }
            }

            c_acc += data.c_dist;
            m_acc += data.m_dist;
        }
        origin = c_acc / float(std::size(point_sequence));
        distance = m_acc / float(std::size(point_sequence));

        // verify each probed point with the current centroid
        xy_long_t o_int = { long(roundf(origin[A_AXIS])), long(roundf(origin[B_AXIS])) };
        for (size_t i = 0; i != std::size(point_sequence); ++i) {
            const auto &seq = point_sequence[i];
            auto &data = points[i];

            xy_long_t c_ab = cdist_translate(data.c_dist, origin);
            xy_long_t c_diff = c_ab - seq - o_int;
            if (c_diff[A_AXIS] || c_diff[B_AXIS]) {
                COREXY_HOME_UNSTABLE = true;
                SERIAL_ECHOLNPAIR("home calibration point (", seq[A_AXIS], ",", seq[B_AXIS],
                    ") invalid A:", c_diff[A_AXIS], " B:", c_diff[B_AXIS],
                    " with origin A:", o_int[A_AXIS], " B:", o_int[B_AXIS]);
                // when even just a point is invalid, we likely have skipped or have a false centroid:
                // no point in revalidating, mark the calibration as an instant failure
                return false;
            }

            data.revalidate = point_is_unstable(data.c_dist, origin);
            if (data.revalidate) {
                COREXY_HOME_UNSTABLE = true;
                SERIAL_ECHOLNPAIR("home calibration point (", seq[A_AXIS], ",", seq[B_AXIS],
                    ") unstable A:", data.c_dist[A_AXIS], " B:", data.c_dist[B_AXIS],
                    " with origin A:", origin[A_AXIS], " B:", origin[B_AXIS]);
                ++new_rev_cnt;
            }
        }

        if (new_rev_cnt > rev_cnt) {
            // we got worse, likely we have moved the centroid: give up
            return false;
        }
        rev_cnt = new_rev_cnt;
    }
    if (rev_cnt) {
        // we left with unstable points, reject calibration
        return false;
    }

    SERIAL_ECHOLNPAIR("home grid origin A:", origin[A_AXIS], " B:", origin[B_AXIS]);
    return true;
}

bool corexy_rehome_xy(float fr_mm_s) {
    // enable endstops locally
    bool endstops_enabled = endstops.is_enabled();
    ScopeGuard endstop_restorer([&]() {
        endstops.enable(endstops_enabled);
    });
    endstops.enable(true);

    if (ENABLED(HOME_Y_BEFORE_X)) {
        if (!homeaxis(Y_AXIS, fr_mm_s, false, nullptr, false)) {
            return false;
        }
    }
    if (!homeaxis(X_AXIS, fr_mm_s, false, nullptr, false)) {
        return false;
    }
    if (DISABLED(HOME_Y_BEFORE_X)) {
        if (!homeaxis(Y_AXIS, fr_mm_s, false, nullptr, false)) {
            return false;
        }
    }
    return true;
}

// Refine home origin precisely on core-XY.
bool corexy_home_refine(float fr_mm_s, CoreXYCalibrationMode mode) {
    // finish previous moves and disable main endstop/crash recovery handling
    planner.synchronize();
#if ENABLED(CRASH_RECOVERY)
    Crash_Temporary_Deactivate ctd;
#endif /*ENABLED(CRASH_RECOVERY)*/

    // disable endstops locally
    bool endstops_enabled = endstops.is_enabled();
    ScopeGuard endstop_restorer([&]() {
        endstops.enable(endstops_enabled);
    });
    endstops.not_homing();

    // reset previous home state
    COREXY_HOME_UNSTABLE = false;

    // reposition parallel to the origin
    xyze_pos_t origin_tmp = current_position;
    origin_tmp[X_AXIS] = (base_home_pos(X_AXIS) - XY_HOMING_ORIGIN_OFFSET * X_HOME_DIR);
    origin_tmp[Y_AXIS] = (base_home_pos(Y_AXIS) - XY_HOMING_ORIGIN_OFFSET * Y_HOME_DIR);
    planner.buffer_line(origin_tmp, fr_mm_s, active_extruder);
    planner.synchronize();

    // align both motors to a full phase
    stepper_wait_for_standstill(_BV(A_AXIS) | _BV(B_AXIS));
    xy_long_t origin_steps = {
        stepper.position(A_AXIS) + phase_backoff_steps(A_AXIS),
        stepper.position(B_AXIS) + phase_backoff_steps(B_AXIS)
    };

    // sanity checks: don't remove these! Issues in repositioning are a result of planner/stepper
    // calculation issues which will show up elsewhere and are NOT just mechanical issues. We need
    // step-accuracy while homing! ask @wavexx when in doubt regarding these
    plan_corexy_raw_move(origin_steps, fr_mm_s);
    xy_long_t raw_move_diff = {
        stepper.position(A_AXIS) - origin_steps[A_AXIS],
        stepper.position(B_AXIS) - origin_steps[B_AXIS]
    };
    if (raw_move_diff[A_AXIS] != 0 || raw_move_diff[B_AXIS] != 0) {
        if (planner.draining()) {
            return false;
        }
        SERIAL_ECHOLN("raw move failed");
        SERIAL_ECHOLNPAIR("diff A:", raw_move_diff[A_AXIS], " B:", raw_move_diff[B_AXIS]);
        bsod("raw move didn't reach requested position");
    }

    stepper_wait_for_standstill(_BV(A_AXIS) | _BV(B_AXIS));
    if (!phase_aligned(A_AXIS) || !phase_aligned(B_AXIS)) {
        if (planner.draining()) {
            return false;
        }
        SERIAL_ECHOLN("phase alignment failed");
        SERIAL_ECHOLNPAIR("phase A:", axis_mscnt(A_AXIS), " B:", axis_mscnt(B_AXIS));
        bsod("phase alignment failed");
    }

    AxisEnum measured_axis = (X_HOME_DIR == Y_HOME_DIR ? B_AXIS : A_AXIS);

    // calibrate if not done already
    CoreXYGridOrigin calibrated_origin = config_store().corexy_grid_origin.get();
    if ((mode == CoreXYCalibrationMode::Force)
        || ((mode == CoreXYCalibrationMode::OnDemand) && calibrated_origin.uninitialized())) {
        SERIAL_ECHOLN("recalibrating homing origin");
        ui.status_printf_P(0, "Recalibrating home. Printer may vibrate and be noisier.");

        xy_pos_t origin, distance;
        if (!measure_origin_multipoint(measured_axis, origin_steps, origin, distance, fr_mm_s)) {
            SERIAL_ECHOLNPAIR("home origin calibration failed");
            return false;
        }

        LOOP_XY(axis) {
            calibrated_origin.origin[axis] = origin[axis];
            calibrated_origin.distance[axis] = distance[axis];
        }
        config_store().corexy_grid_origin.set(calibrated_origin);
    }
    xy_pos_t calibrated_origin_xy = { calibrated_origin.origin[A_AXIS], calibrated_origin.origin[B_AXIS] };

    // measure from current origin
    xy_pos_t c_dist, _;
    if (!measure_phase_cycles(measured_axis, c_dist, _)) {
        return false;
    }

    // validate current origin
    if (point_is_unstable(c_dist, calibrated_origin_xy)) {
        COREXY_HOME_UNSTABLE = true;
        SERIAL_ECHOLNPAIR("home point is unstable");
    }

    // validate from another point in the AB grid
    xy_long_t v_ab_off = { -1, 3 };
    plan_corexy_abgrid_move(origin_steps, v_ab_off, fr_mm_s);
    if (planner.draining()) {
        return false;
    }
    xy_pos_t v_c_dist;
    if (!measure_phase_cycles(measured_axis, v_c_dist, _)) {
        return false;
    }

    xy_long_t c_ab = cdist_translate(c_dist, calibrated_origin_xy);
    xy_long_t v_c_ab = cdist_translate(v_c_dist, calibrated_origin_xy);
    if (v_c_ab - v_ab_off != c_ab) {
        COREXY_HOME_UNSTABLE = true;
        SERIAL_ECHOLNPAIR("home validation point is invalid");
        return false;
    }
    if (point_is_unstable(v_c_dist, calibrated_origin_xy)) {
        COREXY_HOME_UNSTABLE = true;
        SERIAL_ECHOLNPAIR("home validation point is unstable");
    }

    // move back to origin
    plan_corexy_raw_move(origin_steps, fr_mm_s);
    if (planner.draining()) {
        return false;
    }

    // set machine origin
    xy_long_t c_ab_steps = {
        c_ab[X_HOME_DIR == Y_HOME_DIR ? A_AXIS : B_AXIS] * phase_cycle_steps(A_AXIS) * -Y_HOME_DIR,
        c_ab[X_HOME_DIR == Y_HOME_DIR ? B_AXIS : A_AXIS] * phase_cycle_steps(B_AXIS) * -X_HOME_DIR
    };
    xy_pos_t c_mm;
    corexy_ab_to_xy(c_ab_steps, c_mm);
    current_position.x = c_mm[X_AXIS] + origin_tmp[X_AXIS] + XY_HOMING_ORIGIN_OFFSET * X_HOME_DIR;
    current_position.y = c_mm[Y_AXIS] + origin_tmp[Y_AXIS] + XY_HOMING_ORIGIN_OFFSET * Y_HOME_DIR;
    planner.set_machine_position_mm(current_position);

    if (DEBUGGING(LEVELING)) {
        SERIAL_ECHOLNPAIR("calibrated home cycle A:", c_ab[A_AXIS], " B:", c_ab[B_AXIS]);
    }

    return true;
}

bool corexy_home_calibrated() {
    CoreXYGridOrigin calibrated_origin = config_store().corexy_grid_origin.get();
    return !calibrated_origin.uninitialized();
}

bool corexy_home_is_unstable() {
    CoreXYGridOrigin calibrated_origin = config_store().corexy_grid_origin.get();
    return calibrated_origin.uninitialized() || COREXY_HOME_UNSTABLE;
}
