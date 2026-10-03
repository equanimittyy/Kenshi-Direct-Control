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

// Too broad for the movement injection sites; used only at instant stop and the post-release restore.
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
    // Gated on combatModeActive: a stale post-knockdown state would block the release-stop.
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

// Kenshi cannot abort a clip, so movement waits; looping states are excluded so the player can retreat.
static bool isCommittedCombatClip(Character* ch)
{
    if (!ch) return false;
    CombatClass* cc = ch->getCombatClass();
    if (!cc || !cc->combatModeActive) return false;
    swordStateEnum st = cc->getCombatState();
    return st == STARTUP_STATE || st == CHOP_WEAPON
        || st == STUMBLE       || st == REACTION_BLOCK;
}

// Seated characters ignore setDirectMovement; job seats report PRETEND_TO_OPERATE_MACHINERY.
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
    // isUnconcious() is also true for playing-dead and crippled characters, who can still crawl.
    if (prone == PS_KO) return false;
    if (ch->isCurrentlyGettingUp) return false;
    if (prone == PS_PLAYING_DEAD || prone == PS_CRIPPLED) return true;
    if (ch->isDown() && !ch->isUnconcious()) return true;
    return false;
}

// The raw indoor flag flickers on stairs and roofs, flipping the crawl between order and direct mode.
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

// Order-driven crawl misroutes indoors and off roofs, so that machinery stays dormant.
static bool downedOrderDriven(Character* ch)
{
    (void)ch;
    (void)&stableIndoors;   // keep dormant order machinery referenced
    return false;
}

// setDirectMovement is valid only for standing locomotion, so the crawl uses the pathfind order.
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
    dir /= dlen;

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

    // One persistent order: re-issuing every frame restarts pathfinding before any motion.
    if (s_wasdDownedMovementActive
        && dir.dotProduct(s_downedLastDir) > 0.95f
        && nowDI - s_downedLastIssueMs < 1500
        && (s_downedLastDest - posNow).length() > approachAt)
        return;
    s_downedLastDir     = dir;
    s_downedLastIssueMs = nowDI;

    // Short hops: a far destination is off the local navmesh and the path can start the wrong way.
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
