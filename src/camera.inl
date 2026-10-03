static void otsRestoreCameraToRig(CameraClass* cam)
{
    if (!cam) return;
    Ogre::Camera* oc = cam->camera;
    if (oc)
    {
        oc->detachFromParent();
        Ogre::SceneNode* rigNode = cam->getCameraNode();
        if (rigNode)
            rigNode->attachObject(oc);
        if (s_fpCamLocalsSaved)
        {
            oc->setPosition(s_fpSavedCamPos);
            oc->setOrientation(s_fpSavedCamOri);
            oc->setFOVy(s_fpSavedFov);
            if (s_fpSavedNearClip > 0.0f)
                oc->setNearClipDistance(s_fpSavedNearClip);
        }
        if (s_fpHadAutoTrack)
            oc->setAutoTracking(true, cam->getCenterNode());
        if (s_fpNode)
        {
            oc->getSceneManager()->destroySceneNode(s_fpNode);
            s_fpNode = nullptr;
        }
    }
    s_fpNode           = nullptr;
    s_fpCamLocalsSaved = false;
    s_fpHadAutoTrack   = false;
}

static void otsRestoreNames()
{
    if (!s_namesHidden) return;
    if (gui) gui->showNames(s_savedShowNames);
    s_namesHidden = false;
    DebugLog("[WASDCombat] dc_names_restored");
}

static void exitOTS(bool restoreCamera)
{
    if (!s_fpActive) return;
    s_fpActive = false;
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    otsRestoreNames();
    s_fpNode = nullptr;
    DebugLog("[WASDCombat] dc_cam_exited");
}

// The attached RTS camera can only look top-down, so the inventory face-cam detaches
// the camera onto its own root node. The face-cam runs only while the game is paused
// and the character stands.
static void enterOTS()
{
    if (s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    s_otsSavedAltitude = cam->altitude;   // held constant during the face-cam
    cam->stopFollowing();
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDeg)));
    oc->setNearClipDistance(s_fpNearClip);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    if (gui && !s_namesHidden)
    {
        s_savedShowNames = options ? options->showNames : true;
        gui->showNames(false);
        s_namesHidden = true;
        DebugLog("[WASDCombat] dc_names_hidden");
    }
    s_fpActive = true;
    DebugLog("[WASDCombat] dc_cam_entered");
}

// True for a plain inventory and for a backpack character (2 windows), false for any
// trade or loot session. Both inputs are live, never cached; see s_tradeWindowActive.
static bool isOwnInventoryOpen()
{
    return gui && !s_tradeWindowActive
        && !gui->isCharacterEditorMode()   // the editor owns the camera
        && gui->getNumOpenInventoryWindows() >= 1;
}

static bool invMoveThroughEligible()
{
    return s_mode == MODE_FREE_MOVE
        && s_freeMoveAnchor
        && !s_settingInventoryFaceCam
        && gui && gui->isAnyInventoryWindowOpen()
        && !gui->inDialogue()                    // dialogue keeps its vanilla pause
        && !gui->isCharacterEditorMode();
}

// First person shares the detached-camera machinery (s_fpNode, saved camera
// locals) with the inventory face-cam; s_firstPersonActive selects the drive mode.

// The head bone is manually controlled so that animation cannot rescale it; the neck
// and body keep animating.
static void fpSetHeadBoneHidden(bool hide)
{
    if (hide == s_fpHeadBoneHidden) return;
    if (!s_freeMoveAnchor) { s_fpHeadBoneHidden = false; return; }
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (!sk) { s_fpHeadBoneHidden = false; return; }
    static const char* const HEAD_NAMES[] = { "Bip01 Head", "Head" };
    Ogre::OldBone* hb = nullptr;
    for (int i = 0; i < 2 && !hb; ++i)
        if (sk->hasBone(HEAD_NAMES[i]))
            hb = sk->getBone(HEAD_NAMES[i]);
    if (!hb) { s_fpHeadBoneHidden = false; return; }
    if (hide)
    {
        hb->setManuallyControlled(true);
        hb->setScale(Ogre::Vector3(0.001f, 0.001f, 0.001f));
        s_fpHeadBoneHidden = true;
        DebugLog("[WASDCombat] dc_fp_head_hidden");
    }
    else
    {
        hb->setScale(Ogre::Vector3(1.0f, 1.0f, 1.0f));
        hb->setManuallyControlled(false);
        s_fpHeadBoneHidden = false;
        DebugLog("[WASDCombat] dc_fp_head_restored");
    }
}

