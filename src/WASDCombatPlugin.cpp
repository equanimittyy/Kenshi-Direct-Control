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
#include <ogre/OgreOldSkeletonInstance.h>
#include <ogre/OgreOldBone.h>
#include <kenshi/Appearance.h>
#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <mygui/MyGUI.h>

// Declared by hand because <kenshi/gui/ManagementScreen.h> does not compile (broken
// ReorderableList template). Signatures must match that header exactly or the link fails.
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();
    bool getVisible();
};

#define DIAG_VERBOSE 0

#define RETREAT_VERBOSE_DIAG 0

#define LOOT_DIAG 0

struct LocoConfig
{
    float     wasdAccelerationMultiplier;  // pre-charges currentMotion; 1.0 = vanilla
    float     wasdDecelerationMultiplier;  // force-zeros currentMotion on key release; 1.0 = halt only
    ULONGLONG wasdInputGraceMs;
    float     wasdTurnResponsiveness;
    bool      normalizeDiagonalMovement;
    ULONGLONG wasdNudgeTapWindowMs;
};

static const LocoConfig g_loco = {
    /* wasdAccelerationMultiplier */ 1.25f,
    /* wasdDecelerationMultiplier */ 1.35f,
    /* wasdInputGraceMs           */ 25,
    /* wasdTurnResponsiveness     */ 1.25f,
    /* normalizeDiagonalMovement  */ true,
    /* wasdNudgeTapWindowMs       */ 125,
};

struct ReleaseStopConfig
{
    bool      wasdStopOnRelease;
    float     wasdReleaseDecelerationMultiplier; // >1 forces currentMotion to zero after halt()
    ULONGLONG wasdReleaseGraceMs;
    bool      wasdAnchorSnapOnRelease;
    bool      wasdZeroVelocityOnRelease;
};

static const ReleaseStopConfig g_release = {
    /* wasdStopOnRelease                 */ true,
    /* wasdReleaseDecelerationMultiplier */ 999.0f,
    /* wasdReleaseGraceMs               */ 0,
    /* wasdAnchorSnapOnRelease          */ true,
    /* wasdZeroVelocityOnRelease        */ true,
};

struct CameraConfig
{
    bool  dcCameraCloseZoomChestOffset;
    float dcCameraFocusOffsetY;         // Y raise at max zoom-in, tapering to 0 at medium zoom
};

static const CameraConfig g_dcCam = {
    /* dcCameraCloseZoomChestOffset */ true,
    /* dcCameraFocusOffsetY         */ 4.5f,
};

// All flags must stay false in release builds.
struct LogConfig
{
    bool debugLogging;
    bool verboseMovementLogs;
    bool verboseCommittedActionLogs;
    bool debugVerbose;
};

static const LogConfig g_log = {
    /* debugLogging              */ false,
    /* verboseMovementLogs       */ false,
    /* verboseCommittedActionLogs*/ false,
    /* debugVerbose              */ false,
};

// Profiling accumulators hold raw QPC ticks; dc_perf converts them to ms once per second.
static bool     s_profInited          = false;
static LONGLONG s_profFreq            = 1;
static LONGLONG s_prof_mainLoop       = 0;
static LONGLONG s_prof_charMove       = 0;
static LONGLONG s_prof_playerControl  = 0;
static LONGLONG s_prof_committedAct   = 0;
static LONGLONG s_prof_threatScan     = 0;
static LONGLONG s_prof_cameraLock     = 0;
static LONGLONG s_prof_wasdInject     = 0;
static LONGLONG s_prof_combatTarget   = 0;
static ULONGLONG s_prof_windowStart   = 0;
static int      s_nearbyEnemyCount    = 0;
static int      s_chaseFlapsCount     = 0;    // combat enter/exit transitions per perf window
static int      s_pathfindingEnemyCount = 0;  // enemies in TARGET_PATHFINDING*, a path-recalculation proxy

static inline LONGLONG qpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

struct ScopeTimer
{
    LONGLONG  _start;
    LONGLONG& _accum;
    ScopeTimer(LONGLONG& a) : _start(qpcNow()), _accum(a) {}
    ~ScopeTimer() { _accum += qpcNow() - _start; }
};

enum ControlMode { MODE_VANILLA = 0, MODE_FREE_MOVE = 1 };

static volatile ControlMode s_mode   = MODE_VANILLA;
static volatile bool        s_wHeld  = false;
static volatile bool        s_aHeld  = false;
static volatile bool        s_sHeld  = false;
static volatile bool        s_dHeld  = false;
// Set by the poll thread, consumed on the main thread. It only releases the
// post-WASD hold; the click itself is never touched.
static volatile bool        s_rmbPressedEdge = false;
static bool                 s_rmbPrev        = false;  // poll thread only
// In DC a single left click only selects (vanilla); only a double click reassigns
// WASD control. Sentient Sands compatibility depends on this.
static bool                 s_lmbPrev          = false;     // poll thread only
static ULONGLONG            s_lastLmbDownMs    = 0;         // poll thread only
static volatile ULONGLONG   s_lmbDoubleClickMs = 0;        // set by poll, consumed by main
// In DC, CTRL toggles camera rotation instead of hold-to-rotate: cameraUpdate_hook
// forces InputHandler::rotate (0xED) to this state. Reset when DC turns off.
static volatile bool        s_camRotateToggle  = false;
static bool                 s_ctrlPrevPoll     = false;     // poll thread only
// Set while any UI wants the cursor, so CTRL presses inside menus (CTRL+click in
// trade or inventory) do not flip the camera-rotate toggle.
static volatile bool        s_camRotateUiOpen  = false;
// Poll thread: time of the first WASD press, cleared on full release.
static volatile ULONGLONG   s_wasdTapStartMs = 0;
// SelectControl edge: in DC, hands WASD control to the selected character, as an
// alternative to the double-click portrait switch. Vanilla F still re-centers the camera.
static volatile bool        s_fSelectEdge    = false;
// Set and cleared only by the toggle key or a hard shutdown.
static volatile bool        s_userWantsDC         = false;
// Set on LOADGAME teardown; every hook calls only the original while it is set.
// Cleared only when all six stabilization conditions hold.
static volatile bool        s_dcShutdownInProgress = false;
// Each logs once per load event; reset in clearAllState.
static bool s_hookBlockLoggedMain    = false;
static bool s_hookBlockLoggedCharMov = false;
static bool s_hookBlockLoggedPCtrl   = false;
static bool s_hookBlockLoggedRemJob  = false;
static bool s_hookBlockLoggedAddJob  = false;
static bool s_hookBlockLoggedTrade   = false;
static ULONGLONG s_shutdownWaitLogTick = 0;

// Pauses V-mode input and WASD injection while looting without changing s_mode,
// so DC restores automatically.
static volatile bool   s_lootUiSuspendActive    = false;
static bool            s_lootUiWasPrevOpen      = false;
static ULONGLONG       s_lootSuspendStartTick   = 0;
// Edge-latched from showTradeWindow, cleared when all inventory windows close. The gui
// trade fields stay stale after a trade closes, so reading them misclassifies the next
// own inventory as a trade. A character with a backpack opens its own inventory as two windows.
static bool            s_tradeWindowActive      = false;
static const ULONGLONG LOOT_SUSPEND_DEBOUNCE_MS = 250;

// InventoryFaceCam=false: DC movement and the game keep running while an inventory is
// open; walking out of range closes the window natively. Dialogue still pauses.
// s_invMoveThroughActive is latched on open so the close edge knows which path ran;
// ForcedRun records that we unpaused the game, so pause is restored only when we caused it.
static bool            s_invMoveThroughActive      = false;
static bool            s_invMoveThroughForcedRun   = false;
// Kenshi auto-pauses on inventory open and on a switch to another squad member; only
// pauses inside the grace window after those edges are defeated. A later pause is the
// player's and is kept until they unpause. s_invMoveThroughShownChar is compared by
// identity only, never dereferenced: the character behind a closing window can be torn down.
static bool            s_invMoveThroughPlayerPaused = false;
static Character*      s_invMoveThroughShownChar    = nullptr;
static ULONGLONG       s_invMoveThroughEdgeTick     = 0;
// Covers one slow UI-transition frame (~500 ms). A player pause inside the grace is
// eaten once; pausing again sticks.
static const ULONGLONG INV_MT_AUTOPAUSE_GRACE_MS   = 600;
// Vanilla merchant trade never closes on distance, so the plugin closes the trader
// window itself, once per session. Corpse and own-inventory windows close natively.
static bool            s_invTradeCloseRequested    = false;
// Measured as distance walked from the open spot, not distance to the merchant: that can
// already be large at open (trading across a counter) and would close the window at once.
static bool            s_invTradeStartValid        = false;
static Ogre::Vector3   s_invTradeAnchorStart;
static const float     INV_TRADE_AUTOCLOSE_DIST    = 6.0f;
static const float     INV_TRADE_AUTOCLOSE_DIST_SQ = INV_TRADE_AUTOCLOSE_DIST * INV_TRADE_AUTOCLOSE_DIST;

// Volatile input snapshot taken once per mainLoop: per-character hooks fire 100+ times
// per frame and must not pay a volatile read each time.
static ControlMode s_frameMode        = MODE_VANILLA;
static bool        s_frameWasdHeld    = false;
static bool        s_frameLootSuspend = false;

static const float     ATTACK_RANGE    = 500.0f;
static const ULONGLONG SCAN_INTERVAL_MS = 5000; // squad-threat scan

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

// Keybinds are role-based: the poll thread acts on the VK in s_bindVk for each role,
// never on a hard-coded key. Invalid INI entries fall back to the default.
enum KeyRole { KR_FORWARD = 0, KR_LEFT, KR_BACK, KR_RIGHT, KR_TOGGLE, KR_SELECT, KR_FP, KR_SNEAK, KR_COUNT };
static int s_bindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'F', 'P', 'C' };
static const char* const KR_INI_KEY[KR_COUNT] =
    { "MoveForward", "MoveLeft", "MoveBackward", "MoveRight", "ToggleDC", "SelectControl", "FirstPerson", "SneakToggle" };
static const char* const KR_DEFAULT[KR_COUNT] =
    { "W", "A", "S", "D", "V", "F", "P", "C" };
static char    s_bindCfgStr[KR_COUNT][32];
static HMODULE s_thisModule = nullptr;      // captured in DllMain for the INI path

// The face-cam is also suppressed in combat: players who loot mid-fight disliked the
// camera grabbing focus.
static bool    s_settingInventoryFaceCam = true;
// The injected setDirectMovement is otherwise uncapped (~99), so shackled, injured or
// encumbered characters outran everything. The cap is CharStats::getMaxRunSpeed, with a
// hard clamp while chained.
static bool    s_settingWasdSpeedCap     = true;
static float   s_settingWasdSpeedMult    = 1.0f;
// isInCombatMode blips false between swings, and one false frame at inventory open
// latched the face-cam on. So combat is a hard exit, held for a grace period after the
// last in-combat frame.
static ULONGLONG s_lastInCombatMs = 0;
static const ULONGLONG FACECAM_COMBAT_GRACE_MS = 1500;

// Inventory face-cam: a detached Ogre camera (s_fpNode) that faces the character while
// their own inventory is open, so worn gear is visible. A gameplay over-the-shoulder (OTS)
// view was rejected: Kenshi's interior floor render works only with the top-down RTS
// camera. Runs in cameraUpdate_hook, which still runs under the inventory pause.
// Kenshi scale is about 10 cm per unit.
static bool             s_fpActive          = false;
// cameraUpdate_hook runs twice per frame and the two calls disagree on the own-inventory
// window count, so a naive exit toggled the face-cam every frame and broke altitude hold
// and click suppression. Exit only after the signal is absent for this many calls.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// World name-tags are hidden while the face-cam is up because camera input still nudged
// them. s_savedShowNames is read before hiding, so a showNames() that also writes the
// option cannot poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;
static bool             s_otsInvFaceActive  = false;
static Character*       s_otsInvFaceChar    = nullptr; // who it is aimed at (identity-compared only)
// The shoulder view from the first inventory open, restored on close.
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Fraction of client width (positive = right of center), so the crosshair clears the body
// for selection clicks. The cursor is pinned there and mouse-look deltas are measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// The detached camera survives a save-load, so it is re-attached only after the world is
// valid again: re-attaching mid-load crashed, and skipping it left the camera orphaned.
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // face-cam: held to block scroll-zoom

