/**
 * @file selftest_loadcell.cpp
 * @author Radek Vana
 * @copyright Copyright (c) 2021
 */

#include "selftest_loadcell.h"
#include "wizard_config.hpp"
#include "config_features.h" //EXTRUDER_AUTO_loadcell_TEMPERATURE
#include "marlin_server.hpp"
#include "selftest_log.hpp"
#include "loadcell.h"
#include "../../Marlin/src/module/temperature.h"
#include "../../Marlin/src/module/planner.h"
#include "../../Marlin/src/module/stepper.h"
#include "../../Marlin/src/module/endstops.h"
#include "../../Marlin/src/feature/prusa/homing.h"
#include "i_selftest.hpp"
#include "algorithm_scale.hpp"
#include <climits>

LOG_COMPONENT_REF(Selftest);
using namespace selftest;

CSelftestPart_Loadcell::CSelftestPart_Loadcell(IPartHandler &state_machine, const LoadcellConfig_t &config,
    SelftestLoadcell_t &result)
    : rStateMachine(state_machine)
    , rConfig(config)
    , rResult(result)
    , currentZ(0.F)
    , targetZ(0.F)
    , begin_target_temp(thermalManager.temp_hotend[0].target)
    , time_start(SelftestInstance().GetTime())
    , log(1000)
    , log_fast(100) // this is only during 1s (will generate 9-10 logs)
{
    thermalManager.temp_hotend[0].target = 0;
    endstops.enable(true);
    log_info(Selftest, "%s Started", rConfig.partname);
}

CSelftestPart_Loadcell::~CSelftestPart_Loadcell() {
    thermalManager.temp_hotend[0].target = begin_target_temp;
    endstops.enable(false);
}

LoopResult CSelftestPart_Loadcell::stateMoveUpInit() {
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_move_away);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateMoveUp() {
    planner.synchronize(); // finish current move (there should be none)
    endstops.validate_homing_move();

    set_current_from_steppers();
    sync_plan_position();

    // Disable stealthChop if used. Enable diag1 pin on driver.
#if ENABLED(SENSORLESS_HOMING)
    start_sensorless_homing_per_axis(AxisEnum::Z_AXIS);
#endif

    currentZ = current_position.z;
    targetZ = rConfig.z_extra_pos;
    if (targetZ > currentZ) {
        log_info(Selftest, "%s move up, target: %f current: %f", rConfig.partname, double(targetZ), double(currentZ));
        current_position.z = rConfig.z_extra_pos;
        line_to_current_position(rConfig.z_extra_pos_fr);
    } else {
        log_info(Selftest, "%s move up not needed, target: %f > current: %f", rConfig.partname, double(targetZ), double(currentZ));
    }
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateMoveUpWaitFinish() {
    if (planner.movesplanned()) {
        currentZ = current_position.z;
        return LoopResult::RunCurrent;
    }
    log_info(Selftest, "%s move up finished", rConfig.partname);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateToolSelectInit() {
    if (active_extruder != rConfig.tool_nr) {
        IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_tool_select);

        marlin_server_enqueue_gcode_printf("T%d S1", rConfig.tool_nr);

        // go to some reasonable position
        // Use reasonable feedrate as it was likely set by previous Z move
        marlin_server_enqueue_gcode_printf("G0 X50 Y50 F%d", XY_PROBE_SPEED_INITIAL);
    }
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateToolSelectWaitFinish() {
    if (planner.movesplanned()) {
        return LoopResult::RunCurrent;
    }
    return LoopResult::RunNext;
}

//disconnected sensor -> raw_load == 0
//but raw_load == 0 is also valid value
//test rely on hw being unstable, raw_load must be different from 0 at least once during test period
LoopResult CSelftestPart_Loadcell::stateConnectionCheck() {
    int32_t raw_load = loadcell.GetRawValue();
    if (raw_load == INT32_MIN) {
        log_error(Selftest, "%s returned _I32_MIN", rConfig.partname);
        IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_fail);
        return LoopResult::Fail;
    }
    if (raw_load == INT32_MAX) {
        log_error(Selftest, "%s returned _I32_MAX", rConfig.partname);
        IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_fail);
        return LoopResult::Fail;
    }
    SensorData::SensorDataBuffer data_buffer;
    osDelay(200); // wait for some samples
    auto val = data_buffer.GetValue(SensorData::Sensor::loadCell);
    if (!val.attribute.valid || raw_load == 0) {
        if ((SelftestInstance().GetTime() - time_start) > rConfig.max_validation_time) {
            log_error(Selftest, "%s invalid", rConfig.partname);
            IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_fail);
            return LoopResult::Fail;
        } else {
            log_debug(Selftest, "%s data not ready", rConfig.partname);
            return LoopResult::RunCurrent;
        }
    }
    loadcell.EnableHighPrecision();
    loadcell.Tare(Loadcell::TareMode::Continuous);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateCooldownInit() {
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_cooldown);
    thermalManager.setTargetHotend(0, rConfig.tool_nr); // Disable heating for tested hotend
    marlin_server_set_temp_to_display(0, rConfig.tool_nr);
    const float temp = thermalManager.degHotend(rConfig.tool_nr);
    need_cooling = temp > rConfig.cool_temp; // Check if temperature is safe
    if (need_cooling) {
        log_info(Selftest, "%s cooling needed, target: %d current: %f", rConfig.partname, rConfig.cool_temp, (double)temp);
        rConfig.print_fan.EnterSelftestMode();
        rConfig.heatbreak_fan.EnterSelftestMode();
        rConfig.print_fan.SelftestSetPWM(255);     // it will be restored by ExitSelftestMode
        rConfig.heatbreak_fan.SelftestSetPWM(255); // it will be restored by ExitSelftestMode
        log_info(Selftest, "%s fans set to maximum", rConfig.partname);
    }
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateCooldown() {
    const float temp = thermalManager.degHotend(rConfig.tool_nr);

    // still cooling
    if (temp > rConfig.cool_temp) {
        LogInfoTimed(log, "%s cooling down, target: %d current: %f", rConfig.partname, rConfig.cool_temp, (double)temp);
        return LoopResult::RunCurrent;
    }

    log_info(Selftest, "%s cooled down", rConfig.partname);
    return LoopResult::RunNext; // cooled
}

