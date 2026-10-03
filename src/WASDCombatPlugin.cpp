#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SaveManager.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/InputHandler.h>
#include <kenshi/Character.h>
#include <kenshi/CharBody.h>
#include <kenshi/Tasker.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharStats.h>
#include <kenshi/combat/CombatClass.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Globals.h>
#include <kenshi/OptionsHolder.h>
#include <kenshi/Enums.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MainBarGUI.h>
#include <kenshi/gui/OrdersPanel.h>
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreEntity.h>
#include <ogre/OgreOldSkeletonInstance.h>   // first-person head-bone tracking
#include <ogre/OgreOldBone.h>               // first-person head-bone tracking
#include <kenshi/Appearance.h>              // AppearanceBase (head/hair hide, skeleton)
#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>                          // first-person 1kHz raw mouse-look (KenshiFP method)
#include <mmsystem.h>                        // timeBeginPeriod (1ms Sleep granularity for the poll thread)
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <mygui/MyGUI.h>

// Minimal forward declaration for ManagementScreen (world map / faction / tech /
// squad window).  The full KenshiLib header <kenshi/gui/ManagementScreen.h> pulls
// in a decompiled ReorderableList template that fails to compile, so we declare
// only the two methods we call — they resolve by mangled name from KenshiLib.lib
// (same mechanism as SaveManager::getSingleton()).  Signatures MUST match the
// header exactly (public, non-const getVisible) or the linker won't find them.
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();   // RVA 0x2967F0
    bool getVisible();                          // RVA 0x48B3F0
};

// Set to 1 and rebuild to restore verbose per-frame diagnostic logs.
#define DIAG_VERBOSE 0

// Set to 1 to re-enable per-event retreat suppression logs + timing diagnostics.
// 0 = only log retreat start / end / summary (eliminates log spam with many enemies).
#define RETREAT_VERBOSE_DIAG 0

// Set to 1 to re-enable inventory/loot UI diagnostic logs (field states, widget names,
// per-second inventory window breakdown).  0 = silent in release builds.
#define LOOT_DIAG 0

// -----------------------------------------------------------------------
// Locomotion tuning config — adjust values here without changing logic.
// -----------------------------------------------------------------------
struct LocoConfig
{
    float     wasdAccelerationMultiplier;  // pre-charges currentMotion for faster ramp-up (1.0 = vanilla)
    float     wasdDecelerationMultiplier;  // force-zeros currentMotion on key release (1.0 = halt only)
    ULONGLONG wasdInputGraceMs;            // hold previous motion for N ms on key release before stopping
    float     wasdTurnResponsiveness;      // extra limit boost applied on significant direction change
    bool      normalizeDiagonalMovement;   // normalize W+A diagonal to cardinal speed (true = same speed)
    ULONGLONG wasdNudgeTapWindowMs;        // max hold duration (ms) to treat as a nudge tap (100–150 ms)
};

static const LocoConfig g_loco = {
    /* wasdAccelerationMultiplier */ 1.25f,
    /* wasdDecelerationMultiplier */ 1.35f,
    /* wasdInputGraceMs           */ 25,
    /* wasdTurnResponsiveness     */ 1.25f,
    /* normalizeDiagonalMovement  */ true,
    /* wasdNudgeTapWindowMs       */ 125,
};

// -----------------------------------------------------------------------
// WASD release-stop config — governs behavior when all WASD keys are released.
// -----------------------------------------------------------------------
struct ReleaseStopConfig
{
    bool      wasdStopOnRelease;                 // master gate — false disables the whole sequence
    float     wasdReleaseDecelerationMultiplier; // >1 forces currentMotion to zero after halt()
    ULONGLONG wasdReleaseGraceMs;                // ms to hold motion after release (0 = instant stop)
    bool      wasdAnchorSnapOnRelease;           // snap free-move anchor to current pos on release
    bool      wasdZeroVelocityOnRelease;         // zero injected velocity fields on release
};

static const ReleaseStopConfig g_release = {
    /* wasdStopOnRelease                 */ true,
    /* wasdReleaseDecelerationMultiplier */ 999.0f,
    /* wasdReleaseGraceMs               */ 0,
    /* wasdAnchorSnapOnRelease          */ true,
    /* wasdZeroVelocityOnRelease        */ true,
};

// -----------------------------------------------------------------------
// DC camera config — vertical focus offset for close-zoom chest framing.
// -----------------------------------------------------------------------
struct CameraConfig
{
    bool  dcCameraCloseZoomChestOffset; // gate — false disables the offset entirely
    float dcCameraFocusOffsetY;         // world-unit Y raise at max zoom-in (taper to 0 at medium/far)
};

static const CameraConfig g_dcCam = {
    /* dcCameraCloseZoomChestOffset */ true,
    /* dcCameraFocusOffsetY         */ 4.5f,   // navel -> upper chest ~45 cm (user req 2026-06-12)
};

// -----------------------------------------------------------------------
// Logging config — set debugLogging = true for in-game diagnostics.
// In a release build all three should remain false.
// -----------------------------------------------------------------------
struct LogConfig
{
    bool debugLogging;              // master gate — false silences all debug/verbose logs
    bool verboseMovementLogs;       // movement_injection_allowed, free_camera_input_suppressed_dc
    bool verboseCommittedActionLogs;// committed_action_true / committed_action_false detail
    bool debugVerbose;              // per-frame spam: committed_action_true, enemy_targeting_player,
                                    //   movement_injection_allowed
};

static const LogConfig g_log = {
    /* debugLogging              */ false,
    /* verboseMovementLogs       */ false,
    /* verboseCommittedActionLogs*/ false,
    /* debugVerbose              */ false,
};

// -----------------------------------------------------------------------
// Performance profiling — per-second accumulation, emits dc_perf once/sec.
// All accumulators store raw QPC ticks; converted to ms at emit time.
// -----------------------------------------------------------------------
static bool     s_profInited          = false;
static LONGLONG s_profFreq            = 1;    // QPC ticks per second (cached)
static LONGLONG s_prof_mainLoop       = 0;
static LONGLONG s_prof_charMove       = 0;
static LONGLONG s_prof_playerControl  = 0;
static LONGLONG s_prof_committedAct   = 0;
static LONGLONG s_prof_threatScan     = 0;
static LONGLONG s_prof_cameraLock     = 0;
static LONGLONG s_prof_wasdInject     = 0;
static LONGLONG s_prof_combatTarget   = 0;
static ULONGLONG s_prof_windowStart   = 0;
static int      s_nearbyEnemyCount    = 0;    // all enemies in scan, updated in step 8
static int      s_chaseFlapsCount     = 0;    // combat_entered/exited transitions per perf window
static int      s_pathfindingEnemyCount = 0;  // enemies in TARGET_PATHFINDING* (path-recalc proxy)

static inline LONGLONG qpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// Lightweight RAII scope timer — accumulates QPC ticks into a named counter.
struct ScopeTimer
{
    LONGLONG  _start;
    LONGLONG& _accum;
    ScopeTimer(LONGLONG& a) : _start(qpcNow()), _accum(a) {}
    ~ScopeTimer() { _accum += qpcNow() - _start; }
};

// -----------------------------------------------------------------------
// Control modes
// -----------------------------------------------------------------------
enum ControlMode { MODE_VANILLA = 0, MODE_FREE_MOVE = 1 };

static volatile ControlMode s_mode   = MODE_VANILLA;
static volatile bool        s_wHeld  = false;
static volatile bool        s_aHeld  = false;
static volatile bool        s_sHeld  = false;
static volatile bool        s_dHeld  = false;
static volatile bool        s_xPressed = false;
// RMB press edge — set by the poll thread, consumed on the main thread.
// The earliest reliable player-click signal: pure input level, cannot be
// blocked by any game-side dispatch path.  Used only to release the
// post-WASD hold; the click itself is never touched.
static volatile bool        s_rmbPressedEdge = false;
static bool                 s_rmbPrev        = false;  // poll thread only
// LMB double-click detection (poll thread).  In DC mode a SINGLE left click must
// only SELECT a squad member (vanilla), never switch which character WASD drives;
// only a DOUBLE click reassigns control (user req 2026-06-20 — restores pre-1.1.0
// behaviour, needed for Sentient Sands compatibility).  s_lmbDoubleClickMs is the
// timestamp of the most recent detected double-click; the main-thread retarget
// consumes it within a short window.
static bool                 s_lmbPrev          = false;     // poll thread only
static ULONGLONG            s_lastLmbDownMs    = 0;         // poll thread only
static volatile ULONGLONG   s_lmbDoubleClickMs = 0;        // set by poll, consumed by main
// Camera-rotate toggle (user req 2026-06-20): in DC mode the camera-rotate key
// (CTRL) becomes a TOGGLE instead of hold-to-rotate.  The poll thread flips
// s_camRotateToggle on each CTRL press while in DC; cameraUpdate_hook then forces
// InputHandler::rotate (0xED — the flag CameraClass::update reads to rotate) to
// the toggle state, so the mouse rotates the camera continuously until CTRL is
// pressed again.  Reset whenever DC turns off.
static volatile bool        s_camRotateToggle  = false;
static bool                 s_ctrlPrevPoll     = false;     // poll thread only
// Set by cameraUpdate_hook (player camera) when ANY UI wants the cursor.  The poll
// thread reads it to NOT flip the crosshair toggle on CTRL presses made INSIDE a
// menu (Kenshi uses CTRL+click constantly in trade/inventory) — otherwise those
// presses corrupt the crosshair toggle/home (field 2026-06-21, merchant trade).
static volatile bool        s_camRotateUiOpen  = false;
// Poll thread: set on first WASD key press; consumed and cleared on full WASD release.
static volatile ULONGLONG   s_wasdTapStartMs = 0;
// SelectControl ("F" by default, INI-configurable) press edge.  While DC is active,
// this hands WASD control to the currently selected (portrait-highlighted) character
// — an alternative to the double-click-portrait switch, so a plain portrait
// single-click is not the only way to change who WASD drives (user req 2026-06-28).
// Set by handleSelectPress on the down-edge (poll/native path); consumed in the main
// loop.  Vanilla F still re-centers the camera; the retarget centers on the new anchor.
static volatile bool        s_fSelectEdge    = false;
// Authoritative DC intent — set only by V-key; cleared only by V-key or hard shutdown.
static volatile bool        s_userWantsDC         = false;
// Hard-shutdown guard — set true the moment LOADGAME teardown is detected.
// All hooks check this and return immediately (calling orig only) while it is set.
// Cleared only when all six stabilization conditions are confirmed.
static volatile bool        s_dcShutdownInProgress = false;
// Per-hook-type blocked-log flags — each fires once per load event, reset in clearAllState.
static bool s_hookBlockLoggedMain    = false;
static bool s_hookBlockLoggedCharMov = false;
static bool s_hookBlockLoggedPCtrl   = false;
static bool s_hookBlockLoggedRemJob  = false;
static bool s_hookBlockLoggedAddJob  = false;
static bool s_hookBlockLoggedTrade   = false;
// Throttle for dc_loadgame_waiting_for_safe_reacquire — once per second.
static ULONGLONG s_shutdownWaitLogTick = 0;

// NPC loot UI suspension — blocks all V-Mode input and WASD injection while looting.
// s_mode is NOT changed; the suspend is a transparent pause that restores automatically.
static volatile bool   s_lootUiSuspendActive    = false;
static bool            s_lootUiWasPrevOpen      = false;
static ULONGLONG       s_lootSuspendStartTick   = 0;
// TRADE/LOOT classification for the inventory face-cam.  Set when
// showTradeWindow fires (a foreign party — shop/loot/corpse), cleared when ALL
// inventory windows close.  Edge-latched on purpose: the gui trade HAND fields
// (inventoryWindowTrader/NPC/tradeA/tradeB) stay STALE after a trade closes, so
// reading them directly mis-classifies the next OWN inventory as a trade.  Own
// inventory = (live window count >= 1) AND !s_tradeWindowActive — this also
// covers a character with a BACKPACK, whose own inventory opens as TWO windows
// (field 2026-06-17: cnt=2 char=1 with all trade fields 0) and used to be
// wrongly rejected by the old getNumOpenInventoryWindows()==1 test.
static bool            s_tradeWindowActive      = false;
static const ULONGLONG LOOT_SUSPEND_DEBOUNCE_MS = 250;

// Inventory "move-through" mode (user opt-in 2026-06-28 via InventoryFaceCam=false):
// instead of suspending DC while an inventory window is open, keep DC movement +
// camera lock LIVE and keep the game running so the player can WASD around while
// looting/trading.  Walking out of range lets the game close the window naturally.
// Dialogue still pauses (handled separately, never overridden).
// s_invMoveThroughActive is latched on the open edge so the close edge knows which
// path (suspend vs move-through) was taken; s_invMoveThroughForcedRun records that
// we forced the game to keep running so we only touch pause when we caused it.
static bool            s_invMoveThroughActive      = false;
static bool            s_invMoveThroughForcedRun   = false;
// Manual pause during move-through (user req 2026-08-05): Kenshi auto-pauses at
// inventory OPEN and again when the shown inventory SWITCHES to another squad
// member; only those auto-pauses are defeated (grace window after each edge).
// A pause appearing outside the grace is the PLAYER pausing — latched and
// respected (incl. across inventory switches) until they unpause themselves.
// s_invMoveThroughShownChar is IDENTITY ONLY for switch-edge detection — never
// dereferenced (the character behind a closing window can be torn down).
static bool            s_invMoveThroughPlayerPaused = false;
static Character*      s_invMoveThroughShownChar    = nullptr;
static ULONGLONG       s_invMoveThroughEdgeTick     = 0;
// Covers one slow UI-transition frame (mainLoop spikes to ~500 ms there) so the
// auto-pause is still caught; a player pause faster than this after opening/
// switching is eaten once — pressing pause again sticks.
static const ULONGLONG INV_MT_AUTOPAUSE_GRACE_MS   = 600;
// Merchant trade does NOT auto-close on distance in vanilla (you normally can't
// walk while trading).  Corpse/own-inventory windows DO close natively, so this
// explicit close is scoped to the trader window only.  Latched so closeTradeWindow
// fires once per session.
static bool            s_invTradeCloseRequested    = false;
// "Walk away" is measured as distance MOVED from where the controlled character
// stood when the trade opened — NOT absolute distance to the merchant, which can
// already be large at open (e.g. trading across a bar counter) and would slam the
// window shut instantly.  s_invTradeAnchorStart is captured on the first trade
// frame (zero-initialized; guarded by s_invTradeStartValid).
static bool            s_invTradeStartValid        = false;
static Ogre::Vector3   s_invTradeAnchorStart;       // zero-init (static storage)
static const float     INV_TRADE_AUTOCLOSE_DIST    = 6.0f;   // units walked from open spot
static const float     INV_TRADE_AUTOCLOSE_DIST_SQ = INV_TRADE_AUTOCLOSE_DIST * INV_TRADE_AUTOCLOSE_DIST;

// Per-frame snapshot of volatile input state — set once at the top of mainLoop_hook,
// consumed by all per-character hooks that fire during s_mainLoopOrig.
// Eliminates per-character volatile reads (memory fence overhead) for hooks that
// fire 100+ times per frame in large enemy encounters.
static ControlMode s_frameMode        = MODE_VANILLA;
static bool        s_frameWasdHeld    = false;
static bool        s_frameLootSuspend = false;

// Melee/combat awareness range.  No separate middle zone.
static const float     ATTACK_RANGE    = 500.0f;
static const ULONGLONG SCAN_INTERVAL_MS = 5000; // squad-threat scan interval

static const char* modeName(ControlMode m)
{
    return m == MODE_FREE_MOVE ? "FREE_MOVE" : "VANILLA";
}

static void setMode(ControlMode next)
{
    ControlMode prev = s_mode;
    if (next == prev) return;
    s_mode = next;
    char buf[128];
    sprintf_s(buf, sizeof(buf), "[WASDCombat] MODE: %s -> %s", modeName(prev), modeName(next));
    DebugLog(buf);
}

// =======================================================================
// Keybind configuration (v1.6) — user-configurable, international-keyboard
// friendly.  Bindings are ROLE-based: the poll thread reads the virtual-key
// code assigned to each role from s_bindVk and acts on the role, never on a
// hardcoded key.  Loaded from WASDCombatPlugin.ini (next to the plugin DLL)
// at startup; a commented default file is created if missing; any invalid
// entry falls back to its default.  Nothing downstream of the s_*Held flags
// is touched — movement, hold, point-click, camera, combat, and XP logic
// are unchanged.
// =======================================================================
enum KeyRole { KR_FORWARD = 0, KR_LEFT, KR_BACK, KR_RIGHT, KR_TOGGLE, KR_SPEED, KR_SELECT, KR_FP, KR_SNEAK, KR_COUNT };
static int s_bindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'X', 'F', 'P', 'C' };  // defaults
static const char* const KR_INI_KEY[KR_COUNT] =
    { "MoveForward", "MoveLeft", "MoveBackward", "MoveRight", "ToggleDC", "SpeedCycle", "SelectControl", "FirstPerson", "SneakToggle" };
static const char* const KR_DEFAULT[KR_COUNT] =
    { "W", "A", "S", "D", "V", "X", "F", "P", "C" };
static char    s_bindCfgStr[KR_COUNT][32];  // resolved strings for the summary log
static HMODULE s_thisModule = nullptr;      // captured in DllMain for the INI path

// [Settings] feature toggles (separate INI section from [Keybinds]).
// InventoryFaceCam: when true (default) the camera swings to face the character
// while their own inventory is open.  Users who loot/disarm mid-fight disliked
// the cam grabbing focus, so it is both INI-toggleable AND auto-suppressed while
// the character is in combat (user req 2026-06-20).
static bool    s_settingInventoryFaceCam = true;
// WASD speed cap (user req 2026-07-25).  The injected setDirectMovement ran at an
// uncapped ~99 move-limit, so shackled / injured / encumbered characters moved at
// full speed (Rebirth leg-shackle escape) and enemies could not catch a WASD-moving
// player.  When on, cap the WASD move-limit at the character's real max run speed
// (CharStats::getMaxRunSpeed — injury/encumbrance-aware) with an extra hard clamp
// while chained/shackled.  WasdSpeedMult scales it (1.0 = exactly legit).  Cap off
// (WasdSpeedCap=false) restores the legacy uncapped behaviour.
static bool    s_settingWasdSpeedCap     = true;
static float   s_settingWasdSpeedMult    = 1.0f;
// Combat-mode FLICKERS (isInCombatMode blips false between swings / when the
// target is momentarily not engaged).  A single false frame at inventory-open
// used to latch the face-cam on (then the 16-frame close-debounce kept it up
// through the fight).  So "in combat" is treated as a HARD exit (no debounce) and
// held for a short grace after the last in-combat frame to smooth the flicker and
// cover looting/disarming right as a fight ends.
static ULONGLONG s_lastInCombatMs = 0;
static const ULONGLONG FACECAM_COMBAT_GRACE_MS = 1500;

// =======================================================================
// Inventory face-cam — a temporary DETACHED Ogre camera (s_fpNode) that swings
// around to face the selected character's front while their OWN inventory is
// open, so worn gear is visible.  The gameplay over-the-shoulder ("OTS") toggle
// this plumbing was originally built for was SCRAPPED (Kenshi's interior floor
// render is welded to the top-down RTS camera, mutually exclusive with a
// detached view); only the inventory face-cam survives.  Runs in
// cameraUpdate_hook (works under the inventory pause).  Kenshi scale ≈ 10cm/unit.
// =======================================================================
static bool             s_fpActive          = false;
// Inventory face-cam exit debounce.  The camera update hook is called TWICE per
// frame and the two calls DISAGREE on the own-inventory window count (one sees
// 1, the other does not) — so a naive enter/exit toggled detach/re-attach EVERY
// frame (field 2026-06-16/17: 4349 dc_cam_entered/exited pairs while one
// inventory was open).  That left s_fpActive true only half the frames, which
// silently broke the altitude-hold (scroll still zoomed name-tags) and the
// s_fpActive-gated point-click suppression (clicks landing on an "exit" frame
// walked the character).  Fix: require the own-inventory signal to be ABSENT for
// several consecutive calls before exiting, so a single dissenting per-frame
// call can no longer drop the face-cam.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// World name-tag (the floating squad/character labels above heads) hide while
// the inventory face-cam is up.  Scrolling/WASD still nudged those labels and
// camera-input gating couldn't stop it cleanly, so instead we just hide them
// for the duration (user request 2026-06-17) and restore the player's setting
// (`options->showNames`) on close.  s_savedShowNames is read BEFORE hiding so a
// showNames() that also writes the option can't poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;  // face-cam camera distance
static bool             s_otsInvFaceActive  = false;  // own-inventory face-cam engaged
static Character*       s_otsInvFaceChar    = nullptr; // who it is aimed at (identity-compared only)
// Shoulder view saved on the FIRST inventory open, restored on close so the
// player returns to exactly the over-the-shoulder framing they had before.
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Crosshair/cursor horizontal offset as a fraction of client width (+ = right
// of center) so the crosshair clears the character body for selection clicks
// (user req 2026-06-13).  The cursor is pinned to this point and mouse-look
// deltas are measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;  // 0.12 -> 0.10 (closer to the character, user req 2026-06-16)
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// Set when a load/teardown interrupts OTS: the detached Ogre camera persists
// across a save-load, so it must be re-attached to the rig AFTER the world is
// valid again (doing it mid-load crashed; not doing it left the camera stuck
// orphaned at the OTS position — field 2026-06-13).
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // face-cam: held to block scroll-zoom

// =======================================================================
// First-person camera (v1.8-fp, ported into the live line 2026-07-22).  A
// SECOND drive mode for the SAME detached-camera machinery as the inventory
// face-cam above — both detach the Ogre camera onto s_fpNode and save/restore
// the camera locals (s_fpSavedCamPos/Ori/Fov/NearClip, s_fpHadAutoTrack,
// s_fpCamLocalsSaved) the same way, and both drive in cameraUpdate_hook.  They
// differ ONLY in what triggers them and where the camera is placed each frame:
//   * s_fpActive          = the INVENTORY FACE-CAM owns the detached view.
//   * s_firstPersonActive = FIRST-PERSON owns it (eye at the head bone, look out).
// The two are mutually exclusive (never both true — the camera has one node).
// Opening the inventory while first-person is active SUSPENDS first-person for
// the face-cam and AUTO-RETURNS to first-person when the inventory closes
// (s_fpSuspendedForInv).  Toggled by the FirstPerson key (default P) in DC.
// =======================================================================
static bool  s_firstPersonActive   = false;  // detached cam is in first-person drive mode
// s_userWantsFP — PERSISTENT first-person INTENT (mirrors s_userWantsDC).  Survives
// survivable loads (chunk streaming / micro-loads / save-load into gameplay) so FP is
// re-entered after the scene rebuilds; cleared only on P-off, DC-off (V), new game, or
// hard teardown.  Fixes "FP disables when loading between chunks" (user 2026-07-29).
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // P-key edge -> consumed on game thread
static bool  s_fpSuspendedForInv   = false;  // FP paused while the inventory face-cam runs
static float s_fpSensitivityFP     = 1.0f;   // [FirstPerson] Sensitivity (mouse-look scale)
static float s_fpEyeHeight         = 16.5f;  // [FirstPerson] EyeHeight (fallback neck model)
static float s_fpFwdOffset         = 1.2f;   // [FirstPerson] ForwardOffset (eye ahead of neck)
static float s_fpFovDegFP          = 75.0f;  // [FirstPerson] FOV (wide first-person view)
static float s_fpNearClipFP        = 0.2f;   // [FirstPerson] NearClip (don't slice own body)
static float s_fpNeckLimitRad      = 1.309f; // [FirstPerson] NeckLimit (deg -> rad; 75 deg)
static bool  s_fpHideHair          = true;   // [FirstPerson] HideHair
static bool  s_fpHideHead          = true;   // [FirstPerson] HideHead
static bool  s_fpHairHidden        = false;  // restore tracking
static bool  s_fpHeadBoneHidden    = false;  // restore tracking
static float s_fpLeanFwd           = 0.0f;   // smoothed forward lean (fallback model only)
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;  // smoothed head-bone eye
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;  // one-shot head-bone tracking log
static float         s_fpBoneEyeUp       = 1.0f;   // eye height above the resolved mount bone
static float         s_fpEyeUpAdjust     = 0.0f;   // [FirstPerson] EyeUpAdjust — INI nudge added
                                                   // to the bone-mount eye height; raise to keep
                                                   // the chest/shoulders below frame when moving
static float         s_fpMoveLeanUp      = 0.0f;   // [FirstPerson] MoveLeanUp — extra eye height
                                                   // scaled by move speed (0..1); counters the
                                                   // forward jog/sprint lean that swings the body up
static float         s_fpMoveLeanFwd     = 0.0f;   // [FirstPerson] MoveLeanForward — extra eye
                                                   // forward scaled by move speed; pushes past the
                                                   // leaning torso/arms at jog/sprint speed
static float         s_fpMoveNearClip    = 0.0f;   // [FirstPerson] MoveNearClip — near-clip distance
                                                   // blended in by move speed; slices the arm/torso
                                                   // that sweeps into the lens at jog/sprint. 0 = off
static float         s_fpMoveLeanSmooth  = 0.0f;   // runtime: smoothed 0..1 speed factor
static float         s_fpJogForward      = 2.0f;   // [FirstPerson] JogForward — eye pushed forward along
                                                   // body facing while actively JOGging (closes the gap
                                                   // the forward jog lean opens; 0 = off)
static float         s_fpRunForward      = 4.0f;   // [FirstPerson] RunForward — same, while RUN/sprinting
static float         s_fpGaitFwdSmooth   = 0.0f;   // runtime: smoothed gait forward offset (units)
static float         s_fpActionClrSmooth = 0.0f;   // runtime: smoothed committed-action forward clearance (units)
static float         s_fpStreamDist      = 2.0f;   // [FirstPerson] StreamUpdateDist — min metres the eye must
                                                   // move before we re-teleport the game rig for zone/foliage
                                                   // streaming. Per-frame teleport (=0) re-pages grass every
                                                   // frame => flicker while moving; throttling to ~2m streams
                                                   // smoothly. Big value (e.g. 999) ~= never re-stream (test).
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;  // runtime: last rig-teleport position
static bool          s_fpLastStreamValid = false;                // runtime: has s_fpLastStreamPos been set
static float         s_fpGrassRangeMult  = 1.0f;   // [FirstPerson] GrassRangeMult — while FP is active, multiply
                                                   // options->grassRange/foliageRange so grass loads in a wider
                                                   // ring. Kenshi grass range is tuned for the high top-down cam;
                                                   // at ground level the short ring pops at its edge as you turn/
                                                   // move. 1.0 = off (vanilla). Restored exactly on FP exit.
                                                   // NOTE: raising range does NOT fix the distant re-scatter —
                                                   // it just renders more grass that still re-scatters (user
                                                   // 2026-07-29). Reverted to 1..8 clamp / live 1.0.
static float         s_fpSavedGrassRange   = 0.0f;  // runtime: options->grassRange   saved on FP enter
static float         s_fpSavedFoliageRange = 0.0f;  // runtime: options->foliageRange saved on FP enter
static bool          s_fpOptRangeSaved     = false; // runtime: are the two saved values valid
static bool          s_fpCamPreOrig        = false; // [FirstPerson] CamPreOrig — EXPERIMENT (2026-07-25): also drive
                                                    // the FP camera before Kenshi's loop so the foliage pass sees
                                                    // the current view. Proved INERT for the grass flicker (that
                                                    // pass samples the camera on a render/GPU thread we can't reach
                                                    // from the main thread), so DEFAULT OFF — the extra per-frame
                                                    // drives also multiplied the follow/gait lerp rates. 1 = on.
static float         s_fpFollowTurn        = 0.08f; // [FirstPerson] FollowTurn — while the character moves on ITS
                                                    // OWN (point-click / combat / heal approach, NOT WASD), lerp the
                                                    // view yaw toward the movement heading so the camera follows the
                                                    // character like WASD does. 0 = off (view stays on mouse-look).
                                                    // YIELDS to the mouse: only eases in after FollowDelayMs of no
                                                    // mouse-look, so you can freely look around while moving.
static float         s_fpFollowDelayMs     = 400.0f;// [FirstPerson] FollowDelayMs — how long the mouse must be still
                                                    // before the FollowTurn auto-recentre kicks in. Higher = look
                                                    // around longer before the camera drifts back onto the path.
static ULONGLONG     s_fpLastMouseMoveMs   = 0;     // runtime: last tick the player actively moved the look
static int           s_fpFoliageCenterMode = 1;     // [FirstPerson] FoliageCenterFix — reconcile the camera CENTER
                                                    // node (Kenshi's grass/foliage streaming anchor) with the eye
                                                    // WHILE MOVING, via _setDerivedPosition + forced recompute
                                                    // (KenshiFP technique).  1 = on, 0 = off (legacy under-foot pop).
                                                    // At idle the center is left vanilla so panning can't re-scatter.
// [FirstPerson] FreezeCamTest — DIAGNOSTIC (ChatGPT-suggested, 2026-07-29).  Tests whether
// the DISTANT grass re-scatter on rotation is CAMERA-POSITION-driven (positional, fixable)
// vs orientation-driven billboards (engine, accepted).  When 1 + FP + standing still, the
// real camera position (s_fpNode) is LOCKED to the first standstill eye and only ORIENTATION
// updates — so a pure standing pan moves nothing positionally.  If distant grass then HOLDS,
// PagedGeometry reads the (swinging) camera position → engineer a stabilized-pager fix.  If
// it STILL re-scatters, it's billboard shimmer (accepted).  0 = off (normal eye follow).
static int           s_fpFreezeCamTest     = 0;
static Ogre::Vector3 s_fpFrozenEye         = Ogre::Vector3::ZERO;
static bool          s_fpFrozenValid       = false;
static float         s_fpLookSmooth        = 0.4f;  // [FirstPerson] LookSmooth — 0..0.9: smooths the RENDERED view
                                                    // orientation so each frame's rotation delta is smaller, which
                                                    // shrinks the 1-frame render-thread grass re-facing mismatch =
                                                    // less side-to-side foliage flicker. Costs a little mouse-look
                                                    // snappiness (camera "glide"). 0 = off/raw (default).
static float         s_fpYawSm             = 0.0f;  // runtime: render-smoothed yaw (== s_fpYaw when LookSmooth=0)
static float         s_fpPitchSm           = 0.0f;  // runtime: render-smoothed pitch
static bool          s_fpSmValid           = false; // runtime: smoothed view initialised this FP session

// --- KenshiFP-style camera port (2026-07-30) ------------------------------
// The reference mod (github.com/linguine2552/KenshiFP) welds the eye to the head
// bone's TRUE world position (via the game's own Character::getBoneWorldPosition)
// and reads look input from a 1kHz DirectInput device, instead of our synthetic
// "root + rotated model-offset" eye + cursor-warp look.  Ours reconstructed the
// head position by rotating the whole ~2m bone offset by the VIEW yaw, which
// swung the eye on an arc during pure rotation (the grass-repage/camera-swing we
// fought for weeks) and never tracked the real animated head.  These toggles let
// us A/B the new path against the old one.
static bool  s_fpTrueBoneEye  = true;   // [FirstPerson] TrueBoneEye — 1 = weld the eye to the head bone's
                                        // real world position (getBoneWorldPosition); 0 = old synthetic path.
static float s_fpEyeDrop      = 0.0f;   // [FirstPerson] EyeDrop — drop below the head-bone origin to eye level
                                        // (units of the mount bone height; the head bone sits above the eyes).
static bool  s_fpRawMouse     = true;   // [FirstPerson] RawMouse — 1 = 1kHz DirectInput look deltas (framerate-
                                        // independent, smooth); 0 = cursor-warp deltas (old, fps-dependent).
static float s_fpMoveForward  = 0.0f;   // [FirstPerson] MoveForward — speed-scaled forward LEAD so the eye leads
                                        // faster movement instead of lagging the leaning head (KenshiFP).  0 = off.
static float s_fpMoveSpeedRef = 30.0f;  // [FirstPerson] MoveSpeedRef — ground speed (feet-delta/sec) that maps to
                                        // full run = lead 1.0.  Tune so a full sprint gives ~the MoveForward push.
// runtime: feet-delta ground-speed tracker that drives the forward lead (KenshiFP
// keys the lead off ON-SCREEN speed, not the noisy currentMotion magnitude — the
// same signal our own notes found unreliable, 2026-07-24).
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;   // vertical feet sample (stair-climb detector)
static float     s_fpMoveSpeed      = 0.0f;   // low-passed ground speed (units/sec)
static float     s_fpMoveFwdSmooth  = 0.0f;   // smoothed forward-lead offset (units)
static float     s_fpClimbSpeedSmooth = 0.0f; // low-passed vertical speed (units/sec, + = ascending)
static ULONGLONG s_fpFeetTickMs     = 0;      // last feet-speed sample tick

// Ascent-aware forward pullback (user 2026-07-31): on stairs the horizontal
// ForwardOffset drives the eye INTO the rising steps.  While the character climbs
// (positive vertical speed) scale the forward push down toward StairForwardMinScale
// and lift the eye by StairEyeLift, so the camera stops jamming into the steps —
// full offset is restored on flat ground so the body still never clips.
static float s_fpStairForwardReduce   = 0.25f; // [FirstPerson] StairForwardReduce — climb-speed sensitivity: higher
                                               // reaches full pullback at a gentler climb.  0 = feature off.
static float s_fpStairForwardMinScale = 0.30f; // [FirstPerson] StairForwardMinScale — forward-offset fraction kept at
                                               // full climb (0.3 = eye pulled back to 30% of ForwardOffset; 1 = no pullback).
static float s_fpStairEyeLift         = 0.30f; // [FirstPerson] StairEyeLift — extra eye height (units) at full climb,

// Enemy body-clip clearance (v1.3.0 combat polish).  When a live hostile stands
// inside EnemyClearRadius of the anchor, the forward eye offsets (ForwardOffset +
// gait/action pushes) collapse toward EnemyClearMinScale — the same mechanism as
// the stair pullback — so the eye retreats to the (hidden) skull instead of
// poking into the aggressor's model, and the near plane pulls in (EnemyNearClip)
// so whatever still overlaps slices as thin a cross-section as possible.
// Nearest-hostile distance is sampled per frame in mainLoop (game thread, where
// getCharacterUpdateList lives) and consumed here; -1 = no hostile in range.
static float s_fpEnemyClearRadius   = 12.0f;  // [FirstPerson] EnemyClearRadius (units; 0 = off)
static float s_fpEnemyClearMinScale = 0.15f;  // [FirstPerson] EnemyClearMinScale — fwd fraction kept at contact
static float s_fpEnemyNearClip      = 0.10f;  // [FirstPerson] EnemyNearClip — near plane at full overlap (0 = off)
static float s_fpEnemyClearSmooth   = 1.0f;   // runtime: smoothed 0..1 offset scale (1 = no hostile near)
static float s_fpEnemyNearestDist   = -1.0f;  // runtime: nearest live hostile distance (set in mainLoop)

// Interior floor pre-reveal — TRIED AND REVERTED (2026-08-01): re-showing the
// storeys above the anchor (Building::setFloorVisibility after the game's
// updateFloorVisibility pass) worked mechanically, but Kenshi's upper-floor
// meshes are authored for the top-down cutaway — one-sided faces with no
// underside geometry — so from eye level below they render as floating planks,
// hollow shells and sky holes (user screenshots, shinobi tower).  The vanilla
// reveal-on-approach stays; do not re-attempt without a mesh-level solution.

// Below-floor reveal (user 2026-08-01: black voids through stairwell gaps when
// looking DOWN in FP, flickering on fast pans).  In FP restrictPosition — the
// vanilla cutaway refresh — is skipped, and the substitute per-frame
// updateFloorVisibility(characters) pass reveals storeys by where SQUAD MEMBERS
// stand, not what the camera should see: solo character on floor 2 → floors 0-1
// shell geometry hidden → black voids (furniture/NPCs are separate objects, so
// they still rendered, floating in the dark).  Fix: after that pass, force the
// cutaway to "everything up to my floor" via setFloorsVisibility — one
// consistent answer per frame, which also stops the two-writer flicker.
static bool s_fpFloorRevealBelow = true;   // [FirstPerson] FloorRevealBelow (live-retunable via P)

// Sneak toggle (Shift+C while first-person, user req 2026-08-01).  Poll-thread
// edge consumed on the game thread — the OrdersPanel sneak-button path (or
// setStealthMode fallback) must run where the anchor is valid.
static volatile bool s_sneakToggleRequested = false;
                                               // to look over the steps.  0 = off.

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // WASD-recency window that keeps the camera authoritative
                                                    // (body refaces to view, camera never snapped) through the
                                                    // release/coast of a strafe/backpedal — kills the jolt where
                                                    // the neck-limit used to snap the view to the body direction.
static float         s_fpActionClearFwd    = 2.0f;  // [FirstPerson] ActionClearForward — extra forward eye offset
                                                    // during committed actions (attack / block / heal / revive /
                                                    // get-up) to push the camera clear of the swinging/kneeling
                                                    // body that otherwise clips through the lens. 0 = off.
static float         s_fpBodyYaw         = 0.0f;   // runtime: smoothed VISIBLE body yaw (model only —
                                                   // the eye mounts on the view yaw, so turning the
                                                   // body never moves the camera). Lerps toward view.
static const float   FP_BODY_TURN        = 0.20f;  // per-frame body-yaw lerp toward the view
static MyGUI::TextBox* s_fpCrosshair     = nullptr; // viewport-center "+" (lazy-created)
static const float FP_HEAD_LEN     = 1.8f;   // neck-pivot to eye (fallback model)
static const float FP_BONE_SMOOTH  = 0.45f;  // per-frame height lerp (damps stride bob)

// -----------------------------------------------------------------------
// Native Controls-menu keybinds (v1.7.3) — TOGGLE + SPEED ONLY.
//
// HARD CONSTRAINT learned in the field (2026-06-10): Kenshi's InputHandler
// allows exactly ONE command per physical key.  Vanilla camera panning
// owns W/S/A/D as alternate binds, so registering DC movement commands on
// those keys STOLE them from the camera (camera dead in vanilla mode) and
// the theft was then persisted into controls.cfg.  The hybrid design needs
// the same key to pan the camera in vanilla mode and move the character in
// DC mode — Kenshi's native system cannot express that.  Therefore:
//   - Movement keys (W/A/S/D roles) are ALWAYS handled by our own VK
//     polling (INI-configurable, v1.6 system); playerControl_hook already
//     context-switches the camera keys during DC.  NEVER register
//     dc_move_* commands in the InputHandler.
//   - Toggle (V) and Speed (X) sit on keys vanilla leaves unbound — they
//     are registered natively (KEP pattern) and rebindable in the
//     Controls menu.  Presses arrive via key->events in processKeys.
// Evidence the game persists plugin commands when bound: toggle_devtools
// =F12 (KEP) lives in controls.cfg; our dc_ lines were missing because
// vanilla camera lines re-stole W/A/S/D at loadConfig, leaving dc_move_*
// unbound at save time.
// -----------------------------------------------------------------------
static volatile bool s_nativeCommandsRegistered = false; // set by registerNativeCommands
static void watchNativeBindChanges();                 // defined with the keybind hooks below
static void registerNativeCommands(InputHandler* self);  // ditto

struct VkName { const char* name; int vk; };
static const VkName s_vkNames[] =
{
    { "VK_SPACE",   VK_SPACE   }, { "VK_TAB",      VK_TAB      },
    { "VK_RETURN",  VK_RETURN  }, { "VK_BACK",     VK_BACK     },
    { "VK_SHIFT",   VK_SHIFT   }, { "VK_LSHIFT",   VK_LSHIFT   }, { "VK_RSHIFT",   VK_RSHIFT   },
    { "VK_CONTROL", VK_CONTROL }, { "VK_LCONTROL", VK_LCONTROL }, { "VK_RCONTROL", VK_RCONTROL },
    { "VK_MENU",    VK_MENU    }, { "VK_LMENU",    VK_LMENU    }, { "VK_RMENU",    VK_RMENU    },
    { "VK_CAPITAL", VK_CAPITAL },
    { "VK_UP",      VK_UP      }, { "VK_DOWN",     VK_DOWN     },
    { "VK_LEFT",    VK_LEFT    }, { "VK_RIGHT",    VK_RIGHT    },
    { "VK_HOME",    VK_HOME    }, { "VK_END",      VK_END      },
    { "VK_PRIOR",   VK_PRIOR   }, { "VK_NEXT",     VK_NEXT     },
    { "VK_INSERT",  VK_INSERT  }, { "VK_DELETE",   VK_DELETE   },
    { "VK_NUMPAD0", VK_NUMPAD0 }, { "VK_NUMPAD1",  VK_NUMPAD1  }, { "VK_NUMPAD2", VK_NUMPAD2 },
    { "VK_NUMPAD3", VK_NUMPAD3 }, { "VK_NUMPAD4",  VK_NUMPAD4  }, { "VK_NUMPAD5", VK_NUMPAD5 },
    { "VK_NUMPAD6", VK_NUMPAD6 }, { "VK_NUMPAD7",  VK_NUMPAD7  }, { "VK_NUMPAD8", VK_NUMPAD8 },
    { "VK_NUMPAD9", VK_NUMPAD9 },
    { "VK_MULTIPLY", VK_MULTIPLY }, { "VK_ADD",     VK_ADD     },
    { "VK_SUBTRACT", VK_SUBTRACT }, { "VK_DECIMAL", VK_DECIMAL }, { "VK_DIVIDE", VK_DIVIDE },
    { "VK_OEM_1", VK_OEM_1 }, { "VK_OEM_2", VK_OEM_2 }, { "VK_OEM_3", VK_OEM_3 },
    { "VK_OEM_4", VK_OEM_4 }, { "VK_OEM_5", VK_OEM_5 }, { "VK_OEM_6", VK_OEM_6 },
    { "VK_OEM_7", VK_OEM_7 }, { "VK_OEM_8", VK_OEM_8 }, { "VK_OEM_102", VK_OEM_102 },
    { "VK_OEM_PLUS",  VK_OEM_PLUS  }, { "VK_OEM_COMMA",  VK_OEM_COMMA  },
    { "VK_OEM_MINUS", VK_OEM_MINUS }, { "VK_OEM_PERIOD", VK_OEM_PERIOD },
};
static const int NUM_VK_NAMES = sizeof(s_vkNames) / sizeof(s_vkNames[0]);

// parseKeyName — accepts named keys (table above), VK_A..VK_Z / VK_0..VK_9,
// bare single characters, VK_F1..VK_F24 / F1..F24, hex (0x56), or decimal
// virtual-key codes.  Returns -1 when unrecognised.
static int parseKeyName(const char* raw)
{
    char s[32];
    int  n = 0;
    for (const char* p = raw; *p && n < 31; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)toupper((unsigned char)*p);
    }
    s[n] = '\0';
    if (n == 0) return -1;

    // Named-key table — both plain ("SPACE", "NUMPAD5", "OEM_3") and
    // VK_-prefixed ("VK_SPACE") forms are accepted.
    char prefixed[36];
    sprintf_s(prefixed, sizeof(prefixed), "VK_%s", s);
    for (int i = 0; i < NUM_VK_NAMES; ++i)
        if (strcmp(s, s_vkNames[i].name) == 0
            || strcmp(prefixed, s_vkNames[i].name) == 0)
            return s_vkNames[i].vk;

    const char* f = s;
    if (strncmp(f, "VK_", 3) == 0) f += 3;

    if (f[0] == 'F' && f[1] >= '0' && f[1] <= '9')
    {
        int fn = atoi(f + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + fn - 1;
    }
    if (strlen(f) == 1 &&
        ((f[0] >= 'A' && f[0] <= 'Z') || (f[0] >= '0' && f[0] <= '9')))
        return f[0];
    if (s[0] == '0' && s[1] == 'X')
    {
        long v = strtol(s, nullptr, 16);
        if (v > 0 && v < 256) return (int)v;
    }
    {
        char* end = nullptr;
        long  v   = strtol(s, &end, 10);
        if (end != s && *end == '\0' && v > 0 && v < 256) return (int)v;
    }
    return -1;
}

// parseBool — accepts true/false, 1/0, yes/no, on/off (case-insensitive).
// Returns the supplied default for anything unrecognised so a typo can't
// silently flip a feature off.
static bool parseBool(const char* raw, bool dflt)
{
    char s[16];
    int  n = 0;
    for (const char* p = raw; *p && n < 15; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)tolower((unsigned char)*p);
    }
    s[n] = '\0';
    if (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "on"))
        return true;
    if (!strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "off"))
        return false;
    return dflt;
}

static void getConfigPath(char* out, size_t cap)
{
    out[0] = '\0';
    if (s_thisModule &&
        GetModuleFileNameA(s_thisModule, out, (DWORD)cap) > 0)
    {
        char* slash = strrchr(out, '\\');
        if (slash) { *(slash + 1) = '\0'; }
        else       { out[0] = '\0'; }
    }
    strcat_s(out, cap, "WASDCombatPlugin.ini");
}

static void writeDefaultConfig(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // Player-facing settings only (user req 2026-08-01).  Every advanced camera
    // tunable still loads with a safe default when absent, and can be added to
    // [FirstPerson] by name for fine-tuning — the loader reads far more keys
    // than this template ships.
    static const char tmpl[] =
        "[Keybinds]\r\n"
        "; Valid names: letters, digits, F1..F24, SPACE, TAB, SHIFT, CONTROL,\r\n"
        "; arrow keys, NUMPAD0..9, OEM_1..8.  Invalid entries use the default.\r\n"
        "ToggleDC        = V\r\n"
        "MoveForward     = W\r\n"
        "MoveBackward    = S\r\n"
        "MoveLeft        = A\r\n"
        "MoveRight       = D\r\n"
        "SpeedCycle      = X\r\n"
        "SelectControl   = F\r\n"
        "FirstPerson     = P\r\n"
        "; Sneak is the chord SHIFT + this key, first-person only.\r\n"
        "SneakToggle     = C\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; true = camera faces your character when their inventory opens.\r\n"
        "; false = keep moving with WASD while looting/trading.\r\n"
        "InventoryFaceCam = true\r\n"
        "; Cap WASD speed at the character's real max speed (injuries,\r\n"
        "; encumbrance, shackles).  Mult scales it (1.0 = exactly vanilla).\r\n"
        "WasdSpeedCap     = true\r\n"
        "WasdSpeedMult    = 1.0\r\n"
        "\r\n"
        "[FirstPerson]\r\n"
        "; First-person view (press P while Direct Control is on).\r\n"
        "Sensitivity      = 1.0\r\n"
        "; Field of view in degrees (50-110).\r\n"
        "FOV              = 90\r\n"
        "; Hide your own hair / head so they don't block the view.\r\n"
        "HideHair         = 1\r\n"
        "HideHead         = 1\r\n"
        "; Render the storeys below you inside multi-floor buildings\r\n"
        "; (0 = vanilla reveal-on-approach).\r\n"
        "FloorRevealBelow = 1\r\n";
    DWORD written = 0;
    WriteFile(h, tmpl, (DWORD)(sizeof(tmpl) - 1), &written, nullptr);
    CloseHandle(h);
}

// loadFirstPersonConfig — read + clamp the [FirstPerson] tunables from the INI.
// Split out of loadKeybinds so the P-toggle can re-read it live: edit the INI,
// toggle FP off then on, and the new camera values apply with NO game relaunch
// (called again at the top of enterFirstPerson).  Missing keys/section fall
// back to shipped defaults, so existing INIs keep working untouched.
static void loadFirstPersonConfig(const char* path)
{
    char fb[32];
    GetPrivateProfileStringA("FirstPerson", "Sensitivity", "1.0", fb, sizeof(fb), path);
    float sens = (float)atof(fb);
    if (sens < 0.1f) sens = 0.1f;  if (sens > 5.0f) sens = 5.0f;
    s_fpSensitivityFP = sens;
    GetPrivateProfileStringA("FirstPerson", "FOV", "75", fb, sizeof(fb), path);
    float fov = (float)atof(fb);
    if (fov < 50.0f) fov = 50.0f;  if (fov > 110.0f) fov = 110.0f;
    s_fpFovDegFP = fov;
    GetPrivateProfileStringA("FirstPerson", "EyeHeight", "16.5", fb, sizeof(fb), path);
    float eye = (float)atof(fb);
    if (eye < 1.0f) eye = 1.0f;    if (eye > 40.0f) eye = 40.0f;
    s_fpEyeHeight = eye;
    GetPrivateProfileStringA("FirstPerson", "ForwardOffset", "1.2", fb, sizeof(fb), path);
    float fwd = (float)atof(fb);
    if (fwd < 0.0f) fwd = 0.0f;    if (fwd > 10.0f) fwd = 10.0f;
    s_fpFwdOffset = fwd;
    GetPrivateProfileStringA("FirstPerson", "EyeUpAdjust", "0.0", fb, sizeof(fb), path);
    float eyeUp = (float)atof(fb);
    if (eyeUp < -6.0f) eyeUp = -6.0f;  if (eyeUp > 6.0f) eyeUp = 6.0f;
    s_fpEyeUpAdjust = eyeUp;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanUp", "0.0", fb, sizeof(fb), path);
    float mlu = (float)atof(fb);
    if (mlu < 0.0f) mlu = 0.0f;    if (mlu > 12.0f) mlu = 12.0f;
    s_fpMoveLeanUp = mlu;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanForward", "0.0", fb, sizeof(fb), path);
    float mlf = (float)atof(fb);
    if (mlf < 0.0f) mlf = 0.0f;    if (mlf > 12.0f) mlf = 12.0f;
    s_fpMoveLeanFwd = mlf;
    GetPrivateProfileStringA("FirstPerson", "MoveNearClip", "0.0", fb, sizeof(fb), path);
    float mnc = (float)atof(fb);
    if (mnc < 0.0f) mnc = 0.0f;    if (mnc > 10.0f) mnc = 10.0f;
    s_fpMoveNearClip = mnc;
    GetPrivateProfileStringA("FirstPerson", "JogForward", "2.0", fb, sizeof(fb), path);
    float jf = (float)atof(fb);
    if (jf < 0.0f) jf = 0.0f;    if (jf > 20.0f) jf = 20.0f;
    s_fpJogForward = jf;
    GetPrivateProfileStringA("FirstPerson", "RunForward", "4.0", fb, sizeof(fb), path);
    float rf = (float)atof(fb);
    if (rf < 0.0f) rf = 0.0f;    if (rf > 20.0f) rf = 20.0f;
    s_fpRunForward = rf;
    GetPrivateProfileStringA("FirstPerson", "StreamUpdateDist", "2.0", fb, sizeof(fb), path);
    float sud = (float)atof(fb);
    if (sud < 0.0f) sud = 0.0f;    if (sud > 1000.0f) sud = 1000.0f;
    s_fpStreamDist = sud;
    GetPrivateProfileStringA("FirstPerson", "GrassRangeMult", "1.0", fb, sizeof(fb), path);
    float grm = (float)atof(fb);
    if (grm < 1.0f) grm = 1.0f;    if (grm > 8.0f) grm = 8.0f;
    s_fpGrassRangeMult = grm;
    s_fpCamPreOrig = GetPrivateProfileIntA("FirstPerson", "CamPreOrig", 0, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "FollowTurn", "0.08", fb, sizeof(fb), path);
    float ft = (float)atof(fb);
    if (ft < 0.0f) ft = 0.0f;    if (ft > 1.0f) ft = 1.0f;
    s_fpFollowTurn = ft;
    GetPrivateProfileStringA("FirstPerson", "FollowDelayMs", "400", fb, sizeof(fb), path);
    float fd = (float)atof(fb);
    if (fd < 0.0f) fd = 0.0f;    if (fd > 5000.0f) fd = 5000.0f;
    s_fpFollowDelayMs = fd;
    GetPrivateProfileStringA("FirstPerson", "LookSmooth", "0.4", fb, sizeof(fb), path);
    float ls = (float)atof(fb);
    if (ls < 0.0f) ls = 0.0f;    if (ls > 0.9f) ls = 0.9f;
    s_fpLookSmooth = ls;
    s_fpFoliageCenterMode = GetPrivateProfileIntA("FirstPerson", "FoliageCenterFix", 1, path);
    if (s_fpFoliageCenterMode < 0) s_fpFoliageCenterMode = 0;
    if (s_fpFoliageCenterMode > 2) s_fpFoliageCenterMode = 2;
    s_fpFreezeCamTest = GetPrivateProfileIntA("FirstPerson", "FreezeCamTest", 0, path) ? 1 : 0;
    GetPrivateProfileStringA("FirstPerson", "ActionClearForward", "2.0", fb, sizeof(fb), path);
    float acf = (float)atof(fb);
    if (acf < 0.0f) acf = 0.0f;    if (acf > 8.0f) acf = 8.0f;
    s_fpActionClearFwd = acf;
    GetPrivateProfileStringA("FirstPerson", "NeckLimit", "75", fb, sizeof(fb), path);
    float nl = (float)atof(fb);
    if (nl < 30.0f) nl = 30.0f;    if (nl > 170.0f) nl = 170.0f;
    s_fpNeckLimitRad = nl * 3.14159265f / 180.0f;
    GetPrivateProfileStringA("FirstPerson", "NearClip", "0.2", fb, sizeof(fb), path);
    float nc = (float)atof(fb);
    if (nc < 0.05f) nc = 0.05f;    if (nc > 5.0f) nc = 5.0f;
    s_fpNearClipFP = nc;
    s_fpHideHair = GetPrivateProfileIntA("FirstPerson", "HideHair", 1, path) != 0;
    s_fpHideHead = GetPrivateProfileIntA("FirstPerson", "HideHead", 1, path) != 0;
    // KenshiFP-style camera port (2026-07-30).
    s_fpTrueBoneEye = GetPrivateProfileIntA("FirstPerson", "TrueBoneEye", 1, path) != 0;
    s_fpRawMouse    = GetPrivateProfileIntA("FirstPerson", "RawMouse",    1, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "EyeDrop", "0.0", fb, sizeof(fb), path);
    float ed = (float)atof(fb);
    if (ed < -4.0f) ed = -4.0f;    if (ed > 8.0f) ed = 8.0f;
    s_fpEyeDrop = ed;
    GetPrivateProfileStringA("FirstPerson", "MoveForward", "0.0", fb, sizeof(fb), path);
    float mf = (float)atof(fb);
    if (mf < 0.0f) mf = 0.0f;      if (mf > 20.0f) mf = 20.0f;
    s_fpMoveForward = mf;
    GetPrivateProfileStringA("FirstPerson", "MoveSpeedRef", "30.0", fb, sizeof(fb), path);
    float msr = (float)atof(fb);
    if (msr < 1.0f) msr = 1.0f;    if (msr > 2000.0f) msr = 2000.0f;
    s_fpMoveSpeedRef = msr;
    // Ascent-aware stair pullback tunables.
    GetPrivateProfileStringA("FirstPerson", "StairForwardReduce", "0.25", fb, sizeof(fb), path);
    float sfr = (float)atof(fb);
    if (sfr < 0.0f) sfr = 0.0f;    if (sfr > 5.0f) sfr = 5.0f;
    s_fpStairForwardReduce = sfr;
    GetPrivateProfileStringA("FirstPerson", "StairForwardMinScale", "0.30", fb, sizeof(fb), path);
    float sms = (float)atof(fb);
    if (sms < 0.0f) sms = 0.0f;    if (sms > 1.0f) sms = 1.0f;
    s_fpStairForwardMinScale = sms;
    GetPrivateProfileStringA("FirstPerson", "StairEyeLift", "0.30", fb, sizeof(fb), path);
    float sel = (float)atof(fb);
    if (sel < 0.0f) sel = 0.0f;    if (sel > 4.0f) sel = 4.0f;
    s_fpStairEyeLift = sel;
    // Enemy body-clip clearance tunables.
    GetPrivateProfileStringA("FirstPerson", "EnemyClearRadius", "12.0", fb, sizeof(fb), path);
    float ecr = (float)atof(fb);
    if (ecr < 0.0f) ecr = 0.0f;    if (ecr > 60.0f) ecr = 60.0f;
    s_fpEnemyClearRadius = ecr;
    GetPrivateProfileStringA("FirstPerson", "EnemyClearMinScale", "0.15", fb, sizeof(fb), path);
    float ecm = (float)atof(fb);
    if (ecm < 0.0f) ecm = 0.0f;    if (ecm > 0.95f) ecm = 0.95f;
    s_fpEnemyClearMinScale = ecm;
    GetPrivateProfileStringA("FirstPerson", "EnemyNearClip", "0.10", fb, sizeof(fb), path);
    float enc = (float)atof(fb);
    if (enc < 0.0f) enc = 0.0f;    if (enc > 5.0f) enc = 5.0f;
    s_fpEnemyNearClip = enc;
    // Below-floor reveal (0 = legacy character-based reveal only).
    s_fpFloorRevealBelow = GetPrivateProfileIntA("FirstPerson", "FloorRevealBelow", 1, path) != 0;
}

static void loadKeybinds()
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        writeDefaultConfig(path);
        char cbuf[MAX_PATH + 64];
        sprintf_s(cbuf, sizeof(cbuf),
            "[WASDCombat] dc_keybinds_default_config_created path=%s", path);
        DebugLog(cbuf);
    }

    for (int r = 0; r < KR_COUNT; ++r)
    {
        char buf[32] = "";
        GetPrivateProfileStringA("Keybinds", KR_INI_KEY[r], KR_DEFAULT[r],
                                 buf, sizeof(buf), path);
        int vk = parseKeyName(buf);
        if (vk <= 0)
        {
            char ibuf[128];
            sprintf_s(ibuf, sizeof(ibuf),
                "[WASDCombat] dc_keybind_invalid name=%s fallback=%s",
                buf, KR_DEFAULT[r]);
            DebugLog(ibuf);
            strcpy_s(buf, sizeof(buf), KR_DEFAULT[r]);
            vk = parseKeyName(buf);
        }
        s_bindVk[r] = vk;
        strcpy_s(s_bindCfgStr[r], sizeof(s_bindCfgStr[r]), buf);
    }

    for (int i = 0; i < KR_COUNT; ++i)
        for (int j = i + 1; j < KR_COUNT; ++j)
            if (s_bindVk[i] == s_bindVk[j])
            {
                char dbuf[128];
                sprintf_s(dbuf, sizeof(dbuf),
                    "[WASDCombat] dc_keybind_duplicate %s and %s share key 0x%02X",
                    KR_INI_KEY[i], KR_INI_KEY[j], s_bindVk[i]);
                DebugLog(dbuf);
            }

    // [Settings] feature toggles.  Missing key/section => API returns the
    // supplied default ("true"), so existing users who never had this section
    // keep the shipped default ON without touching their file.
    {
        char sbuf[16] = "";
        GetPrivateProfileStringA("Settings", "InventoryFaceCam", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingInventoryFaceCam = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "WasdSpeedCap", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingWasdSpeedCap = parseBool(sbuf, true);

        char mbuf[16] = "";
        GetPrivateProfileStringA("Settings", "WasdSpeedMult", "1.0",
                                 mbuf, sizeof(mbuf), path);
        float wsm = (float)atof(mbuf);
        if (wsm < 0.1f) wsm = 0.1f;   if (wsm > 5.0f) wsm = 5.0f;
        s_settingWasdSpeedMult = wsm;
    }

    // [FirstPerson] tunables — read via the shared helper (also called on every
    // P-enter for live re-tuning).  Missing keys/section fall back to shipped
    // defaults, so existing users keep the defaults without touching their INI.
    loadFirstPersonConfig(path);

    char lbuf[420];
    sprintf_s(lbuf, sizeof(lbuf),
        "[WASDCombat] dc_keybinds_loaded toggle=%s(0x%02X) forward=%s(0x%02X)"
        " back=%s(0x%02X) left=%s(0x%02X) right=%s(0x%02X) speed=%s(0x%02X)"
        " select=%s(0x%02X) inventoryFaceCam=%d",
        s_bindCfgStr[KR_TOGGLE],  s_bindVk[KR_TOGGLE],
        s_bindCfgStr[KR_FORWARD], s_bindVk[KR_FORWARD],
        s_bindCfgStr[KR_BACK],    s_bindVk[KR_BACK],
        s_bindCfgStr[KR_LEFT],    s_bindVk[KR_LEFT],
        s_bindCfgStr[KR_RIGHT],   s_bindVk[KR_RIGHT],
        s_bindCfgStr[KR_SPEED],   s_bindVk[KR_SPEED],
        s_bindCfgStr[KR_SELECT],  s_bindVk[KR_SELECT],
        s_settingInventoryFaceCam ? 1 : 0);
    DebugLog(lbuf);

    char fpbuf[240];
    sprintf_s(fpbuf, sizeof(fpbuf),
        "[WASDCombat] dc_firstperson_cfg key=%s(0x%02X) fov=%.0f sens=%.2f"
        " neckLimitDeg=%.0f hideHead=%d hideHair=%d sneak=SHIFT+%s(0x%02X)"
        " enemyClearR=%.1f",
        s_bindCfgStr[KR_FP], s_bindVk[KR_FP], s_fpFovDegFP, s_fpSensitivityFP,
        s_fpNeckLimitRad * 180.0f / 3.14159265f,
        s_fpHideHead ? 1 : 0, s_fpHideHair ? 1 : 0,
        s_bindCfgStr[KR_SNEAK], s_bindVk[KR_SNEAK],
        s_fpEnemyClearRadius);
    DebugLog(fpbuf);
}

// -----------------------------------------------------------------------
// Press handlers — shared by the poll thread (fallback path) and the
// native processKeys event path.  Loot-suspend gating lives here so both
// paths behave identically.
// -----------------------------------------------------------------------
static void handleTogglePress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE || s_userWantsDC)
    {
        s_userWantsDC = false;
        s_userWantsFP = false;   // FP requires DC; leaving DC clears the FP intent too
        setMode(MODE_VANILLA);
        DebugLog("[WASDCombat] dc_user_intent_off_manual");
    }
    else
    {
        s_userWantsDC = true;
        setMode(MODE_FREE_MOVE);
        DebugLog("[WASDCombat] dc_user_intent_on");
    }
}

static void handleSpeedPress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE)
        s_xPressed = true;
}

static void handleSelectPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge and switches the
    // WASD anchor to the selected/highlighted character.
    if (s_mode == MODE_FREE_MOVE)
        s_fSelectEdge = true;
}

static void handleFirstPersonPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge on the game thread
    // and enters/exits first-person (camera calls must run there, not here).
    if (s_mode == MODE_FREE_MOVE)
        s_fpToggleRequested = true;
}

static void handleSneakPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Shift+C CHORD, first-person only (user req 2026-08-01): the sneak key alone
    // does nothing, so a bare C press can never collide with vanilla or other
    // mod uses of the key.  Final gating (live anchor, FP still active) happens
    // on the game thread where the edge is consumed.
    if (s_mode != MODE_FREE_MOVE || !s_firstPersonActive)
        return;
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    s_sneakToggleRequested = true;
}

static void onPress(int role)
{
    if (role == KR_TOGGLE)       handleTogglePress();
    else if (role == KR_SPEED)   handleSpeedPress();
    else if (role == KR_SELECT)  handleSelectPress();
    else if (role == KR_FP)      handleFirstPersonPress();
    else if (role == KR_SNEAK)   handleSneakPress();
}

// -----------------------------------------------------------------------
// Polling thread
// -----------------------------------------------------------------------
struct PollKey { int role; bool prev; };
static PollKey s_keys[] =
{
    { KR_FORWARD, false }, { KR_LEFT,   false },
    { KR_BACK,    false }, { KR_RIGHT,  false },
    { KR_TOGGLE,  false }, { KR_SPEED,  false },
    { KR_SELECT,  false }, { KR_FP,     false },
    { KR_SNEAK,   false },
};
static const int NUM_KEYS = 9;

static bool isKenshiForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI PollThread(LPVOID)
{
    while (true)
    {
        Sleep(50);
        if (!isKenshiForeground()) continue;
        // Movement roles are ALWAYS VK-polled (INI-configurable) — the
        // native keybind system is one-command-per-key and vanilla camera
        // owns W/S/A/D, so DC movement cannot live there.  Toggle/speed
        // polling stands down once their native commands are registered
        // (presses then arrive via the game's processKeys events).
        for (int i = 0; i < NUM_KEYS; ++i)
        {
            int role = s_keys[i].role;
            if (s_nativeCommandsRegistered
                && (role == KR_TOGGLE || role == KR_SPEED))
                continue;
            bool down = (GetAsyncKeyState(s_bindVk[role]) & 0x8000) != 0;
            if (down == s_keys[i].prev) continue;
            s_keys[i].prev = down;
            bool wasWasd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
            switch (role) {
                case KR_FORWARD: s_wHeld = down; break;
                case KR_LEFT:    s_aHeld = down; break;
                case KR_BACK:    s_sHeld = down; break;
                case KR_RIGHT:   s_dHeld = down; break;
            }
            if (!wasWasd && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
                s_wasdTapStartMs = GetTickCount64();
            if (down) onPress(role);
        }

        // RMB press edge — earliest player-click signal (see global note).
        {
            bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (rmbDown && !s_rmbPrev)
                s_rmbPressedEdge = true;
            s_rmbPrev = rmbDown;
        }

        // LMB double-click edge — two left-press edges within the OS double-click
        // time gate the DC control-switch (see s_lmbDoubleClickMs).  The 50ms poll
        // reliably separates the two down-edges of a normal double-click (~200-
        // 400ms apart); a single click sets only s_lastLmbDownMs and never the
        // double-click timestamp.
        {
            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lmbDown && !s_lmbPrev)
            {
                ULONGLONG nowLB = GetTickCount64();
                if (s_lastLmbDownMs > 0
                    && (nowLB - s_lastLmbDownMs) <= (ULONGLONG)GetDoubleClickTime())
                    s_lmbDoubleClickMs = nowLB;   // double-click
                else
                    s_lmbDoubleClickMs = 0;       // fresh single — clear any stale double
                s_lastLmbDownMs = nowLB;
            }
            s_lmbPrev = lmbDown;
        }

        // CTRL press edge — toggles camera-rotate mode while DC is active (the
        // apply lives in cameraUpdate_hook).  Only flips in DC so the toggle
        // state never desyncs from any out-of-DC CTRL use.
        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Do NOT toggle while a UI is open — CTRL is used for menu actions
            // (CTRL+click to move stacks in trade/inventory); flipping the crosshair
            // toggle there corrupts it (field 2026-06-21).
            if (ctrlDown && !s_ctrlPrevPoll && s_mode == MODE_FREE_MOVE && !s_camRotateUiOpen)
                s_camRotateToggle = !s_camRotateToggle;
            s_ctrlPrevPoll = ctrlDown;
        }
    }
}

// -----------------------------------------------------------------------
// HUD — disabled for reload stability
// -----------------------------------------------------------------------
struct HudWidget { MyGUI::TextBox* label; bool shown; const char* tag;
    HudWidget() : label(nullptr), shown(false), tag("") {} };
static HudWidget s_vHud;
static bool      s_hudReady = false;
static void hudUpdate() {}

// -----------------------------------------------------------------------
// World state — main thread only
// -----------------------------------------------------------------------
static Character*    s_selectedCharacter = nullptr;
static CharMovement* s_selectedMovement  = nullptr;
static Character*    s_freeMoveAnchor    = nullptr;
static bool          s_wasdWasActive     = false;
static bool          s_combatWASDLogged  = false;
static bool          s_retreatLogged     = false;

static bool          s_squadThreat         = false;
static bool          s_consciousAllyThreat = false;

// Cached movement pointer — used by charMovUpdate_hook without dereferencing
// s_freeMoveAnchor (which may be freed during a LOADGAME transition).
static CharMovement* s_anchorMovement = nullptr;

static volatile bool s_loadGuardActive      = false;
static int           s_stabilizationCountdown = 0;
static bool          s_postLoadReacquire   = false;

// DC pointer-loss state — entered when any load signal fires while s_userWantsDC is true.
// Injection pauses; mode stays FREE_MOVE; reacquire loop runs until pointers are valid.
// Times out to hard shutdown only if pointers stay invalid beyond the squad-loss threshold.
static bool           s_dcPtrLossActive      = false;
static ULONGLONG      s_dcPtrLossStartedAt   = 0;  // when pointer loss began (for duration logging)
static ULONGLONG      s_dcPtrLossLastLogTick = 0;  // throttle for periodic duration log
static MoveSpeed      s_dcPreservedSpeedMode  = WALK;
static bool          s_enemyTargetingLogged = false;
static ULONGLONG     s_lastScanTick        = 0;

static ControlMode   s_fmTrackedMode = MODE_VANILLA;

// CombatClass state tracking
static swordStateEnum s_lastCombatState  = COMBAT_FINISHED;
static bool           s_wasPrevInCombat  = false;
static ULONGLONG      s_wasdReleasedTick = 0;

// Combat engagement tracking
static Character*     s_prevAttackTarget  = nullptr;
static bool           s_prevTargetInRange = false;

// Protected animation state tracking (knockdown / get-up / stagger)
static bool           s_wasProtectedState = false;

// Instant-stop: set when WASD movement is applied, cleared on release.
static bool           s_wasdMovementApplied = false;

// Attack commitment — protects CHOP_WEAPON from early interruption
static bool           s_attackCommitmentActive = false;
static ULONGLONG      s_attackCommitmentStart  = 0;

// Healing job active — set/cleared by addJob/removeJob hooks.
// True while any medical job is running on the anchor; DC suspends
// movement injection so the animation is not interrupted.
static bool           s_healingJobActive = false;
// Pending: a medical job arrived while WASD was held.
// Promoted to s_healingJobActive in pre-AI once WASD is released.
static bool           s_healingJobPending = false;

// WASD retreat state — tracks active retreat for suppression diagnostics
static bool          s_wasdRetreatActive      = false;
static bool          s_retreatCleanLogged     = false;
static ULONGLONG     s_retreatActiveStartTick  = 0;
static ULONGLONG     s_lastCombatEnterExitTick = 0;
static bool          s_combatFlickerLogged    = false;

// Post-WASD grace period — suppress combat re-entry after WASD release
static const ULONGLONG POST_WASD_GRACE_MS          = 2000;
static const float     POST_WASD_REENGAGEMENT_RANGE = 200.0f;
static bool            s_postWasdGraceActive        = false;
static ULONGLONG       s_postWasdGraceStart          = 0;
static bool            s_combatReentryAllowed        = false;

// CombatClass::_NV_go suppression — set when go() was skipped this frame
static bool            s_retreatLockGoSuppressed     = false;
// Sticky: once go() is suppressed during a WASD hold, stays true until release
static bool            s_retreatLockEverActive       = false;

// DC OWNERSHIP-HANDOFF COMBAT MODEL (user req 2026-06-21): the controlled
// character's combat AI (CombatClass::_NV_go) runs FULLY AUTONOMOUSLY whenever
// WASD is NOT held, and is SUSPENDED the moment WASD is held (movement owns the
// character) — see combatGo_hook.  No per-frame tug-of-war = no stutter/slide.

// Downed/crippled movement tracking — set each frame applyDownedMovement fires
static bool            s_wasdDownedMovementActive    = false;

// Play-dead exit — once per WASD press; reset when all keys released
static bool            s_playDeadExitDone            = false;

// Combat WASD grace ("absolute movement priority", user req 2026-06-20): while
// in combat, the AI must not react/re-orient until the player has fully released
// WASD for ~half a second.  Rolling between keys (W->A->S->D) or a brief pause
// otherwise instant-stops, letting the combat AI grab the frame to square up to
// the attacker = the "movement pauses / character distracted in combat" reports.
// For this long after the last real key, keep driving the LAST direction so the
// AI never gets a re-orient frame.  Combat-only so out-of-combat stops stay crisp.
// Used by BOTH the charMovUpdate locomotion bridge AND the combatGo_hook ownership
// handoff: for this long after the last real key, movement still owns the character
// (AI stays suspended) so a key-roll doesn't hand control back mid-roll.  250ms
// covers key-rolls while letting the AI resume combat promptly after a real stop
// (in the ownership-handoff model a long grace would delay autonomous combat).
// 140 -> 500 (absolute-priority era) -> 250 (ownership-handoff model).  Tunable.
static const ULONGLONG COMBAT_WASD_BRIDGE_MS         = 250;

// Medical job suppression — set once when any medical job is blocked during a WASD hold,
// cleared on WASD release.  Prevents repeated per-frame log spam.
static bool            s_medicalJobSuppressedThisHold = false;

// Multi-enemy tracking
static int             s_retreatBlockedAttackerCount  = 0;
static int             s_retreatTargetsProcessed      = 0;
static int             s_retreatTargetsCachedSkipped  = 0;
static int             s_lastKnownEnemyCount          = 0;

// Performance: throttle step-7 job removal to once per 250 ms.
static const ULONGLONG JOB_REMOVAL_INTERVAL_MS = 250;
static ULONGLONG       s_jobRemovalLastTick     = 0;

// movement_injection_allowed throttle — emit at most once per second.
static ULONGLONG       s_movInjLogTick         = 0;
// Athletics XP bridge throttle — ticks at 1 s intervals; 0 = timer not yet armed.
static ULONGLONG       s_athleticsXpLastTick   = 0;
// Locomotion feel: direction tracking for turn detection and grace window.
static Ogre::Vector3   s_prevWasdDir           = Ogre::Vector3::ZERO;
static ULONGLONG       s_wasdLastHeldMs        = 0;
// Camera lock: saved freecam state from before DC was activated; restored on DC exit.
static bool            s_savedFreeCameraMode       = false;
// Camera-lock suspension states — DC stays active but tracking is paused.
static bool            s_cameraLockInvSuspend      = false;  // suspended during inventory UI
static bool            s_cameraLockTurretSuspend   = false;  // suspended during turret/mounted use
static bool            s_menuSuspendActive         = false;  // suspended while ou->isPaused() (escape/options/save/load menus)

// -----------------------------------------------------------------------
// Hybrid post-WASD hold (v1.5) — Direct Control is a hybrid play style:
// vanilla point-click movement and orders work normally in V-mode; WASD
// overrides them while pressed; after WASD release the character HOLDS
// where WASD left them until the player gives a new point-click, presses
// WASD again, or toggles V off.  The hold is "WASD parked the character
// here", never "DC owns all locomotion".
//   s_wasdHoldActive: set once at the WASD release edge; cleared by a real
//     player click (playerMove dispatcher, RVA 0x7F95F0), the next WASD
//     press, V transitions, anchor switch, and clearAllState.
//   s_holdPos/s_holdPosValid: X/Z position clamp anchor — motion zeroing
//     alone is too early for indoor routing systems that write position
//     later in the frame, so while holding, the position is restored both
//     post-orig in charMovUpdate and at the end of mainLoop.  Y stays free
//     for gravity/ramp settling.  Invalidated whenever the hold is not
//     enforcing (stale anchor would teleport-snap).
//   Door suppression: door-type addJob/addOrder on the anchor are swallowed
//     ONLY while the hold is active — the window where no player intent
//     exists and stale indoor door tasks used to fire (auto-open bug).
//     Vanilla door behavior everywhere else.  MOVE_CUS_ORDERED is never
//     suppressed (DC's own disengage orders use it).
//   DO NOT hook CharBody::setCurrentAction (KenshiLib error 8 → crash).
// -----------------------------------------------------------------------
static bool           s_wasdHoldActive          = false;
// Active player point-click/order — set in the real click dispatcher
// (playerMove_hook), cleared at the WASD press edge (WASD wins), the WASD
// release edge (the release anchor-snap cancels the order anyway), V
// transitions, anchor switch, and clearAllState.  The hold may only
// enforce when this is false: a live player click always outranks the
// hold, regardless of which was set first.
static bool           s_playerPointClickActive  = false;
static Ogre::Vector3  s_holdPos                 = Ogre::Vector3::ZERO;
static bool           s_holdPosValid            = false;
static bool           s_idleHoldEngaged         = false;  // log edge tracking
static ULONGLONG      s_authGateLogTick         = 0;      // gate log heartbeat
static bool           s_authGateLastAllow       = true;   // gate log change detect
static char           s_authGateLastReason[24]  = "";
static ULONGLONG      s_doorSuppressLogTick     = 0;      // door-suppress log throttle
static ULONGLONG      s_addOrderDiagLogTick     = 0;      // addorder-during-hold diag throttle
static bool           s_hookBlockLoggedPMove    = false;  // once-per-load hook-block log
// DC camera focus offset: saved objectCurrentlyFollowingOffset.y from DC entry; restored on DC exit.
static float           s_savedCamFollowOffY        = 0.0f;

// Performance: session cache — each attacker blocked exactly once per WASD hold.
// No TTL: valid for the entire retreat lock; cleared on WASD release.
// 256 entries covers large guard squads without repeated miss-scans.
static const int  RETREAT_CACHE_SIZE       = 256;
static Character* s_retreatSessionCache[RETREAT_CACHE_SIZE];
static int        s_retreatSessionCacheCount = 0;

// -----------------------------------------------------------------------
// clearAllState
// -----------------------------------------------------------------------
static void otsRestoreNames();   // fwd decl — defined with the OTS camera code
static void clearAllState()
{
    s_mode              = MODE_VANILLA;
    s_wHeld             = false;
    s_aHeld             = false;
    s_sHeld             = false;
    s_dHeld             = false;
    s_camRotateToggle   = false;
    s_lmbDoubleClickMs  = 0;
    s_fSelectEdge       = false;
    s_selectedCharacter = nullptr;
    s_selectedMovement  = nullptr;
    s_freeMoveAnchor    = nullptr;
    s_anchorMovement    = nullptr;
    s_wasdWasActive     = false;
    s_combatWASDLogged  = false;
    s_retreatLogged     = false;
    s_squadThreat       = false;
    s_consciousAllyThreat = false;
    s_enemyTargetingLogged = false;
    s_lastScanTick      = 0;
    s_fmTrackedMode     = MODE_VANILLA;
    s_xPressed          = false;
    s_stabilizationCountdown = 0;
    s_lastCombatState   = COMBAT_FINISHED;
    s_wasPrevInCombat   = false;
    s_wasdReleasedTick  = 0;
    s_prevAttackTarget  = nullptr;
    s_prevTargetInRange = false;
    s_wasProtectedState = false;
    s_wasdMovementApplied    = false;
    s_attackCommitmentActive = false;
    s_attackCommitmentStart  = 0;
    s_healingJobActive       = false;
    s_wasdRetreatActive      = false;
    s_retreatCleanLogged     = false;
    s_retreatActiveStartTick  = 0;
    s_lastCombatEnterExitTick = 0;
    s_combatFlickerLogged    = false;
    s_postWasdGraceActive      = false;
    s_postWasdGraceStart       = 0;
    s_combatReentryAllowed     = false;
    s_retreatLockGoSuppressed     = false;
    s_retreatLockEverActive       = false;
    s_wasdDownedMovementActive    = false;
    s_playDeadExitDone            = false;
    // Inventory face-cam: load/teardown — the scene (and our detached node) is
    // gone, so make NO camera calls here; just drop the runtime flags/pointers.
    s_fpActive                    = false;
    s_fpNode                      = nullptr;
    s_fpCursorCaptured            = false;
    s_fpCamLocalsSaved            = false;
    s_fpHadAutoTrack              = false;
    // First-person: scene (and our detached node/skeleton) is gone — drop the
    // runtime flags/pointers only, make NO camera/skeleton/GUI calls here.
    // Restoring the grass/foliage draw-range IS safe (just writes the options
    // globals) and must happen or a teardown that bypassed exitFirstPerson would
    // leave the range permanently boosted.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
    }
    s_firstPersonActive           = false;
    s_fpToggleRequested           = false;
    s_sneakToggleRequested        = false;
    s_fpEnemyClearSmooth          = 1.0f;
    s_fpEnemyNearestDist          = -1.0f;
    s_fpSuspendedForInv           = false;
    s_fpHeadBoneHidden            = false;
    s_fpHairHidden                = false;
    s_fpHeadSmoothValid           = false;
    s_fpCrosshair                 = nullptr;  // GUI torn down; recreate on next FP enter
    otsRestoreNames();            // re-show name-tags if a teardown left them hidden
    s_otsRestorePending           = false;
    s_invFaceCloseStreak          = INV_FACE_CLOSE_DEBOUNCE;
    s_otsInvFaceActive            = false;
    s_otsInvFaceChar              = nullptr;
    s_otsSavedYaw                 = 0.0f;
    s_otsSavedPitch               = 0.0f;
    s_otsSavedDist                = 14.0f;
    s_medicalJobSuppressedThisHold = false;
    s_retreatBlockedAttackerCount  = 0;
    s_retreatTargetsProcessed      = 0;
    s_retreatTargetsCachedSkipped  = 0;
    s_lastKnownEnemyCount          = 0;
    s_jobRemovalLastTick           = 0;
    s_retreatSessionCacheCount     = 0;
    s_lootUiSuspendActive          = false;
    s_lootUiWasPrevOpen            = false;
    s_tradeWindowActive            = false;
    s_lootSuspendStartTick         = 0;
    s_invMoveThroughActive         = false;
    s_invMoveThroughForcedRun      = false;
    s_invMoveThroughPlayerPaused   = false;
    s_invMoveThroughShownChar      = nullptr;
    s_invMoveThroughEdgeTick       = 0;
    s_invTradeCloseRequested       = false;
    s_invTradeStartValid           = false;
    s_movInjLogTick                = 0;
    s_athleticsXpLastTick          = 0;
    s_prevWasdDir                  = Ogre::Vector3::ZERO;
    s_wasdLastHeldMs               = 0;
    s_savedFreeCameraMode          = false;
    s_prof_mainLoop                = 0;
    s_prof_charMove                = 0;
    s_prof_playerControl           = 0;
    s_prof_committedAct            = 0;
    s_prof_threatScan              = 0;
    s_prof_cameraLock              = 0;
    s_prof_wasdInject              = 0;
    s_prof_combatTarget            = 0;
    s_nearbyEnemyCount             = 0;
    s_chaseFlapsCount              = 0;
    s_pathfindingEnemyCount        = 0;
    s_cameraLockInvSuspend         = false;
    s_cameraLockTurretSuspend      = false;
    s_menuSuspendActive            = false;
    s_wasdHoldActive          = false;
    s_playerPointClickActive  = false;
    s_holdPos                 = Ogre::Vector3::ZERO;
    s_holdPosValid            = false;
    s_idleHoldEngaged         = false;
    s_authGateLogTick         = 0;
    s_authGateLastAllow       = true;
    s_authGateLastReason[0]   = '\0';
    s_doorSuppressLogTick     = 0;
    s_addOrderDiagLogTick     = 0;
    s_hookBlockLoggedPMove    = false;
    s_savedCamFollowOffY           = 0.0f;
    s_wasdTapStartMs               = 0;
    s_userWantsDC           = false;
    s_userWantsFP           = false;   // hard teardown clears FP intent (survivable loads preserve it)
    s_dcPtrLossActive       = false;
    s_dcPtrLossStartedAt    = 0;
    s_dcPtrLossLastLogTick  = 0;
    s_hookBlockLoggedMain    = false;
    s_hookBlockLoggedCharMov = false;
    s_hookBlockLoggedPCtrl   = false;
    s_hookBlockLoggedRemJob  = false;
    s_hookBlockLoggedAddJob  = false;
    s_hookBlockLoggedTrade   = false;
    s_shutdownWaitLogTick    = 0;
    s_healingJobPending      = false;
}

// -----------------------------------------------------------------------
// computeWASDDirection — camera-relative direction helper.
// -----------------------------------------------------------------------
static bool computeWASDDirection(bool bW, bool bA, bool bS, bool bD, Ogre::Vector3& outDir)
{
    if (!ou || !ou->player || !ou->player->camera) return false;
    Ogre::Vector3 camFwd;
    if (s_firstPersonActive)
    {
        // First-person: the game camera controller is detached and stale — the
        // real view heading lives in s_fpYaw (drives mouse-look + the rendered
        // camera orientation).  Basis must match it so W follows the gaze.
        camFwd = Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
    }
    else
    {
        camFwd = ou->player->camera->getFacingDirection();
    }
    camFwd.y = 0.0f;
    float cflen = camFwd.length();
    if (cflen < 0.001f) return false;
    camFwd /= cflen;
    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
    Ogre::Vector3 move = Ogre::Vector3::ZERO;
    if (bW) move += camFwd;
    if (bS) move -= camFwd;
    if (bD) move += camRight;
    if (bA) move -= camRight;
    float mlen = move.length();
    if (mlen < 0.001f) return false;
    outDir = g_loco.normalizeDiagonalMovement ? (move / mlen) : move;
    return true;
}

// -----------------------------------------------------------------------
// wasdMoveLimit — the move-limit passed to setDirectMovement for WASD.
//
// Legacy behaviour forced ~99 (uncapped), which let shackled/injured/encumbered
// characters run at full speed and outrun enemies.  With the cap on (default), the
// limit is the character's REAL max run speed (CharStats::getMaxRunSpeed, which the
// game computes from stats/injuries/encumbrance) scaled by WasdSpeedMult, with an
// extra hard clamp while chained/shackled so the Rebirth shackle escape is a shuffle.
// The turn-responsiveness boost still applies briefly on direction changes for snappy
// turns.  Cap off (WasdSpeedCap=false) → the old uncapped limit.
static float wasdMoveLimit(bool turning)
{
    const float turnBoost = turning ? g_loco.wasdTurnResponsiveness : 1.0f;
    if (!s_settingWasdSpeedCap || !s_freeMoveAnchor)
        return 99.0f * g_loco.wasdAccelerationMultiplier * turnBoost;

    float legit = 99.0f;
    CharStats* st = s_freeMoveAnchor->getStats();
    if (st)
    {
        float m = st->getMaxRunSpeed();      // injury / encumbrance aware
        if (m > 0.1f) legit = m;
    }
    // Shackles hard-limit real movement via a separate mechanism the direct-move
    // bypasses; when chained, clamp to a slow shuffle so the player can't sprint off.
    if (s_freeMoveAnchor->isChainedMode())
    {
        float shuffle = legit * 0.35f;
        if (shuffle > 6.0f) shuffle = 6.0f;   // absolute shuffle ceiling
        legit = shuffle;
    }
    // Sneaking (Shift+C in first-person, or the vanilla sneak button): vanilla
    // movement is capped at the stealth-skill speed; mirror it so WASD-sneak is
    // exactly as fast as the game's own sneak movement, no faster.
    if (st && s_freeMoveAnchor->isStealthMode())
    {
        float sneakMax = st->calculateMaxStealthSpeed();
        if (sneakMax > 0.1f && sneakMax < legit) legit = sneakMax;
    }
    legit *= s_settingWasdSpeedMult;

    // Per-second diagnostic so the real values can be read from the RE_Kenshi log.
    if (g_log.debugVerbose)
    {
        static ULONGLONG s_spdLogTick = 0;
        ULONGLONG t = GetTickCount64();
        if (t - s_spdLogTick >= 1000)
        {
            s_spdLogTick = t;
            char b[160];
            sprintf_s(b, sizeof(b),
                "[WASDCombat] wasd_speed maxRun=%.1f chained=%d mult=%.2f limit=%.1f",
                st ? st->getMaxRunSpeed() : -1.0f,
                s_freeMoveAnchor->isChainedMode() ? 1 : 0,
                s_settingWasdSpeedMult, legit * turnBoost);
            DebugLog(b);
        }
    }
    return legit * turnBoost;
}

// applyPlayerMovement — halt() + setDirectMovement in camera-relative WASD.
// -----------------------------------------------------------------------
static bool applyPlayerMovement(bool bW, bool bA, bool bS, bool bD)
{
    CharMovement* mv = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
    if (!s_freeMoveAnchor || !mv) return false;
    if (!ou || !ou->player || !ou->player->camera) return false;

    Ogre::Vector3 move;
    if (!computeWASDDirection(bW, bA, bS, bD, move)) return false;

    // Turn responsiveness: boost move limit on significant direction change.
    bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
    bool turning    = prevHasDir && (move.dotProduct(s_prevWasdDir) < 0.9f);
    float limit     = wasdMoveLimit(turning);

    mv->halt();
    mv->setDesiredSpeed(mv->speedOrders);
    mv->setDirectMovement(move, limit);
    s_prevWasdDir = move;
    return true;
}

// -----------------------------------------------------------------------
// isProtectedAnimationState — returns true when V-Mode must not interfere.
// -----------------------------------------------------------------------
static bool isProtectedAnimationState(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    if (prone == PS_KO || prone == PS_PLAYING_DEAD) return true;
    if (ch->isDown())              return true;
    if (ch->isCurrentlyGettingUp) return true;
    CombatClass* cc = ch->getCombatClass();
    if (cc && cc->getCombatState() == STUMBLE) return true;
    return false;
}

// -----------------------------------------------------------------------
// isCommittedAction — returns true when the character is executing a vanilla
// action that DC must not interrupt.
//
// Narrowed scope (current pass): only used at instant_stop and
// combat_state_restored_after_wasd_release.  Not used at movement injection
// sites until movement logs confirm it is no longer over-broad.
//
// Committed action state set:
//   STARTUP_STATE    attack windup
//   CHOP_WEAPON      active swing
//   DECISION         attack recovery
//   BLOCK            active block
//   REACTION_BLOCK   parry
//   HESITATE         hesitation between attack cycles
//   STUMBLE          stagger (also caught by isProtectedAnimationState)
//   isDown / PS_KO / PS_PLAYING_DEAD / isCurrentlyGettingUp
//                    knockdown, unconscious, playing dead, get-up
//   s_healingJobActive  medical action in progress
// -----------------------------------------------------------------------
static bool isCommittedAction(Character* ch)
{
    ScopeTimer _tCA(s_prof_committedAct);
    if (!ch) return false;

#define _LOG_COMMITTED(reason) \
    if (g_log.debugVerbose) DebugLog("[WASDCombat] committed_action_true reason=" reason)

    ProneState prone = ch->getProneState();
    if (prone == PS_KO)
        { _LOG_COMMITTED("PS_KO");              return true; }
    if (prone == PS_PLAYING_DEAD)
        { _LOG_COMMITTED("PS_PLAYING_DEAD");    return true; }
    if (ch->isDown())
        { _LOG_COMMITTED("isDown");             return true; }
    if (ch->isCurrentlyGettingUp)
        { _LOG_COMMITTED("isCurrentlyGettingUp"); return true; }
    if (s_healingJobActive)
        { _LOG_COMMITTED("HEALING_JOB");        return true; }

    CombatClass* cc = ch->getCombatClass();
    // Gate the combat-STATE checks on combatModeActive (v1.8.4 lesson): a combat
    // state left STALE after a fight (e.g. recovering from a knockdown — the state
    // machine can sit in DECISION/STUMBLE with combatModeActive already false) must
    // NOT count as a committed action, or it blocks the release-stop and WASD
    // movement is delayed after you get up (field 2026-06-21).  The physical states
    // above (KO/down/getting-up/healing) stay ungated — they are real regardless.
    if (cc && cc->combatModeActive)
    {
        swordStateEnum st = cc->getCombatState();
        if (st == STUMBLE)
            { _LOG_COMMITTED("STUMBLE");        return true; }
        if (st == STARTUP_STATE)
            { _LOG_COMMITTED("STARTUP_STATE");  return true; }
        if (st == CHOP_WEAPON)
            { _LOG_COMMITTED("CHOP_WEAPON");    return true; }
        if (st == DECISION)
            { _LOG_COMMITTED("DECISION");       return true; }
        if (st == BLOCK)
            { _LOG_COMMITTED("BLOCK");          return true; }
        if (st == REACTION_BLOCK)
            { _LOG_COMMITTED("REACTION_BLOCK"); return true; }
        if (st == HESITATE)
            { _LOG_COMMITTED("HESITATE");       return true; }
    }

#undef _LOG_COMMITTED
    DebugLog("[WASDCombat] committed_action_false");
    return false;
}

// -----------------------------------------------------------------------
// isCommittedCombatClip — the character is mid-play in a committed one-shot combat
// CLIP that must finish before WASD movement takes over: their own attack swing
// (windup STARTUP_STATE / strike CHOP_WEAPON), a stagger from being hit (STUMBLE), or
// a parry (REACTION_BLOCK).  Kenshi exposes no way to abort an animation clip, so
// cutting one with movement looks broken / stutters (field 2026-06-22: stutter when
// retreating + after being hit and stumbled).  While this is true: the buffer in
// charMovUpdate HOLDS movement (no inject, no state touched) AND combatGo_hook lets
// go() RUN so the clip advances and finishes — exactly one system drives the body, no
// fighting.  The instant the clip ends, movement resumes (plain walk).  DECISION /
// BLOCK / CIRCLE / WAIT / HESITATE are NOT included — they persist or re-trigger
// attacks, so the player must be able to move/retreat through them.  Gated on
// combatModeActive (stale post-combat states must not count — v1.8.4 lesson).
// -----------------------------------------------------------------------
static bool isCommittedCombatClip(Character* ch)
{
    if (!ch) return false;
    CombatClass* cc = ch->getCombatClass();
    if (!cc || !cc->combatModeActive) return false;
    swordStateEnum st = cc->getCombatState();
    return st == STARTUP_STATE || st == CHOP_WEAPON
        || st == STUMBLE       || st == REACTION_BLOCK;
}

// -----------------------------------------------------------------------
// isAnchoredToFurniture — the character is physically using a UseableStuff object
// (chair / throne / bed / crafting+research machine).  The low-level CharBody action
// for using one is OPERATE_MACHINERY (key 87) when you put them there directly, OR
// PRETEND_TO_OPERATE_MACHINERY (key 221) when they idled onto it via a toggled JOB
// (field diag 2026-06-22 — BOTH must be detected, else a job-sat character rotates in
// place after a squad-switch).  In this state the body is locked to the furniture
// node, so injecting setDirectMovement only ROTATES the model; when detected with WASD
// held we issue a real move order to detach them (see charMovUpdate).  Calls are
// header-declared (KenshiLib-linked) + null-checked.  SIT_AROUND/SIT_ON_THRONE/
// USE_BED*/REST are higher-level AI goals that never surface as the current action;
// kept as harmless belt-and-suspenders.
// -----------------------------------------------------------------------
static bool isSeatedTaskType(TaskType t)
{
    return t == OPERATE_MACHINERY
        || t == PRETEND_TO_OPERATE_MACHINERY   // job-driven idle-at-station (jobs toggled ON)
        || t == SIT_AROUND || t == SIT_ON_THRONE
        || t == USE_BED    || t == USE_BED_ORDER
        || t == REST;
}

static bool isAnchoredToFurniture(Character* ch)
{
    if (!ch) return false;
    if (ch->inSomething == IN_BED) return true;        // sleeping / lying in a bed
    CharBody* body = ch->getBody();
    if (body)
    {
        Tasker* action = body->getCurrentAction();
        if (action && isSeatedTaskType(action->key()))
            return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// isUsingStationaryTurret — true when character is manning a turret/crossbow.
// When WASD is not held, V-Mode must not suppress aiming input.
// When WASD is held, turret use is cancelled (player takes movement authority).
// -----------------------------------------------------------------------
static bool isUsingStationaryTurret(Character* ch)
{
    if (!ch) return false;
    // isUsingTurret is a hand (reference to the turret building); truthy when valid.
    return (bool)(ch->isUsingTurret);
}

// -----------------------------------------------------------------------
// Retreat session cache — each enemy processed once per WASD hold, then
// immediately skipped on every subsequent call with zero overhead.
// Cleared on WASD release via s_retreatSessionCacheCount = 0.
// -----------------------------------------------------------------------
static bool retreatSessionCacheContains(Character* ch)
{
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return true;
    return false;
}

static void retreatSessionCacheAdd(Character* ch)
{
    if (!ch) return;
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return;
    if (s_retreatSessionCacheCount < RETREAT_CACHE_SIZE)
        s_retreatSessionCache[s_retreatSessionCacheCount++] = ch;
    // If full: silently drop — best-effort optimization
}

// -----------------------------------------------------------------------
// isDownedButMovable — true when character is downed/crippled/playing-dead but
// vanilla point-click movement still works (crawl/limp).  Hard blocks (truly
// unconscious, getting-up animation, stumble) return false.
// -----------------------------------------------------------------------
static bool isDownedButMovable(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    // PS_KO is the authoritative hard block — truly knocked out, cannot crawl.
    // Do NOT use isUnconcious() here: it returns true for PS_PLAYING_DEAD and
    // crippled characters in Kenshi even though point-click crawl still works.
    if (prone == PS_KO) return false;
    if (ch->isCurrentlyGettingUp) return false;
    // Playing-dead and crippled can crawl/limp via the point-click path.
    if (prone == PS_PLAYING_DEAD || prone == PS_CRIPPLED) return true;
    // Down but not KO and not getting up — conscious downed state.
    if (ch->isDown() && !ch->isUnconcious()) return true;
    return false;
}

// stableIndoors — isInsideBuildingLoadedInterior with 400 ms hysteresis.
// Stairwell/roof transitions (e.g. stormhouse interior -> roof) flicker the
// raw flag between floor layers; without debounce the crawl flaps between
// order mode and direct mode, each cancelling the other (field finding,
// 2026-06-12: "struggles to move smoothly through the layers").  Anchor-only
// state — DC controls one character at a time.
static bool      s_indoorEffective  = false;
static bool      s_indoorPendingVal = false;
static ULONGLONG s_indoorPendingMs  = 0;

static bool stableIndoors(CharMovement* mv)
{
    bool raw = mv->isInsideBuildingLoadedInterior();
    if (raw == s_indoorEffective)
    {
        s_indoorPendingMs = 0;
        return s_indoorEffective;
    }
    ULONGLONG now = GetTickCount64();
    if (s_indoorPendingMs == 0 || raw != s_indoorPendingVal)
    {
        s_indoorPendingVal = raw;
        s_indoorPendingMs  = now;
        return s_indoorEffective;
    }
    if (now - s_indoorPendingMs >= 400)
    {
        s_indoorEffective = raw;
        s_indoorPendingMs = 0;
        DebugLog(raw ? "[WASDCombat] dc_downed_zone_now_indoors"
                     : "[WASDCombat] dc_downed_zone_now_outdoors");
    }
    return s_indoorEffective;
}

// downedOrderDriven — ALWAYS false since v1.7.17: downed movement is
// direct-injected everywhere, identical to standing WASD.
// History of the crawl saga, so nobody resurrects the order mode:
//   - Orders indoors path-walk the interior network regardless of dest
//     ("directional keys are meaningless indoors", v1.7.13).
//   - Orders outdoors pathfind a blind 10 m dest; on rooftops/elevated
//     ground that dest lands off the structure and the pathfinder routes
//     back DOWN ("bounce off the roof layer", v1.7.16).
//   - Direct injection was proven downed-capable in v1.7.14 ("downed
//     movement feels great indoors") — the original downed stutter that
//     motivated orders was the combat-steering conflict + order-fighters,
//     both fixed independently (v1.7.6 flip, v1.7.11 gates).
// Order machinery (applyDownedMovement, stableIndoors, the step-5 order
// branch, the flag-gated stops) is retained dormant for rollback.
static bool downedOrderDriven(Character* ch)
{
    (void)ch;
    (void)&stableIndoors;   // keep dormant order machinery referenced
    return false;
}

// applyDownedMovement — issue point-click-equivalent order for downed/crippled
// characters.  Uses playerMoveOrderDefault (pathfind/crawl path) rather than
// setDirectMovement, which is only valid for standing locomotion.
static Ogre::Vector3 s_downedLastDir     = Ogre::Vector3::ZERO;
static Ogre::Vector3 s_downedLastDest    = Ogre::Vector3::ZERO;
static ULONGLONG     s_downedLastIssueMs = 0;
static Ogre::Vector3 s_crawlSamplePos    = Ogre::Vector3::ZERO;
static ULONGLONG     s_crawlSampleMs     = 0;

static void applyDownedMovement(bool bW, bool bA, bool bS, bool bD)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    Ogre::Vector3 dir;
    if (!computeWASDDirection(bW, bA, bS, bD, dir)) return;
    float dlen = dir.length();
    if (dlen < 0.001f) return;
    dir /= dlen;   // pathfind dest needs direction only, never diagonal scaling

    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG    nowDI   = GetTickCount64();

    // Diagnostic: once per second while crawling, compare actual motion
    // against the intended camera-relative direction.
    if (!s_wasdDownedMovementActive)
    {
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }
    else if (nowDI - s_crawlSampleMs >= 1000)
    {
        Ogre::Vector3 dmoved = posNow - s_crawlSamplePos;
        char buf[192];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_actual moved=(%.2f,%.2f,%.2f) want=(%.2f,%.2f,%.2f)",
            dmoved.x, dmoved.y, dmoved.z, dir.x, dir.y, dir.z);
        DebugLog(buf);
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }

    // Outdoors only — indoor downed movement is direct-injected (see
    // downedOrderDriven); v1.7.13's indoor short-hop attempt proved the
    // interior router path-walks ANY order regardless of distance.
    const float hopLen     = 100.0f;   // point-click range, 10 m
    const float approachAt = 60.0f;

    // ONE persistent order, like a point-click (per-frame re-issue
    // restarts pathfinding before it produces motion — the never-starts
    // stutter).  This only works because NOTHING else is allowed to fight
    // the order while a downed character holds keys: the press-edge
    // disengage and the post-AI standing injection are both downed-gated
    // (v1.7.11) — they were the hidden order-killers that made the
    // throttled crawl die after a few steps.  Re-issue on first press,
    // direction change, a 1.5 s refresh, or approach of the last dest.
    if (s_wasdDownedMovementActive
        && dir.dotProduct(s_downedLastDir) > 0.95f
        && nowDI - s_downedLastIssueMs < 1500
        && (s_downedLastDest - posNow).length() > approachAt)
        return;
    s_downedLastDir     = dir;
    s_downedLastIssueMs = nowDI;

    // Dest at point-click range.  v1.7.11 used 50 m, which is far enough
    // off the local navmesh that the path's first leg could head in the
    // wrong direction — the user-visible "directions are wrong".  Short
    // hops behave like the nearby point-clicks that are known good.
    Ogre::Vector3 dest = posNow + dir * hopLen;
    s_downedLastDest = dest;
    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
    {
        char buf[224];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_issue keys=%d%d%d%d dir=(%.2f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) dest=(%.1f,%.1f,%.1f)",
            bW?1:0, bA?1:0, bS?1:0, bD?1:0, dir.x, dir.y, dir.z,
            posNow.x, posNow.y, posNow.z, dest.x, dest.y, dest.z);
        DebugLog(buf);
    }
}

// =======================================================================
// OTS action camera — core implementation (ported from OTS_Project_Shelved).
// =======================================================================
static const float FP_RAD_PER_PIXEL = 0.0030f;
static const float FP_PITCH_LIMIT   = 1.45f;   // ~83 degrees, radians

static bool isKenshiForegroundMain()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// --- First-person raw mouse-look via DirectInput (KenshiFP method) ----------
// A second, NON-EXCLUSIVE BACKGROUND DirectInput mouse device reads the same
// high-rate relative stream the game does — Kenshi keeps its own input (we steal
// no registration, so right-click etc. still work).  A dedicated ~1kHz thread
// owns the reads and accumulates counts; each frame consumes the total.  This
// decouples look feel from framerate: the old GetCursorPos/SetCursorPos warp was
// sampled at the frame rate and felt sluggish-then-teleporty at high fps.  The
// To stay independent of the DirectX import libs (dxguid/dinput8 are not reliably
// on the v100 toolset's lib path), we resolve DirectInput8Create at runtime and
// define the GUIDs + mouse data format ourselves — exactly KenshiFP's approach.
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static const GUID DIFP_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID DIFP_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
// DIMOUSESTATE2 axes at offsets 0/4/8 (lX/lY/lZ).  NULL pguid = "any object of
// this type" → the device's relative X/Y/Z map onto these slots.
static DIOBJECTDATAFORMAT DIFP_odf[] = {
    { NULL, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 8, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
};
static const DIDATAFORMAT DIFP_df = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE2), 3, DIFP_odf
};
static IDirectInputDevice8A* s_diMouse      = nullptr;
static bool                  s_diReady      = false;
static volatile LONG         s_diAccX       = 0;
static volatile LONG         s_diAccY       = 0;
static HANDLE                s_diThread     = nullptr;
static volatile LONG         s_diThreadRun  = 0;

static void fpEnsureDInput()
{
    static int tried = 0;
    if (s_diReady || tried >= 600) return;   // retry through early frames, then give up
    tried++;
    HWND w = FindWindowA("OgreD3D11Wnd", nullptr);
    if (!w) w = FindWindowA("OgreD3D9Wnd", nullptr);
    if (!w) w = GetForegroundWindow();
    if (!w) return;
    if (!s_diMouse)
    {
        HMODULE dll = LoadLibraryA("dinput8.dll");
        DI8Create_t create = dll
            ? (DI8Create_t)GetProcAddress(dll, "DirectInput8Create") : nullptr;
        IDirectInput8A* di = nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                     DIFP_IID_IDirectInput8A, (void**)&di, nullptr)) || !di)
        { tried = 600; return; }
        if (FAILED(di->CreateDevice(DIFP_GUID_SysMouse, &s_diMouse, nullptr)) || !s_diMouse)
        { di->Release(); tried = 600; return; }
        di->Release();
        s_diMouse->SetDataFormat(&DIFP_df);
    }
    s_diMouse->SetCooperativeLevel(w, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (SUCCEEDED(s_diMouse->Acquire()))
    {
        s_diReady = true;
        DebugLog("[WASDCombat] dc_fp_dinput_acquired");
    }
}

static DWORD WINAPI fpDInputPollThread(void*)
{
    timeBeginPeriod(1);   // 1ms Sleep granularity for this loop
    while (InterlockedCompareExchange(&s_diThreadRun, 1, 1))
    {
        if (s_diReady && s_diMouse)
        {
            DIMOUSESTATE2 st;
            if (SUCCEEDED(s_diMouse->GetDeviceState(sizeof(st), &st)))
            {
                if (st.lX) InterlockedAdd(&s_diAccX, st.lX);
                if (st.lY) InterlockedAdd(&s_diAccY, st.lY);
            }
            else s_diMouse->Acquire();   // lost (alt-tab): re-acquire, skip this poll
        }
        Sleep(1);
    }
    return 0;
}

static void fpStartDInputThread()
{
    if (s_diThread) return;
    InterlockedExchange(&s_diThreadRun, 1);
    s_diThread = CreateThread(nullptr, 0, fpDInputPollThread, nullptr, 0, nullptr);
}

// Consume (and zero) the accumulated relative deltas since the last call.
static void fpTakeMouseAccum(float* dx, float* dy)
{
    *dx = (float)InterlockedExchange(&s_diAccX, 0);
    *dy = (float)InterlockedExchange(&s_diAccY, 0);
}

// otsRestoreCameraToRig — re-attach the Ogre camera to the game's rig and
// restore its transform/FOV/near-clip/auto-tracking, destroying our detached
// node.  ONLY for the inventory face-cam now (the gameplay OTS is scrapped).
static void otsRestoreCameraToRig(CameraClass* cam)
{
    if (!cam) return;
    Ogre::Camera* oc = cam->camera;
    if (oc)
    {
        oc->detachFromParent();
        Ogre::SceneNode* rigNode = cam->getCameraNode();
        if (rigNode)
            rigNode->attachObject(oc);
        if (s_fpCamLocalsSaved)
        {
            oc->setPosition(s_fpSavedCamPos);
            oc->setOrientation(s_fpSavedCamOri);
            oc->setFOVy(s_fpSavedFov);
            if (s_fpSavedNearClip > 0.0f)
                oc->setNearClipDistance(s_fpSavedNearClip);
        }
        if (s_fpHadAutoTrack)
            oc->setAutoTracking(true, cam->getCenterNode());
        if (s_fpNode)
        {
            oc->getSceneManager()->destroySceneNode(s_fpNode);
            s_fpNode = nullptr;
        }
    }
    s_fpNode           = nullptr;
    s_fpCamLocalsSaved = false;
    s_fpHadAutoTrack   = false;
}

// Restore the floating name-tags to the player's setting if we hid them.  Safe
// to call from any teardown path; only touches gui when names were actually
// hidden and gui is valid.
static void otsRestoreNames()
{
    if (!s_namesHidden) return;
    if (gui) gui->showNames(s_savedShowNames);
    s_namesHidden = false;
    DebugLog("[WASDCombat] dc_names_restored");
}

// exitOTS — re-attach the camera to the rig and re-track the anchor.
static void exitOTS(bool restoreCamera)
{
    if (!s_fpActive) return;
    s_fpActive = false;
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    otsRestoreNames();
    s_fpNode = nullptr;
    DebugLog("[WASDCombat] dc_cam_exited");
}

// enterOTS — detach the Ogre camera onto our own root node so we can aim it
// freely (the attached RTS camera can only look top-down, which is why the
// inventory face-cam can't face the character without detaching).  Used ONLY
// for the inventory face-cam, while the sim is paused and the character is
// standing — none of the walking/floor problems that scrapped the gameplay OTS.
static void enterOTS()
{
    if (s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    s_otsSavedAltitude = cam->altitude;   // held constant during the face-cam
    cam->stopFollowing();
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDeg)));
    oc->setNearClipDistance(s_fpNearClip);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    // Hide the floating name-tags for the duration of the face-cam.
    if (gui && !s_namesHidden)
    {
        s_savedShowNames = options ? options->showNames : true;
        gui->showNames(false);
        s_namesHidden = true;
        DebugLog("[WASDCombat] dc_names_hidden");
    }
    s_fpActive = true;
    DebugLog("[WASDCombat] dc_cam_entered");
}

// Own-inventory = at least one inventory window open (LIVE count, never stale)
// AND no trade/loot session (edge-latched flag, never stale).  True for a plain
// inventory (1 window) AND a backpack character (2 windows); false for any
// shop/loot/corpse trade.  See s_tradeWindowActive for the staleness rationale.
static bool isOwnInventoryOpen()
{
    return gui && !s_tradeWindowActive
        && !gui->isCharacterEditorMode()   // editor owns the camera; don't fight it
        && gui->getNumOpenInventoryWindows() >= 1;
}

// Inventory move-through eligibility (see s_invMoveThroughActive).  Opt-in via
// InventoryFaceCam=false: DC stays live (movement + camera lock, game running)
// while ANY inventory window is open, EXCEPT during dialogue (which keeps its
// vanilla pause).  Gated on an active DC anchor so we never touch a non-DC frame.
static bool invMoveThroughEligible()
{
    return s_mode == MODE_FREE_MOVE
        && s_freeMoveAnchor
        && !s_settingInventoryFaceCam            // opt-in
        && gui && gui->isAnyInventoryWindowOpen()
        && !gui->inDialogue()                    // dialogue still pauses everything
        && !gui->isCharacterEditorMode();
}

// =======================================================================
// First-person camera — functions (ported from the FPS prototype 2026-07-22).
// Shares the detached-camera machinery (s_fpNode, saved cam locals) with the
// inventory face-cam; s_firstPersonActive is the drive-mode selector.
// =======================================================================

// fpSetHeadBoneHidden — modern-FPS head hide: shrink the head bone to near zero
// (manually controlled so animation cannot rescale it).  The face and anything
// mounted to the head bone collapse invisibly; the neck and body keep animating.
static void fpSetHeadBoneHidden(bool hide)
{
    if (hide == s_fpHeadBoneHidden) return;
    if (!s_freeMoveAnchor) { s_fpHeadBoneHidden = false; return; }
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (!sk) { s_fpHeadBoneHidden = false; return; }
    static const char* const HEAD_NAMES[] = { "Bip01 Head", "Head" };
    Ogre::OldBone* hb = nullptr;
    for (int i = 0; i < 2 && !hb; ++i)
        if (sk->hasBone(HEAD_NAMES[i]))
            hb = sk->getBone(HEAD_NAMES[i]);
    if (!hb) { s_fpHeadBoneHidden = false; return; }
    if (hide)
    {
        hb->setManuallyControlled(true);
        hb->setScale(Ogre::Vector3(0.001f, 0.001f, 0.001f));
        s_fpHeadBoneHidden = true;
        DebugLog("[WASDCombat] dc_fp_head_hidden");
    }
    else
    {
        hb->setScale(Ogre::Vector3(1.0f, 1.0f, 1.0f));
        hb->setManuallyControlled(false);
        s_fpHeadBoneHidden = false;
        DebugLog("[WASDCombat] dc_fp_head_restored");
    }
}

// fpGetHeadWorld — world-space position of the anchor's animated head/neck bone.
// CRITICAL: the skeleton MUST come from AppearanceBase::getSkeleton() (the game's
// own getter).  Entity::getSkeleton() on the body entity returns a different
// runtime type whose virtual calls CRASH (the v1.8.9 P-toggle crash).  The
// bone's derived position is clean MODEL space relative to the character root;
// the entity node's transform is stale/wrong-space, so world head = logic-space
// root + bone offset rotated by the character's facing yaw.
static bool fpGetHeadWorld(Ogre::Vector3& out)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return false;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return false;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return false;

    CharMovement* mvB   = s_freeMoveAnchor->movement;
    Ogre::Vector3 root  = mvB->pos;

    // Resolve an EXISTING mount bone.  hasBone (safe on the AppearanceBase
    // skeleton — the only skeleton whose virtual calls don't crash, per the
    // v1.8.9 lesson) both validates the skeleton and picks a name that exists,
    // so the getBoneWorldPosition call below can't hit a missing bone.
    //  * TRUE-world path: prefer the HEAD bone (KenshiFP mounts there); its world
    //    origin is unaffected by the scale-hide, and eye level is a fixed drop
    //    below it.
    //  * legacy synthetic path: base-of-neck first — it keeps animating when the
    //    head is scale-hidden, and the eye sits higher above a neck mount.
    static const char* const BONE_TRUE[]  = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    static const char* const BONE_SYNTH[] = { "Bip01 Neck", "Bip01 Head", "Bip01 Neck1", "Head" };
    static const float        BONE_SYNTH_UP[] = { 2.4f, 1.0f, 2.4f, 1.0f };
    const char* const* names = s_fpTrueBoneEye ? BONE_TRUE : BONE_SYNTH;
    const char* used = nullptr;
    int usedIdx = -1;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(names[i])) { used = names[i]; usedIdx = i; }
    if (!used) return false;

    if (s_fpTrueBoneEye)
    {
        // KenshiFP method: the game's own Character::getBoneWorldPosition composes
        // the full skeleton + entity transform and returns the head bone's TRUE
        // world position — tracking every animation (bob, run-lean, turn) with NO
        // synthetic reconstruction and NO dependence on the view yaw, so a pure
        // pan no longer swings the eye on an arc (the old root+rotate(offset,yaw)
        // formula did).  Same coordinate space as mvB->pos (both game-world), which
        // is the space our root-child s_fpNode consumes.
        Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
        float ddx = head.x - root.x, ddy = head.y - root.y, ddz = head.z - root.z;
        bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f
                      && head.y > root.y - 1.0f;   // head sits above the feet
        if (plausible)
        {
            out           = head;
            s_fpBoneEyeUp = 0.0f;   // eye level handled by EyeDrop in the true-eye path
            if (!s_fpBoneLogged)
            {
                s_fpBoneLogged = true;
                char bbuf[256];
                sprintf_s(bbuf, sizeof(bbuf),
                    "[WASDCombat] dc_fp_truebone bone=%s world=(%.1f,%.1f,%.1f)"
                    " root=(%.1f,%.1f,%.1f)",
                    used, head.x, head.y, head.z, root.x, root.y, root.z);
                DebugLog(bbuf);
            }
            return true;
        }
        // implausible (skeleton mid-load / odd rig) -> fall through to synthetic
    }

    // Legacy synthetic path: root + model-space bone offset rotated by the VIEW yaw.
    Ogre::OldBone* b = sk->getBone(used);
    if (!b) return false;
    s_fpBoneEyeUp = (strstr(used, "Neck") != nullptr) ? 2.4f
                  : (usedIdx >= 0 && !s_fpTrueBoneEye ? BONE_SYNTH_UP[usedIdx] : 1.0f);
    Ogre::Vector3 boneModel = b->_getDerivedPosition();
    float mountYaw = s_fpYawSm;   // render-smoothed view yaw (== s_fpYaw when LookSmooth=0)
    Ogre::Quaternion qBody(Ogre::Radian(mountYaw), Ogre::Vector3::UNIT_Y);
    out = root + qBody * boneModel;

    float ddx = out.x - root.x, ddy = out.y - root.y, ddz = out.z - root.z;
    bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f;
    if (!s_fpBoneLogged)
    {
        s_fpBoneLogged = true;
        char bbuf[256];
        sprintf_s(bbuf, sizeof(bbuf),
            "[WASDCombat] dc_fp_headbone_tracking bone=%s plausible=%d"
            " boneModel=(%.1f,%.1f,%.1f) mountYaw=%.2f world=(%.1f,%.1f,%.1f)",
            used, (int)plausible,
            boneModel.x, boneModel.y, boneModel.z,
            mountYaw, out.x, out.y, out.z);
        DebugLog(bbuf);
    }
    return plausible;
}

// fpShowCrosshair — small sand-colored "+" pinned to the viewport center (the
// point the captured cursor sits on, so LMB/RMB act on whatever it covers).
// Created lazily; pointer nulled on load teardown (recreated against fresh GUI).
static void fpShowCrosshair(bool show)
{
    if (show && !s_fpCrosshair)
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return;
        const MyGUI::IntSize vs = MyGUI::RenderManager::getInstance().getViewSize();
        s_fpCrosshair = g->createWidget<MyGUI::TextBox>("TextBox",
            MyGUI::IntCoord(vs.width / 2 - 16, vs.height / 2 - 16, 32, 32),
            MyGUI::Align::Default, "Pointer");
        if (!s_fpCrosshair) return;
        s_fpCrosshair->setNeedMouseFocus(false);
        s_fpCrosshair->setTextAlign(MyGUI::Align::Center);
        s_fpCrosshair->setCaption("+");
        s_fpCrosshair->setTextColour(MyGUI::Colour(0.83f, 0.76f, 0.60f, 0.95f));
        s_fpCrosshair->setTextShadow(true);
    }
    if (s_fpCrosshair)
        s_fpCrosshair->setVisible(show);
}

// While the RMB hold-menu is open in first-person, tint the hovered option
// yellow-green and the rest parchment.  ContextMenuGUI::optionsList is at 0xF8
// (the class is forward-declared, so the member is read by documented offset).
static const MyGUI::Colour FP_MENU_ACCENT(0.72f, 0.86f, 0.38f, 1.0f);  // yellow-green
static const MyGUI::Colour FP_MENU_NORMAL(0.78f, 0.75f, 0.66f, 1.0f);  // parchment
static void fpTintContextMenu()
{
    if (!ou || !ou->player) return;
    ContextMenu& cm = ou->player->contextMenu;
    if (!cm.isVisible()) return;

    MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
    ContextMenuGUI* menus[2] = { cm.menuGUI, cm.menuGUI2 };
    for (int m = 0; m < 2; ++m)
    {
        if (!menus[m]) continue;
        MyGUI::Widget* list =
            *(MyGUI::Widget**)((char*)menus[m] + 0xF8);  // ContextMenuGUI::optionsList
        if (!list) continue;
        size_t n = list->getChildCount();
        for (size_t i = 0; i < n; ++i)
        {
            MyGUI::Widget* c = list->getChildAt(i);
            if (!c) continue;
            MyGUI::TextBox* tb = c->castType<MyGUI::TextBox>(false);
            if (!tb) continue;
            bool hovered = (focus == c);
            if (!hovered && focus)
                for (MyGUI::Widget* p = focus->getParent(); p; p = p->getParent())
                    if (p == c) { hovered = true; break; }
            tb->setTextColour(hovered ? FP_MENU_ACCENT : FP_MENU_NORMAL);
        }
    }
}

// exitFirstPerson — leave first-person: restore head/hair/crosshair, then hand
// the camera back to the game's rig via the shared otsRestoreCameraToRig helper.
static void exitFirstPerson(bool restoreCamera)
{
    if (!s_firstPersonActive) return;
    s_firstPersonActive = false;
    s_fpCursorCaptured  = false;
    // Restore the exact grass/foliage draw-range the boost overrode on enter.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
        DebugLog("[WASDCombat] dc_fp_grass_range_restored");
    }
    fpShowCrosshair(false);
    fpSetHeadBoneHidden(false);
    if (s_fpHairHidden)
    {
        s_fpHairHidden = false;
        if (s_freeMoveAnchor)
        {
            AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
            if (ap) { ap->shaveHead(false); DebugLog("[WASDCombat] dc_fp_hair_restored"); }
        }
    }
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    DebugLog("[WASDCombat] dc_fp_exited");
}

// enterFirstPerson — detach the camera onto our root node and open the view at
// the anchor's eye.  Guards against the inventory face-cam already owning the
// camera (s_fpActive).  Mirrors enterOTS but sets FP FOV/near-clip and hides
// the head/hair.  Camera calls MUST run on the game thread (consumed in mainLoop).
static void enterFirstPerson()
{
    if (s_firstPersonActive || s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;

    // Live re-tune: re-read [FirstPerson] from the INI on every entry so the
    // player can edit FOV / ForwardOffset / EyeUpAdjust / NearClip, toggle P
    // off then on, and see the new camera immediately — no game relaunch.
    {
        char cfgPath[MAX_PATH];
        getConfigPath(cfgPath, sizeof(cfgPath));
        loadFirstPersonConfig(cfgPath);
    }

    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    cam->stopFollowing();   // zoom left untouched — restored view = pre-FP view
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
    oc->setNearClipDistance(s_fpNearClipFP);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);

    // Open the view centered on the character's current facing.
    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f;
    float dlen = d.length();
    if (dlen > 0.001f)
    {
        d /= dlen;
        s_fpYaw = atan2f(-d.x, -d.z);   // forward = (-sin yaw, 0, -cos yaw)
    }
    s_fpPitch = 0.0f;

    if (s_fpHideHair)
    {
        AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
        if (ap) { ap->shaveHead(true); s_fpHairHidden = true; DebugLog("[WASDCombat] dc_fp_hair_hidden"); }
    }
    // Grass/foliage draw-range boost: Kenshi builds grass to a range tuned for the
    // high top-down camera, so at ground level grass only exists in a short ring
    // that pops at its edge as you turn/move.  While FP owns the view, widen the
    // range so grass is already present before it rotates into frame.  Save the
    // exact originals once (guarded) and restore on exit.  Mult 1.0 = vanilla.
    if (options && s_fpGrassRangeMult > 1.0f && !s_fpOptRangeSaved)
    {
        s_fpSavedGrassRange   = options->grassRange;
        s_fpSavedFoliageRange = options->foliageRange;
        s_fpOptRangeSaved     = true;
        options->grassRange   *= s_fpGrassRangeMult;
        options->foliageRange *= s_fpGrassRangeMult;
        DebugLog("[WASDCombat] dc_fp_grass_range_boosted");
    }
    s_fpLeanFwd         = 0.0f;
    s_fpMoveLeanSmooth  = 0.0f;
    s_fpGaitFwdSmooth   = 0.0f;
    s_fpActionClrSmooth = 0.0f;
    s_fpHaveLastFeet    = false;   // true-bone-eye feet-delta speed tracker
    s_fpMoveSpeed       = 0.0f;
    s_fpMoveFwdSmooth   = 0.0f;
    s_fpFeetTickMs      = 0;
    s_fpSmValid         = false;    // re-seed the render-smoothed view on first frame
    s_fpEnemyClearSmooth = 1.0f;    // enemy-clearance pullback starts released
    s_fpEnemyNearestDist = -1.0f;
    s_fpLastStreamValid = false;    // force a streaming teleport on the first FP frame
    s_fpFrozenValid     = false;    // FreezeCamTest re-captures the frozen eye next standstill
    s_fpBodyYaw         = s_fpYaw;   // body starts aligned with the opening view
    s_fpHeadSmoothValid = false;
    s_fpBoneLogged      = false;
    s_firstPersonActive = true;
    s_fpCursorCaptured  = false;   // first capture pass establishes the center
    if (s_fpHideHead)
        fpSetHeadBoneHidden(true);
    fpShowCrosshair(true);
    DebugLog("[WASDCombat] dc_fp_entered");
}

// fpDriveFrame — the per-frame first-person drive, called from cameraUpdate_hook
// AFTER the game's camera update.  Mouse-look (cursor recentered on the viewport
// center), neck-limit (turn the body when the view exceeds its facing while
// idle), then place the detached node at the head-bone eye looking along the view.
static void fpDriveFrame(CameraClass* thisptr, bool uiOpen)
{
    // Auto-exit: first-person only exists inside DC with a live anchor + no loot UI.
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || !s_freeMoveAnchor->movement || s_lootUiSuspendActive)
    {
        exitFirstPerson(true);
        return;
    }

    // When any UI is up (world map, dialogue, pause, any menu — uiOpen), release
    // the mouse: stop recentering it and hide the crosshair so the free OS cursor
    // can click menu items.  Capture (and the crosshair) resume when the UI closes.
    fpShowCrosshair(!uiOpen);

    // Mouse-look — paused while a menu needs the cursor, AND while the RMB
    // hold-menu is up: the cursor (= crosshair point) is freed so the player can
    // browse the context-menu options; releasing RMB selects.
    bool rmbHeld    = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool ctxVisible = ou->player->contextMenu.isVisible();
    if (rmbHeld || ctxVisible)
        fpTintContextMenu();
    bool captureOk = !uiOpen && !s_menuSuspendActive && !rmbHeld && !ctxVisible
                  && !s_lootUiSuspendActive && isKenshiForegroundMain();
    if (captureOk)
    {
        // RawMouse: ensure the 1kHz DirectInput look device is running.  Its poll
        // thread accumulates hardware deltas off the frame loop; we consume them
        // below.  Falls back to cursor-warp until the device is acquired.
        bool useRaw = s_fpRawMouse;
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }

        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);

            float dx = 0.0f, dy = 0.0f;
            bool  haveDelta = false;
            if (useRaw && s_diReady)
            {
                // Framerate-independent 1kHz DirectInput deltas (raw hardware).
                fpTakeMouseAccum(&dx, &dy);
                haveDelta = s_fpCursorCaptured;    // skip the baseline frame
                SetCursorPos(center.x, center.y);  // keep the crosshair/click point centered
                s_fpCursorCaptured = true;
            }
            else
            {
                // Cursor-warp fallback: RawMouse off, or DI not yet acquired.
                POINT cur;
                if (GetCursorPos(&cur))
                {
                    if (s_fpCursorCaptured)
                    {
                        dx = (float)(cur.x - center.x);
                        dy = (float)(cur.y - center.y);
                        haveDelta = true;
                    }
                    SetCursorPos(center.x, center.y);
                    s_fpCursorCaptured = true;
                }
            }

            if (haveDelta && (dx != 0.0f || dy != 0.0f))
            {
                s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                // Mark active mouse-look (deadzone to ignore 1px jitter) so the
                // point-click FollowTurn yields — it only recentres after the mouse
                // has been still for FollowDelayMs.
                if (dx > 1.0f || dx < -1.0f || dy > 1.0f || dy < -1.0f)
                    s_fpLastMouseMoveMs = GetTickCount64();
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;  // re-baseline when capture resumes
        // Drain deltas accumulated while a UI owns the cursor so the view doesn't
        // jump when capture resumes.
        if (s_fpRawMouse) { float jx, jy; fpTakeMouseAccum(&jx, &jy); }
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    // "Actually moving" — true for WASD, point-click, AND autonomous combat/heal
    // approach movement.  currentlyMoving/currentSpeed are set by the game
    // regardless of what issued the move, so the camera can follow + compensate in
    // all of those cases, not just WASD.
    bool fpMoving = mvFP->currentlyMoving || mvFP->currentSpeed > 0.25f;

    // Body facing.
    //  * CAMERA-PRIORITY (WASD, plus a short grace after release while still
    //    moving): FORCE the body to face the view direction, so movement is strafe-
    //    relative — W walks forward, S backpedals, A/D sidestep — and the body
    //    never rotates away from the camera.  The camera yaw is NEVER touched here,
    //    so it cannot jolt.  The grace is the fix for the backpedal twitch: on the
    //    release/coast frame of an S-walk the character is briefly still facing 180°
    //    from the view, and the old idle neck-limit would SNAP the camera onto that
    //    body direction — a violent jolt.  Holding camera-priority through the coast
    //    keeps refacing the body to the view instead, so by the time we fall to idle
    //    the body is already aligned and nothing snaps.  Motion is unaffected:
    //    setDirectMovement got the world WASD vector directly, so direction here is
    //    facing only, and this hook writes late enough to survive to the frame.
    //  * point-click / autonomous move: camera follows the heading (yields to mouse).
    //  * Idle: neck-limit — turn the body toward the view if you look too far.
    bool strafeGrace = (GetTickCount64() - s_wasdLastHeldMs) < FP_STRAFE_GRACE_MS;
    if (s_frameWasdHeld || (fpMoving && strafeGrace))
    {
        // Smoothly rotate the VISIBLE body toward the view (shortest angle) rather
        // than snapping.  The eye is mounted on the view yaw (fpGetHeadWorld), so
        // this is model-only — the camera never jumps; the character just turns to
        // face the way you look over a few frames as you start moving.
        float dyaw = s_fpYaw - s_fpBodyYaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        s_fpBodyYaw += dyaw * FP_BODY_TURN;
        while (s_fpBodyYaw >  3.14159265f) s_fpBodyYaw -= 6.28318531f;
        while (s_fpBodyYaw < -3.14159265f) s_fpBodyYaw += 6.28318531f;
        mvFP->direction =
            Ogre::Vector3(-sinf(s_fpBodyYaw), 0.0f, -cosf(s_fpBodyYaw));
    }
    else if (fpMoving && s_fpFollowTurn > 0.0f
             && (GetTickCount64() - s_fpLastMouseMoveMs) > (ULONGLONG)s_fpFollowDelayMs)
    {
        // Point-click / combat / heal-approach movement: the CHARACTER leads (walks
        // its own path), so make the CAMERA follow — gently lerp the view yaw toward
        // the body's movement heading so you look where you're going, the mirror of
        // what WASD does.  We do NOT write mvFP->direction here (the game's pathing
        // owns it); we only turn the view.  CRUCIAL: this only runs after the mouse
        // has been STILL for FollowDelayMs — so while you are actively looking around
        // the follow stays out of the way (no fighting your mouse), then eases the
        // view back onto the path once you let go (field 2026-07-25).
        Ogre::Vector3 hd = mvFP->direction;
        hd.y = 0.0f;
        float hlen = hd.length();
        if (hlen > 0.001f)
        {
            hd /= hlen;
            float headYaw = atan2f(-hd.x, -hd.z);
            float d = headYaw - s_fpYaw;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            s_fpYaw += d * s_fpFollowTurn;
            while (s_fpYaw >  3.14159265f) s_fpYaw -= 6.28318531f;
            while (s_fpYaw < -3.14159265f) s_fpYaw += 6.28318531f;
            s_fpBodyYaw = headYaw;   // keep body-yaw synced so a later stop won't snap
        }
    }
    else
    {
        Ogre::Vector3 bd = mvFP->direction;
        bd.y = 0.0f;
        float blen = bd.length();
        if (blen > 0.001f)
        {
            bd /= blen;
            float bodyYaw = atan2f(-bd.x, -bd.z);
            float delta   = s_fpYaw - bodyYaw;
            while (delta >  3.14159265f) delta -= 6.28318531f;
            while (delta < -3.14159265f) delta += 6.28318531f;
            if (delta > s_fpNeckLimitRad || delta < -s_fpNeckLimitRad)
            {
                mvFP->direction =
                    Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
                s_fpYaw = bodyYaw
                        + (delta > 0.0f ?  s_fpNeckLimitRad
                                        : -s_fpNeckLimitRad);
                bodyYaw = s_fpYaw;
            }
            // Keep the smoothed body yaw synced to the resting facing so the next
            // movement turn lerps from where the body actually is (no initial jump).
            s_fpBodyYaw = bodyYaw;
        }
    }

    // LookSmooth: derive a render-smoothed view from the authoritative s_fpYaw/
    // s_fpPitch (which all the control logic above wrote).  Only the VISUAL — the
    // orientation quaternion, the head-bone eye mount, and the forward vectors below
    // — uses the smoothed values; body-facing/motion already used the raw target, so
    // the character still turns crisply.  Smaller per-frame rotation delta shrinks
    // the render-thread grass re-facing mismatch → less side-to-side foliage flicker.
    // LookSmooth=0 → the smoothed value equals the raw value exactly (no change).
    if (!s_fpSmValid) { s_fpYawSm = s_fpYaw; s_fpPitchSm = s_fpPitch; s_fpSmValid = true; }
    {
        float a = 1.0f - s_fpLookSmooth;   // 1.0 = snap (off), <1 = glide
        float dyawS = s_fpYaw - s_fpYawSm;
        while (dyawS >  3.14159265f) dyawS -= 6.28318531f;
        while (dyawS < -3.14159265f) dyawS += 6.28318531f;
        s_fpYawSm += dyawS * a;
        while (s_fpYawSm >  3.14159265f) s_fpYawSm -= 6.28318531f;
        while (s_fpYawSm < -3.14159265f) s_fpYawSm += 6.28318531f;
        s_fpPitchSm += (s_fpPitch - s_fpPitchSm) * a;
    }

    Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Radian(s_fpYawSm),   Ogre::Vector3::UNIT_Y) *
        Ogre::Quaternion(Ogre::Radian(s_fpPitchSm), Ogre::Vector3::UNIT_X);

    // Enemy body-clip clearance: nearest live hostile distance was sampled this
    // frame in mainLoop (-1 = none in range).  Map it to a 0..1 offset scale —
    // 1 at/beyond EnemyClearRadius, EnemyClearMinScale at contact — and smooth
    // it so the eye eases back rather than snapping.  Both eye paths multiply
    // their forward offsets by it, so an aggressor pressing into the lens pulls
    // the eye back to the (hidden) skull instead of poking inside their model.
    {
        float enemyScale = 1.0f;
        if (s_fpEnemyClearRadius > 0.0f && s_fpEnemyNearestDist >= 0.0f
            && s_fpEnemyNearestDist < s_fpEnemyClearRadius)
        {
            float t = s_fpEnemyNearestDist / s_fpEnemyClearRadius;
            enemyScale = s_fpEnemyClearMinScale
                       + t * (1.0f - s_fpEnemyClearMinScale);
        }
        s_fpEnemyClearSmooth += (enemyScale - s_fpEnemyClearSmooth) * 0.15f;
    }

    // Eye position — primary: the ANIMATED head bone, so the camera rides the
    // neck through every pose.  Smoothed on height to damp stride bob; hard-
    // attached horizontally so a sprinting model can't outrun the camera.
    Ogre::Vector3 eye;
    Ogre::Vector3 headWorld;
    if (fpGetHeadWorld(headWorld))
    {
      if (s_fpTrueBoneEye)
      {
        // KenshiFP weld: eye = the head bone's REAL world position, Y taken RAW
        // (welded to head height — no bob smoothing/lag), plus a HORIZONTAL forward
        // push (along yaw only, so looking down does not sink the eye into the
        // chest) so the eye sits at the face rather than inside the skull.  Because
        // headWorld comes from getBoneWorldPosition and is view-independent, a pure
        // pan no longer moves the eye — the arc-swing (and the grass re-page it
        // fed) is gone.
        // Feet-delta ground speed (framerate-independent, low-passed) drives the
        // forward LEAD so the eye leads faster movement instead of trailing the
        // leaning head.  Keyed off ON-SCREEN speed, not the noisy currentMotion
        // magnitude our notes found unreliable (2026-07-24).
        {
            ULONGLONG nowF = GetTickCount64();
            float dt = (s_fpFeetTickMs > 0) ? (float)(nowF - s_fpFeetTickMs) * 0.001f : 0.0f;
            s_fpFeetTickMs = nowF;
            if (dt > 0.001f && dt < 0.25f && s_fpHaveLastFeet)
            {
                float dfx = mvFP->pos.x - s_fpLastFeetX;
                float dfz = mvFP->pos.z - s_fpLastFeetZ;
                float inst = sqrtf(dfx*dfx + dfz*dfz) / dt;
                if (inst > 400.0f) inst = 400.0f;   // reject teleport/paging jumps
                s_fpMoveSpeed += (inst - s_fpMoveSpeed) * 0.20f;
                // Vertical speed for the stair-climb pullback (+ = ascending).
                float vy = (mvFP->pos.y - s_fpLastFeetY) / dt;
                if (vy >  60.0f) vy =  60.0f;        // reject teleport/paging jumps
                else if (vy < -60.0f) vy = -60.0f;
                s_fpClimbSpeedSmooth += (vy - s_fpClimbSpeedSmooth) * 0.20f;
            }
            s_fpLastFeetX = mvFP->pos.x; s_fpLastFeetZ = mvFP->pos.z;
            s_fpLastFeetY = mvFP->pos.y; s_fpHaveLastFeet = true;
        }
        float lead = 0.0f;
        if (s_fpMoveForward > 0.0f && s_fpMoveSpeedRef > 1.0f)
        {
            float gait = s_fpMoveSpeed / s_fpMoveSpeedRef;   // 0 idle .. ~1 run
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            lead = s_fpMoveForward * gait;
        }
        s_fpMoveFwdSmooth += (lead - s_fpMoveFwdSmooth) * 0.15f;

        // Ascent-aware forward pullback: turn the low-passed climb speed into a
        // 0..1 ramp, shrink the forward push toward StairForwardMinScale, and lift
        // the eye by StairEyeLift so the camera clears the rising steps instead of
        // jamming into them.  Inert on flat ground (ascent01 == 0 -> scale 1, lift 0),
        // so open-ground framing and body-clip protection are unchanged.
        float ascent01 = (s_fpClimbSpeedSmooth > 0.0f)
                       ? s_fpClimbSpeedSmooth * s_fpStairForwardReduce : 0.0f;
        if (ascent01 > 1.0f) ascent01 = 1.0f;
        float ascentScale = 1.0f - ascent01 * (1.0f - s_fpStairForwardMinScale);
        float stairLift   = s_fpStairEyeLift * ascent01;

        // Keep the shared 0..1 speed factor alive in this path too — it drives
        // the MoveNearClip blend below.  It previously only updated in the legacy
        // synthetic-eye branch, which left MoveNearClip dead under TrueBoneEye
        // (the default) — found in the 2026-08-01 anti-clip audit.
        {
            float leanTarget = (s_fpMoveSpeedRef > 1.0f)
                             ? s_fpMoveSpeed / s_fpMoveSpeedRef : 0.0f;
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }

        float fwdScale = ascentScale * s_fpEnemyClearSmooth;
        if (g_log.debugLogging)
        {
            static ULONGLONG t = 0; ULONGLONG n = GetTickCount64();
            if (n - t >= 500) { t = n; char b[112];
                sprintf_s(b, sizeof(b),
                    "[WASDCombat] dc_fp_stair climb=%.2f scale=%.2f lift=%.2f",
                    s_fpClimbSpeedSmooth, ascentScale, stairLift);
                DebugLog(b); }
        }

        eye    = headWorld;
        eye.y += -s_fpEyeDrop + s_fpEyeUpAdjust + stairLift;
        Ogre::Vector3 fwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
        eye += fwd * ((s_fpFwdOffset + s_fpMoveFwdSmooth) * fwdScale);

        // Optional additive clearances (default 0 = inert) kept from our tuning:
        // gait-forward while jog/sprinting, and committed-action clearance.  They
        // only engage if the user opts in via the INI; both push along the view.
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed g = mvFP->speedOrders;
                if (g == JOG) gaitTarget = s_fpJogForward;
                else if (g == RUN || g == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f) eye += fwd * (s_fpGaitFwdSmooth * fwdScale);
        }
        {
            bool actionNow = s_fpActionClearFwd > 0.0f && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f) eye += fwd * (s_fpActionClrSmooth * fwdScale);
        }
      }
      else
      {
        if (!s_fpHeadSmoothValid)
        {
            s_fpHeadSmooth      = headWorld;
            s_fpHeadSmoothValid = true;
        }
        else
        {
            s_fpHeadSmooth.x  = headWorld.x;
            s_fpHeadSmooth.z  = headWorld.z;
            s_fpHeadSmooth.y += (headWorld.y - s_fpHeadSmooth.y) * FP_BONE_SMOOTH;
        }
        // Speed-scaled lean compensation: at jog/sprint the model pitches
        // forward and swings the arms/chest up into view.  Ramp a smoothed 0..1
        // speed factor and add extra eye height + forward reach so the camera
        // rises above and past the leaning torso.  Zero when standing/walking,
        // so the natural upright view is untouched.  Magnitudes are INI-tuned.
        {
            float spd = mvFP->currentMotion.length();
            float leanTarget = spd * 0.04f;          // ~1.0 by jog speed
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }
        float leanUp  = s_fpMoveLeanUp  * s_fpMoveLeanSmooth;
        float leanFwd = s_fpMoveLeanFwd * s_fpMoveLeanSmooth;
        eye = s_fpHeadSmooth
            + q * Ogre::Vector3(0.0f, s_fpBoneEyeUp + s_fpEyeUpAdjust + leanUp,
                                -((s_fpFwdOffset + leanFwd) * s_fpEnemyClearSmooth));

        // One-frame look-ahead — ONLY along the view forward.  The bone pose read
        // this frame is the previous frame's animation result; at sprint speed that
        // leaves the camera a stride behind, so feed forward velocity ahead by the
        // frame time.  Projected onto the view forward (positive only) so strafing
        // (A/D) and backpedalling (S) never shove the eye sideways or backward —
        // that lateral/back offset was the strafe "twitch".
        {
            static ULONGLONG s_fpLastTickMs = 0;
            ULONGLONG nowFF = GetTickCount64();
            float dt = (s_fpLastTickMs > 0)
                     ? (float)(nowFF - s_fpLastTickMs) * 0.001f : 0.0f;
            s_fpLastTickMs = nowFF;
            if (dt > 0.05f) dt = 0.05f;
            Ogre::Vector3 vel = mvFP->currentMotion;
            vel.y = 0.0f;
            Ogre::Vector3 viewFwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
            float fwdComp = vel.dotProduct(viewFwd);
            if (fwdComp > 0.0f)
                eye += viewFwd * (fwdComp * dt);
        }

        // Gait-based forward compensation (the reliable jog/sprint fix).  Keyed
        // off the discrete gait tier (speedOrders) — NOT the noisy currentMotion
        // magnitude — and gated on ACTUAL movement (fpMoving) so it also fires for
        // point-click / combat / heal-approach running, not just WASD (that's when
        // the body was clipping through the lens).  While jogging/running the model
        // leans forward and opens a gap; push the eye forward ALONG THE VIEW — during
        // autonomous movement the view now follows the heading (FollowTurn), so the
        // push lands along the direction of travel.  WALK => 0 (walking untouched).
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed gait = mvFP->speedOrders;
                if (gait == JOG)      gaitTarget = s_fpJogForward;
                // RUN = solo sprint; GROUPED = squad-follow speed (group-sprint):
                // treat both as sprint so group movement gets the same fix.
                else if (gait == RUN || gait == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpGaitFwdSmooth * s_fpEnemyClearSmooth);
            }
        }

        // Committed-action body clearance: during attack swings, blocks, heals,
        // revives and get-ups the arms/torso/head swing hard toward the head bone
        // and clip through the lens even while standing (so the gait push above,
        // which needs movement, can't help).  When such an action is active, push
        // the eye forward along the view to sit clear of the swinging body.  Smoothed
        // so it eases in/out.  0 (default) = off — set ActionClearForward to enable.
        {
            bool actionNow = s_fpActionClearFwd > 0.0f
                          && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpActionClrSmooth * s_fpEnemyClearSmooth);
            }
        }
      }   // end legacy synthetic-eye branch
    }
    else
    {
        // Fallback: root-relative neck model + speed lean compensation.
        float speed   = mvFP->currentMotion.length();
        float target  = speed * 0.04f;
        if (target > 2.5f) target = 2.5f;
        s_fpLeanFwd  += (target - s_fpLeanFwd) * 0.15f;

        Ogre::Vector3 neck = mvFP->pos;
        neck.y += (s_fpEyeHeight - FP_HEAD_LEN);
        eye = neck
            + q * Ogre::Vector3(0.0f, FP_HEAD_LEN,
                                -(s_fpFwdOffset + s_fpLeanFwd));
    }

    if (s_fpNode)
    {
        Ogre::Vector3 camPos = eye;
        // FreezeCamTest (diagnostic): while standing still, LOCK the camera position to the
        // first standstill eye so a pure pan changes only orientation — nothing moves
        // positionally.  If distant grass re-scatter stops here, PagedGeometry is reading
        // the (rotation-swinging) camera position and we engineer a stabilized-pager fix.
        if (s_fpFreezeCamTest && !fpMoving)
        {
            if (!s_fpFrozenValid) { s_fpFrozenEye = eye; s_fpFrozenValid = true; }
            camPos = s_fpFrozenEye;
        }
        else s_fpFrozenValid = false;   // moving (or test off) → release, re-capture next stop
        s_fpNode->setPosition(camPos);
        s_fpNode->setOrientation(q);
    }

    // Near-clip management, applied every frame from the base value:
    //  * MoveNearClip — while moving fast, push the near plane OUT to slice away
    //    the arm/torso that the jog/sprint animation sweeps into the lens (blended
    //    by the 0..1 speed factor; snaps back at a stand so the close-up chest
    //    view is untouched).  Off when <= the base near-clip.
    //  * EnemyNearClip — while a hostile overlaps the lens, pull the near plane
    //    IN toward this value so whatever body part still crosses the plane
    //    slices the thinnest possible cross-section instead of opening a big
    //    see-through hole in the aggressor.  Wins over MoveNearClip (takes the
    //    minimum) because an enemy in your face matters more than your own arms.
    if (thisptr->camera)
    {
        float nc = s_fpNearClipFP;
        if (s_fpMoveNearClip > s_fpNearClipFP)
            nc += (s_fpMoveNearClip - s_fpNearClipFP) * s_fpMoveLeanSmooth;
        if (s_fpEnemyNearClip > 0.0f && s_fpEnemyNearClip < nc
            && s_fpEnemyClearMinScale < 1.0f)
        {
            float overlap01 = (1.0f - s_fpEnemyClearSmooth)
                            / (1.0f - s_fpEnemyClearMinScale);
            if (overlap01 < 0.0f) overlap01 = 0.0f;
            if (overlap01 > 1.0f) overlap01 = 1.0f;
            nc += (s_fpEnemyNearClip - nc) * overlap01;
        }
        thisptr->camera->setNearClipDistance(nc);
    }

    // Keep the game-side rig loosely coherent (audio listener, zone/foliage
    // streaming).  teleport() is a JUMP: calling it every frame while the eye
    // moves makes the streamer re-page grass continuously → the foliage flickers
    // in/out while moving (field 2026-07-25).  Throttle it: only re-teleport once
    // the eye has moved StreamUpdateDist metres from the last streamed point, so
    // streaming stays coherent (a couple of metres of lag is invisible to zone
    // paging) without the per-frame thrash.  StreamUpdateDist=0 restores the old
    // every-frame behaviour for A/B testing.
    bool doStream = !s_fpLastStreamValid || s_fpStreamDist <= 0.0f
                  || eye.squaredDistance(s_fpLastStreamPos)
                     >= s_fpStreamDist * s_fpStreamDist;
    if (doStream)
    {
        thisptr->teleport(eye);
        thisptr->targetPositionY = eye.y;
        thisptr->speedY          = 0.0f;
        s_fpLastStreamPos   = eye;
        s_fpLastStreamValid = true;
    }

    // THE grass-flicker fix (2026-07-25): Kenshi's foliage pager streams grass around
    // the camera's CENTER node, not the eye.  In FP the center node lagged at the
    // character's feet while we rendered from the head, so grass paged around the
    // wrong point and popped under foot.  Reconcile them each frame (after teleport,
    // which can reset the center) so the streaming anchor tracks the character.
    //   Anchor to the character ROOT (mvFP->pos), NOT the eye: the eye swings in a
    // small circle when you PAN (it sits ForwardOffset ahead of the head pivot), so
    // anchoring to it moved the streaming center during pure rotation and PagedGeometry
    // RE-SCATTERED the distant grass — the "scatter variation changes as I pan" that
    // read as flicker at speed (field 2026-07-25).  The root position is stable during
    // rotation (only moves when the character actually walks), so panning no longer
    // re-seeds the grass, while streaming still follows the character as they move.
    // Grass-paging fix — technique from linguine2552/KenshiFP (thanks!).  Kenshi's
    // foliage pager keys off the camera CENTER node.  THREE things matter, and our
    // old every-frame local setPosition got all three wrong:
    //  1. Snap ONLY WHILE MOVING.  At idle we leave the center vanilla/untouched —
    //     a stationary center does not move when you pan, so looking around while
    //     standing no longer re-scatters the grass (our old snap to the rotation-
    //     swinging eye was exactly what re-seeded it every frame you turned).
    //  2. Set the WORLD position via _setDerivedPosition (not local setPosition):
    //     the eye lives under our own node, so hand the pager the true world point.
    //  3. FORCE the derived-position recompute (_getDerivedPositionUpdated) so the
    //     SAME frame's paging pass reads the fresh center — a plain set leaves the
    //     derived value the pager reads stale, which was the residual flicker.
    // (Our camera is detached onto s_fpNode, not a child of center, so unlike
    //  KenshiFP we don't have to re-seat a child camera after moving the center.)
    if (s_fpFoliageCenterMode && thisptr->center && s_fpNode && fpMoving)
    {
        Ogre::Vector3 eyeWorld = s_fpNode->_getDerivedPositionUpdated();
        thisptr->center->_setDerivedPosition(eyeWorld);
        thisptr->center->_getDerivedPositionUpdated();
    }
}

// CameraClass::update hook — runs AFTER the game's camera update, BEFORE
// render: this is the only point whose writes survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    // OTS owns the camera: clear the vanilla MMB-rotate state before the
    // game's update sees it, so MMB does nothing while OTS is active.
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;

    // Camera-rotate toggle (user req 2026-06-20): while DC is active and the
    // face-cam doesn't own the camera, force the rotate-intent flag the game
    // reads to TRUE while the toggle is engaged, so CTRL behaves as a TOGGLE
    // (press once to start rotating with the mouse, press again to stop).  Set
    // pre-orig so CameraClass::update sees it this frame.  ONLY force it ON, never
    // OFF — so the vanilla middle-mouse hold-to-rotate still works whenever the
    // toggle is off (user req 2026-06-20).  And never while a UI owns the cursor
    // (loot/trade/inventory via s_lootUiSuspendActive, or NPC dialogue) so the
    // mouse stays free to navigate those menus — the game frees the cursor itself
    // on those paths.  (Suppressing for the pause menu / RMB hold-menu was tried
    // 2026-06-21 but the game does NOT free an already-captured rotate cursor
    // there, so it had no effect and was reverted — toggle CTRL off to free it.)
    // While the toggle is engaged, force the rotate flag the game reads to TRUE so
    // CTRL behaves as a press-on/press-off camera-rotate toggle.  ANY open UI turns
    // the toggle OFF (handled in the player-camera section below), so this never
    // forces rotation while a menu is up — the cursor is free for every UI.  Outside
    // DC / toggle off, the flag is untouched so vanilla MMB hold-rotate still works.
    if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_firstPersonActive && s_camRotateToggle && key)
        key->rotate = true;

    // While the inventory face-cam owns the camera, force controlEnabled=false
    // into the vanilla update — this is Kenshi's OWN gate for "ignore camera
    // input" (it passes false when a UI has focus), so the wheel-zoom and
    // WASD/edge pan never run.  Those were still moving/scaling the world
    // name-tags because the vanilla update lays the tags out from the (just-
    // moved) camera DURING orig, before our post-orig altitude restore could
    // undo it (field 2026-06-17: scrolling/WASD still moved the squad names).
    // We drive the detached camera ourselves via s_fpNode, so we need nothing
    // from orig's input handling here.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // Foliage-sync (field 2026-07-25): drive the FP camera BEFORE orig as well, so
    // Kenshi's per-frame foliage visibility / billboard-facing pass — which samples
    // the camera during its own update — sees THIS frame's view.  Driving only
    // post-orig left the foliage one frame behind the view, so grass blinked on/off
    // across the whole screen while rotating (confirmed by frame-diff of the report
    // video: toggling pixels landed exactly on the grass/shrubs at all distances).
    // The post-orig drive still runs below (re-asserts the pose for render + undoes
    // any clobber by orig); the double-drive is safe because the first call recenters
    // the cursor, so the second reads a ~zero mouse delta.  Player camera + stable
    // scene only.
    if (s_fpCamPreOrig && s_firstPersonActive && ou && ou->player
        && thisptr == ou->player->camera && !ou->isLoadingFromASaveGame())
    {
        ManagementScreen* mgmtPre = ManagementScreen::getSingleton();
        bool uiPre = !controlEnabled || s_lootUiSuspendActive
                   || (mgmtPre && mgmtPre->getVisible())
                   || (gui && (gui->isStatsWindowOpen() || gui->inDialogue()
                               || gui->isPaused() || gui->isAnyInventoryWindowOpen()));
        fpDriveFrame(thisptr, uiPre);
    }

    s_cameraUpdateOrig(thisptr, ctlEnabled);

    // SCENE-FREEING — the Ogre scene is actively being torn down: only `!ou`
    // (world gone) and a save-load in progress qualify.  Touching the camera
    // then reads freed memory (v1.8.0 crash).  Stop driving + mark restore
    // PENDING; keep s_fpNode + saved locals.  Deliberately NOT gated on
    // s_dcShutdownInProgress / s_loadGuardActive: those are mod wait-states
    // that STAY true through char-creation (no player to stabilize on), which
    // would strand the camera detached and hide the new-game character preview
    // (field 2026-06-13).  Once the scene is stable they're safe to ignore.
    bool sceneFreeing = !ou || ou->isLoadingFromASaveGame();
    if (sceneFreeing)
    {
        if (s_fpActive || s_firstPersonActive)
        {
            s_fpActive          = false;
            s_firstPersonActive = false;
            s_fpCursorCaptured  = false;
            s_fpHeadBoneHidden  = false;  // skeleton torn down — restore flags moot
            s_fpHairHidden      = false;
            s_otsRestorePending = true;
            if (s_fpOptRangeSaved && options)   // never leave the range boosted
            {
                options->grassRange   = s_fpSavedGrassRange;
                options->foliageRange = s_fpSavedFoliageRange;
                s_fpOptRangeSaved     = false;
            }
        }
        return;
    }

    // Scene is stable (gameplay OR char-creation/menu).  thisptr is a valid
    // camera even when ou->player is null, so restore via thisptr.
    // (1) Deferred restore from a load/shutdown that hit while OTS was active.
    if (s_otsRestorePending)
    {
        s_otsRestorePending = false;
        s_fpSuspendedForInv = false;   // a load cancels any pending FP auto-return
        otsRestoreCameraToRig(thisptr);
        if (ou && ou->player && s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        otsRestoreNames();   // scene is stable again — safe to re-show names
        DebugLog("[WASDCombat] dc_cam_restored_after_load");
        return;
    }

    // ONLY the player camera drives the inventory face-cam.  Kenshi calls this
    // hook for OTHER cameras too (the inventory portrait render cameras) — if
    // those touched the detached camera it flickered detach/re-attach EVERY
    // frame (field 2026-06-16: dc_cam_entered/exited toggling).  Non-player
    // cameras just return after orig and never touch s_fpActive/the node.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // ANY open UI fully DISABLES the CTRL camera-rotate toggle (user req 2026-06-21
    // — simpler + robust): the moment a menu (dialogue, trade, loot, inventory,
    // pause, anything) is up, turn the toggle OFF and actively release the cursor.
    // The player re-presses CTRL after closing the menu.  `controlEnabled` (the
    // game's own "a UI has focus" flag) is the universal signal; the gui checks are
    // belt-and-suspenders.  s_camRotateUiOpen also gates the poll thread so CTRL+
    // click inside a menu can't re-toggle it.
    // The world MAP and the character STATS window do NOT flip controlEnabled and
    // are not inventory/dialogue/pause — so in FP the cursor stayed pinned to
    // center inside them (field 2026-07-25).  Add them explicitly: the map lives
    // in ManagementScreen (also covers faction/tech/squad tabs), stats via the gui.
    ManagementScreen* mgmt = ManagementScreen::getSingleton();
    bool mgmtOpen = (mgmt && mgmt->getVisible());
    bool statsOpen = (gui && gui->isStatsWindowOpen());
    bool uiOpenNow = !controlEnabled || s_lootUiSuspendActive || mgmtOpen || statsOpen
        || (gui && (gui->inDialogue() || gui->isPaused()
                    || gui->isAnyInventoryWindowOpen()));
    if (uiOpenNow && s_camRotateToggle)
    {
        s_camRotateToggle   = false;
        if (key) key->rotate = false;
        thisptr->isRotating  = false;   // release the cursor capture immediately
    }
    s_camRotateUiOpen = uiOpenNow;

    // Inventory face-cam trigger: DETACH the player camera when the player's OWN
    // inventory opens (exactly one window), re-attach when it closes.  The
    // detach is what lets the camera face the character — the attached RTS
    // camera can only look top-down.  Safe here (sim paused, character standing)
    // unlike the scrapped gameplay OTS.
    {
        // Only TRUE teardown (shutdown / save-load) exits immediately.  Every
        // other eligibility signal — mode, anchor/movement validity AND the
        // own-inventory window count — is treated as FLAPPY and debounced: those
        // all blip for a frame or two when you switch squad members with the
        // inventory open (the window briefly closes+reopens / the anchor's
        // movement ptr churns), and a single-frame dropout used to slam the
        // streak to max → instant detach (field 2026-06-17: a ~0.1s burst of
        // enter/exit + name hide/restore on every squad switch).  Debouncing
        // the soft signals keeps the camera attached across the swap; it then
        // simply re-frames the newly selected character.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Face-cam engages only when enabled in the INI AND the character is NOT
        // in combat — opening inventory mid-fight to loot/disarm an enemy must
        // leave the camera where it is (user req 2026-06-20).  The point-click
        // suppression during inventory is gated separately (s_lootUiSuspendActive),
        // so disabling the face-cam here does NOT let world-clicks move the char.
        // In-combat (with a short grace) is a HARD exit, NOT part of softOK — so a
        // single flicker-false frame can't latch the face-cam through the debounce.
        ULONGLONG nowFC      = GetTickCount64();
        bool inCombatNow     = s_freeMoveAnchor
                            && s_freeMoveAnchor->isInCombatMode(true, true);
        if (inCombatNow) s_lastInCombatMs = nowFC;
        bool inCombatRecent  = inCombatNow
                            || (s_lastInCombatMs > 0
                                && nowFC - s_lastInCombatMs < FACECAM_COMBAT_GRACE_MS);

        bool softOK    = s_settingInventoryFaceCam
                      && s_mode == MODE_FREE_MOVE
                      && s_freeMoveAnchor && s_freeMoveAnchor->movement
                      && isOwnInventoryOpen();

        bool faceCamWanted;
        if (hardStop || inCombatRecent)
        {
            faceCamWanted        = false;
            s_invFaceCloseStreak = INV_FACE_CLOSE_DEBOUNCE;
        }
        else if (softOK)
        {
            faceCamWanted        = true;
            s_invFaceCloseStreak = 0;
        }
        else
        {
            if (s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE)
                s_invFaceCloseStreak++;
            // Keep the face-cam alive across a brief dropout (squad switch, or a
            // single dissenting per-frame camera call); only let it drop once
            // the close signal has persisted for the full debounce.
            faceCamWanted = s_fpActive && s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE;
        }

        // First-person owns the detached camera via s_firstPersonActive (NOT
        // s_fpActive).  If the inventory face-cam now wants the camera, SUSPEND
        // first-person (hand the camera back to the rig) and remember to auto-
        // return when the inventory closes.  If nothing wants the face-cam,
        // first-person simply keeps the camera and the enter/exit below is a
        // no-op (s_fpActive stays false while FP owns the view).
        if (s_firstPersonActive && faceCamWanted)
        {
            exitFirstPerson(true);        // clears s_firstPersonActive, reattaches cam
            s_fpSuspendedForInv = true;
        }

        if (faceCamWanted && !s_fpActive && !s_firstPersonActive)
            enterOTS();
        else if (!faceCamWanted && s_fpActive)
        {
            exitOTS(true);
            if (s_fpSuspendedForInv)
            {
                s_fpSuspendedForInv = false;
                enterFirstPerson();       // inventory closed — auto-return to FP
            }
            return;
        }
    }

    // Robust FP auto-return (belt-and-suspenders).  The primary path above
    // re-enters first-person when the OTS face-cam closes, but a debounce edge or
    // squad-switch churn can clear s_fpActive on a frame where that branch does
    // not fire, stranding the player in top-down DC with the pending return lost.
    // Whenever FP was suspended for the inventory and the inventory is now closed
    // (nothing else owns the camera), re-enter first-person.
    if (s_fpSuspendedForInv && !s_fpActive && !s_firstPersonActive
        && s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive
        && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && !isOwnInventoryOpen())
    {
        s_fpSuspendedForInv = false;
        enterFirstPerson();
        return;
    }

    if (!s_fpActive && !s_firstPersonActive)
        return;

    // First-person drive: place the detached camera at the anchor's head-bone
    // eye and look along the mouse view.  Runs INSTEAD of the face-cam path.
    if (s_firstPersonActive)
    {
        // Interior floor reveal (user 2026-07-31: shinobi thieves-tower floors did
        // NOT load in FP, unlike plain DC; HUD stayed "Floor 0" on an upper storey).
        // The game normally derives the displayed floor from the camera's TRACKED
        // character inside restrictPosition and reveals it — but FP detaches the
        // camera, stops tracking, and runs the update with controlEnabled=false, so
        // restrictPosition never runs and the player's current floor stays 0.  Drive
        // it ourselves: sync the player's current floor to the controlled character's
        // floor, then refresh storey visibility — once per frame, player camera only.
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            int anchorFloor = s_freeMoveAnchor->movement->getCurrentFloor();
            if (anchorFloor != ou->player->getCurrentFloor())
            {
                ou->player->setCurrentFloor(anchorFloor);
                if (g_log.debugLogging)
                {
                    char fbuf[80];
                    sprintf_s(fbuf, sizeof(fbuf),
                        "[WASDCombat] dc_fp_floor_sync floor=%d", anchorFloor);
                    DebugLog(fbuf);
                }
            }
        }
        ou->player->updateFloorVisibility(ou->player->getAllPlayerCharacters());
        // Below-floor reveal (user 2026-08-01): the character-based pass above
        // hides storeys no squad member stands on, so looking DOWN from an upper
        // floor showed black voids where the lower shells should be (and fast
        // pans flickered them as the vanilla update disagreed frame-to-frame).
        // Force the cutaway to "everything up to the anchor's floor" — the same
        // reveal restrictPosition would compute — AFTER that pass so this is the
        // frame's final word.  INI FloorRevealBelow=0 restores the old behaviour
        // (re-read on every P-enter, so it's revertible without a rebuild).
        //
        // Outside-building reveal (user 2026-08-05): vanilla only cuts a building
        // away while the tracked character is INSIDE one, but neither pass here
        // had that gate — with the whole squad outdoors the squad-based pass hid
        // every storey no one stood on and the anchor-floor clamp pinned the rest
        // to floor 0, so owned multi-floor buildings looked cut open from the
        // outside.  When the anchor is not registered in any building, the
        // frame's final word is "no cutaway" instead (identity/null check only —
        // the Building* is never dereferenced or stored).
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            if (s_freeMoveAnchor->movement->building.getBuilding() == nullptr)
                ou->player->resetFloorsVisibility();
            else if (s_fpFloorRevealBelow)
                ou->player->setFloorsVisibility(
                    s_freeMoveAnchor->movement->getCurrentFloor());
        }
        fpDriveFrame(thisptr, uiOpenNow);
        return;
    }

    // Anti scroll-zoom: the wheel still reaches the game's camera zoom inside
    // orig (changing altitude → the world name-tags scale).  Hold the altitude
    // at the value saved on enter so scrolling in the inventory zooms nothing.
    thisptr->altitude = s_otsSavedAltitude;

    // Mouse-look — paused while a UI needs the cursor (context menu visible,
    // pause menu, loot/trade/inventory) or the anchor is KO (free the cursor
    // to pick another squad member).
    bool ctxVisible = ou->player->contextMenu.isVisible();
    bool anchorKO   = s_freeMoveAnchor->isUnconcious();
    // Dialogue / bail-out / conversation UIs need the cursor: free it so the
    // player can click the menu instead of it being pinned to the crosshair
    // (field 2026-06-16, bail-NPC-out menu).  inDialogue covers NPC talk +
    // the prisoner/bail dialogue windows.
    bool inDialogue = (gui && gui->inDialogue());
    bool captureOk  = !s_menuSuspendActive && !ctxVisible
                   && !s_lootUiSuspendActive && !anchorKO && !inDialogue
                   && isKenshiForegroundMain();

    if (captureOk)
    {
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2
                     + (LONG)((rc.right - rc.left) * s_otsCrosshairOffsetX);
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);
            POINT cur;
            if (GetCursorPos(&cur))
            {
                if (s_fpCursorCaptured)
                {
                    float dx = (float)(cur.x - center.x);
                    float dy = (float)(cur.y - center.y);
                    if (dx != 0.0f || dy != 0.0f)
                    {
                        s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                        if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                    }
                }
                SetCursorPos(center.x, center.y);
                s_fpCursorCaptured = true;
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;


    // Own-inventory face-cam (ported from OTS_Project_Shelved, 2026-06-14 for
    // v1.A).  When the player opens their OWN inventory (no trade/loot party on
    // the other side), swing the shoulder camera around to a centered upper-
    // chest shot facing the selected character so worn gear is visible; restore
    // the previous shoulder view when the window closes.  Runs even under the
    // inventory pause (CameraClass::update still ticks) — which is exactly why
    // the OTS detached-node approach frames cleanly where the vanilla camera
    // could not.  Trade/loot keep the normal over-the-shoulder view.
    {
        // Own inventory (incl. a backpack character's 2-window inventory),
        // excluding any shop/loot/corpse trade.  See isOwnInventoryOpen().
        bool ownInv = isOwnInventoryOpen();
        // Face-cam target = the SELECTED (white-highlighted) character, not the
        // DC anchor: clicking portraits while inventory is open changes the
        // selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // PRIMARY = the character whose inventory window is actually open
            // (inventoryWindowCharacter).  At count==1 this is always one of your
            // own squad, including RECRUITED MOD-NPCs (e.g. Wandering Menders'
            // Kumo) that don't report isPlayerCharacter()==true and so used to
            // fall through to the anchor — leaving the camera framing the wrong
            // character (field 2026-06-17).  Fall back to the selected player
            // char, then the DC anchor.
            Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
            if (invChar && invChar->movement)
                invTarget = invChar;
            else if (s_selectedCharacter && s_selectedCharacter->movement
                     && s_selectedCharacter->isPlayerCharacter())
                invTarget = s_selectedCharacter;
            else
                invTarget = s_freeMoveAnchor;
        }

        if (ownInv && (!s_otsInvFaceActive || s_otsInvFaceChar != invTarget))
        {
            if (!s_otsInvFaceActive)
            {
                // First open: remember the player's shoulder view.
                s_otsSavedYaw   = s_fpYaw;
                s_otsSavedPitch = s_fpPitch;
                s_otsSavedDist  = s_otsDistCur;
                DebugLog("[WASDCombat] dc_cam_inventory_face");
            }
            else
            {
                DebugLog("[WASDCombat] dc_cam_inventory_face_retargeted");
            }
            s_otsInvFaceActive = true;
            s_otsInvFaceChar   = invTarget;

            float faceYaw = s_fpYaw + 3.14159265f;   // fallback: spin around
            if (invTarget && invTarget->movement)
            {
                Ogre::Vector3 bd = invTarget->movement->direction;
                bd.y = 0.0f;
                float bl = bd.length();
                if (bl > 0.001f)
                {
                    bd /= bl;
                    faceYaw = atan2f(-bd.x, -bd.z) + 3.14159265f;
                }
            }
            s_fpYaw      = faceYaw;
            s_fpPitch    = -0.02f;                    // near-level at chest height
            s_otsDistCur = 20.0f;                     // pull in for a tight portrait
        }
        else if (!ownInv && s_otsInvFaceActive)
        {
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;
            s_fpYaw      = s_otsSavedYaw;
            s_fpPitch    = s_otsSavedPitch;
            s_otsDistCur = s_otsSavedDist;
            DebugLog("[WASDCombat] dc_cam_inventory_face_restored");
        }
    }

    // Position the DETACHED camera to face the character's FRONT, recomputed
    // EVERY FRAME from the target's current facing — so it always faces the
    // front regardless of which way the character points, and never drifts to
    // the side on a re-open (the old edge-triggered aim was skipped on re-open
    // and left a stale yaw — field 2026-06-16).  Target = the selected (white)
    // character, falling back to the DC anchor.
    // Same target priority as the face-cam swing above: the OPEN inventory
    // window's character first (covers recruited mod-NPCs that fail
    // isPlayerCharacter()), then the selected player char, then the DC anchor.
    Character* tgt = nullptr;
    {
        Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
        if (invChar && invChar->movement)
            tgt = invChar;
        else if (s_selectedCharacter && s_selectedCharacter->movement
                 && s_selectedCharacter->isPlayerCharacter())
            tgt = s_selectedCharacter;
        else
            tgt = s_freeMoveAnchor;
    }
    if (tgt && tgt->movement && s_fpNode)
    {
        CharMovement* mvP = tgt->movement;
        Ogre::Vector3 bd = mvP->direction;
        bd.y = 0.0f;
        float bl = bd.length();
        float faceYaw = (bl > 0.001f) ? atan2f(bd.x, bd.z) : 0.0f;  // forward=-bd=front
        Ogre::Quaternion q =
            Ogre::Quaternion(Ogre::Radian(faceYaw), Ogre::Vector3::UNIT_Y) *
            Ogre::Quaternion(Ogre::Radian(-0.02f),  Ogre::Vector3::UNIT_X);
        Ogre::Vector3 pivot = mvP->pos;
        pivot.y += 12.5f;                                  // upper chest
        // q*(0,0,dist) is along the character's facing (in FRONT of them).
        Ogre::Vector3 eye = pivot + q * Ogre::Vector3(0.0f, 0.5f, 20.0f);
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }
}

// CameraClass::restrictPosition hook.  This call is what REFRESHES the interior
// floor visibility (data 2026-06-16: floors render iff restrictPosition runs —
// camFloor/centerBuilding are NOT the lever).  We used to skip it entirely under
// OTS because its camera clamp yanks the over-the-shoulder view back to the
// floor — but skipping it meant the storey you climbed onto never rendered until
// P off.  Now we RUN it (so the floor refreshes) and immediately re-apply the
// OTS pose we wrote this frame, so the clamp can't move the view.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // While the detached inventory face-cam OR first-person is active, skip the
    // RTS clamp — it would yank our free camera back to the floor.  (Interior floor
    // reveal in FP is handled separately by driving updateFloorVisibility from the
    // camera hook, since restrictPosition does not run while FP owns the camera.)
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}

// -----------------------------------------------------------------------
// CharMovement::_NV_update hook — WASD priority with animation protection
//
// WASD held:   apply halt+MOVE_DIRECTION before original; instant-stop on release.
// WASD not held: original runs freely; protected animations are untouched.
// -----------------------------------------------------------------------
static void (*s_charMovUpdateOrig)(CharMovement* thisptr, float time);

// -----------------------------------------------------------------------
// isDoorInteractionTask — door-related TaskTypes.  Used only by the
// hold-scoped door suppression in addJob_hook / addOrder_hook.
// MOVE_CUS_ORDERED is deliberately absent (DC's own disengage orders).
// -----------------------------------------------------------------------
static bool isDoorInteractionTask(TaskType t)
{
    switch (t)
    {
    case OPEN_DOOR:
    case CLOSE_DOOR:
    case OPEN_DOOR_HERE:
    case CLOSE_DOOR_HERE:
    case LOCK_DOOR:
    case UNLOCK_DOOR:
    case LOCK_DOOR_HERE:
    case UNLOCK_DOOR_HERE:
    case PICK_LOCK:
    case BASH_DOOR:
    case MOVE_TO_BUILDING_DOOR:
    case MOVE_TO_CURRENT_LOCATION_BUILDING_DOOR:
    case MOVE_TO_BUILDING_DOOR_INSIDEPOS:
    case MOVE_TO_BUILDING_DOOR_OUTSIDEPOS:
    case OPEN_DOOR_FOR_CURRENT_LOCATION:
    case OPEN_DOOR_FOR_DESTINATION:
        return true;
    default:
        return false;
    }
}

// -----------------------------------------------------------------------
// computeHoldDecision — single authority decision for the post-WASD hold,
// shared by charMovUpdate_hook (motion gate) and the end-of-mainLoop
// position clamp.  Returns true when vanilla may move the anchor; false
// means the hold keeps the character where WASD parked them.
//
// reason=no_hold is the normal hybrid state: vanilla owns locomotion
// (point-click works) because no WASD release is pending.  The remaining
// exemptions yield the hold to systems that must move or animate the
// character (mirrors the committed-action philosophy; uses
// isProtectedAnimationState, NOT isCommittedAction, because the door
// interaction Tasker owns STARTUP_STATE and would never release the hold).
// -----------------------------------------------------------------------
static bool computeHoldDecision(Character* ch, const char** outReason)
{
    if (!ch)                           { *outReason = "no_character";   return true; }
    if (!s_wasdHoldActive)             { *outReason = "no_hold";        return true; }
    if (s_playerPointClickActive)      { *outReason = "player_click";   return true; }
    if (s_healingJobActive)            { *outReason = "healing";        return true; }
    if (s_cameraLockTurretSuspend)     { *outReason = "turret";         return true; }
    if (s_menuSuspendActive)           { *outReason = "menu";           return true; }
    if (isDownedButMovable(ch))        { *outReason = "downed";         return true; }
    if (isProtectedAnimationState(ch)) { *outReason = "protected_anim"; return true; }
    if (ch->isInCombatMode(true, true)
        && (!s_postWasdGraceActive || s_combatReentryAllowed))
                                       { *outReason = "combat";         return true; }
    *outReason = "hold";
    return false;
}

// True when issuing a playerMoveOrderDefault on this character could trip the
// vanilla "I can't get out of here" pathfinding bark — the character is inside a
// building/interior OR locked in a cage / imprisoned, where the path to ANY
// destination may run through a locked door.  DC then skips its disengage /
// release move-orders and relies on direct injection + the hold-clamp instead.
// Field 2026-06-20: the bark persisted in a "locked building" because
// isInsideBuildingLoadedInterior() returns false unless the interior is loaded/
// rendered — getBuilding()/isIndoors() catch the building regardless, and
// isPrisonerFreeToGo()==false catches cages/shackles.
static bool moveOrderMayBark(Character* ch)
{
    if (!ch) return false;
    CharMovement* mv = ch->movement;
    // Gate ONLY on the indoors signals — they are all false outdoors, so this never
    // regresses the outdoor click-cancel.  isIndoors() is broader than
    // isInsideBuildingLoadedInterior() (the latter is false unless the interior is
    // loaded/rendered).  isPrisonerFreeToGo() is NOT gated on (its return for a
    // free non-prisoner is unverified; gating could skip the disengage everywhere)
    // — it is only LOGGED at the call site for diagnosis.
    return mv && (mv->isInsideBuildingLoadedInterior() || mv->isIndoors());
}

// dcSnapCancelOrder — the standard "cancel any in-flight path order by re-issuing
// a move to the CURRENT position" used by every WASD release/stop path.  Gated on
// moveOrderMayBark: indoors / locked the order pathfinds and fires the vanilla
// "I can't get out of here" bark, so it is SKIPPED — the surrounding halt() +
// motion-zero + the next-frame hold-clamp park the character without it.  Field
// 2026-06-20: the bark was log-proven to come from these ungated snap sites (the
// press-edge disengage was already gated, but the nudge-tap release, the
// structured release-stop, the menu-suspend stop and the heal-start snap were
// NOT) — DC issues NO disengage order yet still barked.
static void dcSnapCancelOrder(Character* ch)
{
    if (ch && ch->movement && !moveOrderMayBark(ch))
        ch->playerMoveOrderDefault(nullptr, nullptr, ch->movement->pos);
}

static void charMovUpdate_hook(CharMovement* thisptr, float time)
{
    // Fast path: non-anchor characters exit immediately with no further work.
    // s_anchorMovement is non-volatile; this single pointer comparison is the
    // only cost for every NPC/guard CharMovement::update call.
    if (thisptr != s_anchorMovement || s_loadGuardActive || s_dcShutdownInProgress)
    {
        if (s_dcShutdownInProgress && !s_hookBlockLoggedCharMov) {
            s_hookBlockLoggedCharMov = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=charMovUpdate"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // Anchor character only from this point.  Time DC-specific work from here.
    ScopeTimer _tCM(s_prof_charMove);
    // Continuously cache the anchor's REAL speed tier every frame.  A traveling /
    // save-load frees the controlled character before the load path can read it,
    // so that path used to fall back to RUN — which overwrote the player's speed
    // with a sprint after every load ("traveling load forces sprint", user
    // 2026-07-31).  Caching the true tier here lets the reacquire restore the
    // ACTUAL speed instead of guessing.  Guarded to real tiers (< GROUPED).
    if (thisptr->speedOrders < GROUPED)
        s_dcPreservedSpeedMode = thisptr->speedOrders;
    // Use frame-cached values so no volatile reads are needed inside the per-frame anchor logic.
    bool wasdHeld    = s_frameWasdHeld;
    bool inVMode     = (s_frameMode == MODE_FREE_MOVE);
    bool lootSuspend = s_frameLootSuspend;

    // Combat WASD transition bridge: if we were just driving and a real key was
    // held within COMBAT_WASD_BRIDGE_MS, and the anchor is in combat, treat the
    // current no-key gap as still-driving (using the last direction) so a key
    // roll (W->A->S->D) doesn't hand the body to the AI to square up.  Does NOT
    // refresh s_wasdLastHeldMs (see the held branch), so it self-expires.
    bool combatBridge = false;
    if (!wasdHeld && inVMode && !lootSuspend && s_wasdMovementApplied
        && s_prevWasdDir.squaredLength() > 0.0001f)
    {
        Character* chB = thisptr->getCharacter();
        if (chB && chB->isInCombatMode(true, true) && s_wasdLastHeldMs > 0
            && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS)
            combatBridge = true;
    }

    if (!inVMode || (!wasdHeld && !combatBridge) || lootSuspend)
    {
        // Not in WASD-drive mode for the anchor.
        if (inVMode && !wasdHeld && !lootSuspend)
        {
            Character*   chR = thisptr->getCharacter();

            // Instant stop: clear residual WASD motion before original runs.
            // Grace window: hold previous motion for wasdInputGraceMs ms after key release
            // so rapid tap/switch doesn't stutter through a stop cycle.
            if (s_wasdMovementApplied)
            {
                ULONGLONG now     = GetTickCount64();
                ULONGLONG graceMs = g_release.wasdReleaseGraceMs;
                bool inGrace      = (graceMs > 0 &&
                                     s_wasdLastHeldMs > 0 &&
                                     (now - s_wasdLastHeldMs) < graceMs);
                if (!inGrace && !isCommittedAction(chR))
                {
                    Ogre::Vector3 velPre = thisptr->currentMotion;
                    thisptr->halt();
                    thisptr->desiredMotion = Ogre::Vector3::ZERO;
                    thisptr->moveLimit     = 0.0f;
                    if (g_loco.wasdDecelerationMultiplier > 1.0f)
                        thisptr->currentMotion = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    s_prevWasdDir         = Ogre::Vector3::ZERO;
                    // The move-to-current-pos snap cancels any in-flight path
                    // order on release.  OUTDOORS it resolves instantly; INDOORS
                    // playerMoveOrderDefault path-walks the interior network even
                    // to the current position, producing a one-frame step before
                    // the hold-clamp engages = "small movement delay when walking
                    // indoors" (field 2026-06-17).  halt()+zeroed motion above and
                    // the X/Z hold-clamp next frame stop the character cleanly, so
                    // indoors we skip the snap.
                    if (s_freeMoveAnchor && !moveOrderMayBark(s_freeMoveAnchor))
                    {
                        s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, thisptr->pos);
                        DebugLog("[WASDCombat] wasd_release_preorig_anchor_snap");
                    }
#if DIAG_VERBOSE
                    char buf[256];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] instant_stop vel_pre=(%.3f,%.3f,%.3f)",
                        velPre.x, velPre.y, velPre.z);
                    DebugLog(buf);
#else
                    (void)velPre;
#endif
                }
            }

            // Post-WASD hold: keep the character where WASD parked them.
            // Motion is zeroed before the original runs and the X/Z
            // position is clamped after it (indoor routing writes position
            // late in the frame).  Vanilla locomotion resumes the moment
            // the hold clears (click / WASD / V OFF) or an exemption
            // yields (healing, turret, menu, downed, protected anim,
            // combat).
            const char* holdReason = "";
            if (!computeHoldDecision(chR, &holdReason))
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = thisptr->pos;
                    s_holdPosValid = true;
                }
                thisptr->halt();
                thisptr->movementMode  = MOVE_DIRECTION;   // path-following ignored
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                if (!s_idleHoldEngaged)
                {
                    s_idleHoldEngaged = true;
                    DebugLog("[WASDCombat] dc_wasd_hold_engaged");
                }
                s_charMovUpdateOrig(thisptr, time);
                thisptr->pos.x         = s_holdPos.x;
                thisptr->pos.z         = s_holdPos.z;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                return;
            }
            else if (s_idleHoldEngaged)
            {
                s_idleHoldEngaged = false;
                if (g_log.debugLogging)
                    DebugLog("[WASDCombat] dc_wasd_hold_released");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // WASD held — apply movement override.
    // Skip injection while turret-suspended, menu-suspended, or healing job is active.
    if (s_cameraLockTurretSuspend || s_menuSuspendActive)
    {
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    if (s_healingJobActive)
    {
        if (!s_medicalJobSuppressedThisHold)
        {
            s_medicalJobSuppressedThisHold = true;
            DebugLog("[WASDCombat] medical_job_blocks_wasd");
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    // Downed/crippled characters OUTDOORS use playerMoveOrderDefault (issued
    // in pre-AI section 5) — halt() + setDirectMovement here would cancel
    // that order.  INDOORS they fall through to the direct-steering branch
    // below, same as standing WASD (orders path-walk interiors).
    {
        Character* chC = thisptr->getCharacter();
        if (chC && downedOrderDriven(chC))
        {
            static ULONGLONG s_crippledChMovTick = 0;
            ULONGLONG nowCC = GetTickCount64();
            if (nowCC - s_crippledChMovTick >= 2000) { s_crippledChMovTick = nowCC;
                DebugLog("[WASDCombat] dc_crippled_state_detected");
                DebugLog("[WASDCombat] dc_crippled_can_move=true");
                DebugLog("[WASDCombat] dc_crippled_using_downed_movement_path"); }

            // Same combat-steering conflict as standing WASD (v1.7.5):
            // lingering combat mode after a fight computes its own
            // movement inside update and fights the crawl order — the
            // downed character stutters and goes nowhere.  Flip the flag
            // for the integration step only; CombatClass::go still sees
            // it in the AI phase.  (A downed character cannot be mid-
            // swing, so no CHOP_WEAPON exclusion is needed here.)
            CombatClass* ccD = chC->getCombatClass();
            bool flippedD = false;
            if (ccD && ccD->combatModeActive)
            {
                ccD->combatModeActive = false;
                flippedD = true;
            }
            s_charMovUpdateOrig(thisptr, time);
            if (flippedD)
            {
                ccD->combatModeActive = true;
                static ULONGLONG s_downedSteerLogTick = 0;
                ULONGLONG nowDS = GetTickCount64();
                if (nowDS - s_downedSteerLogTick >= 1000)
                {
                    s_downedSteerLogTick = nowDS;
                    DebugLog("[WASDCombat] dc_downed_combat_steering_overridden");
                }
            }
            return;
        }
    }
    // Only a REAL key press refreshes the last-held timestamp; a bridge frame
    // must let the bridge window expire (otherwise a single tap would drive
    // forever in combat).
    if (!combatBridge)
        s_wasdLastHeldMs = GetTickCount64();
    s_holdPosValid   = false;  // WASD drives — hold anchor recaptured on next hold

    Ogre::Vector3 wasdDir;
    bool dirOk = computeWASDDirection(s_wHeld, s_aHeld, s_sHeld, s_dHeld, wasdDir);
    // Bridge frame: no live key this instant, reuse the last driven direction so
    // movement carries through the key-roll gap instead of stalling.
    if (!dirOk && combatBridge)
    {
        wasdDir = s_prevWasdDir;
        dirOk   = true;
    }

    // GET UP FROM SEAT / BED / MACHINE (user req 2026-06-22): when the character is
    // anchored to a UseableStuff object (chair / throne / bed / workstation) the body
    // is locked to the node, so setDirectMovement only spins the model in place.
    // Issue ONE player move order toward the held direction — the exact path a
    // point-click takes: it (a) gets the character up with the proper animation, and
    // (b) REPLACES the queued "use object" job so the AI doesn't re-grab the furniture
    // and snap them back when they walk past it again (field 2026-06-22).  Edge-gated
    // (once per sit; re-armed when standing).  Direct WASD injection overrides the
    // order the instant they're standing; the on-release stop cancels any remainder.
    if (dirOk)
    {
        Character* chSeat = thisptr->getCharacter();
        if (chSeat && (isAnchoredToFurniture(chSeat) || chSeat->isCurrentlyGettingUp))
        {
            chSeat->playerWantsMeToGetUp = true;
            // SELF-HEALING re-issue: send the get-up move order at most once per window
            // while anchored — NOT a latched once-per-session flag (that got stuck
            // across squad-switches / job re-sits, so a re-selected character could no
            // longer leave the chair, field 2026-06-22).  Throttled so we don't re-path
            // every frame, but always re-arms for a fresh sit / re-selected character.
            static ULONGLONG s_lastFurnitureExitMs = 0;
            ULONGLONG nowF = GetTickCount64();
            if (isAnchoredToFurniture(chSeat) && (nowF - s_lastFurnitureExitMs) > 600)
            {
                Ogre::Vector3 dest = thisptr->pos + wasdDir * 50.0f;   // ~5m ahead (≈10u/m)
                chSeat->playerMoveOrderDefault(nullptr, nullptr, dest);
                s_lastFurnitureExitMs = nowF;
                DebugLog("[WASDCombat] dc_furniture_exit_move_order");
            }
            s_wasdMovementApplied = false;   // not injecting this frame; let the get-up run
            s_prevWasdDir         = wasdDir;
            static ULONGLONG s_seatGetupTick = 0;
            ULONGLONG nowSG = GetTickCount64();
            if (nowSG - s_seatGetupTick >= 1000) { s_seatGetupTick = nowSG;
                DebugLog("[WASDCombat] dc_wasd_getup_from_furniture"); }
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }

    // FINISH-THE-CLIP buffer (user req 2026-06-21, broadened 2026-06-22): Kenshi can't
    // abort an animation clip, so cutting one with movement looks broken/stutters.
    // BUFFER movement while a committed combat clip plays — the character's own swing
    // (windup/strike), a stagger from being hit (STUMBLE), or a parry (REACTION_BLOCK)
    // — and let it finish in place, THEN move.  go() is left running for these states
    // (so the clip completes + the state advances), then combatGo_hook suppresses it,
    // so no new attack chains and movement flows the instant the clip ends.  We touch
    // NO combat state here (no flip, no cut) — exactly one system drives the body, so
    // nothing fights.  DECISION/BLOCK/CIRCLE/WAIT/HESITATE still yield to movement so
    // you can always retreat.
    if (dirOk && isCommittedCombatClip(thisptr->getCharacter()))
    {
        s_wasdMovementApplied = false;   // the clip owns the body; do not inject
        static ULONGLONG s_clipBufTick = 0;
        ULONGLONG nowSB = GetTickCount64();
        if (nowSB - s_clipBufTick >= 1000) { s_clipBufTick = nowSB;
            DebugLog("[WASDCombat] dc_wasd_buffered_combat_clip"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    if (dirOk)
    {
        // Turn responsiveness: detect direction change and boost limit.
        bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
        bool turning    = prevHasDir && (wasdDir.dotProduct(s_prevWasdDir) < 0.9f);
        float limit     = wasdMoveLimit(turning);

        s_wasdMovementApplied = true;
        { ULONGLONG t = GetTickCount64();
          if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
              if (g_log.debugVerbose)
                  DebugLog("[WASDCombat] movement_injection_allowed"); } }
        thisptr->halt();
        thisptr->animationOverride = false;
        thisptr->movementMode      = MOVE_DIRECTION;
        thisptr->setDesiredSpeed(thisptr->speedOrders);
        thisptr->setDirectMovement(wasdDir, limit);

        // Pre-charge currentMotion to jump-start acceleration ramp-up.
        // The boost is a FRACTION of desiredSpeed; for a healthy runner
        // (desiredSpeed ~999) even a 20% fraction is fast, but for an
        // injured/crippled character (desiredSpeed clamped to ~55) the same
        // fraction is a ~11-unit crawl.  In a big fight at low FPS the engine
        // often fails to integrate currentMotion up past the pre-charge that
        // same frame, so the body lurches between full speed and that crawl —
        // the "stutter, especially when injured/crippled" (field diag
        // 2026-06-13: spd=55, cur oscillating 55 -> 10 -> 0).  Floor the
        // pre-charge at the FULL desired velocity so a speed-capped character
        // always gets their (already-reduced) full speed, never a fraction.
        float accel = (g_loco.wasdAccelerationMultiplier - 1.0f)
                    * (turning ? g_loco.wasdTurnResponsiveness : 1.0f);
        if (accel > 0.0f)
        {
            float boost = accel < 1.0f ? accel : 1.0f;
            float preSpeed = thisptr->desiredSpeed * boost;
            // Floor at the FULL desired velocity so a (reduced) capped character
            // still gets their whole speed and doesn't stutter — BUT never above the
            // WASD move-limit.  Flooring at raw desiredSpeed was what set currentMotion
            // to ~999 every frame and let shackled/injured runners outrun everything
            // (the setDirectMovement limit alone didn't bind because this velocity
            // write does).  Clamp the pre-charge to `limit` so the cap actually holds.
            float floorSpeed = thisptr->desiredSpeed;
            if (s_settingWasdSpeedCap && floorSpeed > limit) floorSpeed = limit;
            if (preSpeed < floorSpeed) preSpeed = floorSpeed;
            if (s_settingWasdSpeedCap && preSpeed > limit) preSpeed = limit;
            thisptr->currentMotion = wasdDir * preSpeed;
        }

        s_prevWasdDir = wasdDir;

        // NORMAL WALK WHILE DRIVING (user req 2026-06-22; refined via dc_armsdown_diag):
        // WASD movement must ALWAYS use plain walk/run locomotion — never a combat-ready
        // stance.  The diagnostic proved the arms-down appears while the character is
        // TARGETED (isInCombatMode / red portrait): flipping combatModeActive off only
        // for the integration step was NOT enough — RESTORING it true afterward kept the
        // combat animation layer (and our COMBAT_FINISHED cut) active under
        // MOVE_DIRECTION, so the body stayed in the lowered-weapon "arms-down" pose every
        // frame.  Fix: while WASD is driving, leave combatModeActive CLEARED (do not
        // restore).  This is XP-safe — go() is already suppressed while driving
        // (combatGo_hook), so the character isn't attacking/earning combat XP while you
        // reposition anyway — and the AI re-establishes combat the instant you release
        // WASD (go() re-runs and re-engages).  Result: plain walk even with an enemy on
        // you; combat (and its animations) resume the moment you stop driving.
        Character*   chFlip = thisptr->getCharacter();
        CombatClass* ccFlip = chFlip ? chFlip->getCombatClass() : nullptr;
        if (ccFlip && ccFlip->combatModeActive)
            ccFlip->combatModeActive = false;
        s_charMovUpdateOrig(thisptr, time);

#if DIAG_VERBOSE
        bool animReenabled = thisptr->animationOverride;
        bool modeChanged   = (thisptr->movementMode != MOVE_DIRECTION);
        if (animReenabled || modeChanged)
            DebugLog("[WASDCombat] combat_locomotion_attempt_detected");
#endif

        // Post-original re-assert: the combat AI re-enables animationOverride /
        // flips movementMode away from MOVE_DIRECTION when it wants to drive the
        // body (positioning OR an action animation), which would override the
        // player's WASD movement.  Re-assert MOVE_DIRECTION unconditionally so the
        // player always wins (absolute priority, user req 2026-06-20).
        {
            Character*   chPost = thisptr->getCharacter();
            CombatClass* ccPost = chPost ? chPost->getCombatClass() : nullptr;
            if (ccPost)
            {
                // Re-assert MOVE_DIRECTION so the AI can never drag/circle/square the
                // character while keys are held (absolute priority, user req
                // 2026-06-20).  We do NOT cut the combat state here anymore: the
                // earlier per-frame `combatState = COMBAT_FINISHED` write FOUGHT the
                // combat system and produced the broken/stutter look (field
                // 2026-06-22).  Sliding is already prevented two cleaner ways — committed
                // clips (swing/stagger/parry) are BUFFERED above so they never move, and
                // for every other state combatModeActive is cleared this frame so plain
                // walk plays (no combat clip to slide).  One system drives at a time.
                {
                    thisptr->animationOverride = false;
                    thisptr->movementMode      = MOVE_DIRECTION;
                    thisptr->setDirectMovement(wasdDir, wasdMoveLimit(false));
                }
                if (g_log.debugLogging && ccPost && ccPost->combatModeActive)
                {
                    static ULONGLONG s_xpCombatLogTick = 0;
                    ULONGLONG _tcx = GetTickCount64();
                    if (_tcx - s_xpCombatLogTick >= 1000) { s_xpCombatLogTick = _tcx;
                        DebugLog("[WASDCombat] dc_xp_vanilla_combat_allowed skill=Combat"); }
                }
            }
        }

    }
    else
    {
        // Race-case stop: snapshot said WASD held but poll thread released keys before
        // direction could be computed.  Zero stale motion fields before vanilla runs.
        if (s_wasdMovementApplied)
        {
            Character* chRace = thisptr->getCharacter();
            if (!isCommittedAction(chRace))
            {
                thisptr->halt();
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                s_prevWasdDir          = Ogre::Vector3::ZERO;
                s_wasdMovementApplied  = false;
                DebugLog("[WASDCombat] wasd_release_race_preorig_zeroed");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
    }
}

// initCombatMode_hook and youKnowImAttacking_hook removed —
// DC no longer blocks enemy combat entry or attack notifications.
// The combat AI runs freely; DC is a movement overlay only.

// -----------------------------------------------------------------------
// combatGo_hook — CombatClass::_NV_go, the per-frame combat AI decision.
// DC OWNERSHIP-HANDOFF MODEL (user req 2026-06-21): clean, exclusive ownership of
// the controlled character's locomotion, NO per-frame tug-of-war (that was the
// root cause of all the combat stutter/slide).
//   - WASD held (+ a short COMBAT_WASD_BRIDGE_MS grace for key-rolls): MOVEMENT
//     owns the character — SKIP go() so the AI takes no action and never steers,
//     and WASD has uncontested control = smooth, instant, no stutter.  Any clip
//     already mid-play is dropped to the run anim by the charMovUpdate cutoff =
//     no slide.
//   - WASD released: the AI owns the character — go() runs FULLY AUTONOMOUSLY
//     (vanilla combat: block/dodge/attack/position), resuming instantly.
// The handoff happens once at the press/release edge, never per-frame.  Only the
// anchor (thisptr->me == s_freeMoveAnchor, me @0x188) in DC; everything else is
// untouched.  Hooking go() is safe (proven in the Focus-Mode line; the door-era
// crash was setCurrentAction, NOT go).
// -----------------------------------------------------------------------
static void (*s_combatGoOrig)(CombatClass* thisptr, float frameTime);
static void combatGo_hook(CombatClass* thisptr, float frameTime)
{
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->me
        && thisptr->me == s_freeMoveAnchor)
    {
        bool wasdHeld     = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool movementOwns = wasdHeld
            || (s_wasdLastHeldMs > 0
                && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS);
        // Let go() RUN while a committed combat clip is mid-play (swing / stagger /
        // parry) so it FINISHES and the state advances (the charMovUpdate buffer holds
        // movement meanwhile); suppress it the instant the clip is done so no NEW attack
        // chains and movement takes over.  Suppressing only OUTSIDE the clip also means
        // we never freeze the state machine mid-clip (the buffer would otherwise stick
        // forever — e.g. a stagger that never advances).
        if (movementOwns && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;   // WASD owns locomotion; AI stands down
            return;                             // skip the combat decision entirely
        }
    }
    s_retreatLockGoSuppressed = false;
    s_combatGoOrig(thisptr, frameTime);
}

// -----------------------------------------------------------------------
// Main-thread hook — GameWorld::mainLoop_GPUSensitiveStuff
//
// Execution order:
//   1.  Safety gate (load-guard)
//   2.  Selection tracking
//   3.  V-Mode transition
//   4.  HUD + X speed key
//   5.  Pre-AI WASD
//   6.  s_mainLoopOrig (AI + CharMovement::update + CombatClass::go)
//   7.  Post-AI combat job suppression
//   8.  Periodic squad-threat scan
//   9.  Post-AI WASD re-application + instant stop
// -----------------------------------------------------------------------
static void (*s_mainLoopOrig)(GameWorld* thisptr, float time);

static void mainLoop_hook(GameWorld* thisptr, float time)
{
    // Hard-shutdown: all DC hook logic suppressed while true.
    // The six-condition stabilization countdown runs inside this block so it
    // advances even while the shutdown flag is held.
    if (s_dcShutdownInProgress)
    {
        if (!s_hookBlockLoggedMain) {
            s_hookBlockLoggedMain = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=mainLoop"); }

        if (ou && ou->player) s_mainLoopOrig(thisptr, time);

        // Six-condition check: runs once per frame after load finishes.
        // Condition 1: load finished (loadSig=false) — implied by the outer if.
        // Condition 2: player valid.
        // Conditions 3-4: selected character and movement pointers valid.
        // Condition 5: 60-frame window with all above held without lapse.
        // Condition 6: no stale anchor from old save remains.
        if (s_loadGuardActive && ou && ou->player && !ou->isLoadingFromASaveGame())
        {
            Character* chStab  = ou->player->selectedCharacter.getCharacter();
            bool chValid       = (chStab != nullptr);
            bool mvValid       = chValid && (chStab->movement != nullptr);
            bool noStaleAnchor = (s_freeMoveAnchor == nullptr);
            bool allClear      = chValid && mvValid && noStaleAnchor;

            if (allClear)
            {
                if (s_stabilizationCountdown == 0)
                {
                    s_stabilizationCountdown = 60;
                    DebugLog("[WASDCombat] reload_stabilization_started");
                }
                s_stabilizationCountdown--;
            }
            else
            {
                s_stabilizationCountdown = 0; // reset if any condition lapses
            }

            {
                ULONGLONG _nowW = GetTickCount64();
                if (_nowW - s_shutdownWaitLogTick >= 1000) { s_shutdownWaitLogTick = _nowW;
                    char _wbuf[256];
                    sprintf_s(_wbuf, sizeof(_wbuf),
                        "[WASDCombat] dc_loadgame_waiting_for_safe_reacquire "
                        "player=valid character=%s movement=%s anchor_clear=%s frames=%d/60",
                        chValid ? "valid" : "invalid",
                        mvValid ? "valid" : "invalid",
                        noStaleAnchor ? "yes" : "no",
                        allClear ? (60 - s_stabilizationCountdown) : 0);
                    DebugLog(_wbuf);
                }
            }

            if (allClear && s_stabilizationCountdown == 0)
            {
                s_loadGuardActive        = false;
                s_postLoadReacquire      = true;
                s_stabilizationCountdown = 0;
                DebugLog("[WASDCombat] dc_loadgame_safe_reacquire_complete");
                s_dcShutdownInProgress   = false;
                DebugLog("[WASDCombat] dc_shutdown_flag_cleared_after_safe_reacquire");
                DebugLog("[WASDCombat] reload_complete_reacquire_started");
            }
        }
        return;
    }

    // Profiling: initialize QPC frequency once; time every mainLoop invocation.
    if (!s_profInited)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_profFreq         = freq.QuadPart;
        s_profInited       = true;
        s_prof_windowStart = GetTickCount64();
    }
    ScopeTimer _tML(s_prof_mainLoop);

    // 1. Safety gate.
    {
        bool ouNull  = (ou == nullptr);
        bool loadSig = ouNull || !ou->player || ou->isLoadingFromASaveGame();

        if (loadSig)
        {
            if (ouNull)
            {
                // Game world gone — full hard shutdown regardless of user intent.
                if (!s_dcShutdownInProgress)
                {
                    s_dcShutdownInProgress = true;
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                    {
                        DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                        s_userWantsDC = false;
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor    = nullptr;
                    s_selectedCharacter = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                    s_anchorMovement    = nullptr;
                    s_selectedMovement  = nullptr;
                    s_prevAttackTarget  = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                    s_savedFreeCameraMode    = false;
                    s_cameraLockInvSuspend   = false;
                    s_cameraLockTurretSuspend = false;
                    s_savedCamFollowOffY     = 0.0f;
                    DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                    s_dcPtrLossActive = false;
                    s_loadGuardActive = true;
                    clearAllState();
                    s_vHud.label = nullptr;
                    s_vHud.shown = false;
                    s_hudReady   = false;
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
                }
                hudUpdate();
                return;
            }

            // ou is valid from here.
            if (s_userWantsDC)
            {
                // CRASH FIX (2026-06-13): a REAL save-load (isLoadingFromASaveGame)
                // frees the controlled character while ou->player may still be
                // valid.  The old preserve path below dereferences
                // s_freeMoveAnchor->movement (for speedOrders AND in the anchorOk
                // check), which reads freed memory and crashed when reloading
                // mid-capture.  A real save-load is NOT a chunk microload (those
                // only null ou->player without the load flag): drop the runtime
                // anchor IMMEDIATELY without touching it, skip ALL DC processing,
                // and run the game's loop so the load proceeds.  s_userWantsDC is
                // preserved, so the reacquire path restores DC once the load
                // completes.  Never deref the anchor while a save-load is active.
                if (ou->isLoadingFromASaveGame())
                {
                    if (!s_dcPtrLossActive)
                    {
                        s_dcPtrLossActive      = true;
                        s_dcPtrLossStartedAt   = GetTickCount64();
                        s_dcPtrLossLastLogTick = 0;
                        // Speed is NOT forced here any more.  charMovUpdate_hook
                        // caches the anchor's real speed tier every frame, so the
                        // reacquire restores the player's ACTUAL speed instead of a
                        // hard-coded RUN ("traveling load forces sprint", user
                        // 2026-07-31).  The anchor is freed on a save-load, so we must
                        // rely on that cached value here rather than deref it.
                        DebugLog("[WASDCombat] dc_realload_anchor_dropped_preserving_intent");
                    }
                    s_freeMoveAnchor    = nullptr;
                    s_anchorMovement    = nullptr;
                    s_selectedCharacter = nullptr;
                    s_selectedMovement  = nullptr;
                    hudUpdate();
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // DC intended — preserve through a chunk microload (player null,
                // no save-load flag).  Characters are NOT freed here, so the
                // anchor deref below is safe.
                if (!s_dcPtrLossActive)
                {
                    s_dcPtrLossActive      = true;
                    s_dcPtrLossStartedAt   = GetTickCount64();
                    s_dcPtrLossLastLogTick = 0;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
                        s_dcPreservedSpeedMode = s_freeMoveAnchor->movement->speedOrders;
                    DebugLog("[WASDCombat] dc_pointer_loss_preserving_user_intent");
                    if (!ou->player)
                        DebugLog("[WASDCombat] dc_pointer_loss_preserved_microload");
                    if (ou->player && ou->player->isTrackingCharacter())
                        DebugLog("[WASDCombat] camera_lock_lost_during_long_stream");
                }

                // No timeout — pointer loss of any duration only pauses injection.
                // Reacquire loop runs indefinitely until pointer is valid or a true hard-shutdown fires.

                // Pointer-validity guard: skip injection if anchor or player is invalid.
                bool anchorOk = (ou->player != nullptr) &&
                                (s_freeMoveAnchor != nullptr) &&
                                (s_freeMoveAnchor->movement != nullptr);
                if (!anchorOk)
                {
                    s_anchorMovement = nullptr;
                    if (g_log.debugLogging)
                    {
                        ULONGLONG nowL = GetTickCount64();
                        if (nowL - s_dcPtrLossLastLogTick >= 500)
                        {
                            s_dcPtrLossLastLogTick = nowL;
                            char dlbuf[80];
                            sprintf_s(dlbuf, sizeof(dlbuf),
                                "[WASDCombat] dc_pointer_loss_duration_ms=%llu",
                                nowL - s_dcPtrLossStartedAt);
                            DebugLog(dlbuf);
                        }
                    }
                    hudUpdate();
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // All pointers valid — restore cache and fall through to DC processing.
                s_anchorMovement = s_freeMoveAnchor->movement;
                // Fall through — injection resumes normally in steps 5 and 9.
            }
            else
            {
                // DC not intended — normal load guard path.
                if (!s_loadGuardActive)
                {
                    if (!s_dcShutdownInProgress)
                    {
                        s_dcShutdownInProgress = true;
                        DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor               = nullptr;
                    s_anchorMovement               = nullptr;
                    s_selectedCharacter            = nullptr;
                    s_selectedMovement             = nullptr;
                    s_prevAttackTarget             = nullptr;
                    s_savedFreeCameraMode          = false;
                    s_cameraLockInvSuspend         = false;
                    s_cameraLockTurretSuspend      = false;
                    s_savedCamFollowOffY           = 0.0f;
                    s_loadGuardActive = true;
                    clearAllState();
                    s_vHud.label = nullptr;
                    s_vHud.shown = false;
                    s_hudReady   = false;
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                }
                hudUpdate();
                if (ou && ou->player) s_mainLoopOrig(thisptr, time);
                return;
            }
        }
        else if (s_dcPtrLossActive)
        {
            // loadSig cleared — run reacquire sequence.
            s_dcPtrLossActive = false;
            Character* chR = ou->player->selectedCharacter.getCharacter();
            if (chR && s_userWantsDC)
            {
                DebugLog("[WASDCombat] dc_reacquire_after_long_stream");
                s_freeMoveAnchor = chR;
                s_anchorMovement = chR->movement;
                s_dcPtrLossLastLogTick = 0;
                if (chR->movement && s_dcPreservedSpeedMode < GROUPED)
                {
                    chR->movement->setDesiredSpeedOrders(s_dcPreservedSpeedMode);
                    chR->movement->setDesiredSpeed(s_dcPreservedSpeedMode);
                }
                if (s_mode != MODE_FREE_MOVE)
                    s_mode = MODE_FREE_MOVE;
                if (ou->player->isTrackingCharacter())
                {
                    DebugLog("[WASDCombat] camera_lock_restore_skipped_already_locked");
                }
                else
                {
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                    DebugLog("[WASDCombat] camera_lock_restored_after_long_stream");
                }
                DebugLog("[WASDCombat] dc_restored_after_long_stream");
            }
        }

        // s_loadGuardActive here means the shutdown countdown was handled inside
        // the s_dcShutdownInProgress block above (DC-shutdown path).  If we reach
        // this point with s_loadGuardActive still true it means the flag was set
        // without s_dcShutdownInProgress (should not occur after this fix), so
        // clear it safely to avoid being stuck.
        if (s_loadGuardActive)
        {
            s_loadGuardActive        = false;
            s_postLoadReacquire      = true;
            s_stabilizationCountdown = 0;
            DebugLog("[WASDCombat] reload_complete_reacquire_started");
        }
    }

    // NEWGAME detection — mainLoop signal poll.
    // Reachable only after safety gate confirms ou and ou->player are valid.
    // sm->signal == NEWGAME (0x4) is set by SaveManager::newGame() before any world teardown.
    // LOADGAME/Continue set signal == LOADGAME (0x2) — cannot produce a false positive here.
    // Gated on DC being active: no-op when DC was never enabled this session.
    // Does NOT set s_dcShutdownInProgress or s_loadGuardActive; LOADGAME machinery handles those
    // after world teardown begins normally.
    {
        SaveManager* sm = SaveManager::getSingleton();
        if (sm && sm->signal == SaveManager::NEWGAME
            && (s_mode == MODE_FREE_MOVE || s_userWantsDC || s_freeMoveAnchor != nullptr))
        {
            DebugLog("[WASDCombat] dc_newgame_signal_seen");
            DebugLog("[WASDCombat] dc_newgame_detection_source=mainloop_signal_poll");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_begin");

            // Destroy user intent first so no subsequent path can re-enter DC.
            s_userWantsDC = false;
            s_userWantsFP = false;   // new game = hard teardown; never re-enter FP into it

            // Tear down OTS while the anchor + camera are still valid: exitOTS
            // re-attaches the DETACHED camera to the rig (otherwise char
            // creation shows no character — the camera stays orphaned on our
            // node) and restores the view-floor; also drop the face-cam state
            // (it otherwise breaks in the new game).  Field 2026-06-15:
            // new-game-while-in-a-save left the OTS camera/face-cam stranded.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            s_fpSuspendedForInv = false;
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;

            // Stop camera tracking and restore freecam state before clearing pointers.
            if (ou->player)
            {
                ou->player->stopTrackCharacter();
                if (ou->player->camera)
                {
                    ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                    ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
                }
            }
            // Force mode and tracked-mode to VANILLA together so step-3 exit-transition
            // does not re-run stopTrackCharacter or show "Direct Control Disabled".
            s_mode          = MODE_VANILLA;
            s_fmTrackedMode = MODE_VANILLA;
            DebugLog("[WASDCombat] dc_newgame_forced_vanilla_mode");

            // Clear anchor and selection pointers before world teardown can free them.
            s_freeMoveAnchor    = nullptr;
            s_anchorMovement    = nullptr;
            s_selectedCharacter = nullptr;
            s_selectedMovement  = nullptr;
            s_prevAttackTarget  = nullptr;

            // Clear camera suspension state.
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            s_menuSuspendActive       = false;

            // Clear WASD and movement state.
            s_wHeld               = false;
            s_aHeld               = false;
            s_sHeld               = false;
            s_dHeld               = false;
            s_xPressed            = false;
            s_wasdWasActive       = false;
            s_wasdMovementApplied = false;
            s_prevWasdDir         = Ogre::Vector3::ZERO;
            s_wasdTapStartMs      = 0;
            s_wasdLastHeldMs      = 0;
            s_frameMode           = MODE_VANILLA;
            s_frameWasdHeld       = false;

            // Clear loot UI suspension.
            s_lootUiSuspendActive  = false;
            s_lootUiWasPrevOpen    = false;
            s_lootSuspendStartTick = 0;

            // Clear healing and committed-action flags.
            s_healingJobActive             = false;
            s_healingJobPending            = false;
            s_medicalJobSuppressedThisHold = false;
            s_attackCommitmentActive       = false;
            s_attackCommitmentStart        = 0;

            // Clear micro-load state — NEWGAME is not a micro-load.
            s_dcPtrLossActive      = false;
            s_dcPtrLossStartedAt   = 0;
            s_dcPtrLossLastLogTick = 0;

            DebugLog("[WASDCombat] dc_newgame_old_state_cleared");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_complete");
        }
    }

    // Loot UI suspension — detect any open inventory window.
    // s_mode is untouched; all V-Mode processing suspends while any inventory is open.
    // Diagnostic logs identify which specific inventory type was detected.
    {
        bool anyInvOpen      = gui && gui->isAnyInventoryWindowOpen();
        int  numInvOpen      = gui ? gui->getNumOpenInventoryWindows() : 0;
        bool npcFieldOpen    = gui && gui->inventoryWindowNPC.getCharacter()       != nullptr;
        bool charFieldOpen   = gui && gui->inventoryWindowCharacter.getCharacter() != nullptr;
        bool traderFieldOpen = gui && gui->inventoryWindowTrader.getCharacter()    != nullptr;
        bool tradeAOpen      = gui && gui->tradeA.getCharacter()                   != nullptr;
        bool tradeBOpen      = gui && gui->tradeB.getCharacter()                   != nullptr;

#if LOOT_DIAG
        // Periodic diagnostics while in V-Mode and any inventory is open.
        if (anyInvOpen && s_mode == MODE_FREE_MOVE)
        {
            static ULONGLONG s_invDiagTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_invDiagTick >= 1000) { s_invDiagTick = t;
                DebugLog("[WASDCombat] any_inventory_ui_detected");
                if (charFieldOpen)
                    DebugLog("[WASDCombat] player_inventory_ui_detected");
                if (npcFieldOpen)
                    DebugLog("[WASDCombat] npc_inventory_ui_detected");
                if (tradeAOpen || tradeBOpen)
                    DebugLog("[WASDCombat] unconscious_body_inventory_ui_detected");
                if (!charFieldOpen && !npcFieldOpen && !traderFieldOpen && !tradeAOpen && !tradeBOpen)
                    DebugLog("[WASDCombat] loot_ui_detection_failed");

                bool modal = MyGUI::InputManager::getInstance().isModalAny();
                if (modal) DebugLog("[WASDCombat] current_ui_modal_state");

                char wbuf[256];
                sprintf_s(wbuf, sizeof(wbuf),
                    "[WASDCombat] current_ui_window_name open=%d npc=%d char=%d trader=%d tradeA=%d tradeB=%d modal=%d",
                    numInvOpen, (int)npcFieldOpen, (int)charFieldOpen, (int)traderFieldOpen,
                    (int)tradeAOpen, (int)tradeBOpen, (int)modal);
                DebugLog(wbuf);

                MyGUI::Widget* mfocus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
                if (mfocus)
                {
                    char abuf[256];
                    sprintf_s(abuf, sizeof(abuf),
                        "[WASDCombat] current_ui_active_widget_name name=%s",
                        mfocus->getName().c_str());
                    DebugLog(abuf);

                    MyGUI::Widget* root = mfocus;
                    while (root->getParent() != nullptr) root = root->getParent();
                    char rbuf[256];
                    sprintf_s(rbuf, sizeof(rbuf),
                        "[WASDCombat] current_ui_root_name name=%s",
                        root->getName().c_str());
                    DebugLog(rbuf);
                }
            }
        }
#endif

        // Move-through eligibility (InventoryFaceCam=false): keep DC live + game
        // running instead of suspending.  Evaluated each frame.
        bool moveThrough = invMoveThroughEligible();

        // Suspension trigger: any open inventory window.
        // showTradeWindow_hook may have already set s_lootUiSuspendActive before
        // the window was visible; this block confirms open/close state and manages
        // s_lootUiWasPrevOpen for edge detection.
        if (moveThrough)
        {
            // --- Inventory MOVE-THROUGH: DC stays live, game keeps running ---
            // Lift any early suspend the trade hook set, and un-suspend the camera
            // lock so movement + tracking continue while the inventory UI is open.
            if (s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
            }
            if (s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = false;
                if (s_freeMoveAnchor && ou->player)
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
            // Kenshi auto-pauses on inventory open and again when the shown
            // inventory switches to another squad member.  Defeat ONLY those
            // auto-pauses (grace window after each open/switch edge) so injected
            // WASD physically moves the character.  A pause appearing OUTSIDE
            // the grace is the player's own — respect it, including across
            // subsequent inventory switches, until the player unpauses (user
            // req 2026-08-05: manual pause must work in move-through mode).
            Character* mtShownChar = gui->inventoryWindowCharacter.getCharacter();
            bool mtOpenEdge   = !s_invMoveThroughActive;
            bool mtSwitchEdge = s_invMoveThroughActive
                                && mtShownChar != s_invMoveThroughShownChar;
            s_invMoveThroughShownChar = mtShownChar;   // identity only
            if (mtOpenEdge || mtSwitchEdge)
            {
                // A paused player switching inventories stays paused: no grace
                // re-arm, so the auto-pause (a no-op on a paused game) is never
                // "defeated" out from under them.
                if (!s_invMoveThroughPlayerPaused)
                    s_invMoveThroughEdgeTick = GetTickCount64();
                if (mtSwitchEdge)
                    DebugLog("[WASDCombat] inv_move_through_switch_edge");
            }
            if (ou && ou->isPaused())
            {
                if (!s_invMoveThroughPlayerPaused
                    && s_invMoveThroughEdgeTick != 0
                    && GetTickCount64() - s_invMoveThroughEdgeTick
                           <= INV_MT_AUTOPAUSE_GRACE_MS)
                {
                    ou->userPause(false);              // Kenshi's auto-pause
                    s_invMoveThroughForcedRun = true;
                }
                else if (!s_invMoveThroughPlayerPaused)
                {
                    s_invMoveThroughPlayerPaused = true;
                    DebugLog("[WASDCombat] inv_move_through_player_pause_respected");
                }
            }
            else if (s_invMoveThroughPlayerPaused)
            {
                s_invMoveThroughPlayerPaused = false;  // player unpaused
                DebugLog("[WASDCombat] inv_move_through_player_unpause");
            }
            if (!s_invMoveThroughActive)
            {
                s_invMoveThroughActive = true;
                DebugLog("[WASDCombat] inv_move_through_begin");
            }
            s_lootUiWasPrevOpen = true;   // track open so the close edge is clean

            // Merchant trade window: vanilla won't close it on distance, so close
            // it ourselves once the controlled character has WALKED far enough from
            // where the trade opened.  Corpse/own loot close natively, so only the
            // trader window is handled here.  Skip player-squad trades (trader is
            // your own squadmate) — those must NOT auto-close.
            Character* trader = gui->inventoryWindowTrader.getCharacter();
            if (trader && !trader->isPlayerCharacter() && !s_invTradeCloseRequested)
            {
                Ogre::Vector3 ap = s_freeMoveAnchor->getPosition();
                if (!s_invTradeStartValid)
                {
                    s_invTradeAnchorStart = ap;   // spot where the trade opened
                    s_invTradeStartValid  = true;
                }
                else
                {
                    float dx = ap.x - s_invTradeAnchorStart.x;
                    float dz = ap.z - s_invTradeAnchorStart.z;
                    if (dx * dx + dz * dz > INV_TRADE_AUTOCLOSE_DIST_SQ)
                    {
                        gui->closeTradeWindow();
                        s_invTradeCloseRequested = true;
                        DebugLog("[WASDCombat] inv_trade_autoclosed_distance");
                    }
                }
            }
        }
        else if (s_invMoveThroughActive)
        {
            // --- Move-through session ending: inventory closed, dialogue began,
            //     DC toggled off, or face-cam re-enabled.  Leave the game running
            //     (player owns pause now); just clear the move-through state. ---
            s_invMoveThroughActive    = false;
            s_invMoveThroughForcedRun = false;
            s_invMoveThroughPlayerPaused = false;
            s_invMoveThroughShownChar    = nullptr;
            s_invMoveThroughEdgeTick     = 0;
            s_invTradeCloseRequested  = false;
            s_invTradeStartValid      = false;
            s_tradeWindowActive       = false;
            s_lootUiWasPrevOpen       = false;
            s_lootUiSuspendActive     = false;
            s_lootSuspendStartTick    = 0;
            DebugLog("[WASDCombat] inv_move_through_end");
        }
        else if (anyInvOpen && !s_lootUiWasPrevOpen)
        {
            // Window is now confirmed open.  Hook may have already set suspension;
            // if not (non-trade path such as showInventoryNPC), set it now.
            if (!s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = true;
                s_lootSuspendStartTick = GetTickCount64();
            }
            // NOTE: do NOT re-derive s_tradeWindowActive from the npc/trader/
            // tradeA/tradeB fields here — they stay STALE after a trade closes
            // (field 2026-06-17: tradeA=1 tradeB=1 persisted across later own-
            // inventory opens, re-flagging them as trades and killing the face-
            // cam).  showTradeWindow_hook is the authoritative trade latch; it
            // fires for shop AND corpse/loot, and we clear it on the close edge.
            s_lootUiWasPrevOpen = true;
            DebugLog("[WASDCombat] loot_ui_open_suspend_vmode");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = true;
                DebugLog("[WASDCombat] camera_lock_suspended_inventory_open");
            }
        }
        else if (!anyInvOpen && s_lootUiWasPrevOpen)
        {
            // Window closed — trade/loot session is over: clear the trade latch
            // immediately (before the suspension debounce) so the next OWN
            // inventory isn't mis-classified.
            s_tradeWindowActive = false;
            // Window closed — enforce debounce before releasing suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= LOOT_SUSPEND_DEBOUNCE_MS)
            {
                s_lootUiSuspendActive = false;
                s_lootUiWasPrevOpen   = false;
                DebugLog("[WASDCombat] loot_ui_closed_restore_vmode");
                if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_cameraLockInvSuspend && ou->player)
                {
                    s_cameraLockInvSuspend = false;
                    DebugLog("[WASDCombat] camera_lock_restored_after_inventory");
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                }
            }
            else
            {
#if LOOT_DIAG
                static ULONGLONG s_debBlockTick = 0;
                ULONGLONG t = GetTickCount64();
                if (t - s_debBlockTick >= 100) { s_debBlockTick = t;
                    DebugLog("[WASDCombat] duplicate_initial_loot_open_blocked"); }
#endif
            }
        }
        else if (s_lootUiSuspendActive && !s_lootUiWasPrevOpen && s_lootSuspendStartTick > 0)
        {
            // Hook fired but window never appeared (cancelled interaction).
            // Release after 1 s to avoid a stuck suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= 1000)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
                s_tradeWindowActive    = false;   // cancelled before any window opened
            }
        }
    }

    // DC anchor self-heal: after a reload the anchor can be lost while DC stays
    // on (s_mode FREE_MOVE) until the player manually toggles DC off/on (field
    // 2026-06-15).  Re-grab it from the live selection so DC (and the inventory
    // face-cam, which needs the anchor) stays available without that dance.
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive && !s_freeMoveAnchor
        && !s_dcShutdownInProgress && !s_loadGuardActive && ou && ou->player)
    {
        Character* sel = s_selectedCharacter;
        if (sel && sel->movement && sel->isPlayerCharacter())
        {
            s_freeMoveAnchor = sel;
            s_anchorMovement = sel->movement;
            ou->player->startTrackCharacter(s_freeMoveAnchor);
            DebugLog("[WASDCombat] dc_anchor_reacquired_selfheal");
        }
    }

    // Safety: the inventory face-cam manages its own enter/exit in
    // cameraUpdate_hook; if it is somehow left active here with no inventory
    // open, drop straight back to the vanilla camera.
    if (s_fpActive && !isOwnInventoryOpen())
        exitOTS(true);

    // Native command registration happens HERE, not in the loadConfig hook:
    // the game loads its keyboard config before RE_Kenshi loads plugins, so
    // that hook never fires on a normal launch (field finding, log-proven).
    if (!s_nativeCommandsRegistered)
        registerNativeCommands(key);

    // Movement flags come from the poll thread (always — see the native
    // keybind block comment).  Only the toggle/speed persistence watchdog
    // runs here: it saves rebinds even if the options menu never calls
    // saveOptions, and logs whether rebinds actually reach Command::bound.
    if (s_nativeCommandsRegistered)
        watchNativeBindChanges();

    // Frame snapshot — read all volatile input state once before any per-character
    // hooks run inside s_mainLoopOrig.  Hooks use s_frame* instead of re-reading
    // volatiles, eliminating memory fence overhead for 100+ hook calls per frame.
    s_frameMode        = s_mode;
    s_frameWasdHeld    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
    s_frameLootSuspend = s_lootUiSuspendActive;

    // 2. Selection tracking.
    {
        Character*    ch = ou->player->selectedCharacter.getCharacter();
        CharMovement* mv = ch ? ch->movement : nullptr;
        if (ch != s_selectedCharacter || mv != s_selectedMovement)
        {
            s_selectedCharacter = ch;
            s_selectedMovement  = mv;
        }

        if (s_postLoadReacquire && ch)
        {
            s_postLoadReacquire = false;
            DebugLog("[WASDCombat] reload_reacquire_complete");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
            {
                DebugLog("[WASDCombat] camera_lock_retarget_after_load");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
        }
    }

    // 3. V-Mode transition.
    { LONGLONG _clStart = qpcNow();
    {
        ControlMode curMode = s_mode;
        bool curInVM  = (curMode         == MODE_FREE_MOVE);
        bool prevInVM = (s_fmTrackedMode == MODE_FREE_MOVE);

        if (curInVM && !prevInVM && !s_lootUiSuspendActive)
        {
            s_freeMoveAnchor      = s_selectedCharacter;
            s_anchorMovement      = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            // Fresh V-mode = full vanilla until the first WASD release; an
            // in-flight point-click order continues normally.
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            s_camRotateToggle        = false;   // start DC with rotate-toggle off

            if (s_freeMoveAnchor && ou->player->camera)
            {
                s_savedFreeCameraMode = ou->player->camera->isFreeCameraMode();
                DebugLog("[WASDCombat] camera_mode_saved");
                ou->player->camera->setFreeCameraMode(false);
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                s_savedCamFollowOffY = ou->player->camera->objectCurrentlyFollowingOffset.y;
                DebugLog("[WASDCombat] camera_lock_enabled_dc");
            }
            ou->showPlayerAMessage("Direct Control Enabled", false);
        }
        else if (!curInVM && prevInVM && !s_lootUiSuspendActive)
        {
            // DC turned off — drop the OTS camera first (re-attach to the rig
            // while the anchor is still valid), then restore vanilla camera.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            s_fpSuspendedForInv = false;
            ou->player->stopTrackCharacter();
            if (ou->player->camera)
            {
                ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
            }
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            // Restore vanilla hold-to-rotate: drop the toggle and clear the forced
            // rotate flag so the camera doesn't stay rotating in vanilla mode.
            s_camRotateToggle         = false;
            if (key) key->rotate      = false;
            DebugLog("[WASDCombat] camera_lock_disabled_restore_freecam");
            s_freeMoveAnchor        = nullptr;
            s_anchorMovement      = nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            ou->showPlayerAMessage("Direct Control Disabled", false);
        }
        else if (curInVM && s_freeMoveAnchor && !s_lootUiSuspendActive)
        {
            Character* sel = s_selectedCharacter;
            // Switch WASD control to a different squad member without leaving DC,
            // two ways: DOUBLE-CLICK their portrait (user req 2026-06-20) or press
            // F while they are selected/highlighted (user req 2026-06-28).  A single
            // click only selects (vanilla), so other NPCs can be inspected/ordered
            // without stealing control (Sentient Sands compat).  Honor a poll-thread
            // double-click within the last ~600ms or a pending F edge, then consume
            // so the switch fires exactly once.
            ULONGLONG dcMs       = s_lmbDoubleClickMs;
            bool      dcSwitch   = (dcMs > 0 && (GetTickCount64() - dcMs) <= 600);
            bool      fSwitch    = s_fSelectEdge;
            bool      wantSwitch = dcSwitch || fSwitch;
            if (wantSwitch && sel && sel != s_freeMoveAnchor && sel->isPlayerCharacter())
            {
                s_lmbDoubleClickMs    = 0;   // consumed
                s_fSelectEdge         = false;
                // First-person head/hair hide is PER-CHARACTER: it was applied to the
                // OUTGOING anchor's own skeleton/appearance.  Restore it on the old
                // anchor BEFORE the pointer moves, or that character keeps a shrunk
                // head (user 2026-07-31: "head of the previous controlled character
                // will still be shrunk") and shaved hair.  fpSetHeadBoneHidden always
                // acts on the CURRENT s_freeMoveAnchor, so the order must be
                // restore-old -> switch -> re-hide-new.
                if (s_firstPersonActive && s_fpHeadBoneHidden)
                    fpSetHeadBoneHidden(false);                 // acts on OLD anchor
                if (s_firstPersonActive && s_fpHairHidden)
                {
                    AppearanceBase* apOld = s_freeMoveAnchor->getAppearance();
                    if (apOld) apOld->shaveHead(false);
                    s_fpHairHidden = false;
                    DebugLog("[WASDCombat] dc_fp_hair_restored_on_switch");
                }
                s_freeMoveAnchor      = sel;
                s_anchorMovement      = sel->movement;
                s_combatWASDLogged    = false;
                s_retreatLogged       = false;
                s_squadThreat         = false;
                s_consciousAllyThreat = false;
                s_enemyTargetingLogged = false;
                s_lastScanTick          = 0;
                // A newly controlled character has not been WASD-parked;
                // its vanilla orders continue until the first release.
                s_wasdHoldActive         = false;
                s_playerPointClickActive = false;
                s_holdPosValid           = false;
                s_idleHoldEngaged        = false;
                DebugLog(fSwitch ? "[WASDCombat] camera_lock_retarget_selection_fkey"
                                 : "[WASDCombat] camera_lock_retarget_selection_doubleclick");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                // Re-apply the FP head/hair hide to the NEW anchor so first-person
                // still hides the now-controlled character's own head/hair.
                if (s_firstPersonActive && s_fpHideHead)
                    fpSetHeadBoneHidden(true);                  // acts on NEW anchor
                if (s_firstPersonActive && s_fpHideHair)
                {
                    AppearanceBase* apNew = s_freeMoveAnchor->getAppearance();
                    if (apNew) { apNew->shaveHead(true); s_fpHairHidden = true;
                        DebugLog("[WASDCombat] dc_fp_hair_hidden_on_switch"); }
                }
            }
            else if (fSwitch)
            {
                // F pressed with no valid target (same char, non-player, or no
                // selection) — consume the edge so it cannot fire on a later frame.
                s_fSelectEdge = false;
            }
        }
        s_fmTrackedMode = curMode;
    }

    // First-person toggle (P): consume the poll-thread edge here on the GAME
    // thread, where the camera + anchor are valid (the poll thread must never
    // touch the camera).  Enter only in DC with a live anchor and when the
    // inventory face-cam does not already own the camera; exiting is always
    // allowed.  A manual toggle also cancels a pending suspend-for-inventory
    // auto-return so the camera doesn't snap back to first-person on close.
    if (s_fpToggleRequested)
    {
        s_fpToggleRequested = false;
        if (s_firstPersonActive)
        {
            s_userWantsFP = false;         // user turned FP off — clear the persistent intent
            exitFirstPerson(true);
        }
        else if (s_fpSuspendedForInv)
        {
            s_userWantsFP = false;         // cancelling the auto-return = user wants FP off
            s_fpSuspendedForInv = false;   // cancel the pending auto-return
        }
        else if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_lootUiSuspendActive
                 && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_userWantsFP = true;          // user turned FP on — persist through loads
            enterFirstPerson();
        }
    }

    // FP LOAD-PERSISTENCE self-heal: if the user wants first-person but a load/stream
    // dropped it, re-enter once the scene + DC anchor are stable again.  enterFirstPerson
    // self-guards + is idempotent; gated so it never fights the inventory face-cam suspend,
    // the OTS restore, a menu, or a still-loading scene.  This is what makes FP survive
    // chunk loads (user 2026-07-29).
    if (s_userWantsFP && !s_firstPersonActive && !s_fpActive && !s_fpSuspendedForInv
        && !s_otsRestorePending && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && ou && ou->player && ou->player->camera && !ou->isLoadingFromASaveGame()
        && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        DebugLog("[WASDCombat] fp_reentered_after_load");
        enterFirstPerson();
    }

    // Sneak toggle (Shift+C while first-person, user req 2026-08-01): consume the
    // poll-thread edge here on the game thread.  Drive the game's OWN sneak
    // button handler (OrdersPanel::toggleStealth — the exact path a mouse click
    // on the SNEAK button takes), so the UI checkbox, the standing order, and
    // the character's stealth state all stay in sync (user 2026-08-01: direct
    // setStealthMode toggled sneak but left the SNEAK button visually off).
    // Same precedent as the X speed key driving OrdersPanel::speedNext.  Falls
    // back to the raw state switch if the panel isn't showing the anchor (no UI
    // to sync in that case).  Stealth skill, detection, and XP all run vanilla
    // (Rule 1: DC invokes the system, it does not reimplement it); WASD speed
    // while sneaking is capped to the real stealth speed in wasdMoveLimit.
    // Sneak deliberately persists across FP/DC exit — it is vanilla state the
    // player can also clear via the sneak button.
    if (s_sneakToggleRequested)
    {
        s_sneakToggleRequested = false;
        if (s_firstPersonActive && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op && op->stealthCheckBox
                && op->ordersCharacter.getCharacter() == s_freeMoveAnchor)
            {
                op->toggleStealth(op->stealthCheckBox);
            }
            else
            {
                s_freeMoveAnchor->setStealthMode(!s_freeMoveAnchor->isStealthMode());
            }
            char sbuf[80];
            sprintf_s(sbuf, sizeof(sbuf),
                "[WASDCombat] dc_fp_sneak_toggle on=%d viaPanel=%d",
                s_freeMoveAnchor->isStealthMode() ? 1 : 0,
                (op && op->stealthCheckBox) ? 1 : 0);
            DebugLog(sbuf);
        }
    }

    // Enemy body-clip clearance sampling (first-person combat, 2026-08-01): find
    // the nearest LIVE hostile to the anchor each frame; fpDriveFrame turns it
    // into an eye pullback + near-clip tighten so an aggressor pressing into the
    // camera can't slice the view open.  Cost control: skipped entirely unless FP
    // is active AND the 5s threat scan saw enemies (or the anchor is in combat);
    // inside the loop a coarse squared-distance cull runs before any game call,
    // so the per-frame work is a few subtractions per loaded character.
    s_fpEnemyNearestDist = -1.0f;
    if (s_firstPersonActive && s_fpEnemyClearRadius > 0.0f
        && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && (s_nearbyEnemyCount > 0
            || s_freeMoveAnchor->isInCombatMode(true, true)))
    {
        const Ogre::Vector3 anchorPos = s_freeMoveAnchor->movement->pos;
        const float cullR  = s_fpEnemyClearRadius * 2.0f;  // coarse pre-cull ring
        const float cullR2 = cullR * cullR;
        float best2 = -1.0f;
        auto& scanChars = thisptr->getCharacterUpdateList();
        for (auto it = scanChars.begin(); it != scanChars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == s_freeMoveAnchor || !c->movement) continue;
            float dx = c->movement->pos.x - anchorPos.x;
            float dz = c->movement->pos.z - anchorPos.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= cullR2) continue;
            if (best2 >= 0.0f && d2 >= best2) continue;
            if (c->isDead() || c->isUnconcious()) continue;   // bodies underfoot: loot freely
            if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
            best2 = d2;
        }
        if (best2 >= 0.0f)
            s_fpEnemyNearestDist = sqrtf(best2);
    }

    // Turret/mounted-use camera-lock suspension.
    // Detects enter/exit of isUsingStationaryTurret to suspend movement injection
    // and camera-lock updates while mounted, then restores exactly once on exit.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player)
    {
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);
        if (atTurret && !s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = true;
            DebugLog("[WASDCombat] camera_lock_suspended_turret_enter");
        }
        else if (!atTurret && s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = false;
            DebugLog("[WASDCombat] camera_lock_restored_after_turret_exit");
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        }
    }
    // Menu/pause suspension — movement injection only; camera lock and anchor preserved throughout.
    // Detects transition into/out of engine pause (escape menu, options, save, load).
    // Explicitly excludes inventory-triggered auto-pause: loot UI suspension owns that path.
    // On false→true: one-time movement stop using the same fields as the structured release-stop path.
    // No stop on subsequent frames while suspended; no camera change on either edge.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool inventoryPausing = s_lootUiSuspendActive || (gui && gui->isAnyInventoryWindowOpen());
        bool menuPausedNow    = ou->isPaused() && !inventoryPausing;
        if (menuPausedNow && !s_menuSuspendActive)
        {
            s_menuSuspendActive = true;
            DebugLog("[WASDCombat] dc_menu_suspend_begin");
            CharMovement* mvM = s_freeMoveAnchor->movement;
            // Only kill momentum / cancel the path order if the character was
            // actively WASD-driving.  A character standing on a player-issued move
            // order has NO WASD momentum, and pausing must leave that order intact
            // — wiping it here was the "move order gets removed when the game
            // pauses while direct controlled" bug (user 2026-07-31).  WASD driving
            // owns no player order (it snaps to current pos on release), so stopping
            // it loses nothing.
            bool hadWasdMomentum = s_wasdMovementApplied
                                 || s_prevWasdDir.squaredLength() > 0.0001f;
            if (mvM && hadWasdMomentum && !isDownedButMovable(s_freeMoveAnchor))
            {
                s_prevWasdDir = Ogre::Vector3::ZERO;
                dcSnapCancelOrder(s_freeMoveAnchor);
                mvM->halt();
                mvM->desiredMotion = Ogre::Vector3::ZERO;
                mvM->moveLimit     = 0.0f;
                if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                    mvM->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                DebugLog("[WASDCombat] dc_menu_suspend_stop_applied");
            }
            else
            {
                DebugLog("[WASDCombat] dc_menu_suspend_stop_skipped_no_wasd_momentum");
            }
        }
        else if (!menuPausedNow && s_menuSuspendActive)
        {
            s_menuSuspendActive = false;
            DebugLog("[WASDCombat] dc_menu_suspend_end");
        }
        // Diagnostic: ou->isPaused() is true but inventory auto-pause is the cause.
        // Fires once per inventory-pause event; resets when the condition clears.
        { static bool s_skipLogged = false;
          if (ou->isPaused() && inventoryPausing && !s_menuSuspendActive)
          { if (!s_skipLogged) { s_skipLogged = true;
                DebugLog("[WASDCombat] dc_menu_suspend_skipped_inventory_pause"); } }
          else { s_skipLogged = false; } }
    }
    // Per-frame DC camera focus offset — raises focus toward chest at close zoom, tapers to zero at medium/far.
    if (s_mode == MODE_FREE_MOVE && g_dcCam.dcCameraCloseZoomChestOffset && !s_fpActive && !s_firstPersonActive &&
        s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player && ou->player->camera)
    {
        CameraClass*  cam    = ou->player->camera;
        Ogre::Vector3 camPos = cam->getCameraPos();
        Ogre::Vector3 ctr    = cam->getCenter();
        float dist = (camPos - ctr).length();
        // Full effect at/inside typical min-zoom distance (~15 units), gone by 5.5 m.
        const float zoomClose = 15.0f, zoomFar = 55.0f;
        float t   = (dist - zoomClose) / (zoomFar - zoomClose);
        float scl = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
        cam->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY + g_dcCam.dcCameraFocusOffsetY * scl;
    }
    s_prof_cameraLock += qpcNow() - _clStart; }   // end camera-lock timer

    // 4. HUD + X speed key.
    hudUpdate();

    if (s_xPressed && !s_lootUiSuspendActive)
    {
        s_xPressed = false;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op)
            {
                op->speedNext(nullptr);
                MoveSpeed ns = MoveSpeed(int((unsigned char)op->speedImageNamesIdx));
                if (ns < GROUPED)
                {
                    s_freeMoveAnchor->movement->setDesiredSpeedOrders(ns);
                    s_freeMoveAnchor->movement->setDesiredSpeed(ns);
                }
                DebugLog(ns == WALK ? "[WASDCombat] speed_walk"
                       : ns == JOG  ? "[WASDCombat] speed_jog"
                                    : "[WASDCombat] speed_run");
            }
        }
    }

    // 5. Pre-AI WASD application.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement && !s_lootUiSuspendActive)
    {
        // Promote deferred healing job once WASD is released.
        if (s_healingJobPending && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobPending = false;
            s_healingJobActive  = true;
            DebugLog("[WASDCombat] dc_heal_resumed_after_wasd_release");
        }
        // Mirror: an ACTIVE heal yields to a held WASD, exactly as a vanilla
        // point-click moves the patient out of treatment (medic re-paths and
        // resumes on release via the promotion above).  This is also the cure
        // for a STUCK heal flag: the auto-heal job stays queued through a
        // knockdown/KO so removeJob never fires to clear s_healingJobActive
        // (field 2026-06-13: ~90 s of medical_job_blocks_wasd after the
        // character went unconscious, WASD pinned to a single disengage step).
        // Demoting to pending on any WASD hold guarantees the player can
        // always move, and the heal resumes the instant they stop.
        else if (s_healingJobActive && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobActive  = false;
            s_healingJobPending = true;
            DebugLog("[WASDCombat] dc_heal_yielded_to_wasd");
        }

        bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
        if (!(bW || bA || bS || bD))
        {
            s_playDeadExitDone       = false;
        }
        if (!(bW || bA || bS || bD) && s_wasdDownedMovementActive)
        {
            // Keys are up but a persistent crawl order is live — cancel it
            // at the current position NOW (pre-AI), before pathfinding can
            // advance it another frame.  Level-triggered: catches any
            // release the step-9 edge stop might miss.  Crawl = keys held,
            // release = stay put.
            CharMovement* mvDC = s_freeMoveAnchor->movement;
            s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDC->pos);
            mvDC->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_release");
        }
        if (bW || bA || bS || bD)
        {
            // WASD breaks playing-dead exactly like a vanilla move order:
            // drop the prone state once per press and let the game decide
            // whether the character can actually stand — if not, they stay
            // crippled/downed and the crawl order below carries them.
            if (!s_playDeadExitDone
                && s_freeMoveAnchor->getProneState() == PS_PLAYING_DEAD)
            {
                s_freeMoveAnchor->setProneState(PS_NORMAL);
                s_playDeadExitDone = true;
                DebugLog("[WASDCombat] dc_play_dead_exit_on_wasd");
            }
            if (isDownedButMovable(s_freeMoveAnchor) && !downedOrderDriven(s_freeMoveAnchor))
            {
                // Downed — direct injection (charMovUpdate steering + step-9)
                // handles it like standing WASD; everywhere since v1.7.17.
                // Only job here: cancel a leftover crawl order from the
                // dormant order mode (flag can't be set anymore — safety).
                if (s_wasdDownedMovementActive)
                {
                    s_freeMoveAnchor->playerMoveOrderDefault(
                        nullptr, nullptr, s_freeMoveAnchor->movement->pos);
                    s_wasdDownedMovementActive = false;
                    DebugLog("[WASDCombat] dc_downed_crawl_switch_to_direct_indoors");
                }
            }
            else if (isDownedButMovable(s_freeMoveAnchor))
            {
                applyDownedMovement(bW, bA, bS, bD);
                s_wasdDownedMovementActive = true;
                static ULONGLONG s_downMoveTick = 0;
                ULONGLONG t = GetTickCount64();
                if (t - s_downMoveTick >= 2000) { s_downMoveTick = t;
                    ProneState prone = s_freeMoveAnchor->getProneState();
                    if (prone == PS_PLAYING_DEAD)   DebugLog("[WASDCombat] playing_dead_state_detected");
                    else if (prone == PS_CRIPPLED)  DebugLog("[WASDCombat] crippled_state_detected");
                    else                            DebugLog("[WASDCombat] downed_state_detected");
                    DebugLog("[WASDCombat] dc_crippled_state_detected");
                    DebugLog("[WASDCombat] dc_crippled_can_move=true");
                    DebugLog("[WASDCombat] dc_crippled_using_downed_movement_path");
                    DebugLog("[WASDCombat] wasd_downed_movement_attempt");
                    DebugLog("[WASDCombat] pointclick_movement_allowed_while_downed");
                    DebugLog("[WASDCombat] wasd_downed_movement_uses_pointclick_path");
                    DebugLog("[WASDCombat] wasd_downed_destination_created");
                    DebugLog("[WASDCombat] wasd_downed_destination_source_wasd");
                    if (s_freeMoveAnchor->isCrippled())
                    {
                        DebugLog("[WASDCombat] wasd_crippled_movement_allowed");
                        DebugLog("[WASDCombat] wasd_crippled_movement_success");
                    } }
            }
            else if (s_freeMoveAnchor->isUnconcious())
            {
                static ULONGLONG s_koTick = 0;
                ULONGLONG t = GetTickCount64();
                if (t - s_koTick >= 2000) { s_koTick = t;
                    DebugLog("[WASDCombat] wasd_true_unconscious_movement_blocked");
                    DebugLog("[WASDCombat] dc_crippled_can_move=false");
                    DebugLog("[WASDCombat] dc_crippled_move_blocked_reason=UNCONSCIOUS"); }
            }
            else if (!s_cameraLockTurretSuspend && !s_menuSuspendActive)
            {
                if (s_healingJobActive)
                {
                    if (!s_medicalJobSuppressedThisHold)
                    {
                        s_medicalJobSuppressedThisHold = true;
                        DebugLog("[WASDCombat] medical_job_blocks_wasd");
                    }
                }
                else
                {
                    { LONGLONG _wi = qpcNow();
                      applyPlayerMovement(bW, bA, bS, bD);
                      s_prof_wasdInject += qpcNow() - _wi; }
                    { ULONGLONG t = GetTickCount64();
                      if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                          if (g_log.debugVerbose)
                              DebugLog("[WASDCombat] movement_injection_allowed"); } }
                }
            }
        }
    }

    // 6. Run original game loop — AI + CharMovement::update + CombatClass::go run here.
    // Combat is untouched at the AI level (DC is locomotion-only); WASD priority
    // over combat locomotion is enforced inside charMovUpdate_hook.
    s_retreatLockGoSuppressed = false;

    // Foliage-sync (field 2026-07-25): the frame's foliage visibility / billboard-
    // facing pass runs INSIDE s_mainLoopOrig, and testing showed it samples the
    // camera BEFORE CameraClass::update fires — so even the pre-orig camera-hook
    // write was a frame late and grass blinked across the whole view while rotating
    // (frame-diff confirmed).  Drive the FP camera HERE, before the entire loop, so
    // that pass sees THIS frame's view.  fpDriveFrame runs again inside (camera hook)
    // to refine the eye from the freshly-animated head bone; the extra call is safe —
    // the cursor recentre makes the later call read a ~zero mouse delta (no double
    // yaw), and the throttled teleport won't double-fire (same eye).  Gated by
    // CamPreOrig; the head bone is one frame stale here (position lag is invisible),
    // but the ORIENTATION is current, which is what the rotation-flicker needs.
    if (s_fpCamPreOrig && s_firstPersonActive && ou && ou->player && ou->player->camera
        && s_freeMoveAnchor && s_freeMoveAnchor->movement && s_mode == MODE_FREE_MOVE
        && !ou->isLoadingFromASaveGame())
    {
        ManagementScreen* mgmtE = ManagementScreen::getSingleton();
        bool uiEarly = s_lootUiSuspendActive || (mgmtE && mgmtE->getVisible())
                     || (gui && (gui->isStatsWindowOpen() || gui->inDialogue()
                                 || gui->isPaused() || gui->isAnyInventoryWindowOpen()));
        fpDriveFrame(ou->player->camera, uiEarly);
    }

    s_mainLoopOrig(thisptr, time);

    // Post-loop load guard.
    if (!ou || !ou->player || ou->isLoadingFromASaveGame())
    {
        // Distinguish true LOADGAME teardown from temporary micro-load pointer loss.
        //   Hard shutdown only when:
        //     (a) ou == null          — game world destroyed (absolute teardown)
        //     (b) !ou->player AND isLoadingFromASaveGame() — player freed during confirmed load
        //   !ou->player WITHOUT a load signal = micro-load travel; preserve DC, do not shutdown.
        bool absoluteHard      = (!ou);
        bool confirmedLoadGame = (!absoluteHard) && (!ou->player) && ou->isLoadingFromASaveGame();
        bool hardLoss          = absoluteHard || confirmedLoadGame;

        if (hardLoss || !s_userWantsDC)
        {
            // True LOADGAME (hardLoss): full shutdown with s_dcShutdownInProgress.
            // DC not intended (!s_userWantsDC): normal load guard only, no shutdown flag.
            if (hardLoss && !s_dcShutdownInProgress)
            {
                s_dcShutdownInProgress = true;
                DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                DebugLog("[WASDCombat] dc_shutdown_triggered_by_true_load");
                if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                {
                    DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                    s_userWantsDC = false;
                }
                s_healingJobActive             = false;
                s_medicalJobSuppressedThisHold = false;
                s_freeMoveAnchor    = nullptr;
                s_selectedCharacter = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                s_anchorMovement    = nullptr;
                s_selectedMovement  = nullptr;
                s_prevAttackTarget  = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                s_savedFreeCameraMode    = false;
                s_cameraLockInvSuspend   = false;
                s_cameraLockTurretSuspend = false;
                s_savedCamFollowOffY     = 0.0f;
                DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
            }
            if (!s_loadGuardActive)
            {
                DebugLog("[WASDCombat] loadgame_signal_hard_shutdown");
                s_loadGuardActive = true;
                clearAllState();
                s_vHud.label = nullptr;
                s_vHud.shown = false;
                s_hudReady   = false;
                if (hardLoss)
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
            }
            return;
        }

        // s_userWantsDC = true and no confirmed LOADGAME signal:
        // Pointer temporarily invalid during chunk travel — preserve DC, skip post-AI steps.
        {
            static ULONGLONG s_skipLogTick = 0;
            ULONGLONG nowSk = GetTickCount64();
            if (nowSk - s_skipLogTick >= 2000) { s_skipLogTick = nowSk;
                DebugLog("[WASDCombat] dc_shutdown_skipped_microload"); }
        }
        return;
    }

    // Skip all DC post-processing if a hard shutdown is in progress.
    if (s_dcShutdownInProgress)
        return;


    // 8. Periodic squad-threat scan.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        ULONGLONG nowMs   = GetTickCount64();
        bool inCombat     = s_freeMoveAnchor->isInCombatMode(true, true);

        if (nowMs - s_lastScanTick >= SCAN_INTERVAL_MS)
        {
            s_lastScanTick = nowMs;
            LONGLONG _tsStart = qpcNow();

            bool foundEnemyTargetingAnchor     = false;
            bool foundEnemyTargetingSquad      = false;
            bool foundConsciousAllyUnderAttack = false;
            int  enemiesTargetingAnchor        = 0;
            int  nearbyEnemies                 = 0;
            int  pathfindingEnemies            = 0;

            auto& allChars = thisptr->getCharacterUpdateList();
            for (auto it = allChars.begin(); it != allChars.end(); ++it)
            {
                Character* c = *it;
                if (!c || c == s_freeMoveAnchor || !c->movement) continue;
                if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
                nearbyEnemies++;
                {
                    CombatClass* ccc = c->getCombatClass();
                    if (ccc)
                    {
                        swordStateEnum cs = ccc->getCombatState();
                        if (cs == TARGET_PATHFINDING || cs == TARGET_PATHFINDING_STARTUP)
                            pathfindingEnemies++;
                    }
                }

                Character* attacked = c->getAttackTarget().getCharacter();
                if (!attacked) continue;

                if (attacked == s_freeMoveAnchor)
                {
                    foundEnemyTargetingAnchor = true;
                    enemiesTargetingAnchor++;
                }
                else if (attacked->isPlayerCharacter())
                {
                    foundEnemyTargetingSquad = true;
                    if (!attacked->isUnconcious() && !attacked->isDead())
                        foundConsciousAllyUnderAttack = true;
                }
            }
            s_lastKnownEnemyCount   = enemiesTargetingAnchor;
            s_nearbyEnemyCount      = nearbyEnemies;
            s_pathfindingEnemyCount = pathfindingEnemies;
            s_prof_threatScan      += qpcNow() - _tsStart;

            {
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (foundEnemyTargetingAnchor && !s_enemyTargetingLogged)
                {
                    if (wasdNow && s_retreatLockGoSuppressed)
                    {
#if RETREAT_VERBOSE_DIAG
                        static ULONGLONG s_ignTick = 0;
                        ULONGLONG tign = GetTickCount64();
                        if (tign - s_ignTick >= 2000) { s_ignTick = tign;
                            DebugLog("[WASDCombat] enemy_targeting_ignored_by_retreat_lock"); }
#endif
                    }
                    else
                    {
                        if (g_log.debugVerbose)
                            DebugLog("[WASDCombat] enemy_targeting_player");
                        s_enemyTargetingLogged = true;
                    }
                }
                if (!inCombat) s_enemyTargetingLogged = false;

#if RETREAT_VERBOSE_DIAG
                // Verbose hostile count / blocked attacker summary.
                if (wasdNow && s_retreatLockGoSuppressed && enemiesTargetingAnchor > 0)
                {
                    static ULONGLONG s_hostileCountTick = 0;
                    ULONGLONG tenc = GetTickCount64();
                    if (tenc - s_hostileCountTick >= 2000) { s_hostileCountTick = tenc;
                        char hbuf[128];
                        sprintf_s(hbuf, sizeof(hbuf),
                            "[WASDCombat] hostile_count_during_retreat count=%d",
                            enemiesTargetingAnchor);
                        DebugLog(hbuf);
                        if (s_retreatBlockedAttackerCount > 0)
                        {
                            char abuf[128];
                            sprintf_s(abuf, sizeof(abuf),
                                "[WASDCombat] retreat_lock_blocked_attacker_count count=%d",
                                s_retreatBlockedAttackerCount);
                            DebugLog(abuf);
                            s_retreatBlockedAttackerCount = 0;
                        }
                    }
                }
#endif
            }

            s_squadThreat         = foundEnemyTargetingSquad;
            s_consciousAllyThreat = foundConsciousAllyUnderAttack;

            // Combat engagement tracking — target acquired/lost, range entry/exit.
            Character* curTgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (curTgt != s_prevAttackTarget)
            {
                if (curTgt && !s_prevAttackTarget)
                    DebugLog("[WASDCombat] combat_target_acquired");
                else if (!curTgt && s_prevAttackTarget)
                    DebugLog("[WASDCombat] combat_target_lost");
                s_prevAttackTarget = curTgt;
            }

            bool curInRange = false;
            if (curTgt && curTgt->movement)
            {
                Ogre::Vector3 toTgt = curTgt->movement->pos - s_freeMoveAnchor->movement->pos;
                toTgt.y = 0.0f;
                float tgtDist = toTgt.length();
                curInRange = (tgtDist <= ATTACK_RANGE);

                if (curInRange && !s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_entered_attack_range dist=%.0f", tgtDist);
                    DebugLog(buf);
                }
                else if (!curInRange && s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_left_attack_range dist=%.0f", tgtDist);
                    DebugLog(buf);
                }
            }
            s_prevTargetInRange = curInRange;
        }
    }

    // ----------------------------------------------------------------
    // 9. Post-AI WASD re-application + instant stop.
    // ----------------------------------------------------------------
    if (!(s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement) || s_lootUiSuspendActive)
    {
        // A live WASD crawl order must not keep moving the character into
        // a menu/suspend — cancel it at the current position.
        if (s_wasdDownedMovementActive
            && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_freeMoveAnchor->playerMoveOrderDefault(
                nullptr, nullptr, s_freeMoveAnchor->movement->pos);
            s_freeMoveAnchor->movement->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_suspend");
        }
        s_wasdWasActive    = false;
        s_combatWASDLogged = false;
        s_retreatLogged    = false;
        s_rmbPressedEdge   = false;  // discard stale click edges outside V-mode
        return;
    }

    bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
    bool wasdActive = bW || bA || bS || bD;
    bool inCombat   = s_freeMoveAnchor->isInCombatMode(true, true);

    // Earliest reliable player-click signal: RMB press edge from the poll
    // thread.  Input-level — cannot be blocked by any game-side dispatch
    // (field finding 2026-06-10: ground clicks during the hold never
    // reached PlayerInterface::playerMove, so the dispatcher hook alone
    // could not release the hold).  A real click while the hold is active
    // releases it immediately; the click's own order then proceeds under
    // vanilla control.  The playerMove and addOrder paths remain as
    // backup/diagnostics.
    if (s_rmbPressedEdge)
    {
        s_rmbPressedEdge = false;
        DebugLog("[WASDCombat] dc_click_input_seen");
        if (s_wasdHoldActive)
        {
            s_wasdHoldActive         = false;
            s_playerPointClickActive = true;
            s_holdPosValid           = false;
            DebugLog("[WASDCombat] dc_wasd_hold_cleared_by_click_input");
        }
    }

    // Combat mode transition — suppress log and detect flicker during WASD retreat.
    if (inCombat && !s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            DebugLog("[WASDCombat] combat_entered");
#if RETREAT_VERBOSE_DIAG
        else
        {
            ULONGLONG t = GetTickCount64();
            if (s_lastCombatEnterExitTick > 0 && t - s_lastCombatEnterExitTick < 500)
            {
                if (!s_combatFlickerLogged) { s_combatFlickerLogged = true;
                    DebugLog("[WASDCombat] combat_state_flicker_detected"); }
            }
            s_lastCombatEnterExitTick = t;
            static ULONGLONG s_enterSuppTick = 0;
            if (t - s_enterSuppTick >= 2000) { s_enterSuppTick = t;
                DebugLog("[WASDCombat] combat_enter_exit_suppressed_during_wasd"); }
        }
#endif
    }
    else if (!inCombat && s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            DebugLog("[WASDCombat] combat_exited");
#if RETREAT_VERBOSE_DIAG
        else
        {
            ULONGLONG t = GetTickCount64();
            if (s_lastCombatEnterExitTick > 0 && t - s_lastCombatEnterExitTick < 500)
            {
                if (!s_combatFlickerLogged) { s_combatFlickerLogged = true;
                    DebugLog("[WASDCombat] combat_state_flicker_detected"); }
            }
            s_lastCombatEnterExitTick = t;
        }
#endif
    }
    s_wasPrevInCombat = inCombat;

    if (wasdActive)
    {
        if (!s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_postWasdGraceActive  = false;
            s_combatReentryAllowed = false;

            // WASD always wins: release the post-WASD hold while keys are
            // held (it re-engages at the next release edge), and clear the
            // active point-click — the disengage order below replaces the
            // clicked order itself.  Capture whether a point-click was actually
            // pending BEFORE clearing it: the disengage move-order is ONLY needed
            // to cancel such a click.  When the player is merely navigating with
            // WASD (no pending click) there is nothing to cancel, so we must NOT
            // issue a move order — that order pathfinds, and inside a locked
            // building (esp. at the locked door/threshold, where isIndoors() reads
            // FALSE) the path-out fails and the vanilla "I can't get out of here"
            // bark fires (field 2026-06-20, log-proven: dc_disengage_order_issued
            // fired with indoors=0 at the threshold).
            bool hadPendingClick     = s_playerPointClickActive;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;

            if (isUsingStationaryTurret(s_freeMoveAnchor))
                DebugLog("[WASDCombat] stationary_crossbow_cancelled_by_wasd");

            // First WASD press: cancel any in-flight point-click/path order with a
            // disengage move order — but ONLY OUTDOORS.  playerMoveOrderDefault
            // ALWAYS issues a MOVE_CUS_ORDERED (task 29) that runs pathfinding, and
            // when the character is locked inside a building the path out runs
            // through a locked door -> the pathfinder fails -> the vanilla "I can't
            // get out of here" bark fires on every WASD press (field 2026-06-20;
            // confirmed it barked even with dest = current pos, because the move
            // order itself is the trigger, not the destination).  So skip the order
            // entirely indoors (same condition the release-snap already uses): there
            // setDirectMovement drives movement and the hold-clamp parks the
            // character on release, so no explicit order-cancel is needed.  Also
            // skipped while the crawl order owns movement (downed outdoors).
            // Only issue the disengage when a point-click was actually pending
            // (hadPendingClick) AND it won't bark (moveOrderMayBark) AND not downed.
            if (s_freeMoveAnchor->movement
                && hadPendingClick
                && !downedOrderDriven(s_freeMoveAnchor)
                && !moveOrderMayBark(s_freeMoveAnchor))
            {
                Ogre::Vector3 cancelDir;
                if (computeWASDDirection(bW, bA, bS, bD, cancelDir))
                {
                    Ogre::Vector3 dest = s_freeMoveAnchor->movement->pos;
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
#if RETREAT_VERBOSE_DIAG
                    DebugLog("[WASDCombat] wasd_pointclick_disengage_path_used");
                    DebugLog("[WASDCombat] pointclick_cancel_function_called_by_wasd");
                    if (inCombat || s_retreatLockGoSuppressed)
                    {
                        DebugLog("[WASDCombat] wasd_attack_target_cleared");
                        DebugLog("[WASDCombat] wasd_combat_focus_cleared");
                        DebugLog("[WASDCombat] wasd_retreat_matches_pointclick_behavior");
                    }
#endif
                }
            }
        }

        if (!s_cameraLockTurretSuspend && !s_menuSuspendActive
            && !downedOrderDriven(s_freeMoveAnchor))    // outdoor downed crawl is
                                                        // order-driven; halt() here
                                                        // was killing it every frame.
                                                        // Indoors-downed = direct
                                                        // injection, must run.
        {
            if (s_healingJobActive)
            {
                if (!s_medicalJobSuppressedThisHold)
                {
                    s_medicalJobSuppressedThisHold = true;
                    DebugLog("[WASDCombat] medical_job_blocks_wasd");
                }
            }
            else
            {
                { LONGLONG _wi = qpcNow();
                  applyPlayerMovement(bW, bA, bS, bD);
                  s_prof_wasdInject += qpcNow() - _wi; }
                { ULONGLONG t = GetTickCount64();
                  if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                      if (g_log.debugVerbose)
                          DebugLog("[WASDCombat] movement_injection_allowed"); } }
            }
        }
        // Note: downed/crippled movement is handled in step 5 (pre-AI) only —
        // playerMoveOrderDefault persists through the AI loop, no re-issue needed.

        // Retreat detection.
        if (inCombat && !s_retreatLogged && ou->player->camera)
        {
            Character* aiTarget = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (aiTarget && aiTarget->movement)
            {
                Ogre::Vector3 camFwd = ou->player->camera->getFacingDirection();
                camFwd.y = 0.0f;
                float cflen = camFwd.length();
                if (cflen > 0.001f)
                {
                    camFwd /= cflen;
                    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
                    Ogre::Vector3 mv = Ogre::Vector3::ZERO;
                    if (bW) mv += camFwd; if (bS) mv -= camFwd;
                    if (bD) mv += camRight; if (bA) mv -= camRight;
                    float mlen = mv.length();
                    if (mlen > 0.001f)
                    {
                        mv /= mlen;
                        Ogre::Vector3 toEnemy = aiTarget->movement->getPosition()
                                              - s_freeMoveAnchor->movement->getPosition();
                        toEnemy.y = 0.0f;
                        float elen = toEnemy.length();
                        if (elen > 0.001f && mv.dotProduct(toEnemy / elen) < -0.3f)
                            s_retreatLogged = true;
                    }
                }
            }
        }

        // Athletics XP bridge — CharMovement::periodicUpdate skips xpRunning when
        // movementMode == MOVE_DIRECTION; award manually during DC WASD movement.
        {
            static const float     WALK_THRESHOLD           = 0.1f;
            static const ULONGLONG ATHLETICS_XP_INTERVAL_MS = 1000;

            float currentSpd = s_freeMoveAnchor->movement->desiredSpeed;

            if (!s_dcShutdownInProgress
                && !s_cameraLockTurretSuspend
                && !s_healingJobActive
                && !isProtectedAnimationState(s_freeMoveAnchor)
                && currentSpd > WALK_THRESHOLD)
            {
                ULONGLONG nowXP   = GetTickCount64();
                ULONGLONG elapsed = nowXP - s_athleticsXpLastTick;

                if (s_athleticsXpLastTick == 0)
                {
                    // First movement frame — arm timer, do not award.
                    s_athleticsXpLastTick = nowXP;
                }
                else if (elapsed >= ATHLETICS_XP_INTERVAL_MS)
                {
                    CharStats* stXP = s_freeMoveAnchor->getStats();
                    if (stXP)
                    {
                        float deltaTime = elapsed / 1000.0f;
                        stXP->xpRunning(deltaTime, currentSpd);
                        if (g_log.debugLogging)
                        {
                            MoveSpeed tier = s_freeMoveAnchor->movement->speedOrders;
                            const char* tierName = (tier == WALK) ? "walk"
                                                 : (tier == JOG)  ? "jog"
                                                 :                  "run";
                            char xpBuf[96];
                            sprintf_s(xpBuf, sizeof(xpBuf),
                                "[WASDCombat] dc_xp_movement_bridge_tick skill=Athletics speed=%s",
                                tierName);
                            DebugLog(xpBuf);
                        }
                    }
                    s_athleticsXpLastTick = nowXP;
                }
            }
            else
            {
                // Guard failed or speed too low — reset so the next movement hold re-arms.
                s_athleticsXpLastTick = 0;
            }
        }
    }
    else
    {
        if (s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_wasdReleasedTick     = GetTickCount64();
            s_postWasdGraceActive  = true;
            s_postWasdGraceStart   = GetTickCount64();
            s_combatReentryAllowed = false;

            // Nudge-tap check — consume tap-start timestamp and decide release path.
            ULONGLONG tapMs      = s_wasdTapStartMs;
            ULONGLONG tapElapsed = (tapMs > 0) ? (GetTickCount64() - tapMs) : ~0ULL;
            bool isNudgeTap      = (tapElapsed <= g_loco.wasdNudgeTapWindowMs);
            s_wasdTapStartMs     = 0;

            if (isNudgeTap && !isDownedButMovable(s_freeMoveAnchor))
            {
                // Nudge-safe release: zero velocity, clear stale dir, snap anchor — no facing correction.
                DebugLog("[WASDCombat] wasd_nudge_tap_detected");
                CharMovement* mvN = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvN)
                {
                    mvN->halt();
                    mvN->desiredMotion    = Ogre::Vector3::ZERO;
                    mvN->moveLimit        = 0.0f;
                    mvN->currentMotion    = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    DebugLog("[WASDCombat] wasd_nudge_stop_no_turnaround");
                    s_prevWasdDir = Ogre::Vector3::ZERO;
                    DebugLog("[WASDCombat] stale_movement_vector_cleared");
                    dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                    DebugLog("[WASDCombat] anchor_snapped_no_facing_change");
                }
            }
            else
            {
                // Structured release-stop sequence — fires once on full WASD release.
                if (g_release.wasdStopOnRelease && s_freeMoveAnchor && s_freeMoveAnchor->movement)
                {
                    if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_detected");
                    CharMovement* mvR = s_freeMoveAnchor->movement;

                    // STARTUP_STATE (attack windup) alone must not block release-stop.
                    // Compute whether a *real* committed action is blocking: re-use
                    // isCommittedAction for the full check, then subtract STARTUP_STATE.
                    bool releaseCommitted = false;
                    if (isCommittedAction(s_freeMoveAnchor))
                    {
                        CombatClass*   ccRel = s_freeMoveAnchor->getCombatClass();
                        swordStateEnum stRel = ccRel ? ccRel->getCombatState() : COMBAT_FINISHED;
                        bool onlyStartup = (stRel == STARTUP_STATE)
                                        && !isProtectedAnimationState(s_freeMoveAnchor)
                                        && !s_healingJobActive;
                        releaseCommitted = !onlyStartup;
                    }

                    if (releaseCommitted)
                    {
                        DebugLog("[WASDCombat] wasd_release_stop_skipped_committed_action");
                    }
                    else if (!downedOrderDriven(s_freeMoveAnchor))
                    {
                        // Standing AND indoors-downed (direct injection) both
                        // stop here; outdoor downed crawl is order-driven and
                        // stops in the downed block below.
                        // 1. Zero DC movement vector.
                        s_prevWasdDir = Ogre::Vector3::ZERO;
                        DebugLog("[WASDCombat] wasd_release_vector_zeroed");

                        // 2. Snap anchor — cancel any pending pathfind destination.
                        if (g_release.wasdAnchorSnapOnRelease)
                        {
                            dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_anchor_snapped");
                        }

                        // 3. Zero injected velocity — halt + force-zero all motion fields.
                        if (g_release.wasdZeroVelocityOnRelease)
                        {
                            mvR->halt();
                            mvR->desiredMotion = Ogre::Vector3::ZERO;
                            mvR->moveLimit     = 0.0f;
                            if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                                mvR->currentMotion = Ogre::Vector3::ZERO;
                            s_wasdMovementApplied = false;
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_velocity_zeroed");
                        }
                    }
                    // else: downed — handled by existing downed-stop block below.
                }
            }

            // Downed/crippled instant stop.
            // Gate on s_wasdDownedMovementActive alone — NOT isDownedButMovable.
            // isDownedButMovable may flip false mid-release (e.g. isCurrentlyGettingUp
            // becomes true inside the AI loop), yet the WASD-issued destination is
            // still pending and must be cancelled.
            if (s_wasdDownedMovementActive)
            {
                CharMovement* mvDown = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvDown)
                {
                    // playerMoveOrderDefault at current pos cancels the MOVE job entirely,
                    // not just the CharMovement destination field.
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDown->pos);
                    mvDown->halt();
                }
                ProneState proneStop = s_freeMoveAnchor ? s_freeMoveAnchor->getProneState()
                                                        : PS_NORMAL;
                DebugLog("[WASDCombat] wasd_downed_key_released");
                DebugLog("[WASDCombat] wasd_downed_destination_cleared");
                DebugLog("[WASDCombat] wasd_downed_cached_direction_cleared");
                DebugLog("[WASDCombat] wasd_downed_movement_stopped");
                if (proneStop == PS_CRIPPLED ||
                    (s_freeMoveAnchor && s_freeMoveAnchor->isCrippled()))
                    DebugLog("[WASDCombat] wasd_crippled_instant_stop_applied");
                DebugLog("[WASDCombat] wasd_downed_pointclick_destination_not_persisted");
            }
            else if (s_freeMoveAnchor && isDownedButMovable(s_freeMoveAnchor))
            {
                // Destination was not WASD-created — likely a real player point-click.
                DebugLog("[WASDCombat] vanilla_pointclick_downed_destination_preserved");
            }
            s_wasdDownedMovementActive    = false;
            s_retreatLockEverActive       = false;
            s_retreatSessionCacheCount    = 0;  // clear session cache on WASD release
            s_retreatTargetsProcessed     = 0;
            s_retreatTargetsCachedSkipped = 0;
            s_retreatBlockedAttackerCount    = 0;
            s_medicalJobSuppressedThisHold   = false;
            // Standing instant stop after AI loop (fallback — fires only if the structured
            // release-stop sequence above did not already clear s_wasdMovementApplied).
            CharMovement* mvStop = s_freeMoveAnchor->movement;
            if (mvStop && s_wasdMovementApplied &&
                !isCommittedAction(s_freeMoveAnchor) &&
                !isDownedButMovable(s_freeMoveAnchor))
            {
                Ogre::Vector3 velPre = mvStop->currentMotion;
                mvStop->halt();
                mvStop->desiredMotion = Ogre::Vector3::ZERO;
                mvStop->moveLimit     = 0.0f;
                if (g_loco.wasdDecelerationMultiplier > 1.0f)
                    mvStop->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                s_prevWasdDir         = Ogre::Vector3::ZERO;

                char buf[192];
                sprintf_s(buf, sizeof(buf),
                    "[WASDCombat] wasd_released instant_stop vel_pre=(%.1f,%.1f,%.1f)",
                    velPre.x, velPre.y, velPre.z);
                DebugLog(buf);
            }

            // Engage the post-WASD hold: WASD left the character here —
            // keep them here until a new point-click, the next WASD press,
            // or V OFF.  Any click made during the drive was already
            // cancelled by the release anchor-snap above, so its flag is
            // cleared too.  (Enforcement + engage log live in the
            // charMovUpdate hold branch and the end-of-frame clamp.)
            s_wasdHoldActive         = true;
            s_playerPointClickActive = false;
        }
    }

    s_wasdWasActive = wasdActive;

    // Post-WASD grace period: suppress combat re-entry unless enemy is close and actively targeting.
    if (!wasdActive)
    {
        if (s_postWasdGraceActive)
        {
            ULONGLONG elapsed = GetTickCount64() - s_postWasdGraceStart;
            if (elapsed >= POST_WASD_GRACE_MS && !isCommittedAction(s_freeMoveAnchor))
            {
                s_postWasdGraceActive  = false;
                s_combatReentryAllowed = true;
                DebugLog("[WASDCombat] combat_state_restored_after_wasd_release");
            }
            else
            {
                // Allow re-engagement only if the old target is within close range and attacking us.
                bool enemyCloseAndActive = false;
                Character* tgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
                if (tgt && tgt->movement)
                {
                    float dist = (tgt->movement->pos - s_freeMoveAnchor->movement->pos).length();
                    bool tgtAttackingUs = (tgt->getAttackTarget().getCharacter() == s_freeMoveAnchor);
                    if (dist <= POST_WASD_REENGAGEMENT_RANGE && tgtAttackingUs)
                        enemyCloseAndActive = true;
                }
                s_combatReentryAllowed = enemyCloseAndActive;
            }
        }
    }
    else
    {
        s_combatReentryAllowed = false;
    }

    // Retreat state tracking — WASD held while combat AI has been suppressed.
    // s_retreatLockEverActive is sticky: set on the first frame go() is suppressed
    // during this WASD hold, cleared on release.  This prevents log bursts on frames
    // where Kenshi skips calling go() (no enemy nearby) while WASD is still held.
    {
        bool curRetreat = wasdActive && s_retreatLockEverActive;
        if (curRetreat && !s_wasdRetreatActive)
        {
            s_wasdRetreatActive      = true;
            s_retreatActiveStartTick = GetTickCount64();
            s_retreatCleanLogged     = false;
            s_combatFlickerLogged    = false;
#if RETREAT_VERBOSE_DIAG
            DebugLog("[WASDCombat] wasd_retreat_state_active");
#endif
        }
        else if (!curRetreat && s_wasdRetreatActive)
        {
            s_wasdRetreatActive = false;
#if RETREAT_VERBOSE_DIAG
            if (!wasdActive)
                DebugLog("[WASDCombat] wasd_manual_retreat_lock_released");
#endif
        }
        if (s_wasdRetreatActive && !s_retreatCleanLogged &&
            GetTickCount64() - s_retreatActiveStartTick >= 1000)
        {
            s_retreatCleanLogged = true;
            DebugLog("[WASDCombat] retreat_clean_locomotion_confirmed");
            if (s_lastKnownEnemyCount > 1)
                DebugLog("[WASDCombat] retreat_locomotion_stable_under_multi_chase");
            DebugLog("[WASDCombat] retreat_perf_optimized");
            char scanBuf[128];
            sprintf_s(scanBuf, sizeof(scanBuf),
                "[WASDCombat] retreat_scan_interval_ms %llu cache_size=%d processed=%d skipped=%d",
                JOB_REMOVAL_INTERVAL_MS, s_retreatSessionCacheCount,
                s_retreatTargetsProcessed, s_retreatTargetsCachedSkipped);
            DebugLog(scanBuf);
#if RETREAT_VERBOSE_DIAG
            {
            char fb[64];
            sprintf_s(fb, sizeof(fb), "[WASDCombat] retreat_attacker_cache_size count=%d",
                s_retreatSessionCacheCount);
            DebugLog(fb);
            if (s_retreatTargetsCachedSkipped > 0)
                DebugLog("[WASDCombat] retreat_fast_path_used");
            }
#endif
        }
    }

    // CombatClass state transition tracking.
    { LONGLONG _ctStart = qpcNow();
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        CombatClass* cc2 = s_freeMoveAnchor->getCombatClass();
        if (cc2)
        {
            swordStateEnum curState = cc2->getCombatState();
            if (curState != s_lastCombatState)
            {
#if RETREAT_VERBOSE_DIAG
                {
                const char* stateNames[] = {
                    "CHOP_WEAPON", "BLOCK", "REACTION_BLOCK", "STARTUP_STATE",
                    "DECISION", "CIRCLE_MENACINGLY", "WAIT_MENACINGLY", "HESITATE",
                    "STUMBLE", "COMBAT_FINISHED", "TARGET_PATHFINDING_STARTUP", "TARGET_PATHFINDING"
                };
                const char* curName  = (curState           < 12) ? stateNames[curState]           : "UNKNOWN";
                const char* prevName = (s_lastCombatState  < 12) ? stateNames[s_lastCombatState]  : "UNKNOWN";
                char buf[192];
                sprintf_s(buf, sizeof(buf),
                    "[WASDCombat] combat_state %s -> %s", prevName, curName);
                DebugLog(buf);
                }

                if (wasdActive)
                {
                    if      (curState == CHOP_WEAPON)
                        DebugLog("[WASDCombat] wasd_prevented_run_back_to_target");
                    else if (curState == BLOCK)
                        DebugLog("[WASDCombat] block_pulse_suppressed_during_wasd");
                    else if (curState == STARTUP_STATE)
                        DebugLog("[WASDCombat] startup_state_suppressed_during_wasd");
                    else if (curState == TARGET_PATHFINDING || curState == TARGET_PATHFINDING_STARTUP)
                        DebugLog("[WASDCombat] target_pathfinding_suppressed_during_wasd");
                    else if (curState == CIRCLE_MENACINGLY)
                        DebugLog("[WASDCombat] circle_menacingly_suppressed_during_wasd");
                }
#endif

                if (curState == CHOP_WEAPON)
                {
                    s_attackCommitmentActive = true;
                    s_attackCommitmentStart  = GetTickCount64();
#if RETREAT_VERBOSE_DIAG
                    DebugLog("[WASDCombat] attack_commitment_started");
#endif
                    if (s_wasdReleasedTick > 0 && (GetTickCount64() - s_wasdReleasedTick) < 5000)
                        DebugLog("[WASDCombat] attack_after_wasd_release");
                }
                else if (s_lastCombatState == CHOP_WEAPON)
                {
                    bool wasdAtEnd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                    if (wasdAtEnd)
                    {
#if RETREAT_VERBOSE_DIAG
                        DebugLog("[WASDCombat] attack_commitment_cancelled_by_wasd");
#endif
                        s_attackCommitmentActive = false;
                        s_attackCommitmentStart  = 0;
                    }
                    else if (curState == DECISION)
                    {
#if RETREAT_VERBOSE_DIAG
                        DebugLog("[WASDCombat] attack_recovery_phase");
#endif
                        // Keep commitment active through recovery.
                    }
                    else if (curState == CIRCLE_MENACINGLY)
                    {
#if RETREAT_VERBOSE_DIAG
                        DebugLog("[WASDCombat] attack_interrupted_by_circle_menacingly");
#endif
                        s_attackCommitmentActive = false;
                        s_attackCommitmentStart  = 0;
                    }
                    else
                    {
#if RETREAT_VERBOSE_DIAG
                        DebugLog("[WASDCombat] attack_commitment_completed");
#endif
                        s_attackCommitmentActive = false;
                        s_attackCommitmentStart  = 0;
                    }
                }
                else if (s_attackCommitmentActive && s_lastCombatState == DECISION)
                {
                    // Recovery phase ended.
                    bool wasdAtEnd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
#if RETREAT_VERBOSE_DIAG
                    if (wasdAtEnd)
                        DebugLog("[WASDCombat] attack_commitment_cancelled_by_wasd");
                    else if (curState == CIRCLE_MENACINGLY)
                        DebugLog("[WASDCombat] attack_interrupted_by_circle_menacingly");
                    else if (curState != CHOP_WEAPON)
                        DebugLog("[WASDCombat] attack_commitment_completed");
#else
                    (void)wasdAtEnd;
#endif
                    if (curState != CHOP_WEAPON)
                    {
                        s_attackCommitmentActive = false;
                        s_attackCommitmentStart  = 0;
                    }
                }

                s_lastCombatState = curState;
            }
        }
    }

    // Protected animation state transition tracking.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool nowProtected = isProtectedAnimationState(s_freeMoveAnchor);

        if (nowProtected && !s_wasProtectedState)
        {
            if (s_freeMoveAnchor->isCurrentlyGettingUp)
                DebugLog("[WASDCombat] getup_animation_started");
            else
                DebugLog("[WASDCombat] knockdown_or_stagger_started");
        }
        else if (!nowProtected && s_wasProtectedState)
        {
            DebugLog("[WASDCombat] protected_animation_completed");
            DebugLog("[WASDCombat] vmode_runtime_resumed");
        }

        s_wasProtectedState = nowProtected;
    }
    s_prof_combatTarget += qpcNow() - _ctStart; }  // end combat-target timer

    // ----------------------------------------------------------------
    // Final hold clamp — LAST DC-controlled write point in the frame.
    // Everything (AI, Taskers, indoor routing, pathing) has already run.
    // While the post-WASD hold is enforcing, restore the anchor's X/Z and
    // zero motion so no system that moved the character mid-frame keeps
    // the displacement.  Y is left free for gravity/ramp settling.
    // ----------------------------------------------------------------
    if (!wasdActive)
    {
        const char* gateReason = "";
        bool allowFC = computeHoldDecision(s_freeMoveAnchor, &gateReason);
        if (!allowFC)
        {
            CharMovement* mvFC = s_freeMoveAnchor->movement;
            if (mvFC)
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = mvFC->pos;
                    s_holdPosValid = true;
                }
                mvFC->pos.x         = s_holdPos.x;
                mvFC->pos.z         = s_holdPos.z;
                mvFC->desiredMotion = Ogre::Vector3::ZERO;
                mvFC->currentMotion = Ogre::Vector3::ZERO;
                mvFC->moveLimit     = 0.0f;
            }
        }
        else
        {
            s_holdPosValid = false;
        }

        // dc_authority_gate diagnostic — on state/reason change + 1 s heartbeat.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = (allowFC != s_authGateLastAllow)
                      || (strcmp(gateReason, s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = allowFC;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), gateReason);
            char gbuf[128];
            sprintf_s(gbuf, sizeof(gbuf),
                "[WASDCombat] dc_authority_gate allowVanillaMove=%d reason=%s",
                (int)allowFC, gateReason);
            DebugLog(gbuf);
        }
    }
    else
    {
        s_holdPosValid = false;

        // Gate diagnostic during WASD drive: WASD owns locomotion.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = s_authGateLastAllow
                      || (strcmp("wasd", s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = false;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), "wasd");
            DebugLog("[WASDCombat] dc_authority_gate allowVanillaMove=0 reason=wasd");
        }
    }

    // dc_perf: emit aggregate profiling line once per second.
    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_prof_windowStart >= 1000)
        {
            float f = (float)s_profFreq / 1000.0f;  // ticks → ms divisor
            char perfBuf[512];
            sprintf_s(perfBuf, sizeof(perfBuf),
                "[WASDCombat] dc_perf mainLoop_ms=%.2f charMove_ms=%.2f"
                " playerControl_ms=%.2f committedAction_ms=%.2f"
                " threatScan_ms=%.2f cameraLock_ms=%.2f"
                " wasdInject_ms=%.2f combatTarget_ms=%.2f"
                " enemyCount=%d nearbyCombatantCount=%d dcMode=%d wasdHeld=%d",
                s_prof_mainLoop      / f,
                s_prof_charMove      / f,
                s_prof_playerControl / f,
                s_prof_committedAct  / f,
                s_prof_threatScan    / f,
                s_prof_cameraLock    / f,
                s_prof_wasdInject    / f,
                s_prof_combatTarget  / f,
                s_lastKnownEnemyCount,
                s_nearbyEnemyCount,
                (int)(s_mode == MODE_FREE_MOVE),
                (int)(s_wHeld || s_aHeld || s_sHeld || s_dHeld));
            DebugLog(perfBuf);

            // Chase diagnostics — emitted once per second alongside dc_perf.
            {
                bool wasdNowC    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                bool enemiesChasing = (s_lastKnownEnemyCount > 0);
                char chaseBuf[256];
                sprintf_s(chaseBuf, sizeof(chaseBuf),
                    "[WASDCombat] dc_chase_perf enemyCount=%d nearbyCombatantCount=%d"
                    " combatStateFlaps=%d pathRecalcSuspected=%d",
                    s_lastKnownEnemyCount, s_nearbyEnemyCount,
                    s_chaseFlapsCount, (int)(s_pathfindingEnemyCount > 0));
                DebugLog(chaseBuf);
                if (enemiesChasing)
                {
                    if (wasdNowC)
                        DebugLog("[WASDCombat] dc_wasd_chase_active");
                    else
                        DebugLog("[WASDCombat] dc_point_click_chase_compare");
                }
                s_chaseFlapsCount = 0;
            }

            // Reset accumulators for the next window.
            s_prof_mainLoop      = 0;
            s_prof_charMove      = 0;
            s_prof_playerControl = 0;
            s_prof_committedAct  = 0;
            s_prof_threatScan    = 0;
            s_prof_cameraLock    = 0;
            s_prof_wasdInject    = 0;
            s_prof_combatTarget  = 0;
            s_prof_windowStart   = nowMs;
        }
    }
}

// -----------------------------------------------------------------------
// ForgottenGUI::showTradeWindow hook — ⛔ NOT INSTALLED since 2026-08-05: the
// RVA is stale (pre-6/21 RE_Kenshi exe); the patch landed mid-instruction in
// an unrelated conversion helper and corrupted it, while loot/trade detection
// ran (and still runs) on the mainLoop GUI poll.  Function kept for when the
// RVA is re-derived (install via verifyPatchSiteBytes; see startPlugin).
// Original description follows.
//
// ForgottenGUI::showTradeWindow hook — earliest possible loot/trade detection.
//
// showTradeWindow is called the instant the player opens a loot/trade window,
// before isAnyInventoryWindowOpen() returns true and before playerControl_hook
// fires for that frame.  Setting s_lootUiSuspendActive here blocks the very
// first startTrackCharacter call that would otherwise produce the opening sound.
//
// s_lootUiWasPrevOpen is intentionally NOT set here — the mainLoop step-2
// detection manages that flag once the window is confirmed open.  Setting it
// here would cause the close-side to immediately fire (window not yet open).
// -----------------------------------------------------------------------
static void (*s_showTradeWindowOrig)(ForgottenGUI*, const hand&, const hand&, TradeWindowType);

static void showTradeWindow_hook(ForgottenGUI* thisptr, const hand& a, const hand& b, TradeWindowType type)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedTrade) {
            s_hookBlockLoggedTrade = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=showTradeWindow"); }
        s_showTradeWindowOrig(thisptr, a, b, type); return; }
    // A trade/loot/corpse window opened — a FOREIGN party is involved.  Latch
    // this so the inventory face-cam never treats it as own inventory; cleared
    // when all inventory windows close (avoids the stale-hand-field problem).
    if (!s_dcShutdownInProgress)
        s_tradeWindowActive = true;
    // Move-through mode (InventoryFaceCam=false) keeps DC live during loot/trade,
    // so do NOT arm the early suspend there — only the face-cam path wants it.
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && s_settingInventoryFaceCam)
    {
        if (!s_lootUiSuspendActive)
        {
            s_lootUiSuspendActive  = true;
            s_lootSuspendStartTick = GetTickCount64();
#if LOOT_DIAG
            DebugLog("[WASDCombat] loot_interaction_edge_detected_before_ui_open");
            DebugLog("[WASDCombat] early_loot_suspend_applied");
            DebugLog("[WASDCombat] second_loot_sound_prevented");
            char dbuf[64];
            sprintf_s(dbuf, sizeof(dbuf),
                "[WASDCombat] loot_open_debounce_ms %llu", LOOT_SUSPEND_DEBOUNCE_MS);
            DebugLog(dbuf);
#endif
        }
#if LOOT_DIAG
        else
        {
            DebugLog("[WASDCombat] duplicate_initial_loot_open_blocked");
        }
#endif
    }
    s_showTradeWindowOrig(thisptr, a, b, type);
}

// -----------------------------------------------------------------------
// PlayerInterface::playerControl hook
// -----------------------------------------------------------------------
static void (*s_playerControlOrig)(PlayerInterface*, InputHandler&);

static void playerControl_hook(PlayerInterface* thisptr, InputHandler& k)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedPCtrl) {
            s_hookBlockLoggedPCtrl = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=playerControl"); }
        s_playerControlOrig(thisptr, k); return; }
    ScopeTimer _tPC(s_prof_playerControl);
    if (!ou || !ou->player || ou->isLoadingFromASaveGame())
    {
        s_playerControlOrig(thisptr, k);
        return;
    }
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive)
    {
        bool wasdHeld = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);

        if (atTurret && !wasdHeld)
        {
            // Manning a turret: preserve directional inputs for turret aiming.
            static ULONGLONG s_turretProtTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_turretProtTick >= 2000) { s_turretProtTick = t;
                DebugLog("[WASDCombat] stationary_crossbow_action_detected");
                DebugLog("[WASDCombat] stationary_action_protected");
                DebugLog("[WASDCombat] vmode_suppression_skipped_stationary_crossbow"); }
        }
        else
        {
            // Suppress camera pan inputs while DC is active.
            // Zeroing k.up/down/left/right prevents keyboard scroll from detaching
            // the camera lock.  During WASD hold this also blocks AI facing override.
            k.up    = false;
            k.down  = false;
            k.left  = false;
            k.right = false;
            k.pgup  = false;
            k.pgdn  = false;
            static ULONGLONG s_panSupLogTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_panSupLogTick >= 2000) { s_panSupLogTick = t;
                if (g_log.debugLogging && g_log.verboseMovementLogs)
                    DebugLog("[WASDCombat] free_camera_input_suppressed_dc"); }
        }
    }
    s_playerControlOrig(thisptr, k);

    if (s_mode == MODE_FREE_MOVE
        && !s_fpActive && !s_firstPersonActive   // OTS/FP own a DETACHED camera node — never
                                     // re-track it here (this fired every frame
                                     // during OTS, fighting the detached camera
                                     // and breaking it after a reload).
        && !s_lootUiSuspendActive
        && !s_cameraLockTurretSuspend
        && !s_dcPtrLossActive
        && s_freeMoveAnchor != nullptr
        && !ou->player->isTrackingCharacter())
    {
        ou->player->startTrackCharacter(s_freeMoveAnchor);
        DebugLog("[WASDCombat] camera_restored_after_external_detach");
    }
}

// -----------------------------------------------------------------------
// taskTypeName — used by removeJob_hook
// -----------------------------------------------------------------------
static const char* taskTypeName(TaskType t)
{
    switch (t)
    {
        case MELEE_ATTACK:               return "MELEE_ATTACK";
        case FOCUSED_MELEE_ATTACK:       return "FOCUSED_MELEE_ATTACK";
        case CHOOSE_ENEMY_AND_ATTACK:    return "CHOOSE_ENEMY_AND_ATTACK";
        case CHOOSE_ATTACKER_OF_ALLY:    return "CHOOSE_ATTACKER_OF_ALLY";
        case ATTACK_CHARACTERS_ATTACKER: return "ATTACK_CHARACTERS_ATTACKER";
        case ATTACK_ATTACKERS_OF:        return "ATTACK_ATTACKERS_OF";
        case PROTECT_ALLIES:             return "PROTECT_ALLIES";
        case ATTACK_ENEMIES:             return "ATTACK_ENEMIES";
        case JOB_MEDIC:                  return "JOB_MEDIC";
        case FIRST_AID_ORDER:            return "FIRST_AID_ORDER";
        case FIRST_AID_ROBOT:            return "FIRST_AID_ROBOT";
        case JOB_REPAIR_ROBOT:           return "JOB_REPAIR_ROBOT";
        case SPLINT_ORDER:               return "SPLINT_ORDER";
        case SPLINT_JOB:                 return "SPLINT_JOB";
        case HEAL_MY_LEGS:               return "HEAL_MY_LEGS";
        default: { static char buf[32]; sprintf_s(buf, sizeof(buf), "TASK_%d", (int)t); return buf; }
    }
}

// -----------------------------------------------------------------------
// Character::removeJob hook — misclassification guard
// -----------------------------------------------------------------------
static void (*s_removeJobOrig)(Character* thisptr, TaskType t);

static void removeJob_hook(Character* thisptr, TaskType t)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedRemJob) {
            s_hookBlockLoggedRemJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=removeJob"); }
        s_removeJobOrig(thisptr, t); return; }
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        bool wasdHeld     = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool isAttackJob  = (t == MELEE_ATTACK            || t == FOCUSED_MELEE_ATTACK  ||
                              t == CHOOSE_ENEMY_AND_ATTACK || t == ATTACK_CHARACTERS_ATTACKER ||
                              t == ATTACK_ENEMIES);
        bool isMedicalJob = (t == JOB_MEDIC       || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER    || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Clear healing job flags — DC movement injection resumes.
        if (isMedicalJob && (s_healingJobActive || s_healingJobPending))
        {
            s_healingJobActive  = false;
            s_healingJobPending = false;
            DebugLog("[WASDCombat] medical_job_ended_restore_dc");
            DebugLog("[WASDCombat] healing_action_completed");
        }

        if (isAttackJob)
        {
            if (wasdHeld)
            {
#if RETREAT_VERBOSE_DIAG
                static ULONGLONG s_chaseTick = 0;
                ULONGLONG tn = GetTickCount64();
                if (tn - s_chaseTick >= 2000) { s_chaseTick = tn;
                    DebugLog("[WASDCombat] wasd_chase_cancelled");
                    DebugLog("[WASDCombat] wasd_old_attack_target_ignored"); }
#endif
            }
            else
            {
                char buf[192];
                sprintf_s(buf, sizeof(buf),
                    "[WASDCombat] WARN removejob_attack_job_misclassification type=%s",
                    taskTypeName(t));
                DebugLog(buf);
            }
        }
    }
    s_removeJobOrig(thisptr, t);
}

// -----------------------------------------------------------------------
// Character::addJob hook — log attack job creation
// -----------------------------------------------------------------------
// -----------------------------------------------------------------------
// PlayerInterface::playerMove hook — ⛔ NOT INSTALLED since 2026-08-05: the
// RVA below is stale (pre-6/21 RE_Kenshi exe) and patching it caused the
// packbull/hive-home navmesh crash.  Function kept for when the RVA is
// re-derived (install via verifyPatchSiteBytes; see startPlugin).  Original
// description follows.
//
// PlayerInterface::playerMove — real player click dispatcher (RVA
// 0x7F95F0, private member, hooked by RVA like showTradeWindow).  Hybrid
// model: every click passes through normally (vanilla owns point-click
// movement in V-mode).  The only DC action is clearing the post-WASD hold
// — a new click is explicit player intent and releases the "stay where
// WASD left you" state BEFORE the dispatcher runs, so the click's own
// door routing is never suppressed.
// -----------------------------------------------------------------------
static void (*s_playerMoveOrig)(PlayerInterface* thisptr, const Ogre::Vector3& pos,
                                Building* destBuilding);

static void playerMove_hook(PlayerInterface* thisptr, const Ogre::Vector3& pos,
                            Building* destBuilding)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedPMove) {
            s_hookBlockLoggedPMove = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=playerMove"); }
        s_playerMoveOrig(thisptr, pos, destBuilding); return; }

    // OTS action mode: WASD + camera IS the movement scheme; ground point-
    // click move orders are swallowed (cursor = crosshair, action-game feel).
    // LMB selection, RMB hold-menu, and menu-issued orders (via addOrder, a
    // different path) all still work.
    if (s_fpActive || s_firstPersonActive)
    {
        static ULONGLONG s_otsClickSupLogTick = 0;
        ULONGLONG nowOC = GetTickCount64();
        if (nowOC - s_otsClickSupLogTick >= 1000)
        {
            s_otsClickSupLogTick = nowOC;
            DebugLog("[WASDCombat] dc_cam_pointclick_move_suppressed");
        }
        return;
    }

    // While an inventory / loot / trade window is open in DC mode, the cursor is
    // freed for the UI — a world click behind the window must NOT walk the
    // character (and must not fight the inventory face-cam).  Menu-issued orders
    // use addOrder, a different path, so equip/transfer still work.
    if (s_mode == MODE_FREE_MOVE && s_lootUiSuspendActive)
    {
        static ULONGLONG s_invClickSupLogTick = 0;
        ULONGLONG nowIC = GetTickCount64();
        if (nowIC - s_invClickSupLogTick >= 1000)
        {
            s_invClickSupLogTick = nowIC;
            DebugLog("[WASDCombat] dc_pointclick_move_suppressed_inventory");
        }
        return;
    }

    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        // Explicit player intent: mark the click active and release the
        // hold BEFORE the dispatcher runs — the order is never suppressed
        // and the hold cannot enforce while it is live.
        s_playerPointClickActive = true;
        if (s_wasdHoldActive)
        {
            s_wasdHoldActive = false;
            s_holdPosValid   = false;
            DebugLog("[WASDCombat] dc_wasd_hold_cleared_by_click");
        }
        if (g_log.debugLogging)
        {
            char pmBuf[160];
            sprintf_s(pmBuf, sizeof(pmBuf),
                "[WASDCombat] dc_playermove_dispatch pos=(%.1f,%.1f,%.1f) bldg=%p",
                pos.x, pos.y, pos.z, (void*)destBuilding);
            DebugLog(pmBuf);
        }
    }

    s_playerMoveOrig(thisptr, pos, destBuilding);
}

// -----------------------------------------------------------------------
// Character::addOrder hook — door suppression on the player-order channel
// (separate from the job queue; proven hookable via GetRealAddress).  Same
// rule as the addJob gate: door-type orders on the anchor are swallowed
// ONLY while the post-WASD hold is active.  Everything else — including
// MOVE_CUS_ORDERED, which DC's own disengage orders use — passes through.
// -----------------------------------------------------------------------
static void (*s_addOrderOrig)(Character* thisptr, Building* dest, TaskType t,
                              RootObject* subject, bool shift, bool clear,
                              const Ogre::Vector3& location);

static void addOrder_hook(Character* thisptr, Building* dest, TaskType t,
                          RootObject* subject, bool shift, bool clear,
                          const Ogre::Vector3& location)
{
    // ANY MOVE while an inventory window is open — swallow it (matches the
    // addJob_hook rule).  subject==nullptr ⇒ a move (to a position OR a
    // building/door); loot/attack/interact/equip carry a subject and pass
    // through.  This is INDEPENDENT of s_fpActive (the face-cam): movement is
    // blocked whenever inventory is open, so a point-click can't walk the
    // character even if the face-cam isn't engaged (field 2026-06-17: moves with
    // a non-null dest leaked through because the old gate required dest==null OR
    // s_fpActive, and s_fpActive is false whenever the face-cam is off/broken).
    // `|| s_fpActive` additionally blocks subject-bearing clicks during the
    // face-cam.  Any PLAYER character, not just the anchor (the face-cam can lock
    // onto the SELECTED character).
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invOrdTick = 0;
        ULONGLONG nowIO = GetTickCount64();
        if (nowIO - s_invOrdTick >= 1000) { s_invOrdTick = nowIO;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addOrder task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    // Diagnostic: every order reaching the anchor while the hold is active.
    // The clear flag is the candidate discriminator between a fresh player
    // click's order and a stale automatic re-issue — field data from this
    // log decides whether addOrder can ever clear the hold safely.
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive)
    {
        ULONGLONG nowAD = GetTickCount64();
        if (nowAD - s_addOrderDiagLogTick >= 1000)
        {
            s_addOrderDiagLogTick = nowAD;
            char dbuf[160];
            sprintf_s(dbuf, sizeof(dbuf),
                "[WASDCombat] dc_addorder_during_hold task=%d clear=%d dest=%p",
                (int)t, (int)clear, (void*)dest);
            DebugLog(dbuf);
        }
    }

    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive && !s_playerPointClickActive
        && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
        && !s_menuSuspendActive
        && isDoorInteractionTask(t))
    {
        ULONGLONG nowDO = GetTickCount64();
        if (nowDO - s_doorSuppressLogTick >= 1000)
        {
            s_doorSuppressLogTick = nowDO;
            char obuf[128];
            sprintf_s(obuf, sizeof(obuf),
                "[WASDCombat] dc_door_addorder_suppressed task=%d", (int)t);
            DebugLog(obuf);
        }
        return;  // swallowed — the order never enters the queue
    }
    s_addOrderOrig(thisptr, dest, t, subject, shift, clear, location);
}

static void (*s_addJobOrig)(Character* thisptr, TaskType t, RootObject* subject,
                             bool shift, bool addDontClear, const Ogre::Vector3& location);

static void addJob_hook(Character* thisptr, TaskType t, RootObject* subject,
                         bool shift, bool addDontClear, const Ogre::Vector3& location)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedAddJob) {
            s_hookBlockLoggedAddJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=addJob"); }
        s_addJobOrig(thisptr, t, subject, shift, addDontClear, location); return; }

    // Ground-click MOVE while an inventory window is open: swallow it so the
    // freed cursor can't walk the controlled character (the OTS "no point-click
    // while in inventory" feel).  The order reaches the anchor here, NOT through
    // playerMove (field log 2026-06-15: playerMove suppression never fired).
    // subject==nullptr ⇒ a pure position move; loot/attack/interact carry a
    // subject, so equipping/looting/attacking are unaffected.  gui->isAny... is
    // authoritative (s_lootUiSuspendActive can lag a frame).
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE
        && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)   // s_fpActive = own-inventory face-cam: block ALL
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invJobTick = 0;
        ULONGLONG nowIJ = GetTickCount64();
        if (nowIJ - s_invJobTick >= 1000) { s_invJobTick = nowIJ;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addJob task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        // Door suppression — ONLY while the post-WASD hold is active: that
        // is the window where no player intent exists and stale indoor
        // door tasks used to auto-fire.  Vanilla door behavior everywhere
        // else (point-clicks, fresh V-mode, suspends, V OFF).
        if (s_wasdHoldActive && !s_playerPointClickActive
            && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
            && !s_menuSuspendActive
            && isDoorInteractionTask(t))
        {
            ULONGLONG nowDJ = GetTickCount64();
            if (nowDJ - s_doorSuppressLogTick >= 1000)
            {
                s_doorSuppressLogTick = nowDJ;
                char jbuf[128];
                sprintf_s(jbuf, sizeof(jbuf),
                    "[WASDCombat] dc_door_addjob_suppressed task=%d", (int)t);
                DebugLog(jbuf);
            }
            return;  // swallowed — the job never enters the queue
        }

        bool isAttackJob  = (t == MELEE_ATTACK            || t == FOCUSED_MELEE_ATTACK      ||
                              t == CHOOSE_ENEMY_AND_ATTACK || t == ATTACK_CHARACTERS_ATTACKER ||
                              t == ATTACK_ENEMIES);
        bool isMedicalJob = (t == JOB_MEDIC        || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT  || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER     || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Healing job detection with WASD-aware deferral.
        if (isMedicalJob)
        {
            if (s_healingJobActive)
            {
                // Already committed — DC has already yielded; do not re-interrupt.
                DebugLog("[WASDCombat] dc_heal_committed_action_preserved");
            }
            else if (!s_healingJobPending)
            {
                DebugLog("[WASDCombat] dc_auto_heal_job_detected");
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (wasdNow)
                {
                    // Defer: WASD active — preserve locomotion until keys release.
                    s_healingJobPending = true;
                    DebugLog("[WASDCombat] dc_auto_heal_deferred_due_to_wasd");
                }
                else
                {
                    // WASD not held: activate immediately and zero stale motion.
                    s_healingJobActive = true;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && !isDownedButMovable(s_freeMoveAnchor))
                    {
                        CharMovement* mvH = s_freeMoveAnchor->movement;
                        mvH->halt();
                        mvH->desiredMotion    = Ogre::Vector3::ZERO;
                        mvH->moveLimit        = 0.0f;
                        mvH->currentMotion    = Ogre::Vector3::ZERO;
                        s_wasdMovementApplied = false;
                        s_prevWasdDir         = Ogre::Vector3::ZERO;
                        dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                        DebugLog("[WASDCombat] medical_job_started_anchor");
                        DebugLog("[WASDCombat] medical_job_movement_zeroed");
                    }
                    DebugLog("[WASDCombat] dc_manual_heal_allowed");
                    DebugLog("[WASDCombat] healing_action_detected");
                }
            }
        }

        if (isAttackJob)
        {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[WASDCombat] attack_job_created type=%s", taskTypeName(t));
            DebugLog(buf);
        }
        if (g_log.debugLogging && !isAttackJob && !isMedicalJob)
        {
            char axbuf[128];
            sprintf_s(axbuf, sizeof(axbuf),
                "[WASDCombat] dc_xp_vanilla_action_allowed skill=%s", taskTypeName(t));
            DebugLog(axbuf);
        }
    }
    s_addJobOrig(thisptr, t, subject, shift, addDontClear, location);
}

// -----------------------------------------------------------------------
// Native Controls-menu keybind hooks (v1.7, KEP pattern)
// -----------------------------------------------------------------------

// InputHandler::loadConfig — register Direct Control commands BEFORE the
// original runs so the game's keyboard config applies any user-saved
// bindings on top of the defaults, and the game persists rebinds itself.
static bool s_processKeysHookOk = false;  // set at install; native registration
                                          // requires the event reader too

// Belt-and-braces persistence: the game saves bound plugin commands to
// controls.cfg (proven by KEP's toggle_devtools=F12), but our own INI copy
// guards against any case where the command ends up unbound at save time.
// The value is the raw bound int (OIS code | modifier masks), round-
// tripped verbatim through InputHandler::bind.
static const int DC_CMD_COUNT = 2;
static const char* const DC_CMD_NAMES[DC_CMD_COUNT] =
{
    "dc_toggle", "dc_speed_cycle"
};

// [NativeBinds] format version.  v1 (no "version=" key) WROTE Command::bound
// (0x40) — which is NOT the keycode (field 2026-06-20: bind(name,1) then read
// bound = 2, MISMATCH), so it persisted garbage ("=1") and menu rebinds never
// survived a restart.  v2 persists the real keycode via getBoundKeys().  On a
// version mismatch the old per-command values are IGNORED (defaults stand) and
// the file is re-stamped, so corrupted v1 INIs self-heal instead of binding the
// command to key 1.
static const int DC_NATIVE_BIND_FORMAT_VERSION = 2;

// readBoundKey — the keycode the dc_ command is CURRENTLY bound to (its first
// bound key) via the public InputHandler::getBoundKeys API.  Returns INT_MIN when
// the command is unknown or unbound.  Do NOT read Command::bound (0x40): it is an
// internal value, not the keycode.
static int readBoundKey(const char* name)
{
    if (!key) return INT_MIN;
    lektor<int> keys = key->getBoundKeys(name);
    if (keys.size() == 0) return INT_MIN;   // command unbound
    return keys[0];
}

// readChangeToken — a CHEAP, non-allocating value that merely CHANGES when the
// command's binding changes, used only for change DETECTION (getBoundKeys returns
// a heap lektor, too costly to poll every few seconds).  Command::bound is not
// the keycode but it does differ per binding, so it is a valid change token.
static int readChangeToken(const char* name)
{
    if (!key) return INT_MIN;
    auto it = key->commands.find(name);
    if (it == key->commands.end()) return INT_MIN;
    return it->second.bound;
}

static void saveNativeBindsToIni(const char* reason)
{
    if (!key) return;
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    // Stamp the format version first so a partially-written file is still
    // recognised as v2 (and never re-applies the v1 garbage).
    {
        char vbuf[16];
        sprintf_s(vbuf, sizeof(vbuf), "%d", DC_NATIVE_BIND_FORMAT_VERSION);
        WritePrivateProfileStringA("NativeBinds", "version", vbuf, path);
    }
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int bound = readBoundKey(DC_CMD_NAMES[i]);
        if (bound == INT_MIN)
        {
            char ebuf[96];
            sprintf_s(ebuf, sizeof(ebuf),
                "[WASDCombat] dc_native_bind_save_failed name=%s reason=unbound_or_not_found",
                DC_CMD_NAMES[i]);
            DebugLog(ebuf);
            continue;
        }
        char val[16];
        sprintf_s(val, sizeof(val), "%d", bound);
        BOOL ok = WritePrivateProfileStringA("NativeBinds", DC_CMD_NAMES[i], val, path);
        char sbuf[160];
        sprintf_s(sbuf, sizeof(sbuf),
            "[WASDCombat] dc_native_bind_saved name=%s bound=%d write_ok=%d",
            DC_CMD_NAMES[i], bound, (int)ok);
        DebugLog(sbuf);
    }
    char rbuf[MAX_PATH + 96];
    sprintf_s(rbuf, sizeof(rbuf),
        "[WASDCombat] dc_native_binds_saved reason=%s path=%s", reason, path);
    DebugLog(rbuf);
}

// Periodic change detector — persistence must not depend on the options
// menu calling saveOptions (and its logs reveal whether a menu rebind even
// updates Command::bound).  Runs from mainLoop every BIND_WATCH_INTERVAL_MS
// once native bindings are registered; first pass only snapshots.
static int       s_bindSnapshot[DC_CMD_COUNT] = { 0, 0 };
static bool      s_bindSnapshotValid    = false;
static ULONGLONG s_bindWatchTick        = 0;
static const ULONGLONG BIND_WATCH_INTERVAL_MS = 3000;

static void watchNativeBindChanges()
{
    ULONGLONG nowBW = GetTickCount64();
    if (nowBW - s_bindWatchTick < BIND_WATCH_INTERVAL_MS) return;
    s_bindWatchTick = nowBW;

    int cur[DC_CMD_COUNT];
    for (int i = 0; i < DC_CMD_COUNT; ++i)
        cur[i] = readChangeToken(DC_CMD_NAMES[i]);   // cheap change-detection token

    if (!s_bindSnapshotValid)
    {
        for (int i = 0; i < DC_CMD_COUNT; ++i) s_bindSnapshot[i] = cur[i];
        s_bindSnapshotValid = true;
        return;
    }

    bool changed = false;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        if (cur[i] != s_bindSnapshot[i])
        {
            char cbuf[160];
            sprintf_s(cbuf, sizeof(cbuf),
                "[WASDCombat] dc_native_bind_change_detected name=%s old=%d new=%d",
                DC_CMD_NAMES[i], s_bindSnapshot[i], cur[i]);
            DebugLog(cbuf);
            s_bindSnapshot[i] = cur[i];
            changed = true;
        }
    }
    if (changed)
        saveNativeBindsToIni("change_detected");
}

static void applyNativeBindsFromIni(InputHandler* self)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    // Migration guard: only apply stored binds written by the CURRENT format.
    // A v1 file (no "version=" key, or < current) stored Command::bound garbage
    // (e.g. "=1"); applying it would bind the command to key 1.  Ignore those,
    // leave the addCommand defaults (V / X) in place, and re-stamp the file so it
    // self-heals to v2 going forward.
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION)
    {
        char mbuf[MAX_PATH + 96];
        sprintf_s(mbuf, sizeof(mbuf),
            "[WASDCombat] dc_native_binds_migrated old_ver=%d -> v%d (defaults kept) path=%s",
            ver, DC_NATIVE_BIND_FORMAT_VERSION, path);
        DebugLog(mbuf);
        saveNativeBindsToIni("format_migration");   // re-stamp version + correct keycodes
        return;
    }

    int applied = 0;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int v = (int)GetPrivateProfileIntA("NativeBinds", DC_CMD_NAMES[i], -1, path);
        if (v > 0)
        {
            // bind() ADDS a key, it does not replace — so the addCommand default
            // (V / X) would remain ALONGSIDE the saved key and BOTH would fire
            // (field 2026-06-20: default + new bind both activated after restart).
            // Unbind the command first so exactly the saved key remains.  This
            // also cleans up any leftover double-binding from the old format.
            self->unbind(std::string(DC_CMD_NAMES[i]));
            self->bind(DC_CMD_NAMES[i], v);
            int after = readBoundKey(DC_CMD_NAMES[i]);
            char abuf[160];
            sprintf_s(abuf, sizeof(abuf),
                "[WASDCombat] dc_native_bind_applied name=%s ini=%d bound_after=%d%s",
                DC_CMD_NAMES[i], v, after,
                (after == v) ? "" : " MISMATCH");
            DebugLog(abuf);
            ++applied;
        }
    }
    char cbuf[MAX_PATH + 96];
    sprintf_s(cbuf, sizeof(cbuf),
        "[WASDCombat] dc_native_binds_applied count=%d path=%s", applied, path);
    DebugLog(cbuf);
}

// iniSavedNativeBind — the keycode a dc_ command was rebound to in the v2
// [NativeBinds] INI, or -1 if the user never rebound it (no file, pre-v2
// format, or value absent).  Mirrors applyNativeBindsFromIni's version gate
// exactly.  Used at registration to decide whether to claim the V/X default
// at all: if the user already moved the command (e.g. to Ctrl+V), registering
// plain V/X would let Kenshi's one-command-per-key rule STEAL V/X from any
// vanilla command (camera tilt, zoom-out) the player bound there, wiping it
// every session (field report 2026-06-23).
static int iniSavedNativeBind(const char* name)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION) return -1;
    int v = (int)GetPrivateProfileIntA("NativeBinds", name, -1, path);
    return (v > 0) ? v : -1;
}

// registerNativeCommands — register dc_toggle / dc_speed_cycle and apply
// INI-persisted bindings.  FIELD FINDING (2026-06-10 log): the game runs
// InputHandler::loadConfig BEFORE RE_Kenshi loads plugins, so a loadConfig
// hook alone never fires.  This is therefore called from the first
// mainLoop pass (key global valid, main thread) — the loadConfig hook
// remains only as a re-registration path if the game ever reloads its
// keyboard config.  Toggle + speed ONLY: movement keys must never be
// registered (one command per key; vanilla camera owns W/S/A/D).
static void registerNativeCommands(InputHandler* self)
{
    if (s_nativeCommandsRegistered || !self) return;
    if (!s_processKeysHookOk)
    {
        // Without the event reader, toggle/speed presses would be lost —
        // stay on the INI/poll fallback entirely.
        DebugLog("[WASDCombat] dc_native_keybinds_skipped_no_event_reader");
        return;
    }
    // Claim the V/X default ONLY when the user has not rebound the command.
    // If a saved rebind exists (e.g. Ctrl+V), register with NO physical key so
    // loadConfig never steals plain V/X from a vanilla camera binding; the
    // saved key is restored by applyNativeBindsFromIni immediately below.
    const int savedToggle = iniSavedNativeBind("dc_toggle");
    const int savedSpeed  = iniSavedNativeBind("dc_speed_cycle");
    self->addCommand("dc_toggle",        0,
                     (savedToggle > 0) ? OIS::KC_UNASSIGNED : OIS::KC_V,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    self->addCommand("dc_speed_cycle",   0,
                     (savedSpeed  > 0) ? OIS::KC_UNASSIGNED : OIS::KC_X,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    char rnbuf[160];
    sprintf_s(rnbuf, sizeof(rnbuf),
        "[WASDCombat] dc_native_register defaults toggle=%s speed=%s",
        (savedToggle > 0) ? "deferred(rebound)" : "V",
        (savedSpeed  > 0) ? "deferred(rebound)" : "X");
    DebugLog(rnbuf);
    applyNativeBindsFromIni(self);       // our INI is the real persistence
    s_nativeCommandsRegistered = true;   // poll-thread toggle/speed stand down
    DebugLog("[WASDCombat] dc_native_keybinds_registered");
}

static void (*s_inputLoadConfigOrig)(InputHandler*);
static void inputLoadConfig_hook(InputHandler* self)
{
    registerNativeCommands(self);
    s_inputLoadConfigOrig(self);
    if (s_nativeCommandsRegistered)
        applyNativeBindsFromIni(self);   // re-assert ours over any cfg reload
}

// OptionsWindow::saveOptions — the game just saved every binding it knows
// about to controls.cfg, which excludes plugin commands; persist ours.
static void (*s_optionsSaveOrig)(OptionsWindow*);
static void optionsSave_hook(OptionsWindow* self)
{
    s_optionsSaveOrig(self);
    if (s_nativeCommandsRegistered)
        saveNativeBindsToIni("save_options");
}

// OptionsWindow::create — after the original builds the options UI, find
// the Controls tab (category 0x19) and append the Direct Control rows.
// Rebinding then uses the game's own press-a-key flow and conflict
// handling; nothing custom is drawn.
static void (*s_optionsCreateOrig)(OptionsWindow*);
static void optionsCreate_hook(OptionsWindow* self)
{
    s_optionsCreateOrig(self);

    DatapanelGUI* controlsTab = nullptr;
    size_t tabCount = self->tabs->getItemCount();
    for (size_t i = 0; i < tabCount; i++)
    {
        DatapanelGUI** panel = self->tabs->getItemDataAt<DatapanelGUI*>(i, false);
        if (panel && *panel != nullptr && (*panel)->currentCategory == 0x19)
        {
            controlsTab = *panel;
            break;
        }
    }
    if (controlsTab)
    {
        // No leading addSpace — it rendered as a large empty gap above the
        // section (field finding).  The "Direct Control:" prefixes are the
        // section marker, KEP-style.  Toggle + speed only: movement keys
        // are VK-polled (WASDCombatPlugin.ini) because the native system
        // is one-command-per-key and vanilla camera owns W/S/A/D.
        controlsTab->addCustomLine(new DataPanelLine_KeyConfig(
            "dc_toggle",        "Direct Control: Toggle",        0x19));
        controlsTab->addCustomLine(new DataPanelLine_KeyConfig(
            "dc_speed_cycle",   "Direct Control: Speed Cycle",   0x19));
        DebugLog("[WASDCombat] dc_controls_menu_section_added");
    }
    else
    {
        DebugLog("[WASDCombat] dc_controls_tab_not_found — bindings still work, menu rows missing");
    }
}

// GameWorld::processKeys — toggle/speed press events arrive in key->events
// for exactly one processKeys cycle; consume them here on the main thread.
// No pause gate: the V toggle has always worked while paused (poll-thread
// behavior preserved).  Loading screens are skipped.
static void (*s_processKeysOrig)(GameWorld* thisptr);
static void processKeys_hook(GameWorld* thisptr)
{
    s_processKeysOrig(thisptr);
    if (s_dcShutdownInProgress || !s_nativeCommandsRegistered)
        return;
    if (gui && gui->isLoadingMessageVisible())
        return;
    for (auto it = key->events.begin(); it != key->events.end(); ++it)
    {
        const std::string& n = (*it)->name;
        if (n == "dc_toggle")           handleTogglePress();
        else if (n == "dc_speed_cycle") handleSpeedPress();
    }
}

// -----------------------------------------------------------------------
// verifyPatchSiteBytes — REQUIRED gate for any hook installed by raw RVA.
//
// KenshiLib::GetRealAddress hooks are symbol-based and survive exe changes;
// raw-RVA hooks do not.  RE_Kenshi regenerates its patched Kenshi_x64.exe
// on its own updates (last: 2026-06-21), silently shifting all code — a
// stale RVA then patches the middle of an unrelated instruction and MinHook
// still reports SUCCESS.  That shipped two landmines in v1.3.0 (playerMove
// RVA → navmesh crash, showTradeWindow RVA → corrupted conversion helper;
// see the retired install blocks in startPlugin).
//
// Usage: record the first `len` bytes at the target RVA from the SAME exe
// the RVA was derived on, and only AddHook when they still match.  On
// mismatch the hook is skipped (feature degrades, nothing corrupts) and the
// actual bytes are logged for re-derivation.
// -----------------------------------------------------------------------
static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected,
                                 size_t len, const char* name)
{
    if (len > 16) len = 16;
    if (memcmp((const void*)addr, expected, len) == 0)
        return true;
    char hex[3 * 16 + 1] = { 0 };
    for (size_t i = 0; i < len; ++i)
        sprintf_s(hex + 3 * i, sizeof(hex) - 3 * i, "%02X ",
                  ((const unsigned char*)addr)[i]);
    char buf[224];
    sprintf_s(buf, sizeof(buf),
        "WASDCombatPlugin: %s RVA hook SKIPPED — patch-site bytes changed (exe updated?), got: %s",
        name, hex);
    ErrorLog(buf);
    return false;
}

// -----------------------------------------------------------------------
// DllMain / startPlugin
// -----------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        s_thisModule = hModule;   // for the keybind INI path (DLL directory)
    if (reason == DLL_PROCESS_DETACH)
        DebugLog("WASDCombatPlugin: unloaded");
    return TRUE;
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("WASDCombatPlugin v1.8.4 — fix post-KO movement (combat-anim buffer gated on actual combat mode); OTS action camera");

    // INI keybinds remain the FALLBACK: loaded unconditionally so the poll
    // thread works from frame one and keeps working if the native command
    // registration never fires (load-order or hook failure).  Once
    // dc_native_keybinds_registered appears, the INI is inert.
    loadKeybinds();

    // Native Controls-menu keybinds (KEP pattern) — all three hooks are
    // header-declared members resolved via GetRealAddress; no raw-RVA hooks.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&InputHandler::loadConfig),
            &inputLoadConfig_hook, &s_inputLoadConfigOrig))
        ErrorLog("WASDCombatPlugin: InputHandler::loadConfig hook FAILED — using INI keybind fallback");
    else
        DebugLog("WASDCombatPlugin: InputHandler::loadConfig hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::create),
            &optionsCreate_hook, &s_optionsCreateOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::create hook FAILED — Controls-menu rows unavailable");
    else
        DebugLog("WASDCombatPlugin: OptionsWindow::create hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::saveOptions),
            &optionsSave_hook, &s_optionsSaveOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::saveOptions hook FAILED — rebinds will not persist across restarts");
    else
        DebugLog("WASDCombatPlugin: saveOptions hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::processKeys),
            &processKeys_hook, &s_processKeysOrig))
        ErrorLog("WASDCombatPlugin: GameWorld::processKeys hook FAILED — using INI keybind fallback");
    else
    {
        s_processKeysHookOk = true;
        DebugLog("WASDCombatPlugin: processKeys hook OK");
    }

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &s_mainLoopOrig))
        ErrorLog("WASDCombatPlugin: mainLoop hook FAILED");
    else
        DebugLog("WASDCombatPlugin: mainLoop hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharMovement::_NV_update),
            &charMovUpdate_hook, &s_charMovUpdateOrig))
        ErrorLog("WASDCombatPlugin: charMovUpdate hook FAILED");
    else
        DebugLog("WASDCombatPlugin: charMovUpdate hook OK");

    // OTS action camera — drive (runs after the game's camera update, before
    // render) and the RTS-clamp bypass.  If either fails, OTS is unavailable
    // but the rest of DC is unaffected.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::update),
            &cameraUpdate_hook, &s_cameraUpdateOrig))
        ErrorLog("WASDCombatPlugin: CameraClass::update hook FAILED — OTS camera unavailable");
    else
        DebugLog("WASDCombatPlugin: cameraUpdate hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::restrictPosition),
            &restrictPos_hook, &s_restrictPosOrig))
        ErrorLog("WASDCombatPlugin: restrictPosition hook FAILED — OTS camera may clamp to floor");
    else
        DebugLog("WASDCombatPlugin: restrictPosition hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&PlayerInterface::playerControl),
            &playerControl_hook, &s_playerControlOrig))
        ErrorLog("WASDCombatPlugin: playerControl hook FAILED");
    else
        DebugLog("WASDCombatPlugin: playerControl hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::removeJob),
            &removeJob_hook, &s_removeJobOrig))
        ErrorLog("WASDCombatPlugin: removeJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: removeJob hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addJob),
            &addJob_hook, &s_addJobOrig))
        ErrorLog("WASDCombatPlugin: addJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: addJob hook OK");

    // addOrder — door suppression on the player-order channel while the
    // post-WASD hold is active.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addOrder),
            &addOrder_hook, &s_addOrderOrig))
        ErrorLog("WASDCombatPlugin: addOrder hook FAILED — door suppression partial (addJob only)");
    else
        DebugLog("WASDCombatPlugin: addOrder hook OK");

    // PlayerInterface::playerMove — NOT INSTALLED (2026-08-05, crash-dump
    // verified).  RVA 0x7F95F0 was derived from the pre-2026-06-21 RE_Kenshi
    // exe; RE_Kenshi regenerated its patched Kenshi_x64.exe on 6/21 and all
    // code shifted.  On the current exe 0x7F95F0 is MID-INSTRUCTION (+0x2ED
    // into a NavMesh-path function, one byte into a 5-byte call at 0x7F95EF):
    // the MinHook E9 byte became that call's displacement low byte, and any
    // pathfind reaching the rare branch at 0x7F95EF jumped into unmapped
    // memory — the "pack bull + right-click inside hive home" crash.  The
    // hook never fired on this exe (0 log lines across full sessions); its
    // job is fully covered by the RMB press-edge poller + addOrder/addJob
    // gates, so nothing replaces it.  To re-enable: re-derive the RVA on the
    // CURRENT exe and install through verifyPatchSiteBytes().

    // combatGo — DC passive-combat model: suppress the controlled character's
    // combat AI unless engaged (manual attack / meleed) and not moving.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_NV_go),
            &combatGo_hook, &s_combatGoOrig))
        ErrorLog("WASDCombatPlugin: combatGo hook FAILED — passive-combat model inactive");
    else
        DebugLog("WASDCombatPlugin: combatGo hook OK");

    // initCombatMode and youKnowImAttacking hooks remain removed — DC does not
    // block combat ENTRY or attack notifications; only the per-frame go() decision
    // is gated (passive-combat model).

    // showTradeWindow — NOT INSTALLED (2026-08-05, same stale-RVA disease as
    // playerMove above).  On the current exe 0x7905D0 is MID-INSTRUCTION
    // (+0x50 into a double→int64 conversion helper at 0x790580, inside a
    // 10-byte movabs), so the patch was corrupting that helper's COMMON path
    // — wrong return values + dirty MMX state on every call — while the hook
    // itself never fired (the function is not showTradeWindow).  Loot/trade
    // detection has been carried entirely by the mainLoop GUI poll
    // (isAnyInventoryWindowOpen → s_lootUiSuspendActive) since 6/21; every
    // tested-good build ran that way, so nothing replaces this either.  To
    // re-enable: re-derive the RVA and install through verifyPatchSiteBytes().

    HANDLE h = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!h)
    {
        char buf[64];
        sprintf_s(buf, sizeof(buf), "WASDCombatPlugin: poll thread FAILED err=%lu", GetLastError());
        ErrorLog(buf);
    }
    else
    {
        CloseHandle(h);
        DebugLog("WASDCombatPlugin: poll thread OK");
    }
}
