# Direct Combat fork plan

## Goal

Fork Direct Control (GPLv3) into a new repository and add an optional mode, Direct Combat, that gives the player the manual-combat features of Manual Combat. We write all new code ourselves.

Direct Combat is a branch of Direct Control. It uses the same activation tree as Direct Control, and it adds two things: it takes over the combat logic, and it turns on block-only mode for the controlled character. When Direct Combat is not active, Direct Control behaves exactly as upstream Direct Control.

Direct Combat adds:

- The combat rules in [Direct Combat mode](#direct-combat-mode).
- Keys 1, 2, and 3 switch between weapons and unarmed. The step 1 spec confirms the slot mapping.
- A third-person shoulder camera. The mouse turns the camera and the character's facing.

Direct Combat is for melee and unarmed fighting only. With a crossbow drawn, the vanilla ranged AI fights, as in Direct Control (see [Rules](#rules)).

Direct Control has no gameplay over-the-shoulder camera. It built one and scrapped it: the comment above `s_fpActive` says that Kenshi's interior floor render is tied to the top-down RTS camera, so a detached camera does not show interior floors. In Direct Control, the camera is the RTS camera that follows the character, with an optional mouse-look toggle (`Ctrl`) and a first-person view (`P`). The detached camera survives only for the inventory face-cam and for first-person. So Direct Combat must build its own shoulder camera (step 8).

## Direct Combat mode

### Activation

`V` stays the only key that turns WASD control on and off. A second key, the Direct Combat key, turns a Direct Combat setting on and off. The Direct Combat key never turns WASD control on by itself.

- The player presses `V` while the setting is on: Direct Control starts with the Direct Combat branch active.
- The player presses `V` while the setting is off: Direct Control starts as upstream.
- The player presses the Direct Combat key while Direct Control is off: only the setting changes. Nothing else happens until the player presses `V`.
- The player presses the Direct Combat key while Direct Control is on: the setting changes, and the branch turns on or off at once. WASD control does not stop and start again, so the camera and the controlled character stay as they are.

Example:

1. The player presses the Direct Combat key once. The setting is on.
2. The player presses `V`. Direct Control starts with Direct Combat: block mode is on, and a left click attacks.
3. The player presses `V`. WASD control stops, and block mode goes back to its old setting.
4. The player presses `V` again. Direct Combat starts again, because the setting is still on.

Direct Combat is active only while both are true: Direct Control is on (`s_mode == MODE_FREE_MOVE`), and the setting is on. So each path in Direct Control's activation tree applies to Direct Combat without change: anchor tracking, the control switch, loot suspension, and load handling. Direct Combat only adds a branch where the combat logic and block mode differ.

The setting is separate from Direct Control's intent flag `s_userWantsDC`. A true game load or a new game turns Direct Control off, as upstream does, but the setting stays on. The next `V` press starts Direct Combat again.

The setting is saved in the INI file, so it also survives a game restart:

- Add `DirectCombat = false` to the `[Settings]` section of the default INI template that Direct Control writes when the file is missing.
- Read it at startup with `GetPrivateProfileStringA`, as Direct Control reads its other `[Settings]` values. An older INI file without the key gives `false`.
- Write it on each Direct Combat key press with `WritePrivateProfileStringA`, as Direct Control writes its `[NativeBinds]` section.

Each Direct Combat key press shows the new setting in the game's own message box with `GameWorld::showPlayerAMessage` (for example, "Direct Combat On" and "Direct Combat Off"). Direct Control already uses this call for "Direct Control Enabled" and "Direct Control Disabled". The message shows also while Direct Control is off, so the player always sees the setting change. Check in game that this is the same box as the "Discovered town" message. If it is not, find the call that the town discovery uses (start from `Town::setDiscovered`).

The press handler ignores the Direct Combat key while `s_lootUiSuspendActive` is set, as `handleTogglePress` does for `V`. Direct Combat also takes no left-click or weapon-key input during loot suspension.

The default key is `/`. In the INI file it is `OEM_2`, which is the `/` key on a US layout; on other layouts `OEM_2` can be a different key. Check that Kenshi does not bind `/` by default.

Register the key as a native command in the game Controls menu, the same way Direct Control registers `dc_toggle` and `dc_speed_cycle` (the `InputHandler::loadConfig`, `OptionsWindow::create`, and `OptionsWindow::saveOptions` hooks). Add an INI fallback in `[Keybinds]`, as Direct Control does for its own keys.

### Block mode

Kenshi has a block-mode toggle in the orders panel. In block mode, the character only blocks and never attacks, and it gets a bonus to melee defence. KenshiLib exposes it in three places:

| Part | KenshiLib member |
|---|---|
| Button | `OrdersPanel::blockmodeButton(MyGUI::Widget*)`, `OrdersPanel::blocksCheckbox` |
| State | `CharStats::_defensiveMode`, `CharStats::isDefensiveMode()` |
| Defence bonus | `CharStats::getMeleeDefence(bool includeDefensiveMode)`, `CharStats::getMeleeDefence_melee(bool includeDefensiveMode)` |

Turn on block mode through the game's own button handler, `OrdersPanel::blockmodeButton`. Direct Control uses the same method for sneak with `OrdersPanel::toggleStealth`, so that the checkbox, the stored order, and the character state stay in sync. The handler toggles, so call it only when `isDefensiveMode()` is false. Set `CharStats::_defensiveMode` directly only when the orders panel does not show the controlled character.

### Rules

These rules apply to the controlled character while Direct Combat is on:

| Event | Rule |
|---|---|
| Direct Combat becomes active (`V` while the setting is on, or the Direct Combat key while Direct Control is on) | The plugin records the block-mode setting and turns on block mode. |
| Not attacking | Block mode stays on. Vanilla block mode stops the AI from attacking and gives the defence bonus. |
| Moving with WASD | Block mode stays on, but the character does not block hits, because Direct Control skips the combat AI during movement. This is the trade-off for moving: more hits land, but the character can dodge by moving. |
| Left click | One attack starts. The block-mode checkbox stays on. The attack is committed: it always plays to the end, and no input cancels it. WASD movement and camera movement are disabled until it ends. |
| Attack ends | The character is in vanilla block mode again. |
| Control moves to another character | The old character gets its recorded setting back. The new character gets block mode, as when Direct Combat becomes active. |
| Direct Combat stops being active (`V`, or the Direct Combat key while Direct Control is on) | The character gets its recorded setting back. Upstream Direct Control behaviour returns, or WASD control stops. |
| Crossbow drawn | The plugin turns block mode off with the button handler, because block mode stops the ranged AI from shooting. A left click does nothing; the vanilla ranged AI fights. |
| Melee weapon or unarmed again | The plugin turns block mode on again. |
| True game load or new game | The world is gone, so the plugin cannot restore the setting. It only clears its state. |

The player never turns block mode off to attack, so there is no toggle spam.

Block mode gives a player-controlled character a defensive edge over AI characters. This is intentional.

### Attack past block mode

Vanilla block mode stops all attacks, also the player's. The manual attack must get one swing past it.

Decision: clear `CharStats::_defensiveMode` directly, not through the button, for the length of the swing, then set it again. The checkbox does not change. The defence bonus is lost during the swing, which is acceptable: the character is open while it swings.

Rejected: find the check in the combat code that stops attacks in block mode, and let one swing through it. This keeps the bonus during the swing, but it needs more reverse engineering, and a hook cannot catch the check if the game reads `_defensiveMode` inline.

### Edge cases

- Block mode may be saved with the character (check in game). If it is, and the player saves while Direct Combat is on, the save stores block mode on. After that save loads, block mode stays on until the player turns it off. The plugin accepts this and does not try to fix it.
- Keys 1, 2, and 3 switch weapons only while Direct Combat is on. Check whether Kenshi binds these keys by default. If it does, Direct Combat must take them only while it is on, and give them back when it turns off.
- A left click during an attack does nothing. The plugin does not queue a next attack.
- A left click with no enemy in reach does nothing.
- Keys that are held when the attack ends take effect at once. For example, if W is held, the character starts to move. Presses during the attack are not queued.
- A left click attacks only when no GUI widget has mouse focus. Clicks on the HUD, portraits, and windows keep their vanilla behaviour.
- First-person works with Direct Combat. `P` switches between the shoulder camera and first-person. All Direct Combat rules apply in both views. In both views the aim cone is in front of the character, because Direct Control's first-person turns the body to face where the player looks.

### Changes to Direct Control behaviour

Each change below applies only while Direct Combat is on. While it is off, the upstream code path runs unchanged.

- No change: `combatGo_hook` skips the combat AI (`CombatClass::_NV_go`) while WASD is held. The block decision runs inside the combat AI, so a moving character does not block. This is the intended trade-off in [Rules](#rules).
- `isCommittedAction` counts the `BLOCK` and `REACTION_BLOCK` combat states as committed actions. Direct Control uses it to hold movement at the WASD instant-stop and after WASD release. In block mode, the combat state is often `BLOCK`, so movement can stay held too long. Remove `BLOCK` from this set.
- `isCommittedCombatClip` includes `REACTION_BLOCK` (a parry), so movement waits for each parry clip to end. In block mode, parries happen often. Kenshi cannot abort a clip, so keep this rule, but check in game that movement still feels responsive.
- Direct Control uses a left double-click on a squad portrait to switch control. The GUI-focus rule in [Edge cases](#edge-cases) keeps this working.

## Code layout

Keep `WASDCombatPlugin.cpp` as upstream has it. Put Direct Combat in its own source files. In the Direct Control hooks, add only short calls into Direct Combat, each behind the Direct Combat on/off flag. This keeps each merge of a future Direct Control update small.

## Rules for each source

| Source | License | What we can use |
|---|---|---|
| Direct Control, [smokefoolius/Kenshi-Direct-Control](https://github.com/smokefoolius/Kenshi-Direct-Control), commit `56bab6b` | GPLv3 | All of it. This is the fork base. |
| KenshiFP, [linguine2552/KenshiFP](https://github.com/linguine2552/KenshiFP), commit `200102f` | GPLv3 | Its reverse-engineering notes (`re/NOTES.md`, `re/rva_1065.md`, `re/scripts/`), and its code with attribution. |
| Manual Combat, Steam Workshop item 3772183372 | None published | Its import table, export table, strings, and its behaviour in game. |

Do not use from Manual Combat:

- Decompiled or disassembled function bodies.
- Its control flow, its tuning constants, or its raw addresses.

If a feature cannot be built from imports, strings, and observed behaviour, derive it from KenshiLib headers, Direct Control, KenshiFP, or our own reverse engineering of `Kenshi_x64.exe`.

GPLv3 obligations for the fork:

- Keep the `LICENSE` file and the copyright notices of Direct Control.
- Put a clear notice in the fork that states we modified Direct Control, with a relevant date (GPLv3 section 5a). A changelog in the README does this.
- Publish the full source for every binary that we release, and link to it from the Workshop page.
- Credit Direct Control and KenshiFP in the README.

## Direct Control baseline

All of these facts come from `WASDCombatPlugin.cpp` at commit `56bab6b`.

The plugin is one file of 7,470 lines. The README says v1.3.1, but the startup log line in `startPlugin` says v1.8.4. So the repository code is newer than the README. The same log line mentions an "OTS action camera", but that text is out of date: the comments above `s_fpActive` and `enterOTS` say that the gameplay OTS camera is scrapped.

`startPlugin` installs 11 hooks. All of them use `KenshiLib::GetRealAddress`, so none of them depend on raw addresses:

| Hook | Purpose in Direct Control |
|---|---|
| `InputHandler::loadConfig` | Registers native keybinds in the game Controls menu. |
| `OptionsWindow::create` | Adds the keybind rows to the Options window. |
| `OptionsWindow::saveOptions` | Saves rebinds across restarts. |
| `GameWorld::processKeys` | Receives the toggle and speed commands from the native keybind system. |
| `GameWorld::_NV_mainLoop_GPUSensitiveStuff` | Main-thread work: selection, mode changes, WASD before and after the AI update, job suppression. |
| `CharMovement::_NV_update` | Applies WASD movement and holds it during committed combat animations. |
| `CameraClass::update` | Drives the first-person camera and the inventory face-cam. |
| `CameraClass::restrictPosition` | Runs the game's interior floor refresh, then puts back the detached camera pose that the game's clamp moved. |
| `PlayerInterface::playerControl` | Gates player orders while Direct Control is on. |
| `Character::removeJob`, `Character::addJob`, `Character::addOrder` | Block AI and squad jobs that fight the WASD movement. |
| `CombatClass::_NV_go` | The "passive-combat model": skips the combat AI while WASD is held, except during a committed combat animation. |

Direct Control does not:

- Let the player start an attack. When the player stands still, the AI fights. When the player moves, the AI stands down.
- Switch weapons from the keyboard.

One possible cause of jank is already visible in Direct Control. `PollThread` reads keys with `GetAsyncKeyState` after `Sleep(50)`. So input is sampled at 20 Hz, which can add up to 50 ms of lag. A left click must start the attack without this delay.

## Candidate KenshiLib API

These come from our `deps/KenshiLib` headers. Direct Control builds against a different KenshiLib revision: it includes `<kenshi/CombatClass.h>`, but our copy has `kenshi/combat/CombatClass.h`. So check each signature against the revision that the fork uses.

| Need | Method |
|---|---|
| Start an attack on a target | `Character::attackTarget(Character* who)` |
| Read the current target | `Character::getAttackTarget()` |
| Read weapons | `Character::getCurrentWeapon()`, `Character::getThePreferredWeapon()`, `Character::getRangedWeapon()` |
| Switch weapons | `Character::drawWeapon(Item*, std::string)`, `Character::sheatheWeapon()`, `CharacterHuman::equipItem(section, item)` |
| Enter combat mode | `CombatClass::initCombatMode(const hand& subject, int end, bool focusedTarget)` |
| Per-frame combat decision | `CombatClass::_NV_go(float)` (already hooked by Direct Control) |
| Read the combat state | `CombatClass::getCombatState()`, `CombatClass::getBlockStateEnum()`, member `CombatClass::combatModeActive` |
| Block mode | `OrdersPanel::blockmodeButton(MyGUI::Widget*)`, `CharStats::isDefensiveMode()`, member `CharStats::_defensiveMode` |
| Crossbow drawn | `Character::getRangedWeapon()`, `Character::shouldUseRangedWeapons()`, member `CharStats::rangedMode`. Confirm in game which one tracks a drawn crossbow. |
| Attack states | `CombatClass::attackState()`, `CombatClass::setCombatState(swordStateEnum)`, `CombatClass::changeState(swordStateEnum, float minTime)` |
| Target selection | `CombatClass::isInAttackZone(Character*)`, `CombatClass::calculateTargetsInAttackZone()` |
| Current technique | `CombatClass::getCurrentTechnique()` |

`attackState`, `setCombatState`, `changeState`, `isInAttackZone`, and `calculateTargetsInAttackZone` are `protected`. Outside the class, C++ does not let us take their address. Use an access shim such as `struct CombatAccess : CombatClass { using CombatClass::attackState; };` and pass `&CombatAccess::attackState` to `GetRealAddress`. Do not fall back to raw addresses. Direct Control's comment above `verifyPatchSiteBytes` records two crashes that stale raw addresses caused after an RE_Kenshi update.

## Threading rule

Change game state only on the main thread, inside the `mainLoop` hook or another game hook. The poll thread only reads input and writes flags that the main thread consumes. Direct Control already does this for some inputs (for example, `s_lmbDoubleClickMs` is "set by poll, consumed by main"). When we touch a module, check it for game-state writes from the poll thread.

## Steps

1. **Mine the Manual Combat binary.**
   - You copy the full Workshop folder `steamapps/workshop/content/233860/3772183372/` to `/workspace/temp/manual_combat/`.
   - I dump the import and export tables with `objdump -p`, and the ASCII and UTF-16 strings with `strings -a` and `strings -el`.
   - MSVC-mangled names do not demangle with `c++filt`. I decode them by hand, or you run `undname.exe` from Visual Studio.
   - Output: `temp/manual_combat_spec.md`. It lists the features, keybinds, config keys, the KenshiLib methods that Manual Combat calls or hooks, and the methods it shares with Direct Control.
   - Verify: each line in the spec names the import or string that it comes from.

2. **List the jank.** You play Manual Combat and Direct Control, and you list each concrete fault (for example, "the camera jitters when I sprint downhill"). Each fault becomes an acceptance check for the fork.

3. **Set up the repository.**
   - Fork `smokefoolius/Kenshi-Direct-Control` into your GitHub account. A fork keeps the history and the attribution.
   - Build with VS2010 (`v100`), KenshiLib, and Boost 1.60, as `WASDCombatPlugin.vcxproj` expects. Find which KenshiLib revision gives `<kenshi/CombatClass.h>`.
   - Verify: the unchanged fork builds, loads in game, and behaves the same as the Workshop release of Direct Control.

4. **Direct Combat activation.** This proves the build, the new source files, and the keybind setup before any combat change.
   - Add the Direct Combat setting, the press handler, the native command, the INI fallback, the INI save, and the message as [Activation](#activation) describes.
   - Verify in game: the key shows in the Controls menu, and a rebind persists after a restart. Each case in [Activation](#activation) gives the stated result, and a log line shows each change. A switch while Direct Control is on does not move the camera. Each press shows the message, also while Direct Control is off. After a true load, Direct Control is off and the setting is still on. After a game restart, the setting is the same as before. An INI file without the `DirectCombat` key loads with the setting off. With the setting off, Direct Control behaves the same as the step 3 build.

5. **Block mode.**
   - Apply the block-mode rules in [Rules](#rules) each time Direct Combat becomes active or stops being active (see [Activation](#activation)), and at a control switch.
   - Remove `BLOCK` from the `isCommittedAction` set while Direct Combat is on.
   - Verify in game: the block-mode checkbox turns on with Direct Combat. In a fight, the character never attacks by itself. It blocks while it stands, does not block while it moves, and blocks again when it stops. A log line shows `getMeleeDefence(true)` above `getMeleeDefence(false)`. The recorded setting comes back when Direct Combat stops being active and when control moves. Check whether a save made during Direct Combat stores block mode on (see [Edge cases](#edge-cases)).

6. **Weapon switching on keys 1, 2, and 3.**
   - The slot mapping comes from the step 1 spec.
   - The poll thread records the key press only while Direct Combat is on. The main thread calls the KenshiLib weapon methods.
   - Verify in game: each key draws the correct weapon in and out of combat. Drawing a crossbow turns block mode off, and drawing a melee weapon or going unarmed turns it on again. With Direct Combat off, the keys keep their vanilla behaviour. The inventory stays correct after a save and load.

7. **Committed attack on left click.**
   - The main thread reads the left button every frame in the `mainLoop` hook and starts the attack. The 50 ms poll thread is too slow for this.
   - Pick the nearest enemy inside a cone in front of the character and inside weapon reach. Start the attack with `Character::attackTarget`.
   - Clear `_defensiveMode` for the length of the swing, as [Attack past block mode](#attack-past-block-mode) decides.
   - From the click until the combat state leaves the attack states (`STARTUP_STATE`, `CHOP_WEAPON`), ignore WASD and freeze the camera and the character's facing. Confirm in game which state marks the end of the swing recovery.
   - When the attack ends, make sure block mode is on again.
   - Verify in game: one click gives one full swing. The block-mode checkbox does not change. WASD, mouse movement, and more clicks during the swing do nothing. The swing hits the aimed enemy when several enemies are near. Combat XP still increases. The click reaches the game within one frame, not after the 50 ms poll delay. All of this works in the shoulder camera and in first-person. With Direct Combat off, a left click behaves as in upstream Direct Control.

8. **Shoulder camera.** Direct Control has no gameplay shoulder camera (see [Goal](#goal)).
   - The camera is on while Direct Combat is active. The mouse turns the camera and the character's facing.
   - Direct Control scrapped its shoulder camera because interior floors did not render with a detached camera. Start from the first-person floor fix: `restrictPos_hook` runs the game's floor refresh, then puts back the detached camera pose. Also read the KenshiFP camera research in `re/NOTES.md` and `DESIGN.md` (for example, the `inBuilding` and `currentFloor` camera fields).
   - `P` switches between the shoulder camera and first-person while Direct Combat is active (see [Edge cases](#edge-cases)).
   - Verify in game: the camera follows the character outdoors and inside multi-storey buildings, and each floor renders. The camera and the facing freeze during an attack. `P` goes to first-person and back to the shoulder camera.

9. **Release.** Complete the GPLv3 obligations in [Rules for each source](#rules-for-each-source). Then publish on Steam Workshop and Nexus with a link to the source.

## Open decisions

- **Repository name.** You own the fork.
