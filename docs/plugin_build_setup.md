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

4. Install [Git LFS](https://git-lfs.com/). The dependency repository in section 3 stores its libraries and its Boost archive in LFS.

## 3. Get the dependencies

`WASDCombatPlugin.vcxproj` reads KenshiLib, Ogre, MyGUI, and Boost 1.60 from a sibling folder, `..\KenshiLib_Examples_deps\`, next to this repository. The [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps) repository has exactly this layout.

The source includes `<kenshi/CombatClass.h>`. KenshiLib v0.3.4 and later moved that header to `kenshi/combat/CombatClass.h`, so the current `master` of KenshiLib_Examples_deps does not build this plugin. Commit `e75769b` ("Fixed missing boost environment variable") is the last commit that has the header at the old path. It contains KenshiLib v0.3.0.

1. In the folder that contains this repository, clone the dependencies and check out that commit. Do not download a ZIP, because a ZIP has no LFS files.

   ```
   git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps.git
   cd KenshiLib_Examples_deps
   git checkout e75769b
   git lfs pull
   ```

2. Run `Setup.bat` in `KenshiLib_Examples_deps\`. It extracts `boost_1_60_0\boost.zip` and sets user environment variables, among them `KENSHILIB_DIR`, which points to `KenshiLib_Examples_deps\KenshiLib`. It runs as administrator.
3. Check that the folders match this layout:

   ```
   <parent folder>\
     Kenshi-Direct-Control\           this repository
     KenshiLib_Examples_deps\
       KenshiLib\
         Include\                     kenshi\CombatClass.h is directly in kenshi\
         Libraries\
           KenshiLib.lib
           MyGUIEngine_x64.lib
           OgreMain_x64.lib
       boost_1_60_0\
         boost\                       the Boost headers
         stage\lib\                   the built Boost libraries
   ```

   Each `.lib` file must be larger than 1 KB. A file of about 130 bytes is an LFS pointer, which means that `git lfs pull` did not run.

The project searches `$(KENSHILIB_DIR)` before the sibling folder. If `KENSHILIB_DIR` points to a different KenshiLib, the build mixes headers from two versions. Keep it on the checkout from step 1, or delete the variable.

## 4. Build the plugin

1. Close Kenshi, if it runs.
2. Open the **Developer Command Prompt** of your Visual Studio version, and go to the repository root.
3. Run the build:

   ```
   msbuild WASDCombatPlugin.sln /p:Configuration=Release /p:Platform=x64
   ```

4. Check that `WASDCombatPlugin\WASDCombatPlugin.dll` exists. The project writes the DLL to this folder, not to `x64\Release\`, which holds only intermediate files.

`BuildStepCompat.targets` lets MSBuild build the `v100` project outside the Visual Studio IDE. The project imports it, so you do not need to do anything for it.

## 5. Install the build in Kenshi

This repository does not contain the complete mod folder. Install the released mod once, then replace its DLL with your build.

1. Download the release from [Nexus Mods](https://www.nexusmods.com/kenshi/mods/2017). Copy its `WASDCombatPlugin` folder into `Kenshi\mods\`.
2. If you subscribe to the mod on the Steam Workshop, unsubscribe. Then only your build loads, and a Workshop update does not replace your DLL.
3. Copy `WASDCombatPlugin\WASDCombatPlugin.dll` from the repository into `Kenshi\mods\WASDCombatPlugin\`, and replace the old file.
4. If you changed `WASDCombatPlugin.ini`, copy it too.

## 6. Test in game

1. Start Kenshi normally, and check the RE_Kenshi text on the main menu.
2. Load a save. Select a character, press `V`, and move with WASD.
3. If something fails, open **Options > Mods > RE_Kenshi Settings**, and read the debug log tab. It shows plugin load errors, and the plugin writes its own lines there with the prefix `[WASDCombat]`.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `MSB4019: The imported project "...Microsoft.Cpp.Default.props" was not found` | The Visual Studio install has no C++ workload. | Section 2, step 1 |
| `MSB8020: The build tools for v100 ... cannot be found` | The 2010 x64 compiler is missing. | Section 2, steps 2 and 3 |
| `C1083: Cannot open include file: 'kenshi/CombatClass.h'` | The KenshiLib headers are v0.3.4 or later. | Section 3, step 1, and the `KENSHILIB_DIR` note |
| `C1083: Cannot open include file` for another `kenshi/`, `ogre/`, `mygui/`, or `boost/` header | `KenshiLib_Examples_deps\` is missing or is not next to this repository, or Boost is not extracted. | Section 3 |
| `LNK1104: cannot open file 'libboost_...-vc100-mt-1_60.lib'` | `Setup.bat` did not extract `stage\lib\`. | Section 3, step 2 |
| `LNK1107: invalid or corrupt file` for a `.lib` file | The file is an LFS pointer. | Section 3, steps 1 and 3 |
| The main menu has no RE_Kenshi text | RE_Kenshi is not installed. | Section 1 |
| `V` does nothing in game | RE_Kenshi did not load the plugin. | Section 5, then the RE_Kenshi debug log |
| The game crashes when it loads the plugin | The DLL was built with a different compiler, or against a KenshiLib version that the game does not have. | Sections 2 and 3 |