// The skeleton MUST come from AppearanceBase::getSkeleton(): Entity::getSkeleton()
// on the body entity returns a different runtime type whose virtual calls crash.
static bool fpGetHeadWorld(Ogre::Vector3& out)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return false;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return false;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return false;

    CharMovement* mvB   = s_freeMoveAnchor->movement;
    Ogre::Vector3 root  = mvB->pos;

    // hasBone validates the skeleton and picks a bone that exists, so
    // getBoneWorldPosition never sees a missing bone. The true-world path prefers the
    // head bone; the synthetic path prefers the neck, which keeps animating while the
    // head is scale-hidden.
    static const char* const BONE_TRUE[]  = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    static const char* const BONE_SYNTH[] = { "Bip01 Neck", "Bip01 Head", "Bip01 Neck1", "Head" };
    static const float        BONE_SYNTH_UP[] = { 2.4f, 1.0f, 2.4f, 1.0f };
    const char* const* names = s_fpTrueBoneEye ? BONE_TRUE : BONE_SYNTH;
    const char* used = nullptr;
    int usedIdx = -1;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(names[i])) { used = names[i]; usedIdx = i; }
    if (!used) return false;

    if (s_fpTrueBoneEye)
    {
        // getBoneWorldPosition gives the bone's true world position, independent of the view
        // yaw, so a pure pan does not swing the eye on an arc as root + rotate(offset, yaw)
        // did.
        Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
        float ddx = head.x - root.x, ddy = head.y - root.y, ddz = head.z - root.z;
        bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f
                      && head.y > root.y - 1.0f;
        if (plausible)
        {
            out           = head;
            s_fpBoneEyeUp = 0.0f;   // eye level comes from EyeDrop on this path
            if (!s_fpBoneLogged)
            {
                s_fpBoneLogged = true;
                char bbuf[256];
                sprintf_s(bbuf, sizeof(bbuf),
                    "[WASDCombat] dc_fp_truebone bone=%s world=(%.1f,%.1f,%.1f)"
                    " root=(%.1f,%.1f,%.1f)",
                    used, head.x, head.y, head.z, root.x, root.y, root.z);
                DebugLog(bbuf);
            }
            return true;
        }
        // Implausible (skeleton still loading or an odd rig): use the synthetic path.
    }

    // The bone's derived position is model space relative to the character root, and
    // the entity node transform is stale, so rotate the offset by the view yaw.
    Ogre::OldBone* b = sk->getBone(used);
    if (!b) return false;
    s_fpBoneEyeUp = (strstr(used, "Neck") != nullptr) ? 2.4f
                  : (usedIdx >= 0 && !s_fpTrueBoneEye ? BONE_SYNTH_UP[usedIdx] : 1.0f);
    Ogre::Vector3 boneModel = b->_getDerivedPosition();
    float mountYaw = s_fpYawSm;
    Ogre::Quaternion qBody(Ogre::Radian(mountYaw), Ogre::Vector3::UNIT_Y);
    out = root + qBody * boneModel;

    float ddx = out.x - root.x, ddy = out.y - root.y, ddz = out.z - root.z;
    bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f;
    if (!s_fpBoneLogged)
    {
        s_fpBoneLogged = true;
        char bbuf[256];
        sprintf_s(bbuf, sizeof(bbuf),
            "[WASDCombat] dc_fp_headbone_tracking bone=%s plausible=%d"
            " boneModel=(%.1f,%.1f,%.1f) mountYaw=%.2f world=(%.1f,%.1f,%.1f)",
            used, (int)plausible,
            boneModel.x, boneModel.y, boneModel.z,
            mountYaw, out.x, out.y, out.z);
        DebugLog(bbuf);
    }
    return plausible;
}