// First person is a second drive mode for the same detached camera as the inventory
// face-cam, with the same s_fpNode and saved camera locals. s_fpActive (face-cam) and
// s_firstPersonActive are mutually exclusive. Opening an inventory in first person suspends
// it for the face-cam and returns to first person on close (s_fpSuspendedForInv).
static bool  s_firstPersonActive   = false;
// Persistent first-person intent, like s_userWantsDC: it survives chunk-streaming and
// save loads so first person re-enters after the scene rebuilds.
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // set by the key edge, consumed on the game thread
static bool  s_fpSuspendedForInv   = false;
static float s_fpSensitivityFP     = 1.0f;
static float s_fpEyeHeight         = 16.5f;  // fallback neck model
static float s_fpFwdOffset         = 1.2f;   // eye ahead of the neck
static float s_fpFovDegFP          = 75.0f;
static float s_fpNearClipFP        = 0.2f;
static float s_fpNeckLimitRad      = 1.309f; // 75 degrees
static bool  s_fpHideHair          = true;
static bool  s_fpHideHead          = true;
static bool  s_fpHairHidden        = false;
static bool  s_fpHeadBoneHidden    = false;
static float s_fpLeanFwd           = 0.0f;   // fallback model only
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;
static float         s_fpBoneEyeUp       = 1.0f;
static float         s_fpEyeUpAdjust     = 0.0f;   // keeps chest and shoulders below frame
static float         s_fpMoveLeanUp      = 0.0f;   // counters the forward jog lean
static float         s_fpMoveLeanFwd     = 0.0f;   // pushes past the leaning torso
static float         s_fpMoveNearClip    = 0.0f;   // slices the arm swinging into the lens
static float         s_fpMoveLeanSmooth  = 0.0f;
static float         s_fpJogForward      = 2.0f;   // closes the gap the jog lean opens
static float         s_fpRunForward      = 4.0f;   // same, while running
static float         s_fpGaitFwdSmooth   = 0.0f;
static float         s_fpActionClrSmooth = 0.0f;
static float         s_fpStreamDist      = 2.0f;   // per-frame rig teleports re-page grass
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;
static bool          s_fpLastStreamValid = false;
static float         s_fpGrassRangeMult  = 1.0f;   // does not fix the distant re-scatter;
                                                   // it only renders more grass that re-scatters.
static float         s_fpSavedGrassRange   = 0.0f;
static float         s_fpSavedFoliageRange = 0.0f;
static bool          s_fpOptRangeSaved     = false;
static bool          s_fpCamPreOrig        = false; // also drive the camera before Kenshi's loop. Default off: it
                                                    // does not fix the grass flicker (that pass reads the camera on a
                                                    // render thread) and it multiplies the follow and gait lerp rates.
static float         s_fpFollowTurn        = 0.08f; // lerps the view yaw toward the heading while the character moves
                                                    // on its own (not WASD), only after FollowDelayMs without mouse-look. 0 = off.
static float         s_fpFollowDelayMs     = 400.0f;
static ULONGLONG     s_fpLastMouseMoveMs   = 0;
static int           s_fpFoliageCenterMode = 1;     // while moving, moves the camera center node (grass streaming anchor)
                                                    // to the eye. At idle the center stays vanilla so panning cannot re-scatter grass.
// FreezeCamTest diagnostic: while standing in FP, lock the camera position and update only
// orientation, to tell positional grass re-scatter apart from billboard shimmer.
static int           s_fpFreezeCamTest     = 0;
static Ogre::Vector3 s_fpFrozenEye         = Ogre::Vector3::ZERO;
static bool          s_fpFrozenValid       = false;
static float         s_fpLookSmooth        = 0.4f;  // 0..0.9; smooths the rendered orientation to reduce render-thread
                                                    // grass flicker, at the cost of mouse-look snappiness.
static float         s_fpYawSm             = 0.0f;  // equals s_fpYaw when LookSmooth = 0
static float         s_fpPitchSm           = 0.0f;
static bool          s_fpSmValid           = false;

// The eye follows the head bone's real world position (Character::getBoneWorldPosition), and
// look input comes from 1 kHz DirectInput. The old path rotated the ~2 m bone offset by the view
// yaw, which swung the eye on an arc during pure rotation and never tracked the animated head.
static bool  s_fpTrueBoneEye  = true;   // 0 = old synthetic eye path
static float s_fpEyeDrop      = 0.0f;   // drop below the head-bone origin, in mount-bone-height units
static bool  s_fpRawMouse     = true;   // 0 = fps-dependent cursor-warp deltas
static float s_fpMoveForward  = 0.0f;   // speed-scaled forward lead so the eye does not lag the leaning head
static float s_fpMoveSpeedRef = 30.0f;  // feet-delta ground speed that maps to a lead of 1.0
// The lead uses feet-delta ground speed because the currentMotion magnitude is too noisy.
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;   // stair-climb detector
static float     s_fpMoveSpeed      = 0.0f;
static float     s_fpMoveFwdSmooth  = 0.0f;
static float     s_fpClimbSpeedSmooth = 0.0f; // + = ascending
static ULONGLONG s_fpFeetTickMs     = 0;

// On stairs the forward offset drives the eye into the rising steps, so while climbing the
// forward push scales down and the eye lifts. Flat ground restores the full offset.
static float s_fpStairForwardReduce   = 0.25f; // higher reaches full pullback at a gentler climb; 0 = off
static float s_fpStairForwardMinScale = 0.30f; // forward-offset fraction kept at full climb
static float s_fpStairEyeLift         = 0.30f; // extra eye height at full climb; 0 = off

// A hostile inside EnemyClearRadius collapses the forward eye offsets toward
// EnemyClearMinScale and pulls the near plane in, so the eye does not poke into its model.
// The nearest-hostile distance is sampled in mainLoop (game thread); -1 = none in range.
static float s_fpEnemyClearRadius   = 12.0f;  // 0 = off
static float s_fpEnemyClearMinScale = 0.15f;  // forward fraction kept at contact
static float s_fpEnemyNearClip      = 0.10f;  // 0 = off
static float s_fpEnemyClearSmooth   = 1.0f;   // 1 = no hostile near
static float s_fpEnemyNearestDist   = -1.0f;

// Re-showing upper storeys from eye level was rejected: Kenshi's upper-floor meshes are
// one-sided for the top-down cutaway and render as floating planks and sky holes from below.

// In FP, restrictPosition (the vanilla cutaway refresh) is skipped, and the substitute
// updateFloorVisibility pass reveals storeys by where squad members stand, which leaves black
// voids below a solo character. So after that pass all floors up to the anchor's floor are
// forced visible: one answer per frame, which also stops the two-writer flicker.
static bool s_fpFloorRevealBelow = true;

// Consumed on the game thread because the sneak-button path needs a valid anchor.
static volatile bool s_sneakToggleRequested = false;

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // after a strafe or backpedal, keeps the camera authoritative through
                                                    // the release, so the neck limit does not snap the view to the body.
static float         s_fpActionClearFwd    = 2.0f;  // extra forward eye offset during committed actions, clear of the body
static float         s_fpBodyYaw         = 0.0f;   // visible body only; the eye mounts on the view yaw
static const float   FP_BODY_TURN        = 0.20f;
static MyGUI::TextBox* s_fpCrosshair     = nullptr;
static const float FP_HEAD_LEN     = 1.8f;   // fallback model
static const float FP_BONE_SMOOTH  = 0.45f;  // damps stride bob

// Kenshi's InputHandler allows one command per physical key, and vanilla camera panning owns
// W/A/S/D. Registering DC movement commands there stole the keys from the camera and persisted
// the theft into controls.cfg, so movement keys are always VK-polled and dc_move_* commands must
// never be registered. Only the toggle sits on a free key and is registered natively.
static volatile bool s_nativeCommandsRegistered = false;
static void watchNativeBindChanges();
static void registerNativeCommands(InputHandler* self);

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

// Unknown values return the default, so a typo cannot silently turn a feature off.
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
    // The template ships player-facing settings only; the loader also reads many
    // advanced [FirstPerson] keys, each with a safe default.
    static const char tmpl[] =
        "[Keybinds]\r\n"
        "; Valid names: letters, digits, F1..F24, SPACE, TAB, SHIFT, CONTROL,\r\n"
        "; arrow keys, NUMPAD0..9, OEM_1..8.  Invalid entries use the default.\r\n"
        "ToggleDC        = V\r\n"
        "MoveForward     = W\r\n"
        "MoveBackward    = S\r\n"
        "MoveLeft        = A\r\n"
        "MoveRight       = D\r\n"
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

// Separate from loadKeybinds so enterFirstPerson can re-read it: INI edits apply on the
// next first-person toggle without a relaunch.
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

    loadFirstPersonConfig(path);

    char lbuf[420];
    sprintf_s(lbuf, sizeof(lbuf),
        "[WASDCombat] dc_keybinds_loaded toggle=%s(0x%02X) forward=%s(0x%02X)"
        " back=%s(0x%02X) left=%s(0x%02X) right=%s(0x%02X)"
        " select=%s(0x%02X) inventoryFaceCam=%d",
        s_bindCfgStr[KR_TOGGLE],  s_bindVk[KR_TOGGLE],
        s_bindCfgStr[KR_FORWARD], s_bindVk[KR_FORWARD],
        s_bindCfgStr[KR_BACK],    s_bindVk[KR_BACK],
        s_bindCfgStr[KR_LEFT],    s_bindVk[KR_LEFT],
        s_bindCfgStr[KR_RIGHT],   s_bindVk[KR_RIGHT],
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

// Shared by the poll thread and the native processKeys path, so loot-suspend gating is
// identical on both.
static void handleTogglePress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE || s_userWantsDC)
    {
        s_userWantsDC = false;
        s_userWantsFP = false;   // FP requires DC
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

static void handleSelectPress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE)
        s_fSelectEdge = true;
}

static void handleFirstPersonPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Camera calls must run on the game thread, so only the edge is set here.
    if (s_mode == MODE_FREE_MOVE)
        s_fpToggleRequested = true;
}

static void handleSneakPress()
{
    if (s_lootUiSuspendActive)
        return;
    // A chord, so a bare C press never collides with vanilla or other mods. Final gating
    // runs on the game thread where the edge is consumed.
    if (s_mode != MODE_FREE_MOVE || !s_firstPersonActive)
        return;
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    s_sneakToggleRequested = true;
}

static void onPress(int role)
{
    if (role == KR_TOGGLE)       handleTogglePress();
    else if (role == KR_SELECT)  handleSelectPress();
    else if (role == KR_FP)      handleFirstPersonPress();
    else if (role == KR_SNEAK)   handleSneakPress();
}

struct PollKey { int role; bool prev; };
static PollKey s_keys[] =
{
    { KR_FORWARD, false }, { KR_LEFT,   false },
    { KR_BACK,    false }, { KR_RIGHT,  false },
    { KR_TOGGLE,  false }, { KR_SELECT, false },
    { KR_FP,      false }, { KR_SNEAK,  false },
};
static const int NUM_KEYS = 8;

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
        // Toggle polling stands down once its native command is registered; presses then
        // arrive through processKeys.
        for (int i = 0; i < NUM_KEYS; ++i)
        {
            int role = s_keys[i].role;
            if (s_nativeCommandsRegistered && role == KR_TOGGLE)
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

        {
            bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (rmbDown && !s_rmbPrev)
                s_rmbPressedEdge = true;
            s_rmbPrev = rmbDown;
        }

        // The 50 ms poll reliably separates the two down-edges of a normal double-click.
        {
            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lmbDown && !s_lmbPrev)
            {
                ULONGLONG nowLB = GetTickCount64();
                if (s_lastLmbDownMs > 0
                    && (nowLB - s_lastLmbDownMs) <= (ULONGLONG)GetDoubleClickTime())
                    s_lmbDoubleClickMs = nowLB;
                else
                    s_lmbDoubleClickMs = 0;       // a single click clears a stale double-click
                s_lastLmbDownMs = nowLB;
            }
            s_lmbPrev = lmbDown;
        }

        // Flips only in DC, so out-of-DC CTRL use never desyncs the toggle.
        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Not while a UI is open: CTRL+click moves stacks in trade and inventory.
            if (ctrlDown && !s_ctrlPrevPoll && s_mode == MODE_FREE_MOVE && !s_camRotateUiOpen)
                s_camRotateToggle = !s_camRotateToggle;
            s_ctrlPrevPoll = ctrlDown;
        }
    }
}

// The HUD is disabled for reload stability.
struct HudWidget { MyGUI::TextBox* label; bool shown; const char* tag;
    HudWidget() : label(nullptr), shown(false), tag("") {} };
static HudWidget s_vHud;
static bool      s_hudReady = false;
static void hudUpdate() {}

// Main thread only.
static Character*    s_selectedCharacter = nullptr;
static CharMovement* s_selectedMovement  = nullptr;
static Character*    s_freeMoveAnchor    = nullptr;
static bool          s_wasdWasActive     = false;
static bool          s_combatWASDLogged  = false;
static bool          s_retreatLogged     = false;

