# Plugin Build Setup

This page sets up a Windows machine to build `WASDCombatPlugin.dll` and to test it in Kenshi. Do the sections in order.

The plugin shares C++ types with Kenshi, which was built with Visual C++ 2010. For this reason, the build needs the 2010 compiler and the exact library versions that KenshiLib uses. A newer compiler or a different library version gives a DLL that crashes the game.

## 1. Install RE_Kenshi

RE_Kenshi loads the plugin into the game. Its installer also installs `KenshiLib.dll`, so KenshiLib needs no separate install in the game.

1. Check that your Kenshi version is one that the current [RE_Kenshi release](https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases) supports. The release title names the versions, for example "For Kenshi 1.0.65 + 1.0.68 (Steam) and 1.0.65 + 1.0.68 (GOG)". The README states that upstream Direct Control was tested with RE_Kenshi 0.3.4 and Kenshi 1.0.65.
2. Download the standard archive, `RE_Kenshi_v<version>.zip`. Do not use the `_loose` archive.
3. Extract the complete archive, and run the `RE_Kenshi_v<version>.exe` installer in it.
4. Start Kenshi normally. The main menu must show `RE_Kenshi v<version> - Kenshi 1.0.<x> - x64 (Newland)`. If it does not, the install failed.

## 2. Install the compilers