// The captured cursor sits at the viewport center, so LMB/RMB act on what the
// crosshair covers. Load teardown nulls the pointer; it is recreated on the fresh GUI.
static void fpShowCrosshair(bool show)
{
    if (show && !s_fpCrosshair)
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return;
        const MyGUI::IntSize vs = MyGUI::RenderManager::getInstance().getViewSize();
        s_fpCrosshair = g->createWidget<MyGUI::TextBox>("TextBox",
            MyGUI::IntCoord(vs.width / 2 - 16, vs.height / 2 - 16, 32, 32),
            MyGUI::Align::Default, "Pointer");
        if (!s_fpCrosshair) return;
        s_fpCrosshair->setNeedMouseFocus(false);
        s_fpCrosshair->setTextAlign(MyGUI::Align::Center);
        s_fpCrosshair->setCaption("+");
        s_fpCrosshair->setTextColour(MyGUI::Colour(0.83f, 0.76f, 0.60f, 0.95f));
        s_fpCrosshair->setTextShadow(true);
    }
    if (s_fpCrosshair)
        s_fpCrosshair->setVisible(show);
}

// ContextMenuGUI is forward-declared, so optionsList is read at its offset 0xF8.
static const MyGUI::Colour FP_MENU_ACCENT(0.72f, 0.86f, 0.38f, 1.0f);
static const MyGUI::Colour FP_MENU_NORMAL(0.78f, 0.75f, 0.66f, 1.0f);
static void fpTintContextMenu()
{
    if (!ou || !ou->player) return;
    ContextMenu& cm = ou->player->contextMenu;
    if (!cm.isVisible()) return;

    MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
    ContextMenuGUI* menus[2] = { cm.menuGUI, cm.menuGUI2 };
    for (int m = 0; m < 2; ++m)
    {
        if (!menus[m]) continue;
        MyGUI::Widget* list =
            *(MyGUI::Widget**)((char*)menus[m] + 0xF8);
        if (!list) continue;
        size_t n = list->getChildCount();
        for (size_t i = 0; i < n; ++i)
        {
            MyGUI::Widget* c = list->getChildAt(i);
            if (!c) continue;
            MyGUI::TextBox* tb = c->castType<MyGUI::TextBox>(false);
            if (!tb) continue;
            bool hovered = (focus == c);
            if (!hovered && focus)
                for (MyGUI::Widget* p = focus->getParent(); p; p = p->getParent())
                    if (p == c) { hovered = true; break; }
            tb->setTextColour(hovered ? FP_MENU_ACCENT : FP_MENU_NORMAL);
        }
    }
}

static void exitFirstPerson(bool restoreCamera)
{
    if (!s_firstPersonActive) return;
    s_firstPersonActive = false;
    s_fpCursorCaptured  = false;
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
        DebugLog("[WASDCombat] dc_fp_grass_range_restored");
    }
    fpShowCrosshair(false);
    fpSetHeadBoneHidden(false);
    if (s_fpHairHidden)
    {
        s_fpHairHidden = false;
        if (s_freeMoveAnchor)
        {
            AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
            if (ap) { ap->shaveHead(false); DebugLog("[WASDCombat] dc_fp_hair_restored"); }
        }
    }
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    DebugLog("[WASDCombat] dc_fp_exited");
}

