static void (*s_mainLoopOrig)(GameWorld* thisptr, float time);

static void mlWaitForSafeReacquire(GameWorld* thisptr, float time)
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
}

static bool mlSafetyGate(GameWorld* thisptr, float time)
{
    bool ouNull  = (ou == nullptr);
    bool loadSig = ouNull || !ou->player || ou->isLoadingFromASaveGame();

    if (loadSig)
    {
        if (ouNull)
        {
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
            return true;
        }

        if (s_userWantsDC)
        {
            // A save-load frees the anchor while ou->player can still be valid: never dereference it here.
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
                return true;
            }

            // A chunk microload does not free characters, so the anchor dereference below is safe.
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
                return true;
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
            return true;
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

    // Fallback so the load guard cannot stick.
    if (s_loadGuardActive)
    {
        s_loadGuardActive        = false;
        s_postLoadReacquire      = true;
        s_stabilizationCountdown = 0;
        DebugLog("[WASDCombat] reload_complete_reacquire_started");
    }
    return false;
}

static void mlHandleNewGameSignal()
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

        // Exit while the camera is valid, or character creation shows no character.
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
        // Set both modes so the step-3 exit transition does not run again.
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

static void mlRunInventoryMoveThrough()
{
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
    // Undo only auto-pauses inside the grace window; a later pause is the player's.
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

    // Vanilla never closes a merchant trade on distance; own-squad trades must not auto-close.
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

static void mlTrackInventoryWindows()
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

    // The trade hook can set the suspension before the window is visible.
    if (moveThrough)
    {
        mlRunInventoryMoveThrough();
    }
    else if (s_invMoveThroughActive)
    {
        // Leave the game running: the player owns pause now.
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
        // Do not re-derive s_tradeWindowActive from the gui trade fields: they stay stale after close.
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

static void mlTrackSelection()
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

static void mlApplyModeTransition()
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
        // A single click only selects, so NPCs can be inspected (Sentient Sands compatibility).
        ULONGLONG dcMs       = s_lmbDoubleClickMs;
        bool      dcSwitch   = (dcMs > 0 && (GetTickCount64() - dcMs) <= 600);
        bool      fSwitch    = s_fSelectEdge;
        bool      wantSwitch = dcSwitch || fSwitch;
        if (wantSwitch && sel && sel != s_freeMoveAnchor && sel->isPlayerCharacter())
        {
            s_lmbDoubleClickMs    = 0;
            s_fSelectEdge         = false;
            // Head hiding acts on the current anchor: restore the old one before the switch.
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

static void mlConsumeFirstPersonToggle()
{
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

    // enterFirstPerson is idempotent; this re-enters after a load or stream drops first person.
    if (s_userWantsFP && !s_firstPersonActive && !s_fpActive && !s_fpSuspendedForInv
        && !s_otsRestorePending && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && ou && ou->player && ou->player->camera && !ou->isLoadingFromASaveGame()
        && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        DebugLog("[WASDCombat] fp_reentered_after_load");
        enterFirstPerson();
    }
}

static void mlConsumeSneakToggle()
{
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
}

static void mlScanNearestEnemyForFp(GameWorld* thisptr)
{
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
}

static void mlUpdateTurretSuspend()
{
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
}

static void mlUpdateMenuSuspend()
{
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
}

static void mlApplyCloseZoomOffset()
{
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
}

static void mlApplyPreAiWasd()
{
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement && !s_lootUiSuspendActive)
    {
        if (s_healingJobPending && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobPending = false;
            s_healingJobActive  = true;
            DebugLog("[WASDCombat] dc_heal_resumed_after_wasd_release");
        }
        // This also frees a stuck heal flag: the job stays queued through a knockdown.
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
            // Level-triggered, so it also catches releases that the step-9 edge stop misses.
            CharMovement* mvDC = s_freeMoveAnchor->movement;
            s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDC->pos);
            mvDC->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_release");
        }
        if (bW || bA || bS || bD)
        {
            if (!s_playDeadExitDone
                && s_freeMoveAnchor->getProneState() == PS_PLAYING_DEAD)
            {
                s_freeMoveAnchor->setProneState(PS_NORMAL);
                s_playDeadExitDone = true;
                DebugLog("[WASDCombat] dc_play_dead_exit_on_wasd");
            }
            if (isDownedButMovable(s_freeMoveAnchor) && !downedOrderDriven(s_freeMoveAnchor))
            {
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
}

static void mlDriveFpCameraBeforeLoop()
{
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
}

static bool mlPostLoopLoadGuard()
{
    if (!ou || !ou->player || ou->isLoadingFromASaveGame())
    {
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
            return true;
        }

        // Chunk-travel microload: keep DC and skip the post-AI steps.
        {
            static ULONGLONG s_skipLogTick = 0;
            ULONGLONG nowSk = GetTickCount64();
            if (nowSk - s_skipLogTick >= 2000) { s_skipLogTick = nowSk;
                DebugLog("[WASDCombat] dc_shutdown_skipped_microload"); }
        }
        return true;
    }
    return false;
}

static void mlScanSquadThreat(GameWorld* thisptr)
{
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
}

static void mlCancelWasdWhileInactive()
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
}

static void mlConsumeClickEdge()
{
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
}

static void mlTrackCombatEdges(bool inCombat, bool wasdActive)
{
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
}

static void mlApplyPostAiWasd(bool bW, bool bA, bool bS, bool bD, bool inCombat)
{
    if (!s_wasdWasActive)
    {
        s_combatWASDLogged     = false;
        s_retreatLogged        = false;
        s_postWasdGraceActive  = false;
        s_combatReentryAllowed = false;

        // Record a pending click first: only that click needs a disengage order.
        bool hadPendingClick     = s_playerPointClickActive;
        s_wasdHoldActive         = false;
        s_playerPointClickActive = false;
        s_holdPosValid           = false;
        s_idleHoldEngaged        = false;

        if (isUsingStationaryTurret(s_freeMoveAnchor))
            DebugLog("[WASDCombat] stationary_crossbow_cancelled_by_wasd");

        // The disengage order pathfinds and barks "I can't get out of here" when the path fails.
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

static void mlHandleWasdRelease()
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

        // Not isDownedButMovable: it can turn false while the WASD destination is still pending.
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

        s_wasdHoldActive         = true;
        s_playerPointClickActive = false;
    }
}

static void mlUpdatePostWasdGrace(bool wasdActive)
{
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
}

static void mlTrackRetreatState(bool wasdActive)
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

static void mlTrackCombatState(bool wasdActive)
{
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
}

static void mlTrackProtectedAnimation()
{
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
}

static void mlApplyHoldClamp(bool wasdActive)
{
    // Final hold clamp after AI and pathing; X/Z only, so gravity can still settle Y.
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
}

static void mlReportPerf()
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

static void mainLoop_hook(GameWorld* thisptr, float time)
{
    // The stabilization countdown runs here so it advances while the shutdown flag is held.
    if (s_dcShutdownInProgress)
    {
        mlWaitForSafeReacquire(thisptr, time);
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
    if (mlSafetyGate(thisptr, time))
        return;

    // newGame() sets NEWGAME (0x4) before world teardown; the LOADGAME (0x2) path sets the flags too late.
    mlHandleNewGameSignal();
    mlTrackInventoryWindows();

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

    if (s_fpActive && !isOwnInventoryOpen())
        exitOTS(true);

    // Registered here: the game loads its keyboard config before RE_Kenshi loads plugins.
    if (!s_nativeCommandsRegistered)
        registerNativeCommands(key);

    if (s_nativeCommandsRegistered)
        watchNativeBindChanges();

    // Hooks read these snapshots, not the volatiles, to avoid a fence on 100+ calls per frame.
    s_frameMode        = s_mode;
    s_frameWasdHeld    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
    s_frameLootSuspend = s_lootUiSuspendActive;

    // 2. Selection tracking.
    mlTrackSelection();

    // 3. V-Mode transition.
    { LONGLONG _clStart = qpcNow();
    mlApplyModeTransition();

    // The poll thread must never touch the camera, so the P edge is consumed here.
    mlConsumeFirstPersonToggle();

    // setStealthMode alone leaves the SNEAK button off, so drive the button's own handler.
    mlConsumeSneakToggle();

    // Feeds the FP clip clearance in fpDriveFrame.
    mlScanNearestEnemyForFp(thisptr);
    mlUpdateTurretSuspend();
    // Inventory auto-pause is excluded: the loot suspension owns it.
    mlUpdateMenuSuspend();
    mlApplyCloseZoomOffset();
    s_prof_cameraLock += qpcNow() - _clStart; }   // end camera-lock timer

    // 4. HUD.
    hudUpdate();

    // 5. Pre-AI WASD application.
    mlApplyPreAiWasd();

    // 6. Original game loop: AI, CharMovement::update, and CombatClass::go.
    s_retreatLockGoSuppressed = false;

    // Also drive the FP camera here: the foliage pass samples the camera before CameraClass::update.
    mlDriveFpCameraBeforeLoop();

    s_mainLoopOrig(thisptr, time);

    if (mlPostLoopLoadGuard())
        return;

    if (s_dcShutdownInProgress)
        return;

    // 8. Periodic squad-threat scan.
    mlScanSquadThreat(thisptr);

    // 9. Post-AI WASD re-application and instant stop.
    if (!(s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement) || s_lootUiSuspendActive)
    {
        mlCancelWasdWhileInactive();
        return;
    }

    bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
    bool wasdActive = bW || bA || bS || bD;
    bool inCombat   = s_freeMoveAnchor->isInCombatMode(true, true);

    // Ground clicks during the hold never reach playerMove, so use the poll-thread RMB edge.
    mlConsumeClickEdge();
    mlTrackCombatEdges(inCombat, wasdActive);

    if (wasdActive)
        mlApplyPostAiWasd(bW, bA, bS, bD, inCombat);
    else
        mlHandleWasdRelease();

    s_wasdWasActive = wasdActive;

    mlUpdatePostWasdGrace(wasdActive);

    // Kenshi skips go() when no enemy is near, so the retreat state would otherwise flap.
    mlTrackRetreatState(wasdActive);

    { LONGLONG _ctStart = qpcNow();
    mlTrackCombatState(wasdActive);
    mlTrackProtectedAnimation();
    s_prof_combatTarget += qpcNow() - _ctStart; }  // end combat-target timer

    mlApplyHoldClamp(wasdActive);
    mlReportPerf();
}
