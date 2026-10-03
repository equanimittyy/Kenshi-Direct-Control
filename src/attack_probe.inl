// Temporary in-game probe: proves that attackTarget with _defensiveMode cleared gives exactly one swing.
// Remove this file and its call once the probe has answered.
static const int       ATTACK_PROBE_VK               = VK_OEM_2;
static const ULONGLONG ATTACK_PROBE_START_TIMEOUT_MS = 3000;
// After the restore, any further swing is one the character started by itself.
static const ULONGLONG ATTACK_PROBE_WATCH_MS         = 3000;

static bool       s_probeKeyPrev          = false;
static Character* s_probeChar             = nullptr;
static ULONGLONG  s_probeStartMs          = 0;
static ULONGLONG  s_probeRestoreMs        = 0;
static bool       s_probeDefensiveBefore  = false;
static float      s_probeMeleeBefore      = 0.0f;
static bool       s_probeInSwing          = false;
static int        s_probeSwings           = 0;
static int        s_probeSwingsAfterRestore = 0;
static int        s_probeLastState        = -1;

static Character* probeNearestEnemy(GameWorld* world, Character* me, float* outDist)
{
    const Ogre::Vector3 myPos = me->movement->pos;
    Character* best  = nullptr;
    float      best2 = -1.0f;
    auto& chars = world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        if (c->isDead() || c->isUnconcious()) continue;
        if (!c->isEnemy(me, true)) continue;
        float dx = c->movement->pos.x - myPos.x;
        float dz = c->movement->pos.z - myPos.z;
        float d2 = dx * dx + dz * dz;
        if (best2 >= 0.0f && d2 >= best2) continue;
        best  = c;
        best2 = d2;
    }
    *outDist = best ? sqrtf(best2) : -1.0f;
    return best;
}

static void probeFinish(const char* reason)
{
    CharStats* stats = s_probeChar->getStats();
    char buf[256];
    sprintf_s(buf, sizeof(buf),
        "[WASDCombat] attack_probe_end reason=%s swings=%d swingsAfterRestore=%d"
        " meleeBefore=%.4f meleeAfter=%.4f defensiveNow=%d",
        reason, s_probeSwings, s_probeSwingsAfterRestore, s_probeMeleeBefore,
        stats ? stats->getStat(STAT_MELEE_ATTACK, true) : -1.0f,
        stats ? (int)stats->_defensiveMode : -1);
    DebugLog(buf);
    s_probeChar = nullptr;
}

static void probeRestoreDefensive(ULONGLONG now)
{
    CharStats* stats = s_probeChar->getStats();
    if (stats)
        stats->_defensiveMode = s_probeDefensiveBefore;
    s_probeRestoreMs = now;
    char buf[128];
    sprintf_s(buf, sizeof(buf), "[WASDCombat] attack_probe_restored defensive=%d elapsedMs=%llu",
        (int)s_probeDefensiveBefore, now - s_probeStartMs);
    DebugLog(buf);
}

static void probeTick()
{
    // A load or control switch can free the character, so it is not touched again.
    if (s_probeChar != s_freeMoveAnchor || s_mode != MODE_FREE_MOVE)
    {
        DebugLog("[WASDCombat] attack_probe_aborted anchor_changed restore_skipped=1");
        s_probeChar = nullptr;
        return;
    }

    ULONGLONG    now   = GetTickCount64();
    CombatClass* cc    = s_probeChar->getCombatClass();
    int          st    = cc ? (int)cc->getCombatState() : -1;
    bool         modeOn = cc && cc->combatModeActive;
    if (st != s_probeLastState)
    {
        char buf[160];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] attack_probe_state %d->%d combatMode=%d elapsedMs=%llu restored=%d",
            s_probeLastState, st, (int)modeOn, now - s_probeStartMs, s_probeRestoreMs ? 1 : 0);
        DebugLog(buf);
        s_probeLastState = st;
    }

    bool attacking = modeOn && (st == STARTUP_STATE || st == CHOP_WEAPON);
    if (attacking && !s_probeInSwing)
    {
        s_probeInSwing = true;
        if (s_probeRestoreMs) ++s_probeSwingsAfterRestore;
        else                  ++s_probeSwings;
    }
    else if (!attacking && s_probeInSwing)
    {
        s_probeInSwing = false;
        if (!s_probeRestoreMs)
            probeRestoreDefensive(now);
    }

    if (!s_probeRestoreMs && s_probeSwings == 0
        && now - s_probeStartMs > ATTACK_PROBE_START_TIMEOUT_MS)
    {
        probeRestoreDefensive(now);
        probeFinish("no_swing");
    }
    else if (s_probeRestoreMs && now - s_probeRestoreMs > ATTACK_PROBE_WATCH_MS)
    {
        probeFinish("done");
    }
}

static void probeStart(GameWorld* world)
{
    Character*   me    = s_freeMoveAnchor;
    CharStats*   stats = me->getStats();
    CombatClass* cc    = me->getCombatClass();
    if (!stats || !cc)
        return;

    float      dist   = -1.0f;
    Character* target = probeNearestEnemy(world, me, &dist);
    if (!target)
    {
        DebugLog("[WASDCombat] attack_probe_rejected no_enemy");
        return;
    }

    s_probeChar               = me;
    s_probeStartMs            = GetTickCount64();
    s_probeRestoreMs          = 0;
    s_probeDefensiveBefore    = stats->_defensiveMode;
    s_probeMeleeBefore        = stats->getStat(STAT_MELEE_ATTACK, true);
    s_probeInSwing            = false;
    s_probeSwings             = 0;
    s_probeSwingsAfterRestore = 0;
    s_probeLastState          = (int)cc->getCombatState();

    stats->_defensiveMode = false;
    me->attackTarget(target);

    char buf[256];
    sprintf_s(buf, sizeof(buf),
        "[WASDCombat] attack_probe_start target=%p dist=%.2f weaponLength=%.2f defensiveBefore=%d"
        " state=%d combatMode=%d targetSet=%d",
        target, dist, stats->getCurrentWeaponLength(), (int)s_probeDefensiveBefore,
        s_probeLastState, (int)cc->combatModeActive,
        me->getAttackTarget().getCharacter() == target ? 1 : 0);
    DebugLog(buf);
}

static void mlRunAttackProbe(GameWorld* world)
{
    bool down = isKenshiForeground() && (GetAsyncKeyState(ATTACK_PROBE_VK) & 0x8000) != 0;
    bool edge = down && !s_probeKeyPrev;
    s_probeKeyPrev = down;

    if (s_probeChar)
    {
        probeTick();
        return;
    }
    if (!edge)
        return;
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor || !s_freeMoveAnchor->movement
        || s_lootUiSuspendActive)
    {
        DebugLog("[WASDCombat] attack_probe_rejected dc_inactive");
        return;
    }
    probeStart(world);
}
