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

// Uses isProtectedAnimationState: the door Tasker owns STARTUP_STATE and would never release the hold.
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

// Indoors any path may cross a locked door and trip the "I can't get out of here" bark.
static bool moveOrderMayBark(Character* ch)
{
    if (!ch) return false;
    CharMovement* mv = ch->movement;
    // isInsideBuildingLoadedInterior() is false until the interior loads, so isIndoors() is also needed.
    return mv && (mv->isInsideBuildingLoadedInterior() || mv->isIndoors());
}

// Skipped where the order could bark; halt() and the hold-clamp park the character anyway.
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
    // Cached every frame: a load frees the character before the load path can read its speed.
    if (thisptr->speedOrders < GROUPED)
        s_dcPreservedSpeedMode = thisptr->speedOrders;
    bool wasdHeld    = s_frameWasdHeld;
    bool inVMode     = (s_frameMode == MODE_FREE_MOVE);
    bool lootSuspend = s_frameLootSuspend;

    // The bridge does not refresh s_wasdLastHeldMs, so it expires on its own.
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
                    // Indoors this order path-walks even to the current position, a one-frame step before the clamp.
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

            // Also clamped after orig: indoor routing writes the position late in the frame.
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
    // halt() + setDirectMovement would cancel an order-driven crawl.
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

            // Lingering combat mode fights the crawl order; CombatClass::go still sees the flag in the AI phase.
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
    // Only a real key press refreshes this; otherwise one tap drives forever in combat.
    if (!combatBridge)
        s_wasdLastHeldMs = GetTickCount64();
    s_holdPosValid   = false;

    Ogre::Vector3 wasdDir;
    bool dirOk = computeWASDDirection(s_wHeld, s_aHeld, s_sHeld, s_dHeld, wasdDir);
    if (!dirOk && combatBridge)
    {
        wasdDir = s_prevWasdDir;
        dirOk   = true;
    }

    // A move order replaces the queued use job, so the AI does not pull them back onto the furniture.
    if (dirOk)
    {
        Character* chSeat = thisptr->getCharacter();
        if (chSeat && (isAnchoredToFurniture(chSeat) || chSeat->isCurrentlyGettingUp))
        {
            chSeat->playerWantsMeToGetUp = true;
            // Throttled re-issue: a once-per-sit latch stuck across squad switches and re-sits.
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

    // Kenshi cannot abort a clip, so movement is buffered until a committed combat clip finishes.
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

        // This velocity write, not setDirectMovement, binds the speed cap; a fractional boost lurched at low FPS.
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

        // combatModeActive stays cleared while driving: restoring it left the body in the arms-down pose.
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

        // The combat AI re-enables animationOverride, so MOVE_DIRECTION is re-asserted after orig.
        {
            Character*   chPost = thisptr->getCharacter();
            CombatClass* ccPost = chPost ? chPost->getCombatClass() : nullptr;
            if (ccPost)
            {
                // Do not write COMBAT_FINISHED here: doing it every frame fought the combat system and stuttered.
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
        // Race: the poll thread released the keys after the WASD snapshot.
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

// Ownership is handed off at the press/release edge; a per-frame tug-of-war caused combat stutter.
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
        // Suppressing go() mid-clip freezes the state machine, so the movement buffer would stick forever.
        if (movementOwns && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;
            return;
        }
    }
    s_retreatLockGoSuppressed = false;
    s_combatGoOrig(thisptr, frameTime);
}
