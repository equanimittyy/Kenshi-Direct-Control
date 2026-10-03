// Inventory face-cam: a detached Ogre camera (s_fpNode) that faces the character while
// their own inventory is open, so worn gear is visible. A gameplay over-the-shoulder (OTS)
// view was rejected: Kenshi's interior floor render works only with the top-down RTS
// camera. Runs in cameraUpdate_hook, which still runs under the inventory pause.
// Kenshi scale is about 10 cm per unit.
static bool             s_fpActive          = false;
// cameraUpdate_hook runs twice per frame and the two calls disagree on the own-inventory
// window count, so a naive exit toggled the face-cam every frame and broke altitude hold
// and click suppression. Exit only after the signal is absent for this many calls.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// World name-tags are hidden while the face-cam is up because camera input still nudged
// them. s_savedShowNames is read before hiding, so a showNames() that also writes the
// option cannot poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;
static bool             s_otsInvFaceActive  = false;
static Character*       s_otsInvFaceChar    = nullptr; // who it is aimed at (identity-compared only)
// The shoulder view from the first inventory open, restored on close.
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Fraction of client width (positive = right of center), so the crosshair clears the body
// for selection clicks. The cursor is pinned there and mouse-look deltas are measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// The detached camera survives a save-load, so it is re-attached only after the world is
// valid again: re-attaching mid-load crashed, and skipping it left the camera orphaned.
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // face-cam: held to block scroll-zoom

// First person is a second drive mode for the same detached camera as the inventory
// face-cam, with the same s_fpNode and saved camera locals. s_fpActive (face-cam) and
// s_firstPersonActive are mutually exclusive. Opening an inventory in first person suspends
// it for the face-cam and returns to first person on close (s_fpSuspendedForInv).
static bool  s_firstPersonActive   = false;
// Persistent first-person intent, like s_userWantsDC: it survives chunk-streaming and
// save loads so first person re-enters after the scene rebuilds.
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // set by the key edge, consumed on the game thread
static bool  s_fpSuspendedForInv   = false;
static float s_fpSensitivityFP     = 1.0f;
static float s_fpEyeHeight         = 16.5f;  // fallback neck model
static float s_fpFwdOffset         = 1.2f;   // eye ahead of the neck
static float s_fpFovDegFP          = 75.0f;
static float s_fpNearClipFP        = 0.2f;
static float s_fpNeckLimitRad      = 1.309f; // 75 degrees
static bool  s_fpHideHair          = true;
static bool  s_fpHideHead          = true;
static bool  s_fpHairHidden        = false;
static bool  s_fpHeadBoneHidden    = false;
static float s_fpLeanFwd           = 0.0f;   // fallback model only
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;
static float         s_fpBoneEyeUp       = 1.0f;
static float         s_fpEyeUpAdjust     = 0.0f;   // keeps chest and shoulders below frame
static float         s_fpMoveLeanUp      = 0.0f;   // counters the forward jog lean
static float         s_fpMoveLeanFwd     = 0.0f;   // pushes past the leaning torso
static float         s_fpMoveNearClip    = 0.0f;   // slices the arm swinging into the lens
static float         s_fpMoveLeanSmooth  = 0.0f;
static float         s_fpJogForward      = 2.0f;   // closes the gap the jog lean opens
static float         s_fpRunForward      = 4.0f;   // same, while running
static float         s_fpGaitFwdSmooth   = 0.0f;
static float         s_fpActionClrSmooth = 0.0f;
static float         s_fpStreamDist      = 2.0f;   // per-frame rig teleports re-page grass
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;
static bool          s_fpLastStreamValid = false;
static float         s_fpGrassRangeMult  = 1.0f;   // does not fix the distant re-scatter;
                                                   // it only renders more grass that re-scatters.
static float         s_fpSavedGrassRange   = 0.0f;
static float         s_fpSavedFoliageRange = 0.0f;
static bool          s_fpOptRangeSaved     = false;
static bool          s_fpCamPreOrig        = false; // also drive the camera before Kenshi's loop. Default off: it
                                                    // does not fix the grass flicker (that pass reads the camera on a
                                                    // render thread) and it multiplies the follow and gait lerp rates.
static float         s_fpFollowTurn        = 0.08f; // lerps the view yaw toward the heading while the character moves
                                                    // on its own (not WASD), only after FollowDelayMs without mouse-look. 0 = off.
