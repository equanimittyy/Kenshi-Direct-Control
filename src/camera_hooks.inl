// Runs AFTER the game's camera update and BEFORE render: only writes here survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;

    // Force rotate ON only, never OFF, so vanilla MMB hold-to-rotate still works.
    if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_firstPersonActive && s_camRotateToggle && key)
        key->rotate = true;

    // controlEnabled=false stops zoom and pan moving the detached camera before name-tags lay out.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // The pre-orig drive stops grass blinking; the second drive reads a zero mouse delta.
    if (s_fpCamPreOrig && s_firstPersonActive && ou && ou->player
        && thisptr == ou->player->camera && !ou->isLoadingFromASaveGame())
    {
        ManagementScreen* mgmtPre = ManagementScreen::getSingleton();
        bool uiPre = !controlEnabled || s_lootUiSuspendActive
                   || (mgmtPre && mgmtPre->getVisible())
                   || (gui && (gui->isStatsWindowOpen() || gui->inDialogue()
                               || gui->isPaused() || gui->isAnyInventoryWindowOpen()));
        fpDriveFrame(thisptr, uiPre);
    }

    s_cameraUpdateOrig(thisptr, ctlEnabled);

    // Only mark pending: the scene may be freed, and the load flags stay set through character creation.
    bool sceneFreeing = !ou || ou->isLoadingFromASaveGame();
    if (sceneFreeing)
    {
        if (s_fpActive || s_firstPersonActive)
        {
            s_fpActive          = false;
            s_firstPersonActive = false;
            s_fpCursorCaptured  = false;
            s_fpHeadBoneHidden  = false;  // the skeleton is gone, so the restore flags are moot
            s_fpHairHidden      = false;
            s_otsRestorePending = true;
            if (s_fpOptRangeSaved && options)   // never leave the range boosted
            {
                options->grassRange   = s_fpSavedGrassRange;
                options->foliageRange = s_fpSavedFoliageRange;
                s_fpOptRangeSaved     = false;
            }
        }
        return;
    }

    // thisptr is a valid camera even when ou->player is null, so restore through thisptr.
    if (s_otsRestorePending)
    {
        s_otsRestorePending = false;
        s_fpSuspendedForInv = false;   // a load cancels any pending FP auto-return
        otsRestoreCameraToRig(thisptr);
        if (ou && ou->player && s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        otsRestoreNames();
        DebugLog("[WASDCombat] dc_cam_restored_after_load");
        return;
    }

    // Inventory portrait cameras also call this hook and must not drive the detached camera.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // The world map and stats window do not clear controlEnabled, so they are checked explicitly.
    ManagementScreen* mgmt = ManagementScreen::getSingleton();
    bool mgmtOpen = (mgmt && mgmt->getVisible());
    bool statsOpen = (gui && gui->isStatsWindowOpen());
    bool uiOpenNow = !controlEnabled || s_lootUiSuspendActive || mgmtOpen || statsOpen
        || (gui && (gui->inDialogue() || gui->isPaused()
                    || gui->isAnyInventoryWindowOpen()));
    if (uiOpenNow && s_camRotateToggle)
    {
        s_camRotateToggle   = false;
        if (key) key->rotate = false;
        thisptr->isRotating  = false;
    }
    s_camRotateUiOpen = uiOpenNow;

    {
        // Mode, anchor and window count blip during a squad switch, so only a true teardown exits at once.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Combat is a hard exit, so one flicker-false frame cannot latch the face-cam.
        ULONGLONG nowFC      = GetTickCount64();
        bool inCombatNow     = s_freeMoveAnchor
                            && s_freeMoveAnchor->isInCombatMode(true, true);
        if (inCombatNow) s_lastInCombatMs = nowFC;
        bool inCombatRecent  = inCombatNow
                            || (s_lastInCombatMs > 0
                                && nowFC - s_lastInCombatMs < FACECAM_COMBAT_GRACE_MS);

        bool softOK    = s_settingInventoryFaceCam
                      && s_mode == MODE_FREE_MOVE
                      && s_freeMoveAnchor && s_freeMoveAnchor->movement
                      && isOwnInventoryOpen();

        bool faceCamWanted;
        if (hardStop || inCombatRecent)
        {
            faceCamWanted        = false;
            s_invFaceCloseStreak = INV_FACE_CLOSE_DEBOUNCE;
        }
        else if (softOK)
        {
            faceCamWanted        = true;
            s_invFaceCloseStreak = 0;
        }
        else
        {
            if (s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE)
                s_invFaceCloseStreak++;
            faceCamWanted = s_fpActive && s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE;
        }

        if (s_firstPersonActive && faceCamWanted)
        {
            exitFirstPerson(true);
            s_fpSuspendedForInv = true;
        }

        if (faceCamWanted && !s_fpActive && !s_firstPersonActive)
            enterOTS();
        else if (!faceCamWanted && s_fpActive)
        {
            exitOTS(true);
            if (s_fpSuspendedForInv)
            {
                s_fpSuspendedForInv = false;
                enterFirstPerson();
            }
            return;
        }
    }

    // Debounce edges can clear s_fpActive without returning to first person, stranding the player top-down.
    if (s_fpSuspendedForInv && !s_fpActive && !s_firstPersonActive
        && s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive
        && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && !isOwnInventoryOpen())
    {
        s_fpSuspendedForInv = false;
        enterFirstPerson();
        return;
    }

    if (!s_fpActive && !s_firstPersonActive)
        return;

    if (s_firstPersonActive)
    {
        // restrictPosition never runs in first person, so sync the floor here or upper floors do not load.
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            int anchorFloor = s_freeMoveAnchor->movement->getCurrentFloor();
            if (anchorFloor != ou->player->getCurrentFloor())
            {
                ou->player->setCurrentFloor(anchorFloor);
                if (g_log.debugLogging)
                {
                    char fbuf[80];
                    sprintf_s(fbuf, sizeof(fbuf),
                        "[WASDCombat] dc_fp_floor_sync floor=%d", anchorFloor);
                    DebugLog(fbuf);
                }
            }
        }
        ou->player->updateFloorVisibility(ou->player->getAllPlayerCharacters());
        // The squad-based pass leaves black voids below a solo anchor; the Building* is only null-checked.
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            if (s_freeMoveAnchor->movement->building.getBuilding() == nullptr)
                ou->player->resetFloorsVisibility();
            else if (s_fpFloorRevealBelow)
                ou->player->setFloorsVisibility(
                    s_freeMoveAnchor->movement->getCurrentFloor());
        }
        fpDriveFrame(thisptr, uiOpenNow);
        return;
    }

    // The wheel still zooms inside orig and scales the name-tags, so hold the saved altitude.
    thisptr->altitude = s_otsSavedAltitude;

    bool ctxVisible = ou->player->contextMenu.isVisible();
    bool anchorKO   = s_freeMoveAnchor->isUnconcious();
    // inDialogue also covers the prisoner and bail dialogue windows, which need the cursor.
    bool inDialogue = (gui && gui->inDialogue());
    bool captureOk  = !s_menuSuspendActive && !ctxVisible
                   && !s_lootUiSuspendActive && !anchorKO && !inDialogue
                   && isKenshiForegroundMain();

    if (captureOk)
    {
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2
                     + (LONG)((rc.right - rc.left) * s_otsCrosshairOffsetX);
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);
            POINT cur;
            if (GetCursorPos(&cur))
            {
                if (s_fpCursorCaptured)
                {
                    float dx = (float)(cur.x - center.x);
                    float dy = (float)(cur.y - center.y);
                    if (dx != 0.0f || dy != 0.0f)
                    {
                        s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                        if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                    }
                }
                SetCursorPos(center.x, center.y);
                s_fpCursorCaptured = true;
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    {
        bool ownInv = isOwnInventoryOpen();
        // Clicking portraits with the inventory open changes the selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // Recruited mod NPCs fail isPlayerCharacter(), so the open window's character comes first.
            Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
            if (invChar && invChar->movement)
                invTarget = invChar;
            else if (s_selectedCharacter && s_selectedCharacter->movement
                     && s_selectedCharacter->isPlayerCharacter())
                invTarget = s_selectedCharacter;
            else
                invTarget = s_freeMoveAnchor;
        }

        if (ownInv && (!s_otsInvFaceActive || s_otsInvFaceChar != invTarget))
        {
            if (!s_otsInvFaceActive)
            {
                s_otsSavedYaw   = s_fpYaw;
                s_otsSavedPitch = s_fpPitch;
                s_otsSavedDist  = s_otsDistCur;
                DebugLog("[WASDCombat] dc_cam_inventory_face");
            }
            else
            {
                DebugLog("[WASDCombat] dc_cam_inventory_face_retargeted");
            }
            s_otsInvFaceActive = true;
            s_otsInvFaceChar   = invTarget;

            float faceYaw = s_fpYaw + 3.14159265f;
            if (invTarget && invTarget->movement)
            {
                Ogre::Vector3 bd = invTarget->movement->direction;
                bd.y = 0.0f;
                float bl = bd.length();
                if (bl > 0.001f)
                {
                    bd /= bl;
                    faceYaw = atan2f(-bd.x, -bd.z) + 3.14159265f;
                }
            }
            s_fpYaw      = faceYaw;
            s_fpPitch    = -0.02f;
            s_otsDistCur = 20.0f;
        }
        else if (!ownInv && s_otsInvFaceActive)
        {
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;
            s_fpYaw      = s_otsSavedYaw;
            s_fpPitch    = s_otsSavedPitch;
            s_otsDistCur = s_otsSavedDist;
            DebugLog("[WASDCombat] dc_cam_inventory_face_restored");
        }
    }

    // Recomputed every frame: an edge-triggered aim left a stale yaw on a re-open.
    Character* tgt = nullptr;
    {
        Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
        if (invChar && invChar->movement)
            tgt = invChar;
        else if (s_selectedCharacter && s_selectedCharacter->movement
                 && s_selectedCharacter->isPlayerCharacter())
            tgt = s_selectedCharacter;
        else
            tgt = s_freeMoveAnchor;
    }
    if (tgt && tgt->movement && s_fpNode)
    {
        CharMovement* mvP = tgt->movement;
        Ogre::Vector3 bd = mvP->direction;
        bd.y = 0.0f;
        float bl = bd.length();
        float faceYaw = (bl > 0.001f) ? atan2f(bd.x, bd.z) : 0.0f;  // forward = -bd, the character's front
        Ogre::Quaternion q =
            Ogre::Quaternion(Ogre::Radian(faceYaw), Ogre::Vector3::UNIT_Y) *
            Ogre::Quaternion(Ogre::Radian(-0.02f),  Ogre::Vector3::UNIT_X);
        Ogre::Vector3 pivot = mvP->pos;
        pivot.y += 12.5f;                                  // upper chest
        Ogre::Vector3 eye = pivot + q * Ogre::Vector3(0.0f, 0.5f, 20.0f);
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }
}

// Floor visibility refreshes only here, but the clamp yanks the detached view, so re-apply the pose.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // Skip the RTS clamp while detached: it yanks the free camera back to the floor.
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}