static bool          s_squadThreat         = false;
static bool          s_consciousAllyThreat = false;

// Lets charMovUpdate_hook avoid dereferencing s_freeMoveAnchor, which a LOADGAME can free.
static CharMovement* s_anchorMovement = nullptr;

static volatile bool s_loadGuardActive      = false;
static int           s_stabilizationCountdown = 0;
static bool          s_postLoadReacquire   = false;

// Pointer loss: a load signal while s_userWantsDC pauses injection, keeps FREE_MOVE and
// reacquires. It becomes a hard shutdown only past the squad-loss threshold.
static bool           s_dcPtrLossActive      = false;
static ULONGLONG      s_dcPtrLossStartedAt   = 0;
static ULONGLONG      s_dcPtrLossLastLogTick = 0;
static MoveSpeed      s_dcPreservedSpeedMode  = WALK;
static bool          s_enemyTargetingLogged = false;
static ULONGLONG     s_lastScanTick        = 0;

static ControlMode   s_fmTrackedMode = MODE_VANILLA;

static swordStateEnum s_lastCombatState  = COMBAT_FINISHED;
static bool           s_wasPrevInCombat  = false;
static ULONGLONG      s_wasdReleasedTick = 0;

static Character*     s_prevAttackTarget  = nullptr;
static bool           s_prevTargetInRange = false;

static bool           s_wasProtectedState = false;

static bool           s_wasdMovementApplied = false;

// Protects CHOP_WEAPON from early interruption.
static bool           s_attackCommitmentActive = false;
static ULONGLONG      s_attackCommitmentStart  = 0;

// Movement injection is suspended while a medical job runs so its animation is not interrupted.
static bool           s_healingJobActive = false;
// A medical job that arrived while WASD was held; promoted once WASD is released.
static bool           s_healingJobPending = false;

static bool          s_wasdRetreatActive      = false;
static bool          s_retreatCleanLogged     = false;
static ULONGLONG     s_retreatActiveStartTick  = 0;
static ULONGLONG     s_lastCombatEnterExitTick = 0;
static bool          s_combatFlickerLogged    = false;

static const ULONGLONG POST_WASD_GRACE_MS          = 2000;
static const float     POST_WASD_REENGAGEMENT_RANGE = 200.0f;
static bool            s_postWasdGraceActive        = false;
static ULONGLONG       s_postWasdGraceStart          = 0;
static bool            s_combatReentryAllowed        = false;

static bool            s_retreatLockGoSuppressed     = false;
// Sticky until WASD release.
static bool            s_retreatLockEverActive       = false;

// Ownership handoff: the anchor's combat AI (CombatClass::_NV_go) runs autonomously unless
// WASD is held, and is suspended while it is (combatGo_hook). No per-frame tug-of-war, so no stutter.

static bool            s_wasdDownedMovementActive    = false;

// Once per WASD press.
static bool            s_playDeadExitDone            = false;

// In combat, a key roll or a brief pause instant-stopped and let the AI square up to the
// attacker. For this long after the last key, movement keeps the last direction and owns the
// character (AI suspended); short enough that autonomous combat resumes promptly.
static const ULONGLONG COMBAT_WASD_BRIDGE_MS         = 250;

// Set once per WASD hold to stop per-frame log spam.
static bool            s_medicalJobSuppressedThisHold = false;

static int             s_retreatBlockedAttackerCount  = 0;
static int             s_retreatTargetsProcessed      = 0;
static int             s_retreatTargetsCachedSkipped  = 0;
static int             s_lastKnownEnemyCount          = 0;

static const ULONGLONG JOB_REMOVAL_INTERVAL_MS = 250;
static ULONGLONG       s_jobRemovalLastTick     = 0;

static ULONGLONG       s_movInjLogTick         = 0;
// 0 = timer not armed.
static ULONGLONG       s_athleticsXpLastTick   = 0;
static Ogre::Vector3   s_prevWasdDir           = Ogre::Vector3::ZERO;
static ULONGLONG       s_wasdLastHeldMs        = 0;
static bool            s_savedFreeCameraMode       = false;
// DC stays active while camera tracking is suspended.
static bool            s_cameraLockInvSuspend      = false;
static bool            s_cameraLockTurretSuspend   = false;  // turret or mounted use
static bool            s_menuSuspendActive         = false;  // ou->isPaused(): escape, options, save and load menus

// Post-WASD hold: after WASD release the character stays where WASD left it until a player
// click, the next WASD press, or the toggle; vanilla point-click works normally otherwise.
// s_holdPos clamps X/Z because indoor routing writes position later in the frame, so the
// position is restored post-orig in charMovUpdate and at the end of mainLoop. Y stays free for
// gravity. s_holdPosValid drops whenever the hold is not enforcing, because a stale position
// would teleport-snap the character.
// Door addJob/addOrder on the anchor are swallowed only while holding, where stale indoor door
// tasks used to open doors. MOVE_CUS_ORDERED is never suppressed: DC's disengage orders use it.
// Do not hook CharBody::setCurrentAction (KenshiLib error 8, crash).
static bool           s_wasdHoldActive          = false;
// A live player click always outranks the hold, whichever was set first.
static bool           s_playerPointClickActive  = false;
static Ogre::Vector3  s_holdPos                 = Ogre::Vector3::ZERO;
static bool           s_holdPosValid            = false;
static bool           s_idleHoldEngaged         = false;
static ULONGLONG      s_authGateLogTick         = 0;
static bool           s_authGateLastAllow       = true;
static char           s_authGateLastReason[24]  = "";
static ULONGLONG      s_doorSuppressLogTick     = 0;
static ULONGLONG      s_addOrderDiagLogTick     = 0;
static bool           s_hookBlockLoggedPMove    = false;
static float           s_savedCamFollowOffY        = 0.0f;

// Each attacker is blocked once per WASD hold; cleared on release.
static const int  RETREAT_CACHE_SIZE       = 256;
static Character* s_retreatSessionCache[RETREAT_CACHE_SIZE];
static int        s_retreatSessionCacheCount = 0;

static void otsRestoreNames();
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
    // Load or teardown: the scene and the detached node are gone, so no camera calls here.
    s_fpActive                    = false;
    s_fpNode                      = nullptr;
    s_fpCursorCaptured            = false;
    s_fpCamLocalsSaved            = false;
    s_fpHadAutoTrack              = false;
    // Restoring the grass and foliage range is safe (it only writes option globals) and must
    // happen, or a teardown that bypassed exitFirstPerson leaves the range boosted.
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
    s_fpCrosshair                 = nullptr;  // GUI torn down; recreated on the next FP enter
    otsRestoreNames();
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
    s_userWantsFP           = false;   // survivable loads keep the FP intent; hard teardown clears it
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

