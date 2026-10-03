// Not installed (stale RVA). Never set s_lootUiWasPrevOpen here: the window is not open yet.
static void (*s_showTradeWindowOrig)(ForgottenGUI*, const hand&, const hand&, TradeWindowType);

static void showTradeWindow_hook(ForgottenGUI* thisptr, const hand& a, const hand& b, TradeWindowType type)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedTrade) {
            s_hookBlockLoggedTrade = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=showTradeWindow"); }
        s_showTradeWindowOrig(thisptr, a, b, type); return; }
    if (!s_dcShutdownInProgress)
        s_tradeWindowActive = true;
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
            // Keyboard pan would detach the camera lock and override AI facing during a WASD hold.
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
        && !s_fpActive && !s_firstPersonActive   // re-tracking fights the detached node
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

// Not installed (stale RVA). The hold is released before dispatch so door routing still works.
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

    // Menu-issued orders use addOrder, so only ground clicks are swallowed here.
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

// Player orders bypass the job queue; MOVE_CUS_ORDERED must pass for DC's disengage orders.
static void (*s_addOrderOrig)(Character* thisptr, Building* dest, TaskType t,
                              RootObject* subject, bool shift, bool clear,
                              const Ogre::Vector3& location);

static void addOrder_hook(Character* thisptr, Building* dest, TaskType t,
                          RootObject* subject, bool shift, bool clear,
                          const Ogre::Vector3& location)
{
    // Not gated on s_fpActive alone: moves with a non-null dest leaked with the face-cam off.
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

    // isAnyInventoryWindowOpen, because s_lootUiSuspendActive can lag one frame.
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
        // Hold only: stale indoor door tasks would otherwise fire on their own.
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
