/*
 * Snappy Turns
 *
 * Faster, more responsive turning for Link and Wolf Link in Dusklight.
 *
 * Vanilla turning (daAlink_c::setSpeedAndAngleNormal / setSpeedAndAngleWolf):
 *   - Each frame, facing eases toward the stick heading by 1/rate of the
 *     remaining angle, capped per frame, and the cap shrinks with stick tilt
 *     squared. That easing is the "delay before Link faces where I pushed".
 *   - A large reversal makes the movement function bail out without turning,
 *     and the caller (checkNextAction / checkNextActionWolf) then picks a
 *     turnaround, skid or slip animation proc.
 *
 * What this mod does, entirely through pre/post hooks on those two functions:
 *   1. Turn speed: measures how far the game itself turned Link this frame and
 *      multiplies that step, clamped so it never overshoots the stick heading.
 *      Scaling the game's own step keeps every vanilla rule intact (stick tilt,
 *      cutscenes, walls, the frames where vanilla deliberately does not turn).
 *   2. Quick turnaround (optional): when the stick reverses, snap facing to the
 *      stick heading before the caller evaluates it, so the turnaround / skid
 *      proc is never chosen. Speed is halved on the pivot frame so the reversal
 *      reads as a pivot rather than a teleport of momentum.
 *
 * Only ordinary free movement is touched (PROC_WAIT / PROC_MOVE and the wolf
 * equivalents). Z-targeting, aiming, swimming, climbing, events, ice and Iron
 * Boots are left to vanilla. The original function always runs; nothing here
 * skips it, so other mods hooking the same functions keep working.
 */

#include <cstdint>
#include <cstdlib>

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/ui.h"

#include "d/actor/d_a_alink.h"
#include "d/d_bg_w_base.h"

DEFINE_MOD();

IMPORT_SERVICE(HookService,   svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService,     svc_ui);

/* ------------------------------------------------------------------
 *  Configuration
 * ------------------------------------------------------------------ */
static ConfigVarHandle g_cfg_link_enabled   = 0;
static ConfigVarHandle g_cfg_wolf_enabled   = 0;
static ConfigVarHandle g_cfg_link_speed     = 0;
static ConfigVarHandle g_cfg_wolf_speed     = 0;
static ConfigVarHandle g_cfg_quick_turn     = 0;

static constexpr int64_t kSpeedMin     = 100;  /* percent: 100 = vanilla */
static constexpr int64_t kSpeedMax     = 500;  /* 500 turns the full remaining angle in one frame */
static constexpr int64_t kSpeedStep    = 25;
static constexpr int64_t kSpeedDefault = 250;

static bool cfg_bool(ConfigVarHandle h) {
    bool v = false;
    svc_config->get_bool(mod_ctx, h, &v);
    return v;
}

static float cfg_speed_multiplier(ConfigVarHandle h) {
    int64_t v = kSpeedDefault;
    svc_config->get_int(mod_ctx, h, &v);
    if (v < kSpeedMin) v = kSpeedMin;
    if (v > kSpeedMax) v = kSpeedMax;
    return static_cast<float>(v) / 100.0f;
}

/* ------------------------------------------------------------------
 *  Angle helpers (s16 binary angles, 0x10000 = 360 degrees)
 * ------------------------------------------------------------------ */
static int angle_distance(s16 a, s16 b) {
    return std::abs(static_cast<int>(static_cast<s16>(a - b)));
}

/* Extend the step the game took this frame (pre -> post) by (mult - 1) times
 * itself, never past target and never in a direction the game did not turn. */
static s16 extend_turn(s16 pre, s16 post, s16 target, float mult) {
    const int step = static_cast<s16>(post - pre);
    if (step == 0 || mult <= 1.0f) return post;

    const int remaining = static_cast<s16>(target - post);
    if (remaining == 0 || (step > 0) != (remaining > 0)) return post;

    int extra = static_cast<int>(static_cast<float>(step) * (mult - 1.0f));
    if (std::abs(extra) > std::abs(remaining)) extra = remaining;
    return static_cast<s16>(post + extra);
}

/* ------------------------------------------------------------------
 *  Shared state: facing captured before the game's turn step
 * ------------------------------------------------------------------ */
struct TurnCapture {
    daAlink_c* link = nullptr;
    s16 angle = 0;
    s16 shape = 0;
};

static TurnCapture g_human_capture;
static TurnCapture g_wolf_capture;

static void capture(TurnCapture& c, daAlink_c* link) {
    c.link  = link;
    c.angle = link->current.angle.y;
    c.shape = link->shape_angle.y;
}

/* Returns the capture if it belongs to this call, and clears it either way so
 * a skipped pre-hook can never leave stale data for a later post-hook. */