LoopResult CSelftestPart_Loadcell::stateCooldownDeinit() {
    if (need_cooling) { // if cooling was needed, return control of fans
        rConfig.print_fan.ExitSelftestMode();
        rConfig.heatbreak_fan.ExitSelftestMode();
        log_info(Selftest, "%s fans disabled", rConfig.partname);
    }
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateCycleMark() {
    return LoopResult::MarkLoop;
}

LoopResult CSelftestPart_Loadcell::stateAskAbortInit() {
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_user_tap_ask_abort);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateAskAbort() {
    const Response response = rStateMachine.GetButtonPressed();
    switch (response) {
    case Response::Abort: // Abort is automatic at state machine level, it should never get here
        log_error(Selftest, "%s user pressed abort, code should not reach this place", rConfig.partname);
        return LoopResult::Abort;
    case Response::Continue:
        log_info(Selftest, "%s user pressed continue", rConfig.partname);
        return LoopResult::RunNext;
    default:
        break;
    }
    return LoopResult::RunCurrent;
}

LoopResult CSelftestPart_Loadcell::stateTapCheckCountDownInit() {
    time_start_countdown = SelftestInstance().GetTime();
    rResult.countdown = SelftestLoadcell_t::countdown_undef;
    rResult.pressed_too_soon = true;
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_user_tap_countdown);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateTapCheckCountDown() {
    int32_t load = loadcell.GetHighPassLoad();
    rResult.progress = scale_percent_avoid_overflow(load, int32_t(0), rConfig.tap_min_load_ok); // tap_min_load_ok is really maximum value, not a bug
    if (load >= rConfig.countdown_load_error_value) {
        log_info(Selftest, "%s load during countdown %dg exceeded error value %dg", rConfig.partname, load, rConfig.countdown_load_error_value);
        rResult.pressed_too_soon = true;
        return LoopResult::GoToMark;
    }
    LogDebugTimed(log, "%s load during countdown %dg", rConfig.partname, load);

    uint32_t countdown_running_ms = SelftestInstance().GetTime() - time_start_countdown;
    uint8_t new_countdown = std::min(int32_t(countdown_running_ms / 1000), int32_t(rConfig.countdown_sec));
    new_countdown = rConfig.countdown_sec - new_countdown;

    rResult.countdown = new_countdown;

    if (countdown_running_ms >= rConfig.countdown_sec * 1000)
        return LoopResult::RunNext;
    else
        return LoopResult::RunCurrent;
}

LoopResult CSelftestPart_Loadcell::stateTapCheckInit() {
    rResult.countdown = SelftestLoadcell_t::countdown_undef;
    time_start_tap = SelftestInstance().GetTime();
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_user_tap_check);
    return LoopResult::RunNext;
}

LoopResult CSelftestPart_Loadcell::stateTapCheck() {
    if ((SelftestInstance().GetTime() - time_start_tap) >= rConfig.tap_timeout_ms) {
        log_info(Selftest, "%s user did not tap", rConfig.partname);
        return LoopResult::GoToMark; // timeout, retry entire touch sequence
    }

    int32_t load = loadcell.GetHighPassLoad();
    bool pass = IsInClosedRange(load, rConfig.tap_min_load_ok, rConfig.tap_max_load_ok);
    if (pass) {
        log_info(Selftest, "%s tap check, load %dg successful in range <%d, %d>", rConfig.partname, load, rConfig.tap_min_load_ok, rConfig.tap_max_load_ok);
    } else {
        LogInfoTimed(log_fast, "%s tap check, load %dg not in range <%d, %d>", rConfig.partname, load, rConfig.tap_min_load_ok, rConfig.tap_max_load_ok);
    }

    rResult.progress = scale_percent_avoid_overflow(load, int32_t(0), rConfig.tap_min_load_ok); // tap_min_load_ok is really maximum value, not a bug
    return pass ? LoopResult::RunNext : LoopResult::RunCurrent;
}

LoopResult CSelftestPart_Loadcell::stateTapOk() {
    log_info(Selftest, "%s finished", rConfig.partname);
    IPartHandler::SetFsmPhase(PhasesSelftest::Loadcell_user_tap_ok);
    return LoopResult::RunNext;
    rResult.progress = 100;
}
