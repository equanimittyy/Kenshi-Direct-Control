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