static bool take_capture(TurnCapture& c, daAlink_c* link, TurnCapture& out) {
    const bool ok = (c.link != nullptr && c.link == link);
    out = c;
    c.link = nullptr;
    return ok;
}

/* ------------------------------------------------------------------
 *  Link (human form)
 * ------------------------------------------------------------------ */
DEFINE_HOOK(&daAlink_c::setSpeedAndAngleNormal, SpeedAndAngleNormal);

static bool human_free_movement(daAlink_c* link) {
    if (link->mProcID != daAlink_c::PROC_WAIT && link->mProcID != daAlink_c::PROC_MOVE) return false;
    if (link->mTargetedActor != nullptr || link->checkAttentionLock()) return false;
    return true;
}

static HookAction on_normal_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    g_human_capture.link = nullptr;
    if (link == nullptr || !cfg_bool(g_cfg_link_enabled)) return HOOK_CONTINUE;
    if (!human_free_movement(link)) return HOOK_CONTINUE;
    capture(g_human_capture, link);
    return HOOK_CONTINUE;
}

static void on_normal_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    TurnCapture c;
    if (link == nullptr || !take_capture(g_human_capture, link, c)) return;
    if (!human_free_movement(link) || !link->checkInputOnR()) return;

    const s16 target = link->mMoveAngle;

    /* Quick turnaround. Vanilla's caller picks procWaitTurn above 0x7800 and
     * procMoveTurn / procSlip above 0x6000 (DIR_BACKWARD); snapping here, before
     * the caller looks, means none of those are chosen. */
    if (cfg_bool(g_cfg_quick_turn) &&
        angle_distance(target, link->current.angle.y) > 0x6000 &&
        !link->checkEventRun() &&
        !link->checkMagneBootsOn() &&
        link->mGndPolySpecialCode != dBgW_SPCODE_ICE)
    {
        link->current.angle.y = target;
        link->shape_angle.y   = target;
        link->speedF       *= 0.5f;
        link->mNormalSpeed *= 0.5f;
        return;
    }

    const float mult = cfg_speed_multiplier(g_cfg_link_speed);
    link->current.angle.y = extend_turn(c.angle, link->current.angle.y, target, mult);
    link->shape_angle.y   = extend_turn(c.shape, link->shape_angle.y,   target, mult);
}

/* ------------------------------------------------------------------
 *  Wolf Link
 * ------------------------------------------------------------------ */
DEFINE_HOOK(&daAlink_c::setSpeedAndAngleWolf, SpeedAndAngleWolf);

static bool wolf_free_movement(daAlink_c* link) {
    return link->mProcID == daAlink_c::PROC_WOLF_WAIT ||
           link->mProcID == daAlink_c::PROC_WOLF_MOVE;
}

static HookAction on_wolf_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    g_wolf_capture.link = nullptr;
    if (link == nullptr || !cfg_bool(g_cfg_wolf_enabled)) return HOOK_CONTINUE;
    if (!wolf_free_movement(link)) return HOOK_CONTINUE;
    capture(g_wolf_capture, link);
    return HOOK_CONTINUE;
}

static void on_wolf_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    TurnCapture c;
    if (link == nullptr || !take_capture(g_wolf_capture, link, c)) return;
    if (!wolf_free_movement(link) || !link->checkInputOnR()) return;

    const s16 target = link->mMoveAngle;
    const s16 shape_target = link->checkWolfShapeReverse() ? static_cast<s16>(target + 0x8000) : target;

    /* The game reverted the turn because the wolf is against a wall
     * (wolfSideBgCheck). Respect it. */
    if (link->checkEndResetFlg1(daPy_py_c::ERFLG1_UNK_200000)) return;

    /* Quick turnaround. Vanilla's caller picks procWolfSlip / procWolfSlipTurn
     * above 0x7000 while not on a steep tilt (field_0x3180). */
    if (cfg_bool(g_cfg_quick_turn) &&
        angle_distance(target, link->current.angle.y) > 0x7000 &&
        std::abs(link->field_0x3180) < 0x5000 &&
        !link->checkEventRun() &&
        link->mGndPolySpecialCode != dBgW_SPCODE_ICE)
    {
        link->current.angle.y = target;
        link->shape_angle.y   = shape_target;
        link->speedF       *= 0.5f;
        link->mNormalSpeed *= 0.5f;
        return;
    }

    const float mult = cfg_speed_multiplier(g_cfg_wolf_speed);
    link->current.angle.y = extend_turn(c.angle, link->current.angle.y, target, mult);
    link->shape_angle.y   = extend_turn(c.shape, link->shape_angle.y,   shape_target, mult);
}

