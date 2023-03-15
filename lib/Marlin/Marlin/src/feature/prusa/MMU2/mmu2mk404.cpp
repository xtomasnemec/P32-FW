#include "mmu2mk404.h"
#include "mmu2_log.h"
#include "mmu2_fsensor.h"
#include "mmu2_reporting.h"
#include "mmu2_power.h"

#include "../../inc/MarlinConfig.h"
#include "../../gcode/gcode.h"
#include "../../lcd/ultralcd.h"
#include "../../libs/buzzer.h"
#include "../../libs/nozzle.h"
#include "../../module/temperature.h"
#include "../../module/planner.h"
#include "../../module/stepper/indirection.h"
#include "../../Marlin.h"
#include "tmp_error_converter.h"    //@@TODO temporary translator for error codes - to be removed once we get the MMU error screens
#include "tmp_progress_converter.h" //@@TODO temporary translator for progress codes - to be removed once we get the MMU status messages correctly

// @@TODO remove this and enable it in the configuration files
// Settings for filament load / unload from the LCD menu.
// This is for Prusa MK3-style extruders. Customize for your hardware.
#define MMU2_FILAMENTCHANGE_EJECT_FEED 80.0
#define MMU2_LOAD_TO_NOZZLE_SEQUENCE \
    { 7.2, 562 },                    \
        { 14.4, 871 },               \
        { 36.0, 1393 },              \
        { 14.4, 871 },               \
    { 50.0, 198 }

//@@TODO extract into configuration if it makes sense

// Nominal distance from the extruder gear to the nozzle tip is 87mm
// However, some slipping may occur and we need separate distances for
// LoadToNozzle and ToolChange.
// - +5mm seemed good for LoadToNozzle,
// - but too much (made blobs) for a ToolChange
static constexpr float MMU2_LOAD_TO_NOZZLE_LENGTH = 87.0F + 5.0F;

// As discussed with our PrusaSlicer profile specialist
// - ToolChange shall not try to push filament into the very tip of the nozzle
// to have some space for additional G-code to tune the extruded filament length
// in the profile
static constexpr float MMU2_TOOL_CHANGE_LOAD_LENGTH = 30.0F;

static constexpr float MMU2_LOAD_TO_NOZZLE_FEED_RATE = 20.0F;
static constexpr uint8_t MMU2_NO_TOOL = 99;
static constexpr uint32_t MMU_BAUD = 115200;

struct E_Step {
    float extrude;       ///< extrude distance in mm
    feedRate_t feedRate; ///< feed rate in mm/s
};

static constexpr E_Step ramming_sequence[] PROGMEM = FILAMENT_MMU2_RAMMING_SEQUENCE;
static constexpr E_Step load_to_nozzle_sequence[] PROGMEM = { MMU2_LOAD_TO_NOZZLE_SEQUENCE };

