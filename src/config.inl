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