static bool computeWASDDirection(bool bW, bool bA, bool bS, bool bD, Ogre::Vector3& outDir)
{
    if (!ou || !ou->player || !ou->player->camera) return false;
    Ogre::Vector3 camFwd;
    if (s_firstPersonActive)
    {
        // The game camera controller is detached and stale in first person, so W follows s_fpYaw.
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

// With WasdSpeedCap on, the limit is CharStats::getMaxRunSpeed times WasdSpeedMult, clamped
// further while chained or sneaking. Off restores the old uncapped ~99.
static float wasdMoveLimit(bool turning)
{
    const float turnBoost = turning ? g_loco.wasdTurnResponsiveness : 1.0f;
    if (!s_settingWasdSpeedCap || !s_freeMoveAnchor)
        return 99.0f * g_loco.wasdAccelerationMultiplier * turnBoost;

    float legit = 99.0f;
    CharStats* st = s_freeMoveAnchor->getStats();
    if (st)
    {
        float m = st->getMaxRunSpeed();
        if (m > 0.1f) legit = m;
    }
    // The direct move bypasses the shackle limit, so clamp to a slow shuffle while chained.
    if (s_freeMoveAnchor->isChainedMode())
    {
        float shuffle = legit * 0.35f;
        if (shuffle > 6.0f) shuffle = 6.0f;
        legit = shuffle;
    }
    // Match vanilla sneak movement, which is capped at the stealth-skill speed.
    if (st && s_freeMoveAnchor->isStealthMode())
    {
        float sneakMax = st->calculateMaxStealthSpeed();
        if (sneakMax > 0.1f && sneakMax < legit) legit = sneakMax;
    }
    legit *= s_settingWasdSpeedMult;

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

static bool applyPlayerMovement(bool bW, bool bA, bool bS, bool bD)
{
    CharMovement* mv = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
    if (!s_freeMoveAnchor || !mv) return false;
    if (!ou || !ou->player || !ou->player->camera) return false;

    Ogre::Vector3 move;
    if (!computeWASDDirection(bW, bA, bS, bD, move)) return false;

    bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
    bool turning    = prevHasDir && (move.dotProduct(s_prevWasdDir) < 0.9f);
    float limit     = wasdMoveLimit(turning);

    mv->halt();
    mv->setDesiredSpeed(mv->speedOrders);
    mv->setDirectMovement(move, limit);
    s_prevWasdDir = move;
    return true;
}

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

// Used only at instant_stop and combat_state_restored_after_wasd_release; it is too broad
// for the movement injection sites.
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
    // Combat states are gated on combatModeActive: a state left stale after a fight (DECISION
    // or STUMBLE after a knockdown) otherwise blocks the release-stop and delays WASD after
    // getting up. The physical states above stay ungated.
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

// A one-shot combat clip that must finish before WASD takes over: Kenshi cannot abort a clip,
// so cutting one with movement stutters. While true, charMovUpdate holds movement and
// combatGo_hook lets go() run, so exactly one system drives the body. DECISION, BLOCK, CIRCLE,
// WAIT and HESITATE persist or re-trigger attacks, so they are excluded to let the player
// retreat. Gated on combatModeActive because stale post-combat states must not count.
static bool isCommittedCombatClip(Character* ch)
{
    if (!ch) return false;
    CombatClass* cc = ch->getCombatClass();
    if (!cc || !cc->combatModeActive) return false;
    swordStateEnum st = cc->getCombatState();
    return st == STARTUP_STATE || st == CHOP_WEAPON
        || st == STUMBLE       || st == REACTION_BLOCK;
}

// A character using furniture (chair, bed, machine) is locked to its node, so
// setDirectMovement only rotates the model; charMovUpdate issues a real move order instead.
// A job-driven seat shows PRETEND_TO_OPERATE_MACHINERY, not OPERATE_MACHINERY, and both must
// match or a job-seated character rotates in place after a squad switch. The SIT, BED and REST
// goals never surface as the current action and are kept as a harmless fallback.
static bool isSeatedTaskType(TaskType t)
{
    return t == OPERATE_MACHINERY
        || t == PRETEND_TO_OPERATE_MACHINERY
        || t == SIT_AROUND || t == SIT_ON_THRONE
        || t == USE_BED    || t == USE_BED_ORDER
        || t == REST;
}

static bool isAnchoredToFurniture(Character* ch)
{
    if (!ch) return false;
    if (ch->inSomething == IN_BED) return true;
    CharBody* body = ch->getBody();
    if (body)
    {
        Tasker* action = body->getCurrentAction();
        if (action && isSeatedTaskType(action->key()))
            return true;
    }
    return false;
}

static bool isUsingStationaryTurret(Character* ch)
{
    if (!ch) return false;
    // isUsingTurret is a handle to the turret building; it is truthy while valid.
    return (bool)(ch->isUsingTurret);
}

// Each enemy is processed once per WASD hold. WASD release clears the cache.
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
    // When the cache is full, drop the entry: the cache is only an optimization.
}

static bool isDownedButMovable(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    // isUnconcious() is also true for playing-dead and crippled characters, which can
    // still crawl, so only PS_KO is a hard block.
    if (prone == PS_KO) return false;
    if (ch->isCurrentlyGettingUp) return false;
    if (prone == PS_PLAYING_DEAD || prone == PS_CRIPPLED) return true;
    if (ch->isDown() && !ch->isUnconcious()) return true;
    return false;
}

// Debounces isInsideBuildingLoadedInterior for 400 ms. On stairwell and roof
// transitions the raw flag flickers between floor layers, and the crawl then flaps
// between order mode and direct mode, each mode cancelling the other.
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

// Always false: downed movement is direct-injected everywhere, like standing WASD.
// Order-driven movement failed: indoors, the interior router path-walks any order
// regardless of the destination; outdoors, a blind 10 m destination on a roof or
// elevated ground lands off the structure and the path routes back down. The order
// machinery stays dormant so it can be restored.
static bool downedOrderDriven(Character* ch)
{
    (void)ch;
    (void)&stableIndoors;   // keep dormant order machinery referenced
    return false;
}

// Uses playerMoveOrderDefault (the pathfind/crawl path) because setDirectMovement
// is valid only for standing locomotion.
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
    dir /= dlen;   // the pathfind destination needs the direction only

    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG    nowDI   = GetTickCount64();

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

    const float hopLen     = 100.0f;   // the point-click range, 10 m
    const float approachAt = 60.0f;

    // Keep ONE persistent order, like a point-click: re-issuing every frame restarts
    // pathfinding before it produces motion. This works only because the press-edge
    // disengage and the post-AI standing injection are gated off for downed characters;
    // otherwise they cancel the order after a few steps.
    if (s_wasdDownedMovementActive
        && dir.dotProduct(s_downedLastDir) > 0.95f
        && nowDI - s_downedLastIssueMs < 1500
        && (s_downedLastDest - posNow).length() > approachAt)
        return;
    s_downedLastDir     = dir;
    s_downedLastIssueMs = nowDI;

    // A far destination (50 m) lies off the local navmesh, so the first leg of the
    // path can head in the wrong direction. Short hops behave like nearby point-clicks.
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

// First-person mouse-look reads a second, non-exclusive background DirectInput
// mouse, so the game keeps its own input. A 1 kHz thread accumulates the relative
// counts and each frame takes the total, which makes the look independent of the
// frame rate (GetCursorPos/SetCursorPos sampling felt sluggish at high fps).
// DirectInput8Create and the GUIDs are resolved here because the dxguid/dinput8
// import libs are not reliably on the v100 lib path.
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static const GUID DIFP_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID DIFP_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
// DIMOUSESTATE2 lX/lY/lZ are at offsets 0/4/8; a NULL pguid matches any axis object.
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

static void fpTakeMouseAccum(float* dx, float* dy)
{
    *dx = (float)InterlockedExchange(&s_diAccX, 0);
    *dy = (float)InterlockedExchange(&s_diAccY, 0);
}

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

static void otsRestoreNames()
{
    if (!s_namesHidden) return;
    if (gui) gui->showNames(s_savedShowNames);
    s_namesHidden = false;
    DebugLog("[WASDCombat] dc_names_restored");
}

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

// The attached RTS camera can only look top-down, so the inventory face-cam detaches
// the camera onto its own root node. The face-cam runs only while the game is paused
// and the character stands.
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

// True for a plain inventory and for a backpack character (2 windows), false for any
// trade or loot session. Both inputs are live, never cached; see s_tradeWindowActive.
static bool isOwnInventoryOpen()
{
    return gui && !s_tradeWindowActive
        && !gui->isCharacterEditorMode()   // the editor owns the camera
        && gui->getNumOpenInventoryWindows() >= 1;
}

static bool invMoveThroughEligible()
{
    return s_mode == MODE_FREE_MOVE
        && s_freeMoveAnchor
        && !s_settingInventoryFaceCam
        && gui && gui->isAnyInventoryWindowOpen()
        && !gui->inDialogue()                    // dialogue keeps its vanilla pause
        && !gui->isCharacterEditorMode();
}

// First person shares the detached-camera machinery (s_fpNode, saved camera
// locals) with the inventory face-cam; s_firstPersonActive selects the drive mode.

// The head bone is manually controlled so that animation cannot rescale it; the neck
// and body keep animating.
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

// The skeleton MUST come from AppearanceBase::getSkeleton(): Entity::getSkeleton()
// on the body entity returns a different runtime type whose virtual calls crash.
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

    // hasBone validates the skeleton and picks a bone that exists, so
    // getBoneWorldPosition never sees a missing bone. The true-world path prefers the
    // head bone; the synthetic path prefers the neck, which keeps animating while the
    // head is scale-hidden.
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
        // getBoneWorldPosition gives the bone's true world position, independent of the view
        // yaw, so a pure pan does not swing the eye on an arc as root + rotate(offset, yaw)
        // did.
        Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
        float ddx = head.x - root.x, ddy = head.y - root.y, ddz = head.z - root.z;
        bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f
                      && head.y > root.y - 1.0f;
        if (plausible)
        {
            out           = head;
            s_fpBoneEyeUp = 0.0f;   // eye level comes from EyeDrop on this path
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
        // Implausible (skeleton still loading or an odd rig): use the synthetic path.
    }

    // The bone's derived position is model space relative to the character root, and
    // the entity node transform is stale, so rotate the offset by the view yaw.
    Ogre::OldBone* b = sk->getBone(used);
    if (!b) return false;
    s_fpBoneEyeUp = (strstr(used, "Neck") != nullptr) ? 2.4f
                  : (usedIdx >= 0 && !s_fpTrueBoneEye ? BONE_SYNTH_UP[usedIdx] : 1.0f);
    Ogre::Vector3 boneModel = b->_getDerivedPosition();
    float mountYaw = s_fpYawSm;
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

// The captured cursor sits at the viewport center, so LMB/RMB act on what the
// crosshair covers. Load teardown nulls the pointer; it is recreated on the fresh GUI.
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

// ContextMenuGUI is forward-declared, so optionsList is read at its offset 0xF8.
static const MyGUI::Colour FP_MENU_ACCENT(0.72f, 0.86f, 0.38f, 1.0f);
static const MyGUI::Colour FP_MENU_NORMAL(0.78f, 0.75f, 0.66f, 1.0f);
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
            *(MyGUI::Widget**)((char*)menus[m] + 0xF8);
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

static void exitFirstPerson(bool restoreCamera)
{
    if (!s_firstPersonActive) return;
    s_firstPersonActive = false;
    s_fpCursorCaptured  = false;
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

// Camera calls MUST run on the game thread.
static void enterFirstPerson()
{
    if (s_firstPersonActive || s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;

    // Re-read [FirstPerson] on every entry so INI edits apply without a relaunch.
    {
        char cfgPath[MAX_PATH];
        getConfigPath(cfgPath, sizeof(cfgPath));
        loadFirstPersonConfig(cfgPath);
    }

    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    cam->stopFollowing();   // zoom is left untouched, so the restored view is the pre-FP view
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
    // Kenshi sizes the grass range for the high top-down camera, so at ground level
    // grass pops in at the edge of a short ring. Widen the range while first person
    // owns the view, and restore the exact saved values on exit.
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
    s_fpHaveLastFeet    = false;
    s_fpMoveSpeed       = 0.0f;
    s_fpMoveFwdSmooth   = 0.0f;
    s_fpFeetTickMs      = 0;
    s_fpSmValid         = false;    // re-seeded on the first frame
    s_fpEnemyClearSmooth = 1.0f;
    s_fpEnemyNearestDist = -1.0f;
    s_fpLastStreamValid = false;    // forces a streaming teleport on the first FP frame
    s_fpFrozenValid     = false;
    s_fpBodyYaw         = s_fpYaw;
    s_fpHeadSmoothValid = false;
    s_fpBoneLogged      = false;
    s_firstPersonActive = true;
    s_fpCursorCaptured  = false;   // the first capture pass establishes the center
    if (s_fpHideHead)
        fpSetHeadBoneHidden(true);
    fpShowCrosshair(true);
    DebugLog("[WASDCombat] dc_fp_entered");
}

// Runs from cameraUpdate_hook AFTER the game's camera update.
static void fpDriveFrame(CameraClass* thisptr, bool uiOpen)
{
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || !s_freeMoveAnchor->movement || s_lootUiSuspendActive)
    {
        exitFirstPerson(true);
        return;
    }

    // Any open UI releases the mouse so the free OS cursor can click menu items.
    fpShowCrosshair(!uiOpen);

    // Mouse-look also pauses while the RMB hold-menu is up, so the freed cursor can
    // browse the options; releasing RMB selects.
    bool rmbHeld    = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool ctxVisible = ou->player->contextMenu.isVisible();
    if (rmbHeld || ctxVisible)
        fpTintContextMenu();
    bool captureOk = !uiOpen && !s_menuSuspendActive && !rmbHeld && !ctxVisible
                  && !s_lootUiSuspendActive && isKenshiForegroundMain();
    if (captureOk)
    {
        // Falls back to cursor warp until the DirectInput device is acquired.
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
                fpTakeMouseAccum(&dx, &dy);
                haveDelta = s_fpCursorCaptured;    // skip the baseline frame
                SetCursorPos(center.x, center.y);  // keep the click point centered
                s_fpCursorCaptured = true;
            }
            else
            {
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
                // The point-click FollowTurn yields until the mouse has been still for
                // FollowDelayMs; the deadzone ignores 1 px jitter.
                if (dx > 1.0f || dx < -1.0f || dy > 1.0f || dy < -1.0f)
                    s_fpLastMouseMoveMs = GetTickCount64();
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;  // re-baseline when capture resumes
        // Drain deltas accumulated while a UI owned the cursor, so the view does not jump.
        if (s_fpRawMouse) { float jx, jy; fpTakeMouseAccum(&jx, &jy); }
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    // currentlyMoving/currentSpeed are set whatever issued the move, so this covers
    // point-click and autonomous combat/heal movement as well as WASD.
    bool fpMoving = mvFP->currentlyMoving || mvFP->currentSpeed > 0.25f;

    // WASD (and a grace period while still coasting): face the body to the view, so
    // movement is strafe-relative and the camera yaw is never touched. Without the
    // grace, the body still faces 180 degrees from the view on the release frame of a
    // backpedal, and the idle neck-limit snaps the camera onto it.
    // Point-click/autonomous: the camera follows the heading.
    // Idle: the neck-limit turns the body when the view turns too far.
    bool strafeGrace = (GetTickCount64() - s_wasdLastHeldMs) < FP_STRAFE_GRACE_MS;
    if (s_frameWasdHeld || (fpMoving && strafeGrace))
    {
        // Turn only the visible body; the eye is mounted on the view yaw, so the camera never jumps.
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
        // The character walks its own path here, so the view follows the heading; the
        // game's pathing owns mvFP->direction. It runs only after the mouse has been still
        // for FollowDelayMs, so it never fights active mouse-look.
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
            s_fpBodyYaw = headYaw;   // kept in sync so a later stop does not snap
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
            // Keep the body yaw synced so the next movement turn does not start with a jump.
            s_fpBodyYaw = bodyYaw;
        }
    }

    // Only the visuals use the smoothed view; body facing and motion use the raw
    // target. A smaller per-frame rotation reduces the grass re-facing mismatch on the
    // render thread, so foliage flickers less. LookSmooth=0 gives the raw value exactly.
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

    // Scale the forward eye offsets down as a hostile gets close, so an aggressor
    // pressing into the lens pulls the eye back to the hidden skull instead of the eye
    // entering their model.
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

    // Legacy path: height is smoothed to damp stride bob, and horizontal position is
    // hard-attached so a sprinting model cannot outrun the camera.
    Ogre::Vector3 eye;
    Ogre::Vector3 headWorld;
    if (fpGetHeadWorld(headWorld))
    {
      if (s_fpTrueBoneEye)
      {
        // The eye is welded to the head bone's world Y (no bob smoothing) and pushed
        // forward along the yaw only, so looking down does not sink the eye into the
        // chest. Ground speed from the feet delta drives the forward lead; the
        // currentMotion magnitude is too noisy for this.
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
            float gait = s_fpMoveSpeed / s_fpMoveSpeedRef;
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            lead = s_fpMoveForward * gait;
        }
        s_fpMoveFwdSmooth += (lead - s_fpMoveFwdSmooth) * 0.15f;

        // While climbing, shrink the forward push and lift the eye so the camera clears
        // the rising steps. On flat ground ascent01 is 0, so this is inert.
        float ascent01 = (s_fpClimbSpeedSmooth > 0.0f)
                       ? s_fpClimbSpeedSmooth * s_fpStairForwardReduce : 0.0f;
        if (ascent01 > 1.0f) ascent01 = 1.0f;
        float ascentScale = 1.0f - ascent01 * (1.0f - s_fpStairForwardMinScale);
        float stairLift   = s_fpStairEyeLift * ascent01;

        // The speed factor must update on this path too: it drives the MoveNearClip blend,
        // which was dead under TrueBoneEye when only the legacy path updated it.
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
        // At jog/sprint the model pitches forward and swings the arms and chest up into
        // view, so raise the eye and push it forward with speed. Walking is unaffected.
        {
            float spd = mvFP->currentMotion.length();
            float leanTarget = spd * 0.04f;          // about 1.0 at jog speed
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }
        float leanUp  = s_fpMoveLeanUp  * s_fpMoveLeanSmooth;
        float leanFwd = s_fpMoveLeanFwd * s_fpMoveLeanSmooth;
        eye = s_fpHeadSmooth
            + q * Ogre::Vector3(0.0f, s_fpBoneEyeUp + s_fpEyeUpAdjust + leanUp,
                                -((s_fpFwdOffset + leanFwd) * s_fpEnemyClearSmooth));

        // The bone pose read this frame is last frame's animation, which leaves the camera
        // a stride behind at sprint. Feed forward velocity ahead by the frame time, along
        // the view forward only: a lateral or backward offset made strafing twitch.
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

        // Keyed off the discrete gait tier, not the noisy currentMotion magnitude, and gated
        // on actual movement so point-click and combat running get it as well. WALK is 0.
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed gait = mvFP->speedOrders;
                if (gait == JOG)      gaitTarget = s_fpJogForward;
                // GROUPED is the squad-follow sprint, so it gets the same fix as RUN.
                else if (gait == RUN || gait == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpGaitFwdSmooth * s_fpEnemyClearSmooth);
            }
        }

        // Attack swings, blocks, heals, revives and get-ups swing the body into the lens
        // even while standing, which the gait push cannot help. 0 (default) = off.
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
      }
    }
    else
    {
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
        // FreezeCamTest (diagnostic): while standing, lock the camera position so a pan
        // changes only the orientation. This tests whether the grass pager reads the
        // camera position.
        if (s_fpFreezeCamTest && !fpMoving)
        {
            if (!s_fpFrozenValid) { s_fpFrozenEye = eye; s_fpFrozenValid = true; }
            camPos = s_fpFrozenEye;
        }
        else s_fpFrozenValid = false;
        s_fpNode->setPosition(camPos);
        s_fpNode->setOrientation(q);
    }

    // MoveNearClip pushes the near plane out at speed to slice away the arms that the
    // jog/sprint animation sweeps into the lens. EnemyNearClip pulls it in while a
    // hostile overlaps the lens, so the plane slices a thin cross-section instead of
    // opening a big hole in the aggressor; it wins because it takes the minimum.
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

    // teleport() keeps the rig coherent (audio listener, zone streaming), but it is a
    // jump: calling it every frame makes the streamer re-page grass continuously, so
    // foliage flickers while moving. Re-teleport only after the eye has moved
    // StreamUpdateDist; StreamUpdateDist=0 restores the every-frame behavior.
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

    // The foliage pager streams grass around the camera CENTER node, which lags at the
    // feet while first person renders from the head (technique from KenshiFP):
    // 1. Move the center only while moving. The eye swings in a small circle during a
    //    pan, so moving the center then re-scatters the distant grass.
    // 2. Set the world position with _setDerivedPosition, because the eye lives under
    //    our own node.
    // 3. Force the derived-position recompute, so the same frame's paging pass reads
    //    the new center instead of a stale value.
    if (s_fpFoliageCenterMode && thisptr->center && s_fpNode && fpMoving)
    {
        Ogre::Vector3 eyeWorld = s_fpNode->_getDerivedPositionUpdated();
        thisptr->center->_setDerivedPosition(eyeWorld);
        thisptr->center->_getDerivedPositionUpdated();
    }
}