static float         s_fpFollowDelayMs     = 400.0f;
static ULONGLONG     s_fpLastMouseMoveMs   = 0;
static int           s_fpFoliageCenterMode = 1;     // while moving, moves the camera center node (grass streaming anchor)
                                                    // to the eye. At idle the center stays vanilla so panning cannot re-scatter grass.
// FreezeCamTest diagnostic: while standing in FP, lock the camera position and update only
// orientation, to tell positional grass re-scatter apart from billboard shimmer.
static int           s_fpFreezeCamTest     = 0;
static Ogre::Vector3 s_fpFrozenEye         = Ogre::Vector3::ZERO;
static bool          s_fpFrozenValid       = false;
static float         s_fpLookSmooth        = 0.4f;  // 0..0.9; smooths the rendered orientation to reduce render-thread
                                                    // grass flicker, at the cost of mouse-look snappiness.
static float         s_fpYawSm             = 0.0f;  // equals s_fpYaw when LookSmooth = 0
static float         s_fpPitchSm           = 0.0f;
static bool          s_fpSmValid           = false;

// The eye follows the head bone's real world position (Character::getBoneWorldPosition), and
// look input comes from 1 kHz DirectInput. The old path rotated the ~2 m bone offset by the view
// yaw, which swung the eye on an arc during pure rotation and never tracked the animated head.
static bool  s_fpTrueBoneEye  = true;   // 0 = old synthetic eye path
static float s_fpEyeDrop      = 0.0f;   // drop below the head-bone origin, in mount-bone-height units
static bool  s_fpRawMouse     = true;   // 0 = fps-dependent cursor-warp deltas
static float s_fpMoveForward  = 0.0f;   // speed-scaled forward lead so the eye does not lag the leaning head
static float s_fpMoveSpeedRef = 30.0f;  // feet-delta ground speed that maps to a lead of 1.0
// The lead uses feet-delta ground speed because the currentMotion magnitude is too noisy.
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;   // stair-climb detector
static float     s_fpMoveSpeed      = 0.0f;
static float     s_fpMoveFwdSmooth  = 0.0f;
static float     s_fpClimbSpeedSmooth = 0.0f; // + = ascending
static ULONGLONG s_fpFeetTickMs     = 0;

// On stairs the forward offset drives the eye into the rising steps, so while climbing the
// forward push scales down and the eye lifts. Flat ground restores the full offset.
static float s_fpStairForwardReduce   = 0.25f; // higher reaches full pullback at a gentler climb; 0 = off
static float s_fpStairForwardMinScale = 0.30f; // forward-offset fraction kept at full climb
static float s_fpStairEyeLift         = 0.30f; // extra eye height at full climb; 0 = off

// A hostile inside EnemyClearRadius collapses the forward eye offsets toward
// EnemyClearMinScale and pulls the near plane in, so the eye does not poke into its model.
// The nearest-hostile distance is sampled in mainLoop (game thread); -1 = none in range.
static float s_fpEnemyClearRadius   = 12.0f;  // 0 = off
static float s_fpEnemyClearMinScale = 0.15f;  // forward fraction kept at contact
static float s_fpEnemyNearClip      = 0.10f;  // 0 = off
static float s_fpEnemyClearSmooth   = 1.0f;   // 1 = no hostile near
static float s_fpEnemyNearestDist   = -1.0f;

// Re-showing upper storeys from eye level was rejected: Kenshi's upper-floor meshes are
// one-sided for the top-down cutaway and render as floating planks and sky holes from below.

// In FP, restrictPosition (the vanilla cutaway refresh) is skipped, and the substitute
// updateFloorVisibility pass reveals storeys by where squad members stand, which leaves black
// voids below a solo character. So after that pass all floors up to the anchor's floor are
// forced visible: one answer per frame, which also stops the two-writer flicker.
static bool s_fpFloorRevealBelow = true;

// Consumed on the game thread because the sneak-button path needs a valid anchor.
static volatile bool s_sneakToggleRequested = false;

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // after a strafe or backpedal, keeps the camera authoritative through
                                                    // the release, so the neck limit does not snap the view to the body.
static float         s_fpActionClearFwd    = 2.0f;  // extra forward eye offset during committed actions, clear of the body
static float         s_fpBodyYaw         = 0.0f;   // visible body only; the eye mounts on the view yaw
static const float   FP_BODY_TURN        = 0.20f;
static MyGUI::TextBox* s_fpCrosshair     = nullptr;
static const float FP_HEAD_LEN     = 1.8f;   // fallback model
static const float FP_BONE_SMOOTH  = 0.45f;  // damps stride bob