// Camera calls MUST run on the game thread.
static void enterFirstPerson()
{
    if (s_firstPersonActive || s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;

    // Re-read [FirstPerson] on every entry so INI edits apply without a relaunch.
    {
        char cfgPath[MAX_PATH];
        getConfigPath(cfgPath, sizeof(cfgPath));
        loadFirstPersonConfig(cfgPath);
    }

    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    cam->stopFollowing();   // zoom is left untouched, so the restored view is the pre-FP view
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
    oc->setNearClipDistance(s_fpNearClipFP);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);

    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f;
    float dlen = d.length();
    if (dlen > 0.001f)
    {
        d /= dlen;
        s_fpYaw = atan2f(-d.x, -d.z);   // forward = (-sin yaw, 0, -cos yaw)
    }
    s_fpPitch = 0.0f;

    if (s_fpHideHair)
    {
        AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
        if (ap) { ap->shaveHead(true); s_fpHairHidden = true; DebugLog("[WASDCombat] dc_fp_hair_hidden"); }
    }
    // Kenshi sizes the grass range for the high top-down camera, so at ground level
    // grass pops in at the edge of a short ring. Widen the range while first person
    // owns the view, and restore the exact saved values on exit.
    if (options && s_fpGrassRangeMult > 1.0f && !s_fpOptRangeSaved)
    {
        s_fpSavedGrassRange   = options->grassRange;
        s_fpSavedFoliageRange = options->foliageRange;
        s_fpOptRangeSaved     = true;
        options->grassRange   *= s_fpGrassRangeMult;
        options->foliageRange *= s_fpGrassRangeMult;
        DebugLog("[WASDCombat] dc_fp_grass_range_boosted");
    }
    s_fpLeanFwd         = 0.0f;
    s_fpMoveLeanSmooth  = 0.0f;
    s_fpGaitFwdSmooth   = 0.0f;
    s_fpActionClrSmooth = 0.0f;
    s_fpHaveLastFeet    = false;
    s_fpMoveSpeed       = 0.0f;
    s_fpMoveFwdSmooth   = 0.0f;
    s_fpFeetTickMs      = 0;
    s_fpSmValid         = false;    // re-seeded on the first frame
    s_fpEnemyClearSmooth = 1.0f;
    s_fpEnemyNearestDist = -1.0f;
    s_fpLastStreamValid = false;    // forces a streaming teleport on the first FP frame
    s_fpFrozenValid     = false;
    s_fpBodyYaw         = s_fpYaw;
    s_fpHeadSmoothValid = false;
    s_fpBoneLogged      = false;
    s_firstPersonActive = true;
    s_fpCursorCaptured  = false;   // the first capture pass establishes the center
    if (s_fpHideHead)
        fpSetHeadBoneHidden(true);
    fpShowCrosshair(true);
    DebugLog("[WASDCombat] dc_fp_entered");
}