// Runs AFTER the game's camera update and BEFORE render: only writes here survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    // Clear the vanilla MMB-rotate state before the game's update, so MMB does nothing.
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;

    // CTRL is a press-on/press-off rotate toggle: force the rotate flag ON (never OFF,
    // so vanilla MMB hold-to-rotate still works). Any open UI turns the toggle off
    // below, because the game does not free an already-captured rotate cursor in the
    // pause menu or the RMB hold-menu.
    if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_firstPersonActive && s_camRotateToggle && key)
        key->rotate = true;

    // controlEnabled=false is Kenshi's own "ignore camera input" gate. While the
    // detached camera is active, the wheel zoom and WASD/edge pan would otherwise move
    // the camera during orig, and the vanilla update lays out the name-tags from that
    // moved camera before any post-orig restore can undo it.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // Also drive the FP camera BEFORE orig: the per-frame foliage visibility and
    // billboard pass samples the camera during the update, and a post-orig drive alone
    // left foliage one frame behind, so grass blinked while rotating. The double drive
    // is safe because the first call recenters the cursor, so the second reads a zero
    // delta.
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

    // Touching the camera while the scene is torn down reads freed memory, so only mark
    // the restore as pending. Do NOT gate on s_dcShutdownInProgress or
    // s_loadGuardActive: they stay true through character creation and would strand
    // the camera detached, hiding the new-game character preview.
    bool sceneFreeing = !ou || ou->isLoadingFromASaveGame();
    if (sceneFreeing)
    {
        if (s_fpActive || s_firstPersonActive)
        {
            s_fpActive          = false;
            s_firstPersonActive = false;
            s_fpCursorCaptured  = false;
            s_fpHeadBoneHidden  = false;  // the skeleton is gone, so the restore flags are moot
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

    // thisptr is a valid camera even when ou->player is null, so restore through thisptr.
    if (s_otsRestorePending)
    {
        s_otsRestorePending = false;
        s_fpSuspendedForInv = false;   // a load cancels any pending FP auto-return
        otsRestoreCameraToRig(thisptr);
        if (ou && ou->player && s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        otsRestoreNames();
        DebugLog("[WASDCombat] dc_cam_restored_after_load");
        return;
    }

    // Kenshi also calls this hook for the inventory portrait cameras; if they drove the
    // detached camera, it would detach and re-attach every frame.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // ANY open UI turns the CTRL rotate toggle off and releases the cursor; the player
    // presses CTRL again after closing it. s_camRotateUiOpen also stops CTRL+click in
    // a menu from re-toggling it. The world map and the stats window do not clear
    // controlEnabled, so they are checked explicitly.
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

    {
        // Only a true teardown exits immediately. Mode, anchor validity and the window
        // count all blip for a frame or two during a squad switch with the inventory open,
        // so they are debounced; otherwise the face-cam detaches and re-attaches on every
        // switch.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Combat disables the face-cam, so opening an inventory mid-fight to loot leaves
        // the camera in place. It is a hard exit, so a single flicker-false frame cannot
        // latch the face-cam through the debounce.
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
            faceCamWanted = s_fpActive && s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE;
        }

        // First person owns the detached camera through s_firstPersonActive, not
        // s_fpActive. Suspend it while the face-cam wants the camera, and return to it
        // when the inventory closes.
        if (s_firstPersonActive && faceCamWanted)
        {
            exitFirstPerson(true);
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
                enterFirstPerson();
            }
            return;
        }
    }

    // A debounce edge or squad-switch churn can clear s_fpActive on a frame where the
    // branch above does not return to first person, which strands the player in
    // top-down view.
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

    if (s_firstPersonActive)
    {
        // The game derives the displayed floor from the tracked character inside
        // restrictPosition, which never runs while first person detaches the camera and
        // passes controlEnabled=false. Without this sync, upper floors do not load.
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
        // The squad-based pass above hides storeys no squad member stands on, so looking
        // down from an upper floor shows black voids. Reveal everything up to the anchor's
        // floor, after that pass, so this is the frame's final word. Vanilla cuts a
        // building away only while the tracked character is inside it, so an anchor in no
        // building resets the cutaway; the Building* is only null-checked, never
        // dereferenced.
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

    // The wheel still reaches the game's camera zoom inside orig, which scales the
    // name-tags; hold the altitude saved on enter.
    thisptr->altitude = s_otsSavedAltitude;

    bool ctxVisible = ou->player->contextMenu.isVisible();
    bool anchorKO   = s_freeMoveAnchor->isUnconcious();
    // inDialogue also covers the prisoner and bail dialogue windows, which need the cursor.
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

    // Own-inventory face-cam: an upper-chest shot facing the character so worn gear is
    // visible. CameraClass::update still ticks under the inventory pause, so the
    // detached node frames cleanly where the vanilla camera could not.
    {
        bool ownInv = isOwnInventoryOpen();
        // Clicking portraits with the inventory open changes the selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // The open window's character comes first: recruited mod NPCs do not report
            // isPlayerCharacter(), so the selection fallback would frame the wrong character.
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
            s_fpPitch    = -0.02f;
            s_otsDistCur = 20.0f;
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

    // Recompute the front-facing pose from the target's facing every frame; an
    // edge-triggered aim left a stale yaw on a re-open. The target priority matches
    // the face-cam swing above.
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
        float faceYaw = (bl > 0.001f) ? atan2f(bd.x, bd.z) : 0.0f;  // forward = -bd, the character's front
        Ogre::Quaternion q =
            Ogre::Quaternion(Ogre::Radian(faceYaw), Ogre::Vector3::UNIT_Y) *
            Ogre::Quaternion(Ogre::Radian(-0.02f),  Ogre::Vector3::UNIT_X);
        Ogre::Vector3 pivot = mvP->pos;
        pivot.y += 12.5f;                                  // upper chest
        Ogre::Vector3 eye = pivot + q * Ogre::Vector3(0.0f, 0.5f, 20.0f);
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }
}

// Interior floor visibility refreshes only when restrictPosition runs, but its
// camera clamp yanks the detached view back. Run it, then re-apply this frame's
// pose.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // Skip the RTS clamp while the camera is detached: it yanks the free camera back to
    // the floor. First person drives the floor reveal from the camera hook instead.
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}

static void (*s_charMovUpdateOrig)(CharMovement* thisptr, float time);

// MOVE_CUS_ORDERED is deliberately absent: DC issues its own disengage orders with it.
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

// The single hold authority, shared by the motion gate and the end-of-mainLoop
// position clamp. Returns true when vanilla may move the anchor. The exemptions
// yield the hold to systems that must move or animate the character. It uses
// isProtectedAnimationState, not isCommittedAction, because the door Tasker owns
// STARTUP_STATE and would never release the hold.
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

// A move order inside a building can trip the vanilla "I can't get out of here"
// bark, because the path to any destination may run through a locked door.
static bool moveOrderMayBark(Character* ch)
{
    if (!ch) return false;
    CharMovement* mv = ch->movement;
    // All of these signals are false outdoors, so the outdoor click-cancel is unchanged.
    // isInsideBuildingLoadedInterior() is false unless the interior is loaded, so
    // isIndoors() is also needed. isPrisonerFreeToGo() is not used: its value for a
    // free non-prisoner is unverified, and it could skip the disengage everywhere.
    return mv && (mv->isInsideBuildingLoadedInterior() || mv->isIndoors());
}

// Cancels any in-flight path order by moving to the current position. Skipped
// where the order could bark; halt(), the zeroed motion and the next-frame
// hold-clamp park the character without it.
static void dcSnapCancelOrder(Character* ch)
{
    if (ch && ch->movement && !moveOrderMayBark(ch))
        ch->playerMoveOrderDefault(nullptr, nullptr, ch->movement->pos);
}

