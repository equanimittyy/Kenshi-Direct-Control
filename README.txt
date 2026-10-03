================================================================
  DIRECT CONTROL FOR KENSHI               v1.3.1
================================================================

Full WASD movement for your selected character. Press V, move with
WASD, and your character responds instantly - overriding click-to-
move, combat AI pathing, and squad orders.

Includes an optional true FIRST-PERSON view - press P and drop
into your character's eyes with mouse-look, your own body and arms
visible as you move, fight, sneak, and heal.

Built for precise, hands-on control during combat, retreats,
ambushes, city navigation, and immersive exploration - without
losing any of Kenshi's charm.


LINKS
-----
  Steam Workshop : https://steamcommunity.com/sharedfiles/filedetails/?id=3737240806
  Nexus Mods     : https://www.nexusmods.com/kenshi/mods/2017?tab=description
  GitHub         : https://github.com/smokefoolius/Kenshi-Direct-Control


CONTROLS
--------
  V          Toggle Direct Control on / off
  W A S D    Move (camera-relative)
  P          Toggle first-person view (while Direct Control is on)
  SHIFT+C    Toggle sneak (while in first person)
  F          Hand control to the selected squad member
  CTRL       Hands-free camera-look toggle
  Dbl-click  Switch control via a squad portrait

  Single-click a portrait to select without taking control;
  F or a double-click hands WASD control to them.


FEATURES
--------
  Instant movement override
    WASD immediately cancels click-to-move destinations and
    overrides combat AI pathing. No startup delay, crisp stop on
    release, indoors and out.

  First-person view   (press P)
    While Direct Control is on, press P to drop into a true first-
    person camera at your character's eyes. Look around with the
    mouse and move with WASD; your own body and arms stay in view
    as you run, fight, block, and heal for a full first-person
    presence. The camera turns your body to face where you look,
    and the mouse frees itself automatically for the map, dialogue,
    inventory, and menus. Press P again to return to the normal
    view. Tune the feel in the config (mouse sensitivity and field
    of view).

  Sneak in first person   (SHIFT+C)
    Toggle sneaking without leaving the view - it presses the
    game's own SNEAK button, so the UI stays in sync and stealth
    skill, detection, and XP all work exactly as vanilla. Movement
    speed while sneaking matches the game's real stealth speed.

  A combat camera that stays clean
    The view keeps clear of the action: your own swings, blocks,
    and heals never clip through the lens, the camera eases back
    off the steps when climbing stairs, and an enemy pressing into
    your face can't slice the view open - while your arms and
    weapon stay visible throughout.

  Combat that gets out of your way
    Stand still and the AI fights on its own - block, dodge,
    attack, earning XP as normal. Hold WASD and it yields so
    movement stays smooth. A committed animation (your swing, a
    stagger, a parry) always finishes before you move - no sliding,
    nothing cut off. Hold WASD to reposition, release to let the
    AI fight.

  Move while you loot and trade   (optional)
    Turn the face-cam off (see CONFIG) to keep moving with WASD
    while an inventory or trade window is open, camera locked on
    your character. Walk away from a merchant and the trade closes
    on its own. Talking to NPCs still pauses as normal.

  Inventory face-cam   (on by default)
    Open a character's own inventory and the camera swings to face
    them so you can see equipped gear. Auto-disabled in combat so
    you can loot and disarm enemies freely.

  Get up and go
    Hold WASD while sitting, lying in a bed, or operating a
    workstation and your character stands up and walks off.

  Per-character control
    Direct Control follows your selected character; switch any time
    with F or a double-click.


WHAT'S NEW IN 1.3.1
-------------------
  - Fixed a crash when right-clicking while controlling a pack
    animal inside a hive home.
  - Move-while-looting mode: pausing the game while a loot or
    trade window is open now works properly.
  - First person: buildings now look solid from the outside.


WHAT'S NEW IN 1.3.0
-------------------
  - First-person view: press P while Direct Control is on for a
    true first-person camera with smooth mouse-look, WASD movement,
    and your own body and arms visible during movement, combat,
    and healing.
  - Sneak from first person with SHIFT+C - synced with the game's
    SNEAK button, full vanilla stealth skill and XP.
  - Combat camera polish: enemies pressing into you no longer clip
    through the view, and the camera stays clear of the steps when
    climbing stairs.
  - Multi-floor interiors render correctly in first person: the
    storeys below you are always solid when you look down a
    stairwell.
  - The cursor frees automatically for the map, dialogue,
    inventory, and menus while in first person, and recaptures on
    close.
  - First-person survives loading between areas and follows you
    when you switch which character has control.


CONFIG  (WASDCombatPlugin.ini in the mod folder)
------------------------------------------------
  [Keybinds]
    Rebind any key here. Valid names: letters (W), digits (5),
    F1..F24, SPACE, TAB, SHIFT, CONTROL, arrow keys, NUMPAD0..9,
    or OEM_1..8 for international layouts. Bad entries fall back
    to the default. Sneak is always SHIFT + the SneakToggle key.
    The DC Toggle can also be rebound in-game under
    Options > Controls (that binding takes priority).

  [Settings]
    InventoryFaceCam = true   (default)
      true  - camera swings to face your character on inventory
              so you can see worn gear.
      false - keep moving with WASD while looting/trading, camera
              locked on your character (walk away to close).

    WasdSpeedCap = true   (default)
      Caps WASD movement at your character's real top speed
      (injuries, encumbrance, shackles all count, as vanilla).
      Set to false to always move at full speed.

    WasdSpeedMult = 1.0
      Scales WASD movement speed (1.0 = exactly vanilla).

  [FirstPerson]
    Sensitivity      = 1.0   Mouse-look speed in first person.
    FOV              = 90    Field of view, in degrees (50-110).
    HideHair         = 1     Hide your own hair in first person.
    HideHead         = 1     Hide your own head in first person
                             (so it doesn't block the view).
    FloorRevealBelow = 1     Render the storeys below you inside
                             multi-floor buildings (0 = vanilla
                             reveal-on-approach).

    Every other camera value is tuned automatically; advanced
    tunables exist for fine-tuning and use safe defaults when not
    present in the file.


INSTALLATION
------------
  1. Install RE_Kenshi (required mod loader).
  2. Copy the "WASDCombatPlugin" folder into:  Kenshi/mods/
     so you have:  Kenshi/mods/WASDCombatPlugin/WASDCombatPlugin.dll
  3. Launch the game. RE_Kenshi loads the plugin automatically.


REQUIREMENTS
------------
  - Kenshi (Steam or GOG)
  - RE_Kenshi mod loader (must be installed and active)


COMPATIBILITY & NOTES
---------------------
  - Tested with RE_Kenshi 0.3.4 (Kenshi 1.0.65). No game data
    files are modified, so it is safe alongside large mod lists
    and most combat, AI, and faction mods.
  - Direct Control drives one character at a time - the selected
    one. Other squad members follow their normal orders.
  - You cannot attack while actively moving - stop, and the AI
    fights on its own.
  - During knockdown, stagger, or get-up animations the character
    cannot be moved; movement resumes when the animation completes.
  - Downed or crippled characters can still be moved using the
    game's crawl / limp system.
  - While manning a turret, aiming stays vanilla until you press
    WASD, which steps you off the turret.
  - First person: distant grass may shimmer slightly when you turn
    the view - an engine quirk that eye-level viewing makes more
    visible (it exists in the normal camera too).

CREDITS
-------
  The first-person grass streaming technique, which moves the
  camera center node only while the character moves, comes from
  KenshiFP.

================================================================
  Developed as "WASDCombatPlugin" using the RE_Kenshi SDK.
================================================================