1. Install Visual Studio 2019 or later with the **Desktop development with C++** workload. This gives the editor and MSBuild.
2. Install the Visual C++ 2010 x64 compiler, as the [KenshiLib README](https://github.com/BFrizzleFoShizzle/KenshiLib#compiling) documents:
   1. Get Visual Studio 2010 Professional, Premium, or Ultimate, and Visual Studio 2010 SP1, from the [Wayback Machine](https://archive.org/search?query=visual+studio+2010), where the KenshiLib README points. All three editions contain the same compiler.
   2. Install Visual Studio 2010. In the setup, keep Visual C++ selected, including its x64 compiler tools.
   3. Install Visual Studio 2010 SP1.

3. Check that the x64 compiler works and has SP1. Open a new Command Prompt, and run these two commands, one per line:

   ```
   "%VS100COMNTOOLS%..\..\VC\vcvarsall.bat" amd64
   cl
   ```

   The first line of the output must be `Microsoft (R) C/C++ Optimizing Compiler Version 16.00.40219.01 for x64`. `16.00` and `for x64` identify the 2010 x64 compiler, and `40219` shows that SP1 is installed. Visual Studio 2010 sets `VS100COMNTOOLS` to its own folder, so the commands work on any install drive. Close the prompt afterwards, because `vcvarsall.bat` changes its environment.

## 3. Fill deps/

`WASDCombatPlugin.vcxproj` reads KenshiLib and Boost from `deps\` at the repository root. Git ignores this folder. It holds exactly four folders: `Include\` and `Libraries\` from KenshiLib, and `boost\` and `lib64-msvc-10.0\` from Boost.

1. Choose the KenshiLib version that the installed RE_Kenshi ships. On the [KenshiLib releases page](https://github.com/BFrizzleFoShizzle/KenshiLib/releases), the notes of that release say so, for example "KenshiLib v0.5.0 - this is the version shipped in RE_Kenshi 0.3.5". The version must be v0.3.4 or later, because the source includes `<kenshi/combat/CombatClass.h>`, and earlier versions have that header at `kenshi/CombatClass.h`.
2. Download two archives of that release:

   | Archive | What you take from it |
   |---|---|
   | `https://github.com/BFrizzleFoShizzle/KenshiLib/archive/refs/tags/v<version>.zip`, the "Source code (zip)" link of the release | The `Include\` and `Libraries\` folders |
   | `KenshiLib_v<version>.zip`, a release asset | The `KenshiLib.lib` file |

   The tag archive matches `KenshiLib.lib` of the same release. **Code > Download ZIP** on the repository page gives the default branch instead, which can be ahead of the release.

3. From the source archive, copy the `Include\` and `Libraries\` folders into `deps\KenshiLib\`.
4. From the release asset, copy `KenshiLib.lib` into `deps\KenshiLib\Libraries\`.
5. Get Boost 1.60.0 built with the Visual C++ 2010 x64 compiler. boost.org offers only source archives, so use the prebuilt installer that the Boost project publishes on SourceForge:
   1. Download [`boost_1_60_0-msvc-10.0-64.exe`](https://sourceforge.net/projects/boost/files/boost-binaries/1.60.0/boost_1_60_0-msvc-10.0-64.exe/download) and run it.
   2. On the folder page of the installer, click **Browse** and select the `deps\` folder of the repository. The installer adds `boost_1_60_0\` to the selected folder itself, so the install folder becomes `deps\boost_1_60_0\`.
   3. In `deps\boost_1_60_0\`, keep the `boost\` and `lib64-msvc-10.0\` folders, and delete the other files and folders that the installer added.

6. Check that `deps\` matches this layout. `deps\KenshiLib\` and `deps\boost_1_60_0\` each contain their two folders and nothing else.

   ```
   deps\
     KenshiLib\
       Include\                   the complete folder from the source archive
       Libraries\                 the complete folder from the source archive, plus KenshiLib.lib
         KenshiLib.lib
         mygui\MyGUIEngine_x64.lib
         ogre\OgreMain_x64.lib
     boost_1_60_0\
       boost\                     the Boost headers
       lib64-msvc-10.0\           the built Boost libraries
   ```

The Ogre and MyGUI libraries in KenshiLib match the game, so the build uses those.

## 4. Build the plugin

1. In the repository root, run `package_release.cmd`, and select an option:

   | Option | Result |
   |---|---|
   | **1. Rebuild and repackage** | Builds **Release \| x64**, then makes the release zip |
   | **2. Rebuild only** | Builds **Release \| x64** |
   | **3. Repackage** | Makes the release zip from the last build |

   The script finds MSBuild with `vswhere`. Packaging runs `scripts\package_release.ps1` in Windows PowerShell, which every Windows version has.

   To build by hand instead, open the **Developer Command Prompt** of your Visual Studio version, go to the repository root, and run:

   ```
   msbuild WASDCombatPlugin.sln /p:Configuration=Release /p:Platform=x64
   ```

2. Check that `dist\WASDCombatPlugin-<version>.zip` exists. The version comes from the `version=` line in `mod\mod.info`. Packaging stops if the DLL was not built with the Visual C++ 2010 toolset.

| Path | Contents |
|---|---|
| `mod\` | The mod files that the build does not make: `RE_Kenshi.json`, which tells RE_Kenshi to load the DLL, `WASDCombatPlugin.mod`, an empty Kenshi data file that puts the mod in the launcher mod list, and `mod.info`, which holds the release version |
| `dist\bin\Release\` | The linker output: the DLL, and the `.pdb` file for crash debugging |
| `dist\obj\` | The intermediate files |
| `dist\stage\WASDCombatPlugin\` | The mod folder that the zip contains |
| `dist\WASDCombatPlugin-<version>.zip` | The release zip |

Git ignores `dist\`. The zip does not contain `WASDCombatPlugin.ini`. The plugin writes a default INI when none exists, so a shipped INI would only reset the settings of each player who updates.

`BuildStepCompat.targets` lets MSBuild build the `v100` project outside the Visual Studio IDE. The project imports it, so you do not need to do anything for it.

## 5. Install the build in Kenshi

1. If you subscribe to the mod on the Steam Workshop, unsubscribe. Then only your build loads.
2. Extract `dist\WASDCombatPlugin-<version>.zip` into `Kenshi\mods\`, and replace the files that are there. The result is `Kenshi\mods\WASDCombatPlugin\WASDCombatPlugin.dll`.
3. In the Kenshi launcher, open the mod list and enable **WASDCombatPlugin**.

## 6. Test in game

1. Start Kenshi normally, and check the RE_Kenshi text on the main menu.
2. Load a save. Select a character, press `V`, and move with WASD.
3. If something fails, open **Options > Mods > RE_Kenshi Settings**, and read the debug log tab. It shows plugin load errors, and the plugin writes its own lines there with the prefix `[WASDCombat]`.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `MSB4019: The imported project "...Microsoft.Cpp.Default.props" was not found` | The Visual Studio install has no C++ workload. | Section 2, step 1 |
| `MSB8020: The build tools for v100 ... cannot be found` | The 2010 x64 compiler is missing. | Section 2, steps 2 and 3 |
| `C1083: Cannot open include file: 'kenshi/combat/CombatClass.h'` | The KenshiLib headers are older than v0.3.4. | Section 3, step 1 |
| `C1083: Cannot open include file` for another `kenshi/`, `ogre/`, `mygui/`, or `boost/` header, or for `Debug.h` | `deps\` is incomplete. | Section 3 |
| `LNK1104: cannot open file 'KenshiLib.lib'` or `'libboost_...-vc100-mt-1_60.lib'` | A library is not where the project looks. | Section 3, steps 4 to 6 |
| The main menu has no RE_Kenshi text | RE_Kenshi is not installed. | Section 1 |
| `V` does nothing in game | RE_Kenshi did not load the plugin. | Section 5, then the RE_Kenshi debug log |
| The game crashes when it loads the plugin | The DLL was built with a different compiler, or against a KenshiLib version that the game does not have. | Sections 2 and 3 |