namespace MMU2 {

void execute_extruder_sequence(const E_Step *sequence, int steps);

MMU2 mmu2;

MMU2::MMU2()
    : logic(
#ifdef PRUSA_MMU2
        &mmu2Serial
#else
        nullptr
#endif
        )
    , extruder(MMU2_NO_TOOL)
    , resume_position()
    , resume_hotend_temp(0)
    , logicStepLastStatus(StepStatus::Finished)
    , state(State_t::Stopped)
    , mmu_print_saved(false)
    , loadFilamentStarted(false)
    , loadingToNozzle(false) {
}

#ifdef PRUSA_MMU2
void MMU2::Start() {
    mmu2Serial.begin(MMU_BAUD);

    PowerOn();
    mmu2Serial.flush(); // make sure the UART buffer is clear before starting communication

    extruder = MMU2_NO_TOOL;
    state = State_t::Connecting;

    // start the communication
    logic.Start();
}

void MMU2::Stop() {
    StopKeepPowered();
    PowerOff();
}

void MMU2::StopKeepPowered() {
    state = State_t::Stopped;
    logic.Stop();
    mmu2Serial.close();
}

void MMU2::Reset(ResetForm level) {
    switch (level) {
    case Software:
        ResetX0();
        break;
    case ResetPin:
        TriggerResetPin();
        break;
    case CutThePower:
        PowerCycle();
        break;
    default:
        break;
    }
}

void MMU2::ResetX0() {
    logic.ResetMMU(); // Send soft reset
}

void MMU2::TriggerResetPin() {
    reset();
}

void MMU2::PowerCycle() {
    // cut the power to the MMU and after a while restore it
    PowerOff();
    safe_delay(1000);
    PowerOn();
}

void MMU2::PowerOff() {
    power_off();
}

void MMU2::PowerOn() {
    power_on();
}

void MMU2::mmu_loop() {
    // We only leave this method if the current command was successfully completed - that's the Marlin's way of blocking operation
    // Atomic compare_exchange would have been the most appropriate solution here, but this gets called only in Marlin's task,
    // so thread safety should be kept
    static bool avoidRecursion = false;
    if (avoidRecursion)
        return;
    avoidRecursion = true;

    logicStepLastStatus = LogicStep(); // it looks like the mmu_loop doesn't need to be a blocking call

    avoidRecursion = false;
}

#else
void MMU2::Start() {}
void MMU2::Stop() {}
void MMU2::Reset(ResetForm /*level*/) {}
void MMU2::mmu_loop() {}
#endif

struct ReportingRAII {
    CommandInProgress cip;
    inline ReportingRAII(CommandInProgress cip)
        : cip(cip) {
        BeginReport(cip, (uint16_t)ProgressCode::EngagingIdler);
    }
    inline ~ReportingRAII() {
        EndReport(cip, (uint16_t)ProgressCode::OK);
    }
};

bool MMU2::WaitForMMUReady() {
    switch (State()) {
    case State_t::Stopped:
        return false;
    case State_t::Connecting:
        // shall we wait until the MMU reconnects?
        // fire-up a fsm_dlg and show "MMU not responding"?
    default:
        return true;
    }
}

bool MMU2::tool_change(uint8_t index) {
    if (!WaitForMMUReady())
        return false;

    if (index != extruder) {
        ReportingRAII rep(CommandInProgress::ToolChange);
        BlockRunoutRAII blockRunout;

        planner.synchronize();

        enable_E0();
        logic.ToolChange(index);       // let the MMU pull the filament out and push a new one in
        manage_response(false, false); // true, true);

        // reset current position to whatever the planner thinks it is
        // @@TODO is there some "standard" way of doing this?
        current_position.e = Planner::get_machine_position_mm()[3];

        extruder = index; //filament change is finished
        SetActiveExtruder(0);

        // @@TODO really report onto the serial? May be for the Octoprint? Not important now
        //        SERIAL_ECHO_START();
        //        SERIAL_ECHOLNPAIR(MSG_ACTIVE_EXTRUDER, int(extruder));
    }
    return true;
}

/// Handle special T?/Tx/Tc commands
///
///- T? Gcode to extrude shouldn't have to follow, load to extruder wheels is done automatically
///- Tx Same as T?, except nozzle doesn't have to be preheated. Tc must be placed after extruder nozzle is preheated to finish filament load.
///- Tc Load to nozzle after filament was prepared by Tx and extruder nozzle is already heated.
bool MMU2::tool_change(const char *special) {
    if (!WaitForMMUReady())
        return false;

#if 1
    BlockRunoutRAII blockRunout;

    switch (*special) {
    case '?': {
        uint8_t index = 0; // mmu2_choose_filament(); //@@TODO GUI - user selects
        while (!thermalManager.wait_for_hotend(active_extruder, false))
            safe_delay(100);
        load_filament_to_nozzle(index); // TODO this will not work
    } break;

    case 'x': {
        planner.synchronize();
        uint8_t index = 0; //mmu2_choose_filament(); //@@TODO GUI - user selects
        disable_E0();
        logic.ToolChange(index);
        manage_response(false, false); // true, true);

        enable_E0();
        extruder = index;
        SetActiveExtruder(0);
    } break;

    case 'c': {
        while (!thermalManager.wait_for_hotend(active_extruder, false))
            safe_delay(100);
        execute_extruder_sequence((const E_Step *)load_to_nozzle_sequence, COUNT(load_to_nozzle_sequence));
    } break;
    }

#endif
    return true;
}

uint8_t MMU2::get_current_tool() {
    return extruder == MMU2_NO_TOOL ? -1 : extruder;
}

bool MMU2::set_filament_type(uint8_t index, uint8_t type) {
    if (!WaitForMMUReady())
        return false;

    // @@TODO - this is not supported in the new MMU yet
    // cmd_arg = filamentType;
    // command(MMU_CMD_F0 + index);

    manage_response(false, false); // true, true);

    return true;
}

bool MMU2::unload() {
    if (!WaitForMMUReady())
        return false;

    logic.UnloadFilament();
    manage_response(false, false); // false, true);

    BUZZ(200, 404);

    // no active tool
    extruder = MMU2_NO_TOOL;
    return true;
}

bool MMU2::cut_filament(uint8_t index) {
    if (!WaitForMMUReady())
        return false;

    ReportingRAII rep(CommandInProgress::CutFilament);
    logic.CutFilament(index);
    manage_response(false, false); // false, true);

    return true;
}

bool MMU2::load_filament(uint8_t index) {
    if (!WaitForMMUReady())
        return false;

    ReportingRAII rep(CommandInProgress::LoadFilament);
    logic.LoadFilament(index);
    manage_response(false, false);
    BUZZ(200, 404);

    return true;
}

struct LoadingToNozzleRAII {
    MMU2 &mmu2;
    constexpr inline LoadingToNozzleRAII(MMU2 &mmu2)
        : mmu2(mmu2) {
        mmu2.loadingToNozzle = true;
    }
    inline ~LoadingToNozzleRAII() {
        mmu2.loadingToNozzle = false;
    }
};

bool MMU2::load_filament_to_nozzle(uint8_t index) {
    if (!WaitForMMUReady())
        return false;

    LoadingToNozzleRAII ln(*this);

    // used for MMU-menu operation "Load to Nozzle"
    BlockRunoutRAII blockRunout;

    enable_E0(); //why?

    if (extruder != MMU2_NO_TOOL) { // we already have some filament loaded - free it + shape its tip properly
        return false;
        // if i want to change it, i need to do it at higher level
        // cannot use filament_ramming();
    }

    logic.ToolChange(index);
    manage_response(false, false); // true, true);

    // reset current position to whatever the planner thinks it is
    // @@TODO is there some "standard" way of doing this?
    current_position.e = Planner::get_machine_position_mm()[3];

    extruder = index;
    SetActiveExtruder(0);

    //        BUZZ(200, 404);
    return true;
}

bool MMU2::eject_filament(uint8_t index, bool recover) {
    if (!WaitForMMUReady())
        return false;

    if (thermalManager.tooColdToExtrude(active_extruder)) {
        BUZZ(200, 404);
        LCD_ALERTMESSAGEPGM(MSG_HOTEND_TOO_COLD);
        return false;
    }

    ReportingRAII rep(CommandInProgress::EjectFilament);
    enable_E0();
    current_position.e -= MMU2_FILAMENTCHANGE_EJECT_FEED;
    line_to_current_position(2500 / 60);
    planner.synchronize();
    logic.EjectFilament(index);
    manage_response(false, false);

    if (recover) {
        //        LCD_MESSAGEPGM(MSG_MMU2_EJECT_RECOVER);
        BUZZ(200, 404);
        wait_for_user = true;
        //#if ENABLED(HOST_PROMPT_SUPPORT)
        //        host_prompt_do(PROMPT_USER_CONTINUE, PSTR("MMU2 Eject Recover"), PSTR("Continue"));
        //#endif
        //#if ENABLED(EXTENSIBLE_UI)
        //        ExtUI::onUserConfirmRequired_P(PSTR("MMU2 Eject Recover"));
        //#endif
        while (wait_for_user)
            idle(true);
        BUZZ(200, 404);
        BUZZ(200, 404);

        // logic.Command(); //@@TODO command(MMU_CMD_R0);
        manage_response(false, false);
    }

    ui.reset_status();

    // no active tool
    extruder = MMU2_NO_TOOL;

    BUZZ(200, 404);

    disable_E0();

    return true;
}

void MMU2::Button(uint8_t index) {
    logic.Button(index);
}

void MMU2::Home(uint8_t mode) {
    logic.Home(mode);
}

void MMU2::SaveAndPark(bool move_axes, bool turn_off_nozzle) {
    static constexpr xyz_pos_t park_point = NOZZLE_PARK_POINT_M600;
    if (!mmu_print_saved) { // First occurrence. Save current position, park print head, disable nozzle heater.
        LogEchoEvent("Saving and parking");
        planner.synchronize();

        mmu_print_saved = true;

        resume_hotend_temp = thermalManager.degTargetHotend(active_extruder);
        resume_position = current_position;

        if (move_axes && all_axes_homed())
            nozzle.park(2, park_point);

        if (turn_off_nozzle) {
            LogEchoEvent("Heater off");
            thermalManager.setTargetHotend(0, active_extruder);
        }
    }
    // keep the motors powered forever (until some other strategy is chosen)
    gcode.reset_stepper_timeout();
}

void MMU2::ResumeAndUnPark(bool move_axes, bool turn_off_nozzle) {
    if (mmu_print_saved) {
        LogEchoEvent("Resuming print");

        if (turn_off_nozzle && resume_hotend_temp) {
            MMU2_ECHO_MSG("Restoring hotend temperature ");
            SERIAL_ECHOLN(resume_hotend_temp);
            thermalManager.setTargetHotend(resume_hotend_temp, active_extruder);

            while (!thermalManager.wait_for_hotend(active_extruder, false)) {
                safe_delay(1000);
            }
            LogEchoEvent("Hotend temperature reached");
        }

        if (move_axes && all_axes_homed()) {
            LogEchoEvent("Resuming XYZ");

            // Move XY to starting position, then Z
            do_blocking_move_to_xy(resume_position, feedRate_t(NOZZLE_PARK_XY_FEEDRATE));

            // Move Z_AXIS to saved position
            do_blocking_move_to_z(resume_position.z, feedRate_t(NOZZLE_PARK_Z_FEEDRATE));
        } else {
            LogEchoEvent("NOT resuming XYZ");
        }
    }
}

void MMU2::CheckUserInput() {
    auto btn = ButtonPressed((uint16_t)lastErrorCode);
    switch (btn) {
    case Left:
    case Middle:
    case Right:
        Button(btn);
        break;
    case RestartMMU:
        Reset(CutThePower);
        break;
    case StopPrint:
        // @@TODO not sure if we shall handle this high level operation at this spot
        break;
    default:
        break;
    }
}

/// Originally, this was used to wait for response and deal with timeout if necessary.
/// The new protocol implementation enables much nicer and intense reporting, so this method will boil down
/// just to verify the result of an issued command (which was basically the original idea)
///
/// It is closely related to mmu_loop() (which corresponds to our ProtocolLogic::Step()), which does NOT perform any blocking wait for a command to finish.
/// But - in case of an error, the command is not yet finished, but we must react accordingly - move the printhead elsewhere, stop heating, eat a cat or so.
/// That's what's being done here...
void MMU2::manage_response(const bool move_axes, const bool turn_off_nozzle) {
    mmu_print_saved = false;

    KEEPALIVE_STATE(PAUSED_FOR_USER);

    for (;;) {
        // in our new implementation, we know the exact state of the MMU at any moment, we do not have to wait for a timeout
        // So in this case we shall decide if the operation is:
        // - still running -> wait normally in idle()
        // - failed -> then do the safety moves on the printer like before
        // - finished ok -> proceed with reading other commands

        idle(true); // calls LogicStep() and remembers its return status

        switch (logicStepLastStatus) {
        case Finished:
            // command/operation completed, let Marlin continue its work
            // the E may have some more moves to finish - wait for them
            planner.synchronize();
            return;
        case VersionMismatch: // this basically means the MMU will be disabled until reconnected
            return;
        case CommunicationTimeout:
        case CommandError:
        case ProtocolError:
            SaveAndPark(move_axes, turn_off_nozzle); // and wait for the user to resolve the problem
            CheckUserInput();
            break;
        case CommunicationRecovered: // @@TODO communication recovered and may be an error recovered as well
            // may be the logic layer can detect the change of state a respond with one "Recovered" to be handled here
            ResumeAndUnPark(move_axes, turn_off_nozzle);
            break;
        case Processing: // wait for the MMU to respond
        default:
            break;
        }
    }
}

StepStatus MMU2::LogicStep() {
    StepStatus ss = logic.Step();
    switch (ss) {
    case Finished:
    case Processing:
        OnMMUProgressMsg(logic.Progress());
        break;
    case CommandError:
        ReportError(logic.Error());
        break;
    case CommunicationTimeout:
        state = State_t::Connecting;
        ReportError(ErrorCode::MMU_NOT_RESPONDING);
        break;
    case ProtocolError:
        state = State_t::Connecting;
        ReportError(ErrorCode::PROTOCOL_ERROR);
        break;
    case VersionMismatch:
        StopKeepPowered();
        ReportError(ErrorCode::VERSION_MISMATCH);
        break;
    default:
        break;
    }

    if (logic.Running()) {
        state = State_t::Active;
    }
    return ss;
}

void MMU2::filament_ramming() {
    execute_extruder_sequence((const E_Step *)ramming_sequence, sizeof(ramming_sequence) / sizeof(E_Step));
}

void MMU2::execute_extruder_sequence(const E_Step *sequence, int steps) {

    planner.synchronize();
    enable_E0();

    const E_Step *step = sequence;

    for (uint8_t i = 0; i < steps; i++) {
        const float es = pgm_read_float(&(step->extrude));
        const feedRate_t fr_mm_m = pgm_read_float(&(step->feedRate));

        //    DEBUG_ECHO_START();
        //    DEBUG_ECHOLNPAIR("E step ", es, "/", fr_mm_m);

        current_position.e += es;
        line_to_current_position(MMM_TO_MMS(fr_mm_m));
        planner.synchronize();

        step++;
    }

    disable_E0();
}

void MMU2::SetActiveExtruder(uint8_t ex) {
#ifdef PRUSA_MMU2
    active_extruder = ex;
#endif
}

constexpr int strlen_constexpr(const char *str) {
    return *str ? 1 + strlen_constexpr(str + 1) : 0;
}

void MMU2::ReportError(ErrorCode ec) {
    // Due to a potential lossy error reporting layers linked to this hook
    // we'd better report everything to make sure especially the error states
    // do not get lost.
    // - The good news here is the fact, that the MMU reports the errors repeatedly until resolved.
    // - The bad news is, that MMU not responding may repeatedly occur on printers not having the MMU at all.
    //
    // Not sure how to properly handle this situation, options:
    // - skip reporting "MMU not responding" (at least for now)
    // - report only changes of states (we can miss an error message)
    // - may be some combination of MMUAvailable + UseMMU flags and decide based on their state
    // Right now the filtering of MMU_NOT_RESPONDING is done in ReportErrorHook() as it is not a problem if mmu2mk404.cpp
    ReportErrorHook((CommandInProgress)logic.CommandInProgress(), (uint16_t)ec);

    if (ec != lastErrorCode) { // deduplicate: only report changes in error codes into the log
        lastErrorCode = ec;

        // Log error format: MMU2:E=32766 TextDescription
        char msg[64];
        snprintf(msg, sizeof(msg), "MMU2:E=%hu", (uint16_t)ec);
        // Append a human readable form of the error code(s)
        TranslateErr((uint16_t)ec, msg, sizeof(msg));

        // beware - the prefix in the message ("MMU2") will get stripped by the logging subsystem
        // and a correct MMU2 component will be assigned accordingly - see appmain.cpp
        // Therefore I'm not calling MMU2_ERROR_MSG or MMU2_ECHO_MSG here
        SERIAL_ECHO_START();
        SERIAL_ECHOLN(msg);
    }

    static_assert(mmu2Magic[0] == 'M'
            && mmu2Magic[1] == 'M'
            && mmu2Magic[2] == 'U'
            && mmu2Magic[3] == '2'
            && mmu2Magic[4] == ':'
            && strlen_constexpr(mmu2Magic) == 5,
        "MMU2 logging prefix mismatch, must be updated at various spots");
}

void MMU2::ReportProgress(ProgressCode pc) {
    ReportProgressHook((CommandInProgress)logic.CommandInProgress(), (uint16_t)pc);

    // Log progress - example: MMU2:P=123 EngageIdler
    char msg[64];
    snprintf(msg, sizeof(msg), "MMU2:P=%hu", (uint16_t)pc);
    // Append a human readable form of the progress code
    TranslateProgress((uint16_t)pc, msg, sizeof(msg));

    SERIAL_ECHO_START();
    SERIAL_ECHOLN(msg);
}

void MMU2::OnMMUProgressMsg(ProgressCode pc) {
    if (pc != lastProgressCode) {
        ReportProgress(pc);
        lastProgressCode = pc;

        // Act accordingly - one-time handling
        switch (pc) {
        case ProgressCode::FeedingToBondtech:
            // prepare for the movement of the E-motor
            planner.synchronize();
            sync_plan_position();
            enable_E0();
            loadFilamentStarted = true;
            break;
        default:
            // do nothing yet
            break;
        }
    } else {
        // Act accordingly - every status change (even the same state)
        switch (pc) {
        case ProgressCode::FeedingToBondtech:
            if (WhereIsFilament() == FilamentState::AT_FSENSOR && loadFilamentStarted) { // fsensor triggered, move the extruder to help loading
                // rotate the extruder motor - no planner sync, just add more moves - as long as they are roughly at the same speed as the MMU is pushing,
                // it really doesn't matter
                // char tmp[64]; // @@TODO this shouldn't be needed anymore, but kept here in case of something strange
                //               // happens in Marlin again
                // snprintf(tmp,sizeof (tmp), "E moveTo=%4.1f f=%4.0f s=%hu\n", current_position.e, feedrate_mm_s, feedrate_percentage);
                // MMU2_ECHO_MSG(tmp);

                // Ideally we'd use:
                // line_to_current_position(MMU2_LOAD_TO_NOZZLE_FEED_RATE);
                // However, as it ignores MBL completely (which I don't care about in case of E-movement),
                // we need to take the raw Z coordinates and only add some movement to E
                // otherwise we risk planning a very short Z move with an extremely long E-move,
                // which obviously ends up in a disaster (over/underflow of E/Z steps).
                // The problem becomes obvious in Planner::_populate_block when computing da, db, dc like this:
                //   const int32_t da = target.a - position.a, db = target.b - position.b, dc = target.c - position.c;
                // And since current_position[3] != position_float[3], we get a tiny move in Z, which is something I really want to avoid here
                // @@TODO is there a "standard" way of doing this?
                xyze_pos_t tgt = Planner::get_machine_position_mm();
                const float e = loadingToNozzle ? MMU2_LOAD_TO_NOZZLE_LENGTH : MMU2_TOOL_CHANGE_LOAD_LENGTH;
                tgt[3] += e / planner.e_factor[active_extruder];
                planner.buffer_line(tgt, MMU2_LOAD_TO_NOZZLE_FEED_RATE, active_extruder); // @@TODO magic constant - must match the feedrate of the MMU
                loadFilamentStarted = false;
            }
            break;
        default:
            // do nothing yet
            break;
        }
    }
}

void MMU2::LogErrorEvent(const char *msg) {
    MMU2_ERROR_MSG(msg);
    SERIAL_ECHOLN();
}

void MMU2::LogEchoEvent(const char *msg) {
    MMU2_ECHO_MSG(msg);
    SERIAL_ECHOLN();
}

} // namespace MMU2