# Direct Control for Kenshi

**Full WASD movement for your selected character — with an optional true first-person view.**

Press `V`, move with WASD, and your character responds instantly — overriding click-to-move, combat AI pathing, and squad orders. Press `P` and drop into your character's eyes with mouse-look, your own body and arms visible as you move, fight, sneak, and heal.

Built for precise, hands-on control during combat, retreats, ambushes, city navigation, and immersive exploration — without losing any of Kenshi's charm.

**Current version: v1.3.1**

## Downloads

| | |
|---|---|
| Steam Workshop | https://steamcommunity.com/sharedfiles/filedetails/?id=3737240806 |
| Nexus Mods | https://www.nexusmods.com/kenshi/mods/2017 |

## Controls

| Key | Action |
|---|---|
| `V` | Toggle Direct Control on / off |
| `W` `A` `S` `D` | Move (camera-relative) |
| `P` | Toggle first-person view (while Direct Control is on) |
| `Shift`+`C` | Toggle sneak (while in first person) |
| `F` | Hand control to the selected squad member |
| `Ctrl` | Hands-free camera-look toggle |
| Double-click portrait | Switch control via a squad portrait |

Single-click a portrait to select without taking control; `F` or a double-click hands WASD control to them. All keys are rebindable — see [Configuration](#configuration).

## Features

- **Instant movement override** — WASD immediately cancels click-to-move destinations and overrides combat AI pathing. No startup delay, crisp stop on release, indoors and out.
- **First-person view** (`P`) — a true first-person camera at your character's eyes. Look around with the mouse, move with WASD; your own body and arms stay in view as you run, fight, block, and heal. The camera turns your body to face where you look, and the mouse frees itself automatically for the map, dialogue, inventory, and menus.
- **Sneak in first person** (`Shift`+`C`) — presses the game's own SNEAK button, so the UI stays in sync and stealth skill, detection, and XP all work exactly as vanilla.
- **A combat camera that stays clean** — your own swings, blocks, and heals never clip through the lens; the camera eases back off the steps when climbing stairs; an enemy pressing into your face can't slice the view open.
- **Combat that gets out of your way** — stand still and the AI fights on its own (block, dodge, attack, earning XP as normal). Hold WASD and it yields so movement stays smooth. A committed animation (your swing, a stagger, a parry) always finishes before you move.
- **Move while you loot and trade** (optional) — turn the face-cam off to keep moving with WASD while an inventory or trade window is open. Walk away from a merchant and the trade closes on its own.
- **Inventory face-cam** (on by default) — open a character's own inventory and the camera swings to face them so you can see equipped gear. Auto-disabled in combat so you can loot and disarm enemies freely.
- **Get up and go** — hold WASD while sitting, lying in a bed, or operating a workstation and your character stands up and walks off.
- **Per-character control** — Direct Control follows your selected character; switch any time with `F` or a double-click.

## Installation

1. Install [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases) (required mod loader).
2. Copy the `WASDCombatPlugin` folder from the release zip into `Kenshi/mods/` so you have:
   `Kenshi/mods/WASDCombatPlugin/WASDCombatPlugin.dll`
3. Launch the game. RE_Kenshi loads the plugin automatically.

Or subscribe on the Steam Workshop and skip the manual copy.

### Requirements

- Kenshi (Steam or GOG)
- RE_Kenshi mod loader (must be installed and active)

## Configuration

Edit `WASDCombatPlugin.ini` in the mod folder:

- **`[Keybinds]`** — rebind any key. Valid names: letters (`W`), digits (`5`), `F1`..`F24`, `SPACE`, `TAB`, `SHIFT`, `CONTROL`, arrow keys, `NUMPAD0`..`9`, or `OEM_1`..`8` for international layouts. Sneak is always `Shift` + the SneakToggle key. The DC Toggle can also be rebound in-game under Options → Controls (that binding takes priority).
- **`[Settings]`**
  - `InventoryFaceCam = true` — camera faces your character on inventory so you can see worn gear. Set `false` to keep moving with WASD while looting/trading instead.
  - `WasdSpeedCap = true` — caps WASD movement at your character's real top speed (injuries, encumbrance, shackles all count, as vanilla). Set `false` to always move at full speed.
  - `WasdSpeedMult = 1.0` — scales WASD movement speed.
- **`[FirstPerson]`**
  - `Sensitivity = 1.0` — mouse-look speed.
  - `FOV = 90` — field of view in degrees (50–110).
  - `HideHair = 1` / `HideHead = 1` — hide your own hair/head so they don't block the view.
  - `FloorRevealBelow = 1` — render the storeys below you inside multi-floor buildings (`0` = vanilla reveal-on-approach).

## Compatibility & Notes

- Tested with RE_Kenshi 0.3.4 (Kenshi 1.0.65). No game data files are modified, so it is safe alongside large mod lists and most combat, AI, and faction mods.
- Direct Control drives one character at a time — the selected one. Other squad members follow their normal orders.
- You cannot attack while actively moving — stop, and the AI fights on its own.
- During knockdown, stagger, or get-up animations the character cannot be moved; movement resumes when the animation completes.
- Downed or crippled characters can still be moved using the game's crawl / limp system.
- While manning a turret, aiming stays vanilla until you press WASD, which steps you off the turret.
- First person: distant grass may shimmer slightly when you turn the view — an engine quirk that eye-level viewing makes more visible (it exists in the normal camera too).

## Changelog

### v1.3.1
- Fixed a crash when right-clicking while controlling a pack animal inside a hive home.
- Move-while-looting mode: pausing the game while a loot or trade window is open now works properly.
- First person: buildings now look solid from the outside.

### v1.3.0
- First-person view: press `P` while Direct Control is on for a true first-person camera with smooth mouse-look, WASD movement, and your own body and arms visible during movement, combat, and healing.
- Sneak from first person with `Shift`+`C` — synced with the game's SNEAK button, full vanilla stealth skill and XP.
- Combat camera polish: enemies pressing into you no longer clip through the view, and the camera stays clear of the steps when climbing stairs.
- Multi-floor interiors render correctly in first person: the storeys below you are always solid when you look down a stairwell.
- The cursor frees automatically for the map, dialogue, inventory, and menus while in first person, and recaptures on close.
- First-person survives loading between areas and follows you when you switch which character has control.

### v1.2.2
- `F` hands control to the selected squad member (rebindable).
- `InventoryFaceCam = false` move-through mode: keep moving with WASD while an inventory or trade window is open.
- Keybind and settings INI added.

### Earlier versions
See the Nexus Mods changelog for the full history.

## Building from Source

The plugin is one C++ translation unit (`src/WASDCombatPlugin.cpp` and the `src/*.inl` files it includes) built as an x64 DLL against the RE_Kenshi plugin API.

1. Open `WASDCombatPlugin.sln` in Visual Studio (Desktop development with C++ workload).
2. The project expects [KenshiLib](https://github.com/KenshiReclaimer/KenshiLib/) headers and libraries — either set the `KENSHILIB_DIR` environment variable, or place the [KenshiLib example dependencies](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps) (KenshiLib, Ogre, MyGUI, Boost 1.60) in a sibling `KenshiLib_Examples_deps` folder as referenced by the `.vcxproj`.
3. Build **Release | x64**. The output `WASDCombatPlugin.dll` goes in `Kenshi/mods/WASDCombatPlugin/` alongside the `.ini`.

## License

[GPL-3.0](LICENSE)

---

*Developed as "WASDCombatPlugin" using the RE_Kenshi SDK.*
