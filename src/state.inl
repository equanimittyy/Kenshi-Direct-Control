enum ControlMode { MODE_VANILLA = 0, MODE_FREE_MOVE = 1 };

static volatile ControlMode s_mode   = MODE_VANILLA;
static volatile bool        s_wHeld  = false;
static volatile bool        s_aHeld  = false;
static volatile bool        s_sHeld  = false;
static volatile bool        s_dHeld  = false;
// Releases only the post-WASD hold; the click itself is never touched.
static volatile bool        s_rmbPressedEdge = false;
static bool                 s_rmbPrev        = false;  // poll thread only
// A single left click only selects; Sentient Sands compatibility depends on it.
static bool                 s_lmbPrev          = false;     // poll thread only
static ULONGLONG            s_lastLmbDownMs    = 0;         // poll thread only
static volatile ULONGLONG   s_lmbDoubleClickMs = 0;        // set by poll, consumed by main
// CTRL toggles camera rotation in DC; cameraUpdate_hook forces InputHandler::rotate (0xED).
static volatile bool        s_camRotateToggle  = false;
static bool                 s_ctrlPrevPoll     = false;     // poll thread only
static volatile bool        s_camRotateUiOpen  = false;
static volatile ULONGLONG   s_wasdTapStartMs = 0;
static volatile bool        s_fSelectEdge    = false;
// Set and cleared only by the toggle key or a hard shutdown.
static volatile bool        s_userWantsDC         = false;
// Every hook calls only the original while set; cleared once stabilization completes.
static volatile bool        s_dcShutdownInProgress = false;
static bool s_hookBlockLoggedMain    = false;
static bool s_hookBlockLoggedCharMov = false;
static bool s_hookBlockLoggedPCtrl   = false;
static bool s_hookBlockLoggedRemJob  = false;
static bool s_hookBlockLoggedAddJob  = false;
static bool s_hookBlockLoggedTrade   = false;
static ULONGLONG s_shutdownWaitLogTick = 0;

// Suspends input without changing s_mode, so DC restores automatically.
static volatile bool   s_lootUiSuspendActive    = false;
static bool            s_lootUiWasPrevOpen      = false;
static ULONGLONG       s_lootSuspendStartTick   = 0;
// Latched: the gui trade fields stay stale after a trade closes.
static bool            s_tradeWindowActive      = false;
static const ULONGLONG LOOT_SUSPEND_DEBOUNCE_MS = 250;

// ForcedRun: the plugin unpaused the game, so it restores the pause only in that case.
static bool            s_invMoveThroughActive      = false;
static bool            s_invMoveThroughForcedRun   = false;
// Compared by identity only: the character behind a closing window can be torn down.
static bool            s_invMoveThroughPlayerPaused = false;
static Character*      s_invMoveThroughShownChar    = nullptr;
static ULONGLONG       s_invMoveThroughEdgeTick     = 0;
// Covers one slow UI-transition frame (~500 ms).
static const ULONGLONG INV_MT_AUTOPAUSE_GRACE_MS   = 600;
// Vanilla merchant trade never closes on distance, so the plugin closes it.
static bool            s_invTradeCloseRequested    = false;
// Distance walked, not to the merchant: trading across a counter starts far away.
static bool            s_invTradeStartValid        = false;
static Ogre::Vector3   s_invTradeAnchorStart;
static const float     INV_TRADE_AUTOCLOSE_DIST    = 6.0f;
static const float     INV_TRADE_AUTOCLOSE_DIST_SQ = INV_TRADE_AUTOCLOSE_DIST * INV_TRADE_AUTOCLOSE_DIST;

// Snapshot once per mainLoop: per-character hooks fire 100+ times per frame.
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

enum KeyRole { KR_FORWARD = 0, KR_LEFT, KR_BACK, KR_RIGHT, KR_TOGGLE, KR_SELECT, KR_FP, KR_SNEAK, KR_COUNT };
static int s_bindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'F', 'P', 'C' };
static const char* const KR_INI_KEY[KR_COUNT] =
    { "MoveForward", "MoveLeft", "MoveBackward", "MoveRight", "ToggleDC", "SelectControl", "FirstPerson", "SneakToggle" };
static const char* const KR_DEFAULT[KR_COUNT] =
    { "W", "A", "S", "D", "V", "F", "P", "C" };
static char    s_bindCfgStr[KR_COUNT][32];
static HMODULE s_thisModule = nullptr;

static bool    s_settingInventoryFaceCam = true;
// The injected movement is otherwise uncapped (~99), so impaired characters outran all.
static bool    s_settingWasdSpeedCap     = true;
static float   s_settingWasdSpeedMult    = 1.0f;
// isInCombatMode blips false between swings, so combat exit waits a grace period.
static ULONGLONG s_lastInCombatMs = 0;
static const ULONGLONG FACECAM_COMBAT_GRACE_MS = 1500;
