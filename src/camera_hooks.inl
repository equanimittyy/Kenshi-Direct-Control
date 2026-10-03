// Runs AFTER the game's camera update and BEFORE render: only writes here survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    // Clear the vanilla MMB-rotate state before the game's update, so MMB does nothing.
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;

    // CTRL is a press-on/press-off rotate toggle: force the rotate flag ON (never OFF,
    // so vanilla MMB hold-to-rotate still works). Any open UI turns the toggle off
    // below, because the game does not free an already-captured rotate cursor in the
    // pause menu or the RMB hold-menu.
    if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_firstPersonActive && s_camRotateToggle && key)
        key->rotate = true;

    // controlEnabled=false is Kenshi's own "ignore camera input" gate. While the
    // detached camera is active, the wheel zoom and WASD/edge pan would otherwise move
    // the camera during orig, and the vanilla update lays out the name-tags from that
    // moved camera before any post-orig restore can undo it.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // Also drive the FP camera BEFORE orig: the per-frame foliage visibility and
    // billboard pass samples the camera during the update, and a post-orig drive alone
    // left foliage one frame behind, so grass blinked while rotating. The double drive
    // is safe because the first call recenters the cursor, so the second reads a zero
    // delta.
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

    // Touching the camera while the scene is torn down reads freed memory, so only mark
    // the restore as pending. Do NOT gate on s_dcShutdownInProgress or
    // s_loadGuardActive: they stay true through character creation and would strand
    // the camera detached, hiding the new-game character preview.
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

    // Kenshi also calls this hook for the inventory portrait cameras; if they drove the
    // detached camera, it would detach and re-attach every frame.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // ANY open UI turns the CTRL rotate toggle off and releases the cursor; the player
    // presses CTRL again after closing it. s_camRotateUiOpen also stops CTRL+click in
    // a menu from re-toggling it. The world map and the stats window do not clear
    // controlEnabled, so they are checked explicitly.
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
        thisptr->isRotating  = false;   // release the cursor capture immediately
    }
    s_camRotateUiOpen = uiOpenNow;

    {
        // Only a true teardown exits immediately. Mode, anchor validity and the window
        // count all blip for a frame or two during a squad switch with the inventory open,
        // so they are debounced; otherwise the face-cam detaches and re-attaches on every
        // switch.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Combat disables the face-cam, so opening an inventory mid-fight to loot leaves
        // the camera in place. It is a hard exit, so a single flicker-false frame cannot
        // latch the face-cam through the debounce.
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

        // First person owns the detached camera through s_firstPersonActive, not
        // s_fpActive. Suspend it while the face-cam wants the camera, and return to it
        // when the inventory closes.
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

    // A debounce edge or squad-switch churn can clear s_fpActive on a frame where the
    // branch above does not return to first person, which strands the player in
    // top-down view.
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
        // The game derives the displayed floor from the tracked character inside
        // restrictPosition, which never runs while first person detaches the camera and
        // passes controlEnabled=false. Without this sync, upper floors do not load.
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
        // The squad-based pass above hides storeys no squad member stands on, so looking
        // down from an upper floor shows black voids. Reveal everything up to the anchor's
        // floor, after that pass, so this is the frame's final word. Vanilla cuts a
        // building away only while the tracked character is inside it, so an anchor in no
        // building resets the cutaway; the Building* is only null-checked, never
        // dereferenced.
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

    // The wheel still reaches the game's camera zoom inside orig, which scales the
    // name-tags; hold the altitude saved on enter.
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

    // Own-inventory face-cam: an upper-chest shot facing the character so worn gear is
    // visible. CameraClass::update still ticks under the inventory pause, so the
    // detached node frames cleanly where the vanilla camera could not.
    {
        bool ownInv = isOwnInventoryOpen();
        // Clicking portraits with the inventory open changes the selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // The open window's character comes first: recruited mod NPCs do not report
            // isPlayerCharacter(), so the selection fallback would frame the wrong character.
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

            float faceYaw = s_fpYaw + 3.14159265f;   // fallback: spin around
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

    // Recompute the front-facing pose from the target's facing every frame; an
    // edge-triggered aim left a stale yaw on a re-open. The target priority matches
    // the face-cam swing above.
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

// Interior floor visibility refreshes only when restrictPosition runs, but its
// camera clamp yanks the detached view back. Run it, then re-apply this frame's
// pose.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // Skip the RTS clamp while the camera is detached: it yanks the free camera back to
    // the floor. First person drives the floor reveal from the camera hook instead.
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}