/* ------------------------------------------------------------------
 *  Mods menu panel
 * ------------------------------------------------------------------ */
static void add_toggle(ModContext* ctx, UiElementHandle panel, const char* label,
                       const char* help, ConfigVarHandle var) {
    UiControlDesc d = UI_CONTROL_DESC_INIT;
    d.kind = UI_CONTROL_TOGGLE;
    d.label = label;
    d.help_rml = help;
    d.binding = UI_BINDING_CONFIG_VAR;
    d.config_var = var;
    svc_ui->pane_add_control(ctx, panel, &d, nullptr);
}

static void add_speed(ModContext* ctx, UiElementHandle panel, const char* label,
                      const char* help, ConfigVarHandle var) {
    UiControlDesc d = UI_CONTROL_DESC_INIT;
    d.kind = UI_CONTROL_NUMBER;
    d.label = label;
    d.help_rml = help;
    d.binding = UI_BINDING_CONFIG_VAR;
    d.config_var = var;
    d.min = kSpeedMin;
    d.max = kSpeedMax;
    d.step = kSpeedStep;
    d.suffix = "%";
    svc_ui->pane_add_control(ctx, panel, &d, nullptr);
}

static ModResult on_build_panel(ModContext* ctx, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(ctx, panel, "Link");
    add_toggle(ctx, panel, "Snappy turns for Link",
        "Faster turning for Link in human form during normal movement.", g_cfg_link_enabled);
    add_speed(ctx, panel, "Link turn speed",
        "How quickly Link turns to face the stick. 100% is vanilla; 500% faces the stick "
        "almost immediately.", g_cfg_link_speed);

    svc_ui->pane_add_section(ctx, panel, "Wolf Link");
    add_toggle(ctx, panel, "Snappy turns for Wolf Link",
        "Faster turning for Wolf Link during normal movement and sustained dashing. The initial "
        "dash burst keeps vanilla steering.", g_cfg_wolf_enabled);
    add_speed(ctx, panel, "Wolf Link turn speed",
        "How quickly Wolf Link turns to face the stick. 100% is vanilla; 500% faces the "
        "stick almost immediately.", g_cfg_wolf_speed);

    svc_ui->pane_add_section(ctx, panel, "Turnarounds");
    add_toggle(ctx, panel, "Quick turnaround",
        "Reversing the stick pivots instantly instead of playing the turnaround, skid or "
        "slip animation. Applies to each form that has Snappy Turns enabled. Ice and Iron "
        "Boots keep vanilla behavior.", g_cfg_quick_turn);
    return MOD_OK;
}

/* ------------------------------------------------------------------
 *  Entry points
 * ------------------------------------------------------------------ */
static ModResult register_bool(const char* name, bool def, ConfigVarHandle* out) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = CONFIG_VAR_BOOL;
    d.default_bool = def;
    return svc_config->register_var(mod_ctx, &d, out);
}

static ModResult register_int(const char* name, int64_t def, ConfigVarHandle* out) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = CONFIG_VAR_INT;
    d.default_int = def;
    return svc_config->register_var(mod_ctx, &d, out);
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;

    r = register_bool("link_enabled", true, &g_cfg_link_enabled);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var link_enabled");
    r = register_bool("wolf_enabled", true, &g_cfg_wolf_enabled);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var wolf_enabled");
    r = register_int("link_turn_speed", kSpeedDefault, &g_cfg_link_speed);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var link_turn_speed");
    r = register_int("wolf_turn_speed", kSpeedDefault, &g_cfg_wolf_speed);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var wolf_turn_speed");
    r = register_bool("quick_turnaround", true, &g_cfg_quick_turn);
    if (r != MOD_OK) return mods::set_error(error, r, "register_var quick_turnaround");

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = on_build_panel;
    r = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (r != MOD_OK) return mods::set_error(error, r, "register_mods_panel");

    r = mods::hook::add_pre<SpeedAndAngleNormal>(svc_hook, on_normal_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "setSpeedAndAngleNormal pre");
    r = mods::hook::add_post<SpeedAndAngleNormal>(svc_hook, on_normal_post);
    if (r != MOD_OK) return mods::set_error(error, r, "setSpeedAndAngleNormal post");

    r = mods::hook::add_pre<SpeedAndAngleWolf>(svc_hook, on_wolf_pre);
    if (r != MOD_OK) return mods::set_error(error, r, "setSpeedAndAngleWolf pre");
    r = mods::hook::add_post<SpeedAndAngleWolf>(svc_hook, on_wolf_post);
    if (r != MOD_OK) return mods::set_error(error, r, "setSpeedAndAngleWolf post");

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}

}  /* extern "C" */
