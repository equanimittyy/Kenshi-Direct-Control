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
