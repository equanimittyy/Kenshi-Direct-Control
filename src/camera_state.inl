// The face-cam is driven from cameraUpdate_hook, which still runs under the inventory pause.
static bool             s_fpActive          = false;
// cameraUpdate_hook runs twice per frame and the calls disagree, so the exit is debounced.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// Read before hiding: a showNames() that also writes the option would poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;
static bool             s_otsInvFaceActive  = false;
static Character*       s_otsInvFaceChar    = nullptr; // identity-compared only
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Fraction of client width; the cursor is pinned there and mouse-look deltas measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// Re-attached only once the world is valid again: re-attaching mid-load crashed.
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // held to block scroll-zoom

// s_fpActive (face-cam) and s_firstPersonActive share s_fpNode and are mutually exclusive.
static bool  s_firstPersonActive   = false;
// Survives chunk streaming and save loads, so first person re-enters after the rebuild.
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // set by the key edge, consumed on the game thread
static bool  s_fpSuspendedForInv   = false;
static float s_fpSensitivityFP     = 1.0f;
static float s_fpEyeHeight         = 16.5f;
static float s_fpFwdOffset         = 1.2f;
static float s_fpFovDegFP          = 75.0f;
static float s_fpNearClipFP        = 0.2f;
static float s_fpNeckLimitRad      = 1.309f; // 75 degrees
static bool  s_fpHideHair          = true;
static bool  s_fpHideHead          = true;
static bool  s_fpHairHidden        = false;
static bool  s_fpHeadBoneHidden    = false;
static float s_fpLeanFwd           = 0.0f;
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;
static float         s_fpBoneEyeUp       = 1.0f;
static float         s_fpEyeUpAdjust     = 0.0f;
static float         s_fpMoveLeanUp      = 0.0f;
static float         s_fpMoveLeanFwd     = 0.0f;
static float         s_fpMoveNearClip    = 0.0f;
static float         s_fpMoveLeanSmooth  = 0.0f;
static float         s_fpJogForward      = 2.0f;
static float         s_fpRunForward      = 4.0f;
static float         s_fpGaitFwdSmooth   = 0.0f;
static float         s_fpActionClrSmooth = 0.0f;
static float         s_fpStreamDist      = 2.0f;   // per-frame rig teleports re-page grass
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;
static bool          s_fpLastStreamValid = false;
static float         s_fpGrassRangeMult  = 1.0f;   // only renders more grass; does not fix re-scatter
static float         s_fpSavedGrassRange   = 0.0f;
static float         s_fpSavedFoliageRange = 0.0f;
static bool          s_fpOptRangeSaved     = false;
static bool          s_fpCamPreOrig        = false; // off: no flicker fix, multiplies lerp rates
static float         s_fpFollowTurn        = 0.08f;
static float         s_fpFollowDelayMs     = 400.0f;
static ULONGLONG     s_fpLastMouseMoveMs   = 0;
static int           s_fpFoliageCenterMode = 1;     // idle stays vanilla so panning cannot re-scatter
// FreezeCamTest diagnostic: separates positional grass re-scatter from billboard shimmer.
static int           s_fpFreezeCamTest     = 0;
static Ogre::Vector3 s_fpFrozenEye         = Ogre::Vector3::ZERO;
static bool          s_fpFrozenValid       = false;
static float         s_fpLookSmooth        = 0.4f;  // 0..0.9; trades look snappiness for less flicker
static float         s_fpYawSm             = 0.0f;
static float         s_fpPitchSm           = 0.0f;
static bool          s_fpSmValid           = false;

// The old synthetic eye swung on an arc during pure rotation and never tracked the head.
static bool  s_fpTrueBoneEye  = true;
static float s_fpEyeDrop      = 0.0f;   // mount-bone-height units
static bool  s_fpRawMouse     = true;   // 0 = fps-dependent cursor-warp deltas
static float s_fpMoveForward  = 0.0f;
static float s_fpMoveSpeedRef = 30.0f;  // ground speed mapped to a lead of 1.0
// Feet-delta speed, because the currentMotion magnitude is too noisy.
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;
static float     s_fpMoveSpeed      = 0.0f;
static float     s_fpMoveFwdSmooth  = 0.0f;
static float     s_fpClimbSpeedSmooth = 0.0f; // + = ascending
static ULONGLONG s_fpFeetTickMs     = 0;

// Climbing pulls the eye back and lifts it, or it clips into the rising steps.
static float s_fpStairForwardReduce   = 0.25f; // 0 = off
static float s_fpStairForwardMinScale = 0.30f;
static float s_fpStairEyeLift         = 0.30f; // extra eye height at full climb; 0 = off

// A close hostile shrinks the eye offsets so the eye does not poke into its model.
static float s_fpEnemyClearRadius   = 12.0f;  // 0 = off
static float s_fpEnemyClearMinScale = 0.15f;
static float s_fpEnemyNearClip      = 0.10f;  // 0 = off
static float s_fpEnemyClearSmooth   = 1.0f;
static float s_fpEnemyNearestDist   = -1.0f;

// Upper storeys stay hidden: their one-sided meshes render as sky holes from below.

// Floors up to the anchor's are forced visible: updateFloorVisibility leaves voids below a solo character.
static bool s_fpFloorRevealBelow = true;

// Consumed on the game thread because the sneak-button path needs a valid anchor.
static volatile bool s_sneakToggleRequested = false;

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // stops the neck limit snapping the view on release
static float         s_fpActionClearFwd    = 2.0f;
static float         s_fpBodyYaw         = 0.0f;   // visible body only; the eye mounts on the view yaw
static const float   FP_BODY_TURN        = 0.20f;
static MyGUI::TextBox* s_fpCrosshair     = nullptr;
static const float FP_HEAD_LEN     = 1.8f;
static const float FP_BONE_SMOOTH  = 0.45f;  // damps stride bob