static void charMovUpdate_hook(CharMovement* thisptr, float time)
{
    // This pointer comparison is the only cost for every non-anchor CharMovement::update.
    if (thisptr != s_anchorMovement || s_loadGuardActive || s_dcShutdownInProgress)
    {
        if (s_dcShutdownInProgress && !s_hookBlockLoggedCharMov) {
            s_hookBlockLoggedCharMov = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=charMovUpdate"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    ScopeTimer _tCM(s_prof_charMove);
    // Cache the real speed tier every frame: a travel or save load frees the character
    // before the load path can read it, and the old RUN fallback forced a sprint after
    // every load.
    if (thisptr->speedOrders < GROUPED)
        s_dcPreservedSpeedMode = thisptr->speedOrders;
    bool wasdHeld    = s_frameWasdHeld;
    bool inVMode     = (s_frameMode == MODE_FREE_MOVE);
    bool lootSuspend = s_frameLootSuspend;

    // Combat key-roll bridge: a short gap between keys keeps driving in the last
    // direction, so the AI does not square up mid-roll. The bridge does not refresh
    // s_wasdLastHeldMs, so it expires on its own.
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
        if (inVMode && !wasdHeld && !lootSuspend)
        {
            Character*   chR = thisptr->getCharacter();

            // The grace window stops a rapid tap or key switch from stuttering through a stop.
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
                    // Indoors, playerMoveOrderDefault path-walks the interior network even to the
                    // current position, which gives a one-frame step before the hold-clamp engages.
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

            // Position is clamped after orig as well, because indoor routing writes the
            // position late in the frame.
            const char* holdReason = "";
            if (!computeHoldDecision(chR, &holdReason))
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = thisptr->pos;
                    s_holdPosValid = true;
                }
                thisptr->halt();
                thisptr->movementMode  = MOVE_DIRECTION;
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
    // An order-driven downed crawl must not get halt() + setDirectMovement, which would
    // cancel the order.
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

            // Lingering combat mode computes its own movement inside update and fights the
            // crawl order. Clear the flag for the integration step only; CombatClass::go
            // still sees it in the AI phase.
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
    // Only a real key press refreshes the timestamp; otherwise one tap would drive
    // forever in combat.
    if (!combatBridge)
        s_wasdLastHeldMs = GetTickCount64();
    s_holdPosValid   = false;  // recaptured on the next hold

    Ogre::Vector3 wasdDir;
    bool dirOk = computeWASDDirection(s_wHeld, s_aHeld, s_sHeld, s_dHeld, wasdDir);
    if (!dirOk && combatBridge)
    {
        wasdDir = s_prevWasdDir;
        dirOk   = true;
    }

    // Seated, sleeping or working characters are locked to the furniture node, so
    // setDirectMovement only spins the model. A point-click move order gets them up
    // with the proper animation and replaces the queued use job, so the AI does not
    // pull them back onto the furniture.
    if (dirOk)
    {
        Character* chSeat = thisptr->getCharacter();
        if (chSeat && (isAnchoredToFurniture(chSeat) || chSeat->isCurrentlyGettingUp))
        {
            chSeat->playerWantsMeToGetUp = true;
            // Throttled re-issue, not a once-per-sit latch: a latch stuck across squad
            // switches and job re-sits, so the character could no longer leave the chair.
            static ULONGLONG s_lastFurnitureExitMs = 0;
            ULONGLONG nowF = GetTickCount64();
            if (isAnchoredToFurniture(chSeat) && (nowF - s_lastFurnitureExitMs) > 600)
            {
                Ogre::Vector3 dest = thisptr->pos + wasdDir * 50.0f;   // about 5 m ahead (10 units per metre)
                chSeat->playerMoveOrderDefault(nullptr, nullptr, dest);
                s_lastFurnitureExitMs = nowF;
                DebugLog("[WASDCombat] dc_furniture_exit_move_order");
            }
            s_wasdMovementApplied = false;   // let the get-up run
            s_prevWasdDir         = wasdDir;
            static ULONGLONG s_seatGetupTick = 0;
            ULONGLONG nowSG = GetTickCount64();
            if (nowSG - s_seatGetupTick >= 1000) { s_seatGetupTick = nowSG;
                DebugLog("[WASDCombat] dc_wasd_getup_from_furniture"); }
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }

    // Kenshi cannot abort an animation clip, so cutting one with movement stutters.
    // Buffer movement while a committed combat clip (swing, stagger, parry) finishes
    // in place; combatGo_hook then suppresses go(), so no new attack chains. No combat
    // state is touched here, so only one system drives the body. Other combat states
    // still yield to movement so the player can always retreat.
    if (dirOk && isCommittedCombatClip(thisptr->getCharacter()))
    {
        s_wasdMovementApplied = false;
        static ULONGLONG s_clipBufTick = 0;
        ULONGLONG nowSB = GetTickCount64();
        if (nowSB - s_clipBufTick >= 1000) { s_clipBufTick = nowSB;
            DebugLog("[WASDCombat] dc_wasd_buffered_combat_clip"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    if (dirOk)
    {
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

        // Pre-charge currentMotion. At low FPS the engine often fails to integrate past the
        // pre-charge, so a fractional boost makes an injured character (desiredSpeed about
        // 55) lurch between full speed and a crawl. The floor is the full desired speed,
        // clamped to the WASD move-limit: this velocity write, not setDirectMovement, is
        // what binds the speed cap.
        float accel = (g_loco.wasdAccelerationMultiplier - 1.0f)
                    * (turning ? g_loco.wasdTurnResponsiveness : 1.0f);
        if (accel > 0.0f)
        {
            float boost = accel < 1.0f ? accel : 1.0f;
            float preSpeed = thisptr->desiredSpeed * boost;
            float floorSpeed = thisptr->desiredSpeed;
            if (s_settingWasdSpeedCap && floorSpeed > limit) floorSpeed = limit;
            if (preSpeed < floorSpeed) preSpeed = floorSpeed;
            if (s_settingWasdSpeedCap && preSpeed > limit) preSpeed = limit;
            thisptr->currentMotion = wasdDir * preSpeed;
        }

        s_prevWasdDir = wasdDir;

        // WASD always uses plain walk/run locomotion. Restoring combatModeActive after the
        // integration step kept the combat animation layer on, so the body stayed in the
        // arms-down pose. Leave it cleared while driving: go() is already suppressed, so
        // no combat XP is lost, and the AI re-engages on release.
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

        // The combat AI re-enables animationOverride and changes movementMode when it wants
        // to drive the body, so re-assert MOVE_DIRECTION after orig.
        {
            Character*   chPost = thisptr->getCharacter();
            CombatClass* ccPost = chPost ? chPost->getCombatClass() : nullptr;
            if (ccPost)
            {
                // Do not cut the combat state here: writing COMBAT_FINISHED every frame fought the
                // combat system and stuttered. Committed clips are buffered above, and
                // combatModeActive is cleared, so nothing slides.
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
        // Race: the snapshot said WASD was held, but the poll thread released the keys
        // before the direction was computed.
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

// Exclusive ownership of the anchor's locomotion, handed off at the press/release
// edge, never fought per frame (the per-frame tug-of-war caused the combat
// stutter). While WASD is held, plus the key-roll bridge, go() is skipped so the AI
// never steers. On release go() runs fully autonomously.
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
        // Let go() run while a committed clip plays so the clip finishes and the state
        // advances; suppressing it mid-clip would freeze the state machine and the movement
        // buffer would stick forever.
        if (movementOwns && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;
            return;
        }
    }
    s_retreatLockGoSuppressed = false;
    s_combatGoOrig(thisptr, frameTime);
}

// mainLoop_hook execution order:
//   1. Safety gate (load-guard)
//   2. Selection tracking
//   3. V-Mode transition
//   4. HUD
//   5. Pre-AI WASD
//   6. s_mainLoopOrig (AI + CharMovement::update + CombatClass::go)
//   7. Post-AI combat job suppression
//   8. Periodic squad-threat scan
//   9. Post-AI WASD re-application + instant stop
static void (*s_mainLoopOrig)(GameWorld* thisptr, float time);

static void mainLoop_hook(GameWorld* thisptr, float time)
{
    // The stabilization countdown runs here so it advances while the shutdown flag is held.
    if (s_dcShutdownInProgress)
    {
        if (!s_hookBlockLoggedMain) {
            s_hookBlockLoggedMain = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=mainLoop"); }

        if (ou && ou->player) s_mainLoopOrig(thisptr, time);

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
                s_stabilizationCountdown = 0;
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

            if (s_userWantsDC)
            {
                // A real save-load frees the anchor while ou->player can still be valid, so never
                // dereference it here: drop it and keep s_userWantsDC so the reacquire restores DC.
                if (ou->isLoadingFromASaveGame())
                {
                    if (!s_dcPtrLossActive)
                    {
                        s_dcPtrLossActive      = true;
                        s_dcPtrLossStartedAt   = GetTickCount64();
                        s_dcPtrLossLastLogTick = 0;
                        // Do not touch speed: charMovUpdate_hook already cached the real tier for the reacquire.
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

                // Chunk microload (null player, no save-load flag): characters are not freed, so the
                // anchor dereference below is safe.
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

                // No timeout: pointer loss of any length only pauses injection until the anchor is valid.

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

                s_anchorMovement = s_freeMoveAnchor->movement;
            }
            else
            {
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

        // Fallback: the shutdown block normally clears the load guard; clear it here so it cannot stick.
        if (s_loadGuardActive)
        {
            s_loadGuardActive        = false;
            s_postLoadReacquire      = true;
            s_stabilizationCountdown = 0;
            DebugLog("[WASDCombat] reload_complete_reacquire_started");
        }
    }

    // SaveManager::newGame() sets NEWGAME (0x4) before world teardown; LOADGAME (0x2) cannot match.
    // The LOADGAME path sets the shutdown and load-guard flags after teardown begins.
    {
        SaveManager* sm = SaveManager::getSingleton();
        if (sm && sm->signal == SaveManager::NEWGAME
            && (s_mode == MODE_FREE_MOVE || s_userWantsDC || s_freeMoveAnchor != nullptr))
        {
            DebugLog("[WASDCombat] dc_newgame_signal_seen");
            DebugLog("[WASDCombat] dc_newgame_detection_source=mainloop_signal_poll");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_begin");

            s_userWantsDC = false;
            s_userWantsFP = false;

            // Exit OTS and first person while the anchor and camera are valid: exitOTS re-attaches the
            // detached camera to the rig, otherwise character creation shows no character.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            s_fpSuspendedForInv = false;
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;

            if (ou->player)
            {
                ou->player->stopTrackCharacter();
                if (ou->player->camera)
                {
                    ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                    ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
                }
            }
            // Set both modes so the step-3 exit transition does not stop tracking again or show
            // "Direct Control Disabled".
            s_mode          = MODE_VANILLA;
            s_fmTrackedMode = MODE_VANILLA;
            DebugLog("[WASDCombat] dc_newgame_forced_vanilla_mode");

            // Clear the pointers before world teardown frees their targets.
            s_freeMoveAnchor    = nullptr;
            s_anchorMovement    = nullptr;
            s_selectedCharacter = nullptr;
            s_selectedMovement  = nullptr;
            s_prevAttackTarget  = nullptr;

            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            s_menuSuspendActive       = false;

            s_wHeld               = false;
            s_aHeld               = false;
            s_sHeld               = false;
            s_dHeld               = false;
            s_wasdWasActive       = false;
            s_wasdMovementApplied = false;
            s_prevWasdDir         = Ogre::Vector3::ZERO;
            s_wasdTapStartMs      = 0;
            s_wasdLastHeldMs      = 0;
            s_frameMode           = MODE_VANILLA;
            s_frameWasdHeld       = false;

            s_lootUiSuspendActive  = false;
            s_lootUiWasPrevOpen    = false;
            s_lootSuspendStartTick = 0;

            s_healingJobActive             = false;
            s_healingJobPending            = false;
            s_medicalJobSuppressedThisHold = false;
            s_attackCommitmentActive       = false;
            s_attackCommitmentStart        = 0;

            s_dcPtrLossActive      = false;
            s_dcPtrLossStartedAt   = 0;
            s_dcPtrLossLastLogTick = 0;

            DebugLog("[WASDCombat] dc_newgame_old_state_cleared");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_complete");
        }
    }

    // V-Mode processing pauses while any inventory window is open; s_mode does not change.
    {
        bool anyInvOpen      = gui && gui->isAnyInventoryWindowOpen();
        int  numInvOpen      = gui ? gui->getNumOpenInventoryWindows() : 0;
        bool npcFieldOpen    = gui && gui->inventoryWindowNPC.getCharacter()       != nullptr;
        bool charFieldOpen   = gui && gui->inventoryWindowCharacter.getCharacter() != nullptr;
        bool traderFieldOpen = gui && gui->inventoryWindowTrader.getCharacter()    != nullptr;
        bool tradeAOpen      = gui && gui->tradeA.getCharacter()                   != nullptr;
        bool tradeBOpen      = gui && gui->tradeB.getCharacter()                   != nullptr;

#if LOOT_DIAG
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

        bool moveThrough = invMoveThroughEligible();

        // showTradeWindow_hook can set the suspension before the window is visible; this block
        // confirms the open and close edges.
        if (moveThrough)
        {
            // Move-through: lift any early suspend from the trade hook so movement and tracking continue.
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
            // Kenshi auto-pauses on inventory open and on a squad-member switch. Undo only pauses inside
            // the grace window after those edges; a later pause is the player's own and stays.
            Character* mtShownChar = gui->inventoryWindowCharacter.getCharacter();
            bool mtOpenEdge   = !s_invMoveThroughActive;
            bool mtSwitchEdge = s_invMoveThroughActive
                                && mtShownChar != s_invMoveThroughShownChar;
            s_invMoveThroughShownChar = mtShownChar;
            if (mtOpenEdge || mtSwitchEdge)
            {
                // No grace re-arm while the player has paused, so their pause is never undone.
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
                    ou->userPause(false);
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
                s_invMoveThroughPlayerPaused = false;
                DebugLog("[WASDCombat] inv_move_through_player_unpause");
            }
            if (!s_invMoveThroughActive)
            {
                s_invMoveThroughActive = true;
                DebugLog("[WASDCombat] inv_move_through_begin");
            }
            s_lootUiWasPrevOpen = true;

            // Vanilla never closes a merchant trade window on distance, so close it after the anchor
            // walks away. Trades with your own squad must not auto-close.
            Character* trader = gui->inventoryWindowTrader.getCharacter();
            if (trader && !trader->isPlayerCharacter() && !s_invTradeCloseRequested)
            {
                Ogre::Vector3 ap = s_freeMoveAnchor->getPosition();
                if (!s_invTradeStartValid)
                {
                    s_invTradeAnchorStart = ap;
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
            // Move-through ends: leave the game running, because the player owns pause now.
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
            // Non-trade paths such as showInventoryNPC do not set the suspension in the hook.
            if (!s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = true;
                s_lootSuspendStartTick = GetTickCount64();
            }
            // Do not re-derive s_tradeWindowActive from the npc/trader/tradeA/tradeB fields: they stay
            // stale after a trade closes and mark later own-inventory opens as trades.
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
            // Clear the trade latch before the debounce so the next own inventory is not a trade.
            s_tradeWindowActive = false;
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
            // The hook fired but no window opened; release after 1 s so the suspension cannot stick.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= 1000)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
                s_tradeWindowActive    = false;
            }
        }
    }

    // A reload can lose the anchor while DC stays on; take it again from the selection.
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

    // Safety net: the face-cam normally exits in cameraUpdate_hook.
    if (s_fpActive && !isOwnInventoryOpen())
        exitOTS(true);

    // Register here, not in the loadConfig hook: the game loads its keyboard config before
    // RE_Kenshi loads plugins.
    if (!s_nativeCommandsRegistered)
        registerNativeCommands(key);

    // The watchdog saves rebinds even when the options menu never calls saveOptions.
    if (s_nativeCommandsRegistered)
        watchNativeBindChanges();

    // Hooks inside s_mainLoopOrig read these snapshots, not the volatiles, to avoid a memory
    // fence on each of 100+ calls per frame.
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
            // A fresh V-mode stays vanilla until the first WASD release; a point-click order continues.
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            s_camRotateToggle        = false;

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
            // Exit OTS first so the camera re-attaches to the rig while the anchor is still valid.
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
            // Clear the forced rotate flag, or the camera keeps rotating in vanilla mode.
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
            // Switch control by portrait double-click or F. A single click only selects, so other
            // NPCs can be inspected without taking control (Sentient Sands compatibility).
            ULONGLONG dcMs       = s_lmbDoubleClickMs;
            bool      dcSwitch   = (dcMs > 0 && (GetTickCount64() - dcMs) <= 600);
            bool      fSwitch    = s_fSelectEdge;
            bool      wantSwitch = dcSwitch || fSwitch;
            if (wantSwitch && sel && sel != s_freeMoveAnchor && sel->isPlayerCharacter())
            {
                s_lmbDoubleClickMs    = 0;
                s_fSelectEdge         = false;
                // The FP head and hair hide is per character, and fpSetHeadBoneHidden acts on the current
                // anchor: restore the old anchor before the switch, then hide the new one.
                if (s_firstPersonActive && s_fpHeadBoneHidden)
                    fpSetHeadBoneHidden(false);
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
                // The new anchor has no WASD hold; its vanilla orders continue until the first release.
                s_wasdHoldActive         = false;
                s_playerPointClickActive = false;
                s_holdPosValid           = false;
                s_idleHoldEngaged        = false;
                DebugLog(fSwitch ? "[WASDCombat] camera_lock_retarget_selection_fkey"
                                 : "[WASDCombat] camera_lock_retarget_selection_doubleclick");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                if (s_firstPersonActive && s_fpHideHead)
                    fpSetHeadBoneHidden(true);
                if (s_firstPersonActive && s_fpHideHair)
                {
                    AppearanceBase* apNew = s_freeMoveAnchor->getAppearance();
                    if (apNew) { apNew->shaveHead(true); s_fpHairHidden = true;
                        DebugLog("[WASDCombat] dc_fp_hair_hidden_on_switch"); }
                }
            }
            else if (fSwitch)
            {
                // No valid target: consume the edge so it cannot fire on a later frame.
                s_fSelectEdge = false;
            }
        }
        s_fmTrackedMode = curMode;
    }

    // The poll thread must never touch the camera, so the P edge is consumed here. A manual
    // toggle also cancels a pending return to first person after the inventory closes.
    if (s_fpToggleRequested)
    {
        s_fpToggleRequested = false;
        if (s_firstPersonActive)
        {
            s_userWantsFP = false;
            exitFirstPerson(true);
        }
        else if (s_fpSuspendedForInv)
        {
            s_userWantsFP = false;
            s_fpSuspendedForInv = false;
        }
        else if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_lootUiSuspendActive
                 && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_userWantsFP = true;
            enterFirstPerson();
        }
    }

    // Re-enter first person after a load or stream dropped it; enterFirstPerson is idempotent.
    if (s_userWantsFP && !s_firstPersonActive && !s_fpActive && !s_fpSuspendedForInv
        && !s_otsRestorePending && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && ou && ou->player && ou->player->camera && !ou->isLoadingFromASaveGame()
        && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        DebugLog("[WASDCombat] fp_reentered_after_load");
        enterFirstPerson();
    }

    // Use the game's sneak button handler so the SNEAK checkbox, standing order, and stealth
    // state stay in sync; setStealthMode alone leaves the button off. Sneak persists after
    // FP or DC exit because it is vanilla state.
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

    // Nearest live hostile for the FP clip clearance in fpDriveFrame. The squared-distance
    // cull runs before any game call to keep the per-frame cost low.
    s_fpEnemyNearestDist = -1.0f;
    if (s_firstPersonActive && s_fpEnemyClearRadius > 0.0f
        && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && (s_nearbyEnemyCount > 0
            || s_freeMoveAnchor->isInCombatMode(true, true)))
    {
        const Ogre::Vector3 anchorPos = s_freeMoveAnchor->movement->pos;
        const float cullR  = s_fpEnemyClearRadius * 2.0f;
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

    // Suspend movement injection and the camera lock on a turret; restore once on exit.
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
    // A menu pause stops WASD momentum once; the camera lock and anchor stay. Inventory
    // auto-pause is excluded because the loot suspension owns it.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool inventoryPausing = s_lootUiSuspendActive || (gui && gui->isAnyInventoryWindowOpen());
        bool menuPausedNow    = ou->isPaused() && !inventoryPausing;
        if (menuPausedNow && !s_menuSuspendActive)
        {
            s_menuSuspendActive = true;
            DebugLog("[WASDCombat] dc_menu_suspend_begin");
            CharMovement* mvM = s_freeMoveAnchor->movement;
            // Stop only WASD momentum: a player-issued move order must survive the pause.
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
        { static bool s_skipLogged = false;
          if (ou->isPaused() && inventoryPausing && !s_menuSuspendActive)
          { if (!s_skipLogged) { s_skipLogged = true;
                DebugLog("[WASDCombat] dc_menu_suspend_skipped_inventory_pause"); } }
          else { s_skipLogged = false; } }
    }
    // Raise the camera focus toward the chest at close zoom; taper to zero at far zoom.
    if (s_mode == MODE_FREE_MOVE && g_dcCam.dcCameraCloseZoomChestOffset && !s_fpActive && !s_firstPersonActive &&
        s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player && ou->player->camera)
    {
        CameraClass*  cam    = ou->player->camera;
        Ogre::Vector3 camPos = cam->getCameraPos();
        Ogre::Vector3 ctr    = cam->getCenter();
        float dist = (camPos - ctr).length();
        const float zoomClose = 15.0f, zoomFar = 55.0f;
        float t   = (dist - zoomClose) / (zoomFar - zoomClose);
        float scl = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
        cam->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY + g_dcCam.dcCameraFocusOffsetY * scl;
    }
    s_prof_cameraLock += qpcNow() - _clStart; }   // end camera-lock timer

    // 4. HUD.
    hudUpdate();

    // 5. Pre-AI WASD application.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement && !s_lootUiSuspendActive)
    {
        if (s_healingJobPending && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobPending = false;
            s_healingJobActive  = true;
            DebugLog("[WASDCombat] dc_heal_resumed_after_wasd_release");
        }
        // An active heal yields to held WASD, like a vanilla point-click. This also frees a stuck
        // heal flag: the job stays queued through a knockdown, so removeJob never clears it.
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
            // Cancel a live crawl order before pathfinding advances it. Level-triggered, so it also
            // catches releases that the step-9 edge stop misses.
            CharMovement* mvDC = s_freeMoveAnchor->movement;
            s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDC->pos);
            mvDC->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_release");
        }
        if (bW || bA || bS || bD)
        {
            // WASD breaks playing-dead like a vanilla move order; the game decides if the character can stand.
            if (!s_playDeadExitDone
                && s_freeMoveAnchor->getProneState() == PS_PLAYING_DEAD)
            {
                s_freeMoveAnchor->setProneState(PS_NORMAL);
                s_playDeadExitDone = true;
                DebugLog("[WASDCombat] dc_play_dead_exit_on_wasd");
            }
            if (isDownedButMovable(s_freeMoveAnchor) && !downedOrderDriven(s_freeMoveAnchor))
            {
                // Direct injection drives downed movement; only cancel a leftover crawl order.
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

    // 6. Original game loop: AI, CharMovement::update, and CombatClass::go.
    s_retreatLockGoSuppressed = false;

    // Drive the FP camera before the loop: the foliage pass inside s_mainLoopOrig samples the
    // camera before CameraClass::update, so a later write is a frame late and grass blinks
    // while rotating. The second call from the camera hook is safe: the cursor recentre gives
    // it a near-zero mouse delta, and the throttled teleport does not fire twice.
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
        // Hard shutdown only when ou is null or the player was freed in a confirmed save-load.
        // A null player without the load flag is a microload, so DC stays.
        bool absoluteHard      = (!ou);
        bool confirmedLoadGame = (!absoluteHard) && (!ou->player) && ou->isLoadingFromASaveGame();
        bool hardLoss          = absoluteHard || confirmedLoadGame;

        if (hardLoss || !s_userWantsDC)
        {
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

        // Microload during chunk travel: keep DC and skip the post-AI steps.
        {
            static ULONGLONG s_skipLogTick = 0;
            ULONGLONG nowSk = GetTickCount64();
            if (nowSk - s_skipLogTick >= 2000) { s_skipLogTick = nowSk;
                DebugLog("[WASDCombat] dc_shutdown_skipped_microload"); }
        }
        return;
    }

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

    // 9. Post-AI WASD re-application and instant stop.
    if (!(s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement) || s_lootUiSuspendActive)
    {
        // A live crawl order must not keep moving the character into a menu or suspend.
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
        s_rmbPressedEdge   = false;
        return;
    }

    bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
    bool wasdActive = bW || bA || bS || bD;
    bool inCombat   = s_freeMoveAnchor->isInCombatMode(true, true);

    // The poll-thread RMB edge is the only click signal that game-side dispatch cannot block:
    // ground clicks during the hold never reach PlayerInterface::playerMove.
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

            // WASD always wins: release the hold and clear the point-click. Record a pending click
            // first, because only that click needs a disengage order.
            bool hadPendingClick     = s_playerPointClickActive;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;

            if (isUsingStationaryTurret(s_freeMoveAnchor))
                DebugLog("[WASDCombat] stationary_crossbow_cancelled_by_wasd");

            // The disengage order pathfinds. In a locked building or at its door the path fails and
            // the game barks "I can't get out of here" on each press, even with dest = current pos.
            // So issue it only for a pending click, when moveOrderMayBark is false and no crawl
            // order owns movement.
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
            && !downedOrderDriven(s_freeMoveAnchor))    // halt() would kill the outdoor crawl order each frame
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
        // Downed movement runs in step 5 only: its order persists through the AI loop.

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

        // CharMovement::periodicUpdate skips xpRunning in MOVE_DIRECTION mode, so award it here.
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

            ULONGLONG tapMs      = s_wasdTapStartMs;
            ULONGLONG tapElapsed = (tapMs > 0) ? (GetTickCount64() - tapMs) : ~0ULL;
            bool isNudgeTap      = (tapElapsed <= g_loco.wasdNudgeTapWindowMs);
            s_wasdTapStartMs     = 0;

            if (isNudgeTap && !isDownedButMovable(s_freeMoveAnchor))
            {
                // A nudge tap stops without a facing correction, so the character does not turn around.
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
                if (g_release.wasdStopOnRelease && s_freeMoveAnchor && s_freeMoveAnchor->movement)
                {
                    if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_detected");
                    CharMovement* mvR = s_freeMoveAnchor->movement;

                    // An attack windup (STARTUP_STATE) alone must not block the release stop.
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
                        // The outdoor downed crawl is order-driven and stops in the downed block below.
                        s_prevWasdDir = Ogre::Vector3::ZERO;
                        DebugLog("[WASDCombat] wasd_release_vector_zeroed");

                        if (g_release.wasdAnchorSnapOnRelease)
                        {
                            dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_anchor_snapped");
                        }

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
                }
            }

            // Gate on s_wasdDownedMovementActive, not isDownedButMovable: that can turn false
            // mid-release while the WASD destination is still pending.
            if (s_wasdDownedMovementActive)
            {
                CharMovement* mvDown = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvDown)
                {
                    // An order at the current position cancels the MOVE job, not only the destination.
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
                // Not WASD-created, so this is a real point-click destination; keep it.
                DebugLog("[WASDCombat] vanilla_pointclick_downed_destination_preserved");
            }
            s_wasdDownedMovementActive    = false;
            s_retreatLockEverActive       = false;
            s_retreatSessionCacheCount    = 0;
            s_retreatTargetsProcessed     = 0;
            s_retreatTargetsCachedSkipped = 0;
            s_retreatBlockedAttackerCount    = 0;
            s_medicalJobSuppressedThisHold   = false;
            // Fallback for when the structured release stop above did not run.
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

            // Hold the character here until a new point-click, WASD press, or DC off. The release
            // snap already cancelled any click made during the drive.
            s_wasdHoldActive         = true;
            s_playerPointClickActive = false;
        }
    }

    s_wasdWasActive = wasdActive;

    // Post-WASD grace: block combat re-entry unless the target is close and attacking.
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

    // s_retreatLockEverActive stays set for the whole hold, because Kenshi skips go() when no
    // enemy is near and the retreat state would otherwise flap.
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

    // Final hold clamp: the last DC write in the frame, after AI, taskers, and pathing.
    // Restore only X/Z, so gravity and ramps can still settle Y.
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

    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_prof_windowStart >= 1000)
        {
            float f = (float)s_profFreq / 1000.0f;
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

// Not installed: the hard-coded RVA is stale for the current RE_Kenshi exe and
// lands mid-instruction in an unrelated helper (see startPlugin).  Loot/trade
// detection runs on the mainLoop GUI poll instead.  Re-derive the RVA and
// install through verifyPatchSiteBytes before enabling.
// s_lootUiWasPrevOpen must NOT be set here: the window is not open yet, so the
// close-side detection in mainLoop would fire at once.
static void (*s_showTradeWindowOrig)(ForgottenGUI*, const hand&, const hand&, TradeWindowType);

static void showTradeWindow_hook(ForgottenGUI* thisptr, const hand& a, const hand& b, TradeWindowType type)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedTrade) {
            s_hookBlockLoggedTrade = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=showTradeWindow"); }
        s_showTradeWindowOrig(thisptr, a, b, type); return; }
    // Latched so the inventory face-cam never treats a foreign window as own
    // inventory; the hand fields can be stale, so they are not used for this.
    if (!s_dcShutdownInProgress)
        s_tradeWindowActive = true;
    // Move-through mode (InventoryFaceCam=false) keeps DC live during loot/trade.
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
            // The directional inputs aim the turret, so they must pass through.
            static ULONGLONG s_turretProtTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_turretProtTick >= 2000) { s_turretProtTick = t;
                DebugLog("[WASDCombat] stationary_crossbow_action_detected");
                DebugLog("[WASDCombat] stationary_action_protected");
                DebugLog("[WASDCombat] vmode_suppression_skipped_stationary_crossbow"); }
        }
        else
        {
            // Keyboard camera pan would detach the camera lock, and during a WASD
            // hold it would also override the AI facing.
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
        && !s_fpActive && !s_firstPersonActive   // OTS/FP own a detached camera node;
                                     // re-tracking here fights it and breaks
                                     // it after a reload.
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

// Not installed: the hard-coded RVA 0x7F95F0 is stale for the current RE_Kenshi
// exe, and patching it crashed pathfinding (see startPlugin).  Re-derive the RVA
// and install through verifyPatchSiteBytes before enabling.
// The hold is released BEFORE the dispatcher runs so the click's own door
// routing is never suppressed by the hold.
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

    // In OTS/FP, WASD is the movement scheme, so ground point-click moves are
    // swallowed.  Menu-issued orders use addOrder, a different path, and still work.
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

    // A world click behind an open inventory window must not walk the character.
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

// The player-order channel is separate from the job queue, so door suppression
// needs this hook as well as addJob_hook.  MOVE_CUS_ORDERED must pass, because
// DC's own disengage orders use it.
static void (*s_addOrderOrig)(Character* thisptr, Building* dest, TaskType t,
                              RootObject* subject, bool shift, bool clear,
                              const Ogre::Vector3& location);

static void addOrder_hook(Character* thisptr, Building* dest, TaskType t,
                          RootObject* subject, bool shift, bool clear,
                          const Ogre::Vector3& location)
{
    // subject==nullptr means a move (to a position or to a building/door).  The
    // gate must not depend on s_fpActive alone: moves with a non-null dest leaked
    // through when the face-cam was off.  It applies to any player character,
    // because the face-cam can lock onto the selected character, not the anchor.
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

    // Diagnostic: the clear flag may tell a fresh player click apart from a stale
    // automatic re-issue, which decides whether addOrder can safely clear the hold.
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
        return;
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

    // Ground-click moves during inventory reach the anchor here, not through
    // playerMove.  gui->isAnyInventoryWindowOpen is used because
    // s_lootUiSuspendActive can lag one frame.
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE
        && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)   // the own-inventory face-cam blocks all jobs
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
        // Only during the post-WASD hold: there is no player intent then, and
        // stale indoor door tasks would otherwise fire on their own.
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
            return;
        }

        bool isAttackJob  = (t == MELEE_ATTACK            || t == FOCUSED_MELEE_ATTACK      ||
                              t == CHOOSE_ENEMY_AND_ATTACK || t == ATTACK_CHARACTERS_ATTACKER ||
                              t == ATTACK_ENEMIES);
        bool isMedicalJob = (t == JOB_MEDIC        || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT  || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER     || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        if (isMedicalJob)
        {
            if (s_healingJobActive)
            {
                DebugLog("[WASDCombat] dc_heal_committed_action_preserved");
            }
            else if (!s_healingJobPending)
            {
                DebugLog("[WASDCombat] dc_auto_heal_job_detected");
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (wasdNow)
                {
                    s_healingJobPending = true;
                    DebugLog("[WASDCombat] dc_auto_heal_deferred_due_to_wasd");
                }
                else
                {
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

static bool s_processKeysHookOk = false;  // native registration needs the event reader too

// The game saves bound plugin commands to controls.cfg, but this INI copy also
// covers the case where a command is unbound at save time.
static const int DC_CMD_COUNT = 1;
static const char* const DC_CMD_NAMES[DC_CMD_COUNT] =
{
    "dc_toggle"
};

// Format v1 stored Command::bound, which is not the keycode, so rebinds never
// survived a restart.  v2 stores the real keycode from getBoundKeys().  Older
// files are ignored and re-stamped, so they cannot bind the command to key 1.
static const int DC_NATIVE_BIND_FORMAT_VERSION = 2;

// Do NOT read Command::bound here: it is an internal value, not the keycode.
static int readBoundKey(const char* name)
{
    if (!key) return INT_MIN;
    lektor<int> keys = key->getBoundKeys(name);
    if (keys.size() == 0) return INT_MIN;   // command unbound
    return keys[0];
}

// Change detection only.  getBoundKeys allocates, which is too costly to poll;
// Command::bound is not the keycode, but it differs per binding.
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
    // Version first, so a partially written file is never read as v1.
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

// Persistence must not depend on the options menu calling saveOptions.
static int       s_bindSnapshot[DC_CMD_COUNT] = { 0 };
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
        cur[i] = readChangeToken(DC_CMD_NAMES[i]);

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

    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION)
    {
        char mbuf[MAX_PATH + 96];
        sprintf_s(mbuf, sizeof(mbuf),
            "[WASDCombat] dc_native_binds_migrated old_ver=%d -> v%d (defaults kept) path=%s",
            ver, DC_NATIVE_BIND_FORMAT_VERSION, path);
        DebugLog(mbuf);
        saveNativeBindsToIni("format_migration");
        return;
    }

    int applied = 0;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int v = (int)GetPrivateProfileIntA("NativeBinds", DC_CMD_NAMES[i], -1, path);
        if (v > 0)
        {
            // bind() adds a key and does not replace, so without the unbind the
            // default and the saved key would both fire.
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

// Returns -1 when the user never rebound the command.  The version gate must
// match applyNativeBindsFromIni.
static int iniSavedNativeBind(const char* name)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION) return -1;
    int v = (int)GetPrivateProfileIntA("NativeBinds", name, -1, path);
    return (v > 0) ? v : -1;
}

// The game runs InputHandler::loadConfig before RE_Kenshi loads plugins, so this
// is called from the first mainLoop pass; the loadConfig hook only covers a
// later config reload.  Movement keys must never be registered: one command per
// key, and the vanilla camera owns W/A/S/D.
static void registerNativeCommands(InputHandler* self)
{
    if (s_nativeCommandsRegistered || !self) return;
    if (!s_processKeysHookOk)
    {
        // Without the event reader, toggle presses would be lost.
        DebugLog("[WASDCombat] dc_native_keybinds_skipped_no_event_reader");
        return;
    }
    // With a saved rebind, register with no key: Kenshi allows one command per
    // key, so claiming plain V would steal it from any vanilla command bound
    // there, every session.
    const int savedToggle = iniSavedNativeBind("dc_toggle");
    self->addCommand("dc_toggle",        0,
                     (savedToggle > 0) ? OIS::KC_UNASSIGNED : OIS::KC_V,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    char rnbuf[160];
    sprintf_s(rnbuf, sizeof(rnbuf),
        "[WASDCombat] dc_native_register defaults toggle=%s",
        (savedToggle > 0) ? "deferred(rebound)" : "V");
    DebugLog(rnbuf);
    applyNativeBindsFromIni(self);
    s_nativeCommandsRegistered = true;   // poll-thread toggle stands down
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

// controls.cfg excludes plugin commands, so persist ours here.
static void (*s_optionsSaveOrig)(OptionsWindow*);
static void optionsSave_hook(OptionsWindow* self)
{
    s_optionsSaveOrig(self);
    if (s_nativeCommandsRegistered)
        saveNativeBindsToIni("save_options");
}

static void (*s_optionsCreateOrig)(OptionsWindow*);
static void optionsCreate_hook(OptionsWindow* self)
{
    s_optionsCreateOrig(self);

    DatapanelGUI* controlsTab = nullptr;
    size_t tabCount = self->tabs->getItemCount();
    for (size_t i = 0; i < tabCount; i++)
    {
        DatapanelGUI** panel = self->tabs->getItemDataAt<DatapanelGUI*>(i, false);
        if (panel && *panel != nullptr && (*panel)->currentCategory == 0x19)   // Controls tab
        {
            controlsTab = *panel;
            break;
        }
    }
    if (controlsTab)
    {
        // No leading addSpace: it renders as a large empty gap above the row.
        controlsTab->addCustomLine(new DataPanelLine_KeyConfig(
            "dc_toggle",        "Direct Control: Toggle",        0x19));
        DebugLog("[WASDCombat] dc_controls_menu_section_added");
    }
    else
    {
        DebugLog("[WASDCombat] dc_controls_tab_not_found — bindings still work, menu rows missing");
    }
}

// Events stay in key->events for exactly one processKeys cycle.  No pause gate:
// the toggle must work while the game is paused.
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
        if (n == "dc_toggle") handleTogglePress();
    }
}

// Required gate for any hook installed by raw RVA.  GetRealAddress hooks are
// symbol-based and survive exe changes; raw RVAs do not.  RE_Kenshi regenerates
// its patched exe on its own updates and shifts all code, so a stale RVA patches
// the middle of an unrelated instruction while MinHook still reports SUCCESS.
// Record `expected` from the same exe the RVA came from.  On a mismatch the hook
// is skipped, so the feature degrades without corruption.
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

    // Loaded unconditionally so the poll thread works if native command
    // registration never happens (load order or hook failure).
    loadKeybinds();

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

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addOrder),
            &addOrder_hook, &s_addOrderOrig))
        ErrorLog("WASDCombatPlugin: addOrder hook FAILED — door suppression partial (addJob only)");
    else
        DebugLog("WASDCombatPlugin: addOrder hook OK");

    // playerMove is not installed.  On the current exe, RVA 0x7F95F0 is one byte
    // into a 5-byte call inside a NavMesh path function, so the MinHook jump byte
    // became that call's displacement and pathfinding that reached it jumped into
    // unmapped memory (crash: pack bull + right-click inside a hive home).  The
    // RMB press-edge poll and the addOrder/addJob gates cover its job.

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_NV_go),
            &combatGo_hook, &s_combatGoOrig))
        ErrorLog("WASDCombatPlugin: combatGo hook FAILED — passive-combat model inactive");
    else
        DebugLog("WASDCombatPlugin: combatGo hook OK");

    // initCombatMode and youKnowImAttacking are deliberately not hooked: DC must
    // not block combat entry or attack notifications, only the per-frame go().

    // showTradeWindow is not installed.  On the current exe, RVA 0x7905D0 is
    // inside a 10-byte movabs of a double-to-int64 conversion helper, so the patch
    // corrupted that helper's common path while the hook itself never fired.  The
    // mainLoop GUI poll (isAnyInventoryWindowOpen) covers loot/trade detection.

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
