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
