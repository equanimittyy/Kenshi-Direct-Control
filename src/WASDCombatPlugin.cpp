#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SaveManager.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/InputHandler.h>
#include <kenshi/Character.h>
#include <kenshi/CharBody.h>
#include <kenshi/Tasker.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharStats.h>
#include <kenshi/combat/CombatClass.h>
#include <kenshi/CameraClass.h>
#include <kenshi/Globals.h>
#include <kenshi/OptionsHolder.h>
#include <kenshi/Enums.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MainBarGUI.h>
#include <kenshi/gui/OrdersPanel.h>
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreEntity.h>
#include <ogre/OgreOldSkeletonInstance.h>
#include <ogre/OgreOldBone.h>
#include <kenshi/Appearance.h>
#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <mygui/MyGUI.h>

// Declared by hand because <kenshi/gui/ManagementScreen.h> does not compile (broken
// ReorderableList template). Signatures must match that header exactly or the link fails.
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();
    bool getVisible();
};

#define DIAG_VERBOSE 0

#define RETREAT_VERBOSE_DIAG 0

#define LOOT_DIAG 0

// The .inl files are fragments of this one translation unit, not headers: they share
// file-static state and must stay in this order.
#include "config.inl"
#include "state.inl"
#include "camera_state.inl"
#include "ini_config.inl"
#include "input.inl"
#include "session_state.inl"
#include "movement.inl"
#include "mouse_input.inl"
#include "camera.inl"
#include "camera_hooks.inl"
#include "movement_hooks.inl"
#include "main_loop.inl"
#include "game_hooks.inl"
#include "native_keybinds.inl"

// Required gate for any hook installed by raw RVA.  GetRealAddress hooks are
// symbol-based and survive exe changes; raw RVAs do not.  RE_Kenshi regenerates
// its patched exe on its own updates and shifts all code, so a stale RVA patches
// the middle of an unrelated instruction while MinHook still reports SUCCESS.
// Record `expected` from the same exe the RVA came from.  On a mismatch the hook
// is skipped, so the feature degrades without corruption.
static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected,
                                 size_t len, const char* name)
{
    if (len > 16) len = 16;
    if (memcmp((const void*)addr, expected, len) == 0)
        return true;
    char hex[3 * 16 + 1] = { 0 };
    for (size_t i = 0; i < len; ++i)
        sprintf_s(hex + 3 * i, sizeof(hex) - 3 * i, "%02X ",
                  ((const unsigned char*)addr)[i]);
    char buf[224];
    sprintf_s(buf, sizeof(buf),
        "WASDCombatPlugin: %s RVA hook SKIPPED — patch-site bytes changed (exe updated?), got: %s",
        name, hex);
    ErrorLog(buf);
    return false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        s_thisModule = hModule;   // for the keybind INI path (DLL directory)
    if (reason == DLL_PROCESS_DETACH)
        DebugLog("WASDCombatPlugin: unloaded");
    return TRUE;
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("WASDCombatPlugin v1.8.4 — fix post-KO movement (combat-anim buffer gated on actual combat mode); OTS action camera");

    // Loaded unconditionally so the poll thread works if native command
    // registration never happens (load order or hook failure).
    loadKeybinds();

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&InputHandler::loadConfig),
            &inputLoadConfig_hook, &s_inputLoadConfigOrig))
        ErrorLog("WASDCombatPlugin: InputHandler::loadConfig hook FAILED — using INI keybind fallback");
    else
        DebugLog("WASDCombatPlugin: InputHandler::loadConfig hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::create),
            &optionsCreate_hook, &s_optionsCreateOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::create hook FAILED — Controls-menu rows unavailable");
    else
        DebugLog("WASDCombatPlugin: OptionsWindow::create hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::saveOptions),
            &optionsSave_hook, &s_optionsSaveOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::saveOptions hook FAILED — rebinds will not persist across restarts");
    else
        DebugLog("WASDCombatPlugin: saveOptions hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::processKeys),
            &processKeys_hook, &s_processKeysOrig))
        ErrorLog("WASDCombatPlugin: GameWorld::processKeys hook FAILED — using INI keybind fallback");
    else
    {
        s_processKeysHookOk = true;
        DebugLog("WASDCombatPlugin: processKeys hook OK");
    }

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &s_mainLoopOrig))
        ErrorLog("WASDCombatPlugin: mainLoop hook FAILED");
    else
        DebugLog("WASDCombatPlugin: mainLoop hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharMovement::_NV_update),
            &charMovUpdate_hook, &s_charMovUpdateOrig))
        ErrorLog("WASDCombatPlugin: charMovUpdate hook FAILED");
    else
        DebugLog("WASDCombatPlugin: charMovUpdate hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::update),
            &cameraUpdate_hook, &s_cameraUpdateOrig))
        ErrorLog("WASDCombatPlugin: CameraClass::update hook FAILED — OTS camera unavailable");
    else
        DebugLog("WASDCombatPlugin: cameraUpdate hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::restrictPosition),
            &restrictPos_hook, &s_restrictPosOrig))
        ErrorLog("WASDCombatPlugin: restrictPosition hook FAILED — OTS camera may clamp to floor");
    else
        DebugLog("WASDCombatPlugin: restrictPosition hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&PlayerInterface::playerControl),
            &playerControl_hook, &s_playerControlOrig))
        ErrorLog("WASDCombatPlugin: playerControl hook FAILED");
    else
        DebugLog("WASDCombatPlugin: playerControl hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::removeJob),
            &removeJob_hook, &s_removeJobOrig))
        ErrorLog("WASDCombatPlugin: removeJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: removeJob hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addJob),
            &addJob_hook, &s_addJobOrig))
        ErrorLog("WASDCombatPlugin: addJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: addJob hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addOrder),
            &addOrder_hook, &s_addOrderOrig))
        ErrorLog("WASDCombatPlugin: addOrder hook FAILED — door suppression partial (addJob only)");
    else
        DebugLog("WASDCombatPlugin: addOrder hook OK");

    // playerMove is not installed.  On the current exe, RVA 0x7F95F0 is one byte
    // into a 5-byte call inside a NavMesh path function, so the MinHook jump byte
    // became that call's displacement and pathfinding that reached it jumped into
    // unmapped memory (crash: pack bull + right-click inside a hive home).  The
    // RMB press-edge poll and the addOrder/addJob gates cover its job.

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_NV_go),
            &combatGo_hook, &s_combatGoOrig))
        ErrorLog("WASDCombatPlugin: combatGo hook FAILED — passive-combat model inactive");
    else
        DebugLog("WASDCombatPlugin: combatGo hook OK");

    // initCombatMode and youKnowImAttacking are deliberately not hooked: DC must
    // not block combat entry or attack notifications, only the per-frame go().

    // showTradeWindow is not installed.  On the current exe, RVA 0x7905D0 is
    // inside a 10-byte movabs of a double-to-int64 conversion helper, so the patch
    // corrupted that helper's common path while the hook itself never fired.  The
    // mainLoop GUI poll (isAnyInventoryWindowOpen) covers loot/trade detection.

    HANDLE h = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!h)
    {
        char buf[64];
        sprintf_s(buf, sizeof(buf), "WASDCombatPlugin: poll thread FAILED err=%lu", GetLastError());
        ErrorLog(buf);
    }
    else
    {
        CloseHandle(h);
        DebugLog("WASDCombatPlugin: poll thread OK");
    }
}