// Runs from cameraUpdate_hook AFTER the game's camera update.
static void fpDriveFrame(CameraClass* thisptr, bool uiOpen)
{
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || !s_freeMoveAnchor->movement || s_lootUiSuspendActive)
    {
        exitFirstPerson(true);
        return;
    }

    // Any open UI releases the mouse so the free OS cursor can click menu items.
    fpShowCrosshair(!uiOpen);

    // Mouse-look also pauses while the RMB hold-menu is up, so the freed cursor can
    // browse the options; releasing RMB selects.
    bool rmbHeld    = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool ctxVisible = ou->player->contextMenu.isVisible();
    if (rmbHeld || ctxVisible)
        fpTintContextMenu();
    bool captureOk = !uiOpen && !s_menuSuspendActive && !rmbHeld && !ctxVisible
                  && !s_lootUiSuspendActive && isKenshiForegroundMain();
    if (captureOk)
    {
        // Falls back to cursor warp until the DirectInput device is acquired.
        bool useRaw = s_fpRawMouse;
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }

        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);

            float dx = 0.0f, dy = 0.0f;
            bool  haveDelta = false;
            if (useRaw && s_diReady)
            {
                fpTakeMouseAccum(&dx, &dy);
                haveDelta = s_fpCursorCaptured;    // skip the baseline frame
                SetCursorPos(center.x, center.y);  // keep the click point centered
                s_fpCursorCaptured = true;
            }
            else
            {
                POINT cur;
                if (GetCursorPos(&cur))
                {
                    if (s_fpCursorCaptured)
                    {
                        dx = (float)(cur.x - center.x);
                        dy = (float)(cur.y - center.y);
                        haveDelta = true;
                    }
                    SetCursorPos(center.x, center.y);
                    s_fpCursorCaptured = true;
                }
            }

            if (haveDelta && (dx != 0.0f || dy != 0.0f))
            {
                s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                // The point-click FollowTurn yields until the mouse has been still for
                // FollowDelayMs; the deadzone ignores 1 px jitter.
                if (dx > 1.0f || dx < -1.0f || dy > 1.0f || dy < -1.0f)
                    s_fpLastMouseMoveMs = GetTickCount64();
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;  // re-baseline when capture resumes
        // Drain deltas accumulated while a UI owned the cursor, so the view does not jump.
        if (s_fpRawMouse) { float jx, jy; fpTakeMouseAccum(&jx, &jy); }
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    // currentlyMoving/currentSpeed are set whatever issued the move, so this covers
    // point-click and autonomous combat/heal movement as well as WASD.
    bool fpMoving = mvFP->currentlyMoving || mvFP->currentSpeed > 0.25f;

    // WASD (and a grace period while still coasting): face the body to the view, so
    // movement is strafe-relative and the camera yaw is never touched. Without the
    // grace, the body still faces 180 degrees from the view on the release frame of a
    // backpedal, and the idle neck-limit snaps the camera onto it.
    // Point-click/autonomous: the camera follows the heading.
    // Idle: the neck-limit turns the body when the view turns too far.
    bool strafeGrace = (GetTickCount64() - s_wasdLastHeldMs) < FP_STRAFE_GRACE_MS;
    if (s_frameWasdHeld || (fpMoving && strafeGrace))
    {
        // Turn only the visible body; the eye is mounted on the view yaw, so the camera never jumps.
        float dyaw = s_fpYaw - s_fpBodyYaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        s_fpBodyYaw += dyaw * FP_BODY_TURN;
        while (s_fpBodyYaw >  3.14159265f) s_fpBodyYaw -= 6.28318531f;
        while (s_fpBodyYaw < -3.14159265f) s_fpBodyYaw += 6.28318531f;
        mvFP->direction =
            Ogre::Vector3(-sinf(s_fpBodyYaw), 0.0f, -cosf(s_fpBodyYaw));
    }
    else if (fpMoving && s_fpFollowTurn > 0.0f
             && (GetTickCount64() - s_fpLastMouseMoveMs) > (ULONGLONG)s_fpFollowDelayMs)
    {
        // The character walks its own path here, so the view follows the heading; the
        // game's pathing owns mvFP->direction. It runs only after the mouse has been still
        // for FollowDelayMs, so it never fights active mouse-look.
        Ogre::Vector3 hd = mvFP->direction;
        hd.y = 0.0f;
        float hlen = hd.length();
        if (hlen > 0.001f)
        {
            hd /= hlen;
            float headYaw = atan2f(-hd.x, -hd.z);
            float d = headYaw - s_fpYaw;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            s_fpYaw += d * s_fpFollowTurn;
            while (s_fpYaw >  3.14159265f) s_fpYaw -= 6.28318531f;
            while (s_fpYaw < -3.14159265f) s_fpYaw += 6.28318531f;
            s_fpBodyYaw = headYaw;   // kept in sync so a later stop does not snap
        }
    }
    else
    {
        Ogre::Vector3 bd = mvFP->direction;
        bd.y = 0.0f;
        float blen = bd.length();
        if (blen > 0.001f)
        {
            bd /= blen;
            float bodyYaw = atan2f(-bd.x, -bd.z);
            float delta   = s_fpYaw - bodyYaw;
            while (delta >  3.14159265f) delta -= 6.28318531f;
            while (delta < -3.14159265f) delta += 6.28318531f;
            if (delta > s_fpNeckLimitRad || delta < -s_fpNeckLimitRad)
            {
                mvFP->direction =
                    Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
                s_fpYaw = bodyYaw
                        + (delta > 0.0f ?  s_fpNeckLimitRad
                                        : -s_fpNeckLimitRad);
                bodyYaw = s_fpYaw;
            }
            // Keep the body yaw synced so the next movement turn does not start with a jump.
            s_fpBodyYaw = bodyYaw;
        }
    }

    // Only the visuals use the smoothed view; body facing and motion use the raw
    // target. A smaller per-frame rotation reduces the grass re-facing mismatch on the
    // render thread, so foliage flickers less. LookSmooth=0 gives the raw value exactly.
    if (!s_fpSmValid) { s_fpYawSm = s_fpYaw; s_fpPitchSm = s_fpPitch; s_fpSmValid = true; }
    {
        float a = 1.0f - s_fpLookSmooth;   // 1.0 = snap (off), <1 = glide
        float dyawS = s_fpYaw - s_fpYawSm;
        while (dyawS >  3.14159265f) dyawS -= 6.28318531f;
        while (dyawS < -3.14159265f) dyawS += 6.28318531f;
        s_fpYawSm += dyawS * a;
        while (s_fpYawSm >  3.14159265f) s_fpYawSm -= 6.28318531f;
        while (s_fpYawSm < -3.14159265f) s_fpYawSm += 6.28318531f;
        s_fpPitchSm += (s_fpPitch - s_fpPitchSm) * a;
    }

    Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Radian(s_fpYawSm),   Ogre::Vector3::UNIT_Y) *
        Ogre::Quaternion(Ogre::Radian(s_fpPitchSm), Ogre::Vector3::UNIT_X);

    // Scale the forward eye offsets down as a hostile gets close, so an aggressor
    // pressing into the lens pulls the eye back to the hidden skull instead of the eye
    // entering their model.
    {
        float enemyScale = 1.0f;
        if (s_fpEnemyClearRadius > 0.0f && s_fpEnemyNearestDist >= 0.0f
            && s_fpEnemyNearestDist < s_fpEnemyClearRadius)
        {
            float t = s_fpEnemyNearestDist / s_fpEnemyClearRadius;
            enemyScale = s_fpEnemyClearMinScale
                       + t * (1.0f - s_fpEnemyClearMinScale);
        }
        s_fpEnemyClearSmooth += (enemyScale - s_fpEnemyClearSmooth) * 0.15f;
    }

    // Legacy path: height is smoothed to damp stride bob, and horizontal position is
    // hard-attached so a sprinting model cannot outrun the camera.
    Ogre::Vector3 eye;
    Ogre::Vector3 headWorld;
    if (fpGetHeadWorld(headWorld))
    {
      if (s_fpTrueBoneEye)
      {
        // The eye is welded to the head bone's world Y (no bob smoothing) and pushed
        // forward along the yaw only, so looking down does not sink the eye into the
        // chest. Ground speed from the feet delta drives the forward lead; the
        // currentMotion magnitude is too noisy for this.
        {
            ULONGLONG nowF = GetTickCount64();
            float dt = (s_fpFeetTickMs > 0) ? (float)(nowF - s_fpFeetTickMs) * 0.001f : 0.0f;
            s_fpFeetTickMs = nowF;
            if (dt > 0.001f && dt < 0.25f && s_fpHaveLastFeet)
            {
                float dfx = mvFP->pos.x - s_fpLastFeetX;
                float dfz = mvFP->pos.z - s_fpLastFeetZ;
                float inst = sqrtf(dfx*dfx + dfz*dfz) / dt;
                if (inst > 400.0f) inst = 400.0f;   // reject teleport/paging jumps
                s_fpMoveSpeed += (inst - s_fpMoveSpeed) * 0.20f;
                float vy = (mvFP->pos.y - s_fpLastFeetY) / dt;
                if (vy >  60.0f) vy =  60.0f;        // reject teleport/paging jumps
                else if (vy < -60.0f) vy = -60.0f;
                s_fpClimbSpeedSmooth += (vy - s_fpClimbSpeedSmooth) * 0.20f;
            }
            s_fpLastFeetX = mvFP->pos.x; s_fpLastFeetZ = mvFP->pos.z;
            s_fpLastFeetY = mvFP->pos.y; s_fpHaveLastFeet = true;
        }
        float lead = 0.0f;
        if (s_fpMoveForward > 0.0f && s_fpMoveSpeedRef > 1.0f)
        {
            float gait = s_fpMoveSpeed / s_fpMoveSpeedRef;
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            lead = s_fpMoveForward * gait;
        }
        s_fpMoveFwdSmooth += (lead - s_fpMoveFwdSmooth) * 0.15f;

        // While climbing, shrink the forward push and lift the eye so the camera clears
        // the rising steps. On flat ground ascent01 is 0, so this is inert.
        float ascent01 = (s_fpClimbSpeedSmooth > 0.0f)
                       ? s_fpClimbSpeedSmooth * s_fpStairForwardReduce : 0.0f;
        if (ascent01 > 1.0f) ascent01 = 1.0f;
        float ascentScale = 1.0f - ascent01 * (1.0f - s_fpStairForwardMinScale);
        float stairLift   = s_fpStairEyeLift * ascent01;

        // The speed factor must update on this path too: it drives the MoveNearClip blend,
        // which was dead under TrueBoneEye when only the legacy path updated it.
        {
            float leanTarget = (s_fpMoveSpeedRef > 1.0f)
                             ? s_fpMoveSpeed / s_fpMoveSpeedRef : 0.0f;
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }

        float fwdScale = ascentScale * s_fpEnemyClearSmooth;
        if (g_log.debugLogging)
        {
            static ULONGLONG t = 0; ULONGLONG n = GetTickCount64();
            if (n - t >= 500) { t = n; char b[112];
                sprintf_s(b, sizeof(b),
                    "[WASDCombat] dc_fp_stair climb=%.2f scale=%.2f lift=%.2f",
                    s_fpClimbSpeedSmooth, ascentScale, stairLift);
                DebugLog(b); }
        }

        eye    = headWorld;
        eye.y += -s_fpEyeDrop + s_fpEyeUpAdjust + stairLift;
        Ogre::Vector3 fwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
        eye += fwd * ((s_fpFwdOffset + s_fpMoveFwdSmooth) * fwdScale);

        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed g = mvFP->speedOrders;
                if (g == JOG) gaitTarget = s_fpJogForward;
                else if (g == RUN || g == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f) eye += fwd * (s_fpGaitFwdSmooth * fwdScale);
        }
        {
            bool actionNow = s_fpActionClearFwd > 0.0f && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f) eye += fwd * (s_fpActionClrSmooth * fwdScale);
        }
      }
      else
      {
        if (!s_fpHeadSmoothValid)
        {
            s_fpHeadSmooth      = headWorld;
            s_fpHeadSmoothValid = true;
        }
        else
        {
            s_fpHeadSmooth.x  = headWorld.x;
            s_fpHeadSmooth.z  = headWorld.z;
            s_fpHeadSmooth.y += (headWorld.y - s_fpHeadSmooth.y) * FP_BONE_SMOOTH;
        }
        // At jog/sprint the model pitches forward and swings the arms and chest up into
        // view, so raise the eye and push it forward with speed. Walking is unaffected.
        {
            float spd = mvFP->currentMotion.length();
            float leanTarget = spd * 0.04f;          // about 1.0 at jog speed
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }
        float leanUp  = s_fpMoveLeanUp  * s_fpMoveLeanSmooth;
        float leanFwd = s_fpMoveLeanFwd * s_fpMoveLeanSmooth;
        eye = s_fpHeadSmooth
            + q * Ogre::Vector3(0.0f, s_fpBoneEyeUp + s_fpEyeUpAdjust + leanUp,
                                -((s_fpFwdOffset + leanFwd) * s_fpEnemyClearSmooth));

        // The bone pose read this frame is last frame's animation, which leaves the camera
        // a stride behind at sprint. Feed forward velocity ahead by the frame time, along
        // the view forward only: a lateral or backward offset made strafing twitch.
        {
            static ULONGLONG s_fpLastTickMs = 0;
            ULONGLONG nowFF = GetTickCount64();
            float dt = (s_fpLastTickMs > 0)
                     ? (float)(nowFF - s_fpLastTickMs) * 0.001f : 0.0f;
            s_fpLastTickMs = nowFF;
            if (dt > 0.05f) dt = 0.05f;
            Ogre::Vector3 vel = mvFP->currentMotion;
            vel.y = 0.0f;
            Ogre::Vector3 viewFwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
            float fwdComp = vel.dotProduct(viewFwd);
            if (fwdComp > 0.0f)
                eye += viewFwd * (fwdComp * dt);
        }

        // Keyed off the discrete gait tier, not the noisy currentMotion magnitude, and gated
        // on actual movement so point-click and combat running get it as well. WALK is 0.
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed gait = mvFP->speedOrders;
                if (gait == JOG)      gaitTarget = s_fpJogForward;
                // GROUPED is the squad-follow sprint, so it gets the same fix as RUN.
                else if (gait == RUN || gait == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpGaitFwdSmooth * s_fpEnemyClearSmooth);
            }
        }

        // Attack swings, blocks, heals, revives and get-ups swing the body into the lens
        // even while standing, which the gait push cannot help. 0 (default) = off.
        {
            bool actionNow = s_fpActionClearFwd > 0.0f
                          && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpActionClrSmooth * s_fpEnemyClearSmooth);
            }
        }
      }
    }
    else
    {
        float speed   = mvFP->currentMotion.length();
        float target  = speed * 0.04f;
        if (target > 2.5f) target = 2.5f;
        s_fpLeanFwd  += (target - s_fpLeanFwd) * 0.15f;

        Ogre::Vector3 neck = mvFP->pos;
        neck.y += (s_fpEyeHeight - FP_HEAD_LEN);
        eye = neck
            + q * Ogre::Vector3(0.0f, FP_HEAD_LEN,
                                -(s_fpFwdOffset + s_fpLeanFwd));
    }

    if (s_fpNode)
    {
        Ogre::Vector3 camPos = eye;
        // FreezeCamTest (diagnostic): while standing, lock the camera position so a pan
        // changes only the orientation. This tests whether the grass pager reads the
        // camera position.
        if (s_fpFreezeCamTest && !fpMoving)
        {
            if (!s_fpFrozenValid) { s_fpFrozenEye = eye; s_fpFrozenValid = true; }
            camPos = s_fpFrozenEye;
        }
        else s_fpFrozenValid = false;
        s_fpNode->setPosition(camPos);
        s_fpNode->setOrientation(q);
    }

    // MoveNearClip pushes the near plane out at speed to slice away the arms that the
    // jog/sprint animation sweeps into the lens. EnemyNearClip pulls it in while a
    // hostile overlaps the lens, so the plane slices a thin cross-section instead of
    // opening a big hole in the aggressor; it wins because it takes the minimum.
    if (thisptr->camera)
    {
        float nc = s_fpNearClipFP;
        if (s_fpMoveNearClip > s_fpNearClipFP)
            nc += (s_fpMoveNearClip - s_fpNearClipFP) * s_fpMoveLeanSmooth;
        if (s_fpEnemyNearClip > 0.0f && s_fpEnemyNearClip < nc
            && s_fpEnemyClearMinScale < 1.0f)
        {
            float overlap01 = (1.0f - s_fpEnemyClearSmooth)
                            / (1.0f - s_fpEnemyClearMinScale);
            if (overlap01 < 0.0f) overlap01 = 0.0f;
            if (overlap01 > 1.0f) overlap01 = 1.0f;
            nc += (s_fpEnemyNearClip - nc) * overlap01;
        }
        thisptr->camera->setNearClipDistance(nc);
    }

    // teleport() keeps the rig coherent (audio listener, zone streaming), but it is a
    // jump: calling it every frame makes the streamer re-page grass continuously, so
    // foliage flickers while moving. Re-teleport only after the eye has moved
    // StreamUpdateDist; StreamUpdateDist=0 restores the every-frame behavior.
    bool doStream = !s_fpLastStreamValid || s_fpStreamDist <= 0.0f
                  || eye.squaredDistance(s_fpLastStreamPos)
                     >= s_fpStreamDist * s_fpStreamDist;
    if (doStream)
    {
        thisptr->teleport(eye);
        thisptr->targetPositionY = eye.y;
        thisptr->speedY          = 0.0f;
        s_fpLastStreamPos   = eye;
        s_fpLastStreamValid = true;
    }

    // The foliage pager streams grass around the camera CENTER node, which lags at the
    // feet while first person renders from the head (technique from KenshiFP):
    // 1. Move the center only while moving. The eye swings in a small circle during a
    //    pan, so moving the center then re-scatters the distant grass.
    // 2. Set the world position with _setDerivedPosition, because the eye lives under
    //    our own node.
    // 3. Force the derived-position recompute, so the same frame's paging pass reads
    //    the new center instead of a stale value.
    if (s_fpFoliageCenterMode && thisptr->center && s_fpNode && fpMoving)
    {
        Ogre::Vector3 eyeWorld = s_fpNode->_getDerivedPositionUpdated();
        thisptr->center->_setDerivedPosition(eyeWorld);
        thisptr->center->_getDerivedPositionUpdated();
    }
}
