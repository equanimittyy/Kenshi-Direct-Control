static bool s_processKeysHookOk = false;  // native registration needs the event reader too

// The game saves bound plugin commands to controls.cfg, but this INI copy also
// covers the case where a command is unbound at save time.
static const int DC_CMD_COUNT = 1;
static const char* const DC_CMD_NAMES[DC_CMD_COUNT] =
{
    "dc_toggle"
};

// Format v1 stored Command::bound, which is not the keycode, so rebinds never
// survived a restart.  v2 stores the real keycode from getBoundKeys().  Older
// files are ignored and re-stamped, so they cannot bind the command to key 1.
static const int DC_NATIVE_BIND_FORMAT_VERSION = 2;

// Do NOT read Command::bound here: it is an internal value, not the keycode.
static int readBoundKey(const char* name)
{
    if (!key) return INT_MIN;
    lektor<int> keys = key->getBoundKeys(name);
    if (keys.size() == 0) return INT_MIN;   // command unbound
    return keys[0];
}

// Change detection only.  getBoundKeys allocates, which is too costly to poll;
// Command::bound is not the keycode, but it differs per binding.
static int readChangeToken(const char* name)
{
    if (!key) return INT_MIN;
    auto it = key->commands.find(name);
    if (it == key->commands.end()) return INT_MIN;
    return it->second.bound;
}

static void saveNativeBindsToIni(const char* reason)
{
    if (!key) return;
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    // Version first, so a partially written file is never read as v1.
    {
        char vbuf[16];
        sprintf_s(vbuf, sizeof(vbuf), "%d", DC_NATIVE_BIND_FORMAT_VERSION);
        WritePrivateProfileStringA("NativeBinds", "version", vbuf, path);
    }
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int bound = readBoundKey(DC_CMD_NAMES[i]);
        if (bound == INT_MIN)
        {
            char ebuf[96];
            sprintf_s(ebuf, sizeof(ebuf),
                "[WASDCombat] dc_native_bind_save_failed name=%s reason=unbound_or_not_found",
                DC_CMD_NAMES[i]);
            DebugLog(ebuf);
            continue;
        }
        char val[16];
        sprintf_s(val, sizeof(val), "%d", bound);
        BOOL ok = WritePrivateProfileStringA("NativeBinds", DC_CMD_NAMES[i], val, path);
        char sbuf[160];
        sprintf_s(sbuf, sizeof(sbuf),
            "[WASDCombat] dc_native_bind_saved name=%s bound=%d write_ok=%d",
            DC_CMD_NAMES[i], bound, (int)ok);
        DebugLog(sbuf);
    }
    char rbuf[MAX_PATH + 96];
    sprintf_s(rbuf, sizeof(rbuf),
        "[WASDCombat] dc_native_binds_saved reason=%s path=%s", reason, path);
    DebugLog(rbuf);
}

// Persistence must not depend on the options menu calling saveOptions.
static int       s_bindSnapshot[DC_CMD_COUNT] = { 0 };
static bool      s_bindSnapshotValid    = false;
static ULONGLONG s_bindWatchTick        = 0;
static const ULONGLONG BIND_WATCH_INTERVAL_MS = 3000;

static void watchNativeBindChanges()
{
    ULONGLONG nowBW = GetTickCount64();
    if (nowBW - s_bindWatchTick < BIND_WATCH_INTERVAL_MS) return;
    s_bindWatchTick = nowBW;

    int cur[DC_CMD_COUNT];
    for (int i = 0; i < DC_CMD_COUNT; ++i)
        cur[i] = readChangeToken(DC_CMD_NAMES[i]);

    if (!s_bindSnapshotValid)
    {
        for (int i = 0; i < DC_CMD_COUNT; ++i) s_bindSnapshot[i] = cur[i];
        s_bindSnapshotValid = true;
        return;
    }

    bool changed = false;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        if (cur[i] != s_bindSnapshot[i])
        {
            char cbuf[160];
            sprintf_s(cbuf, sizeof(cbuf),
                "[WASDCombat] dc_native_bind_change_detected name=%s old=%d new=%d",
                DC_CMD_NAMES[i], s_bindSnapshot[i], cur[i]);
            DebugLog(cbuf);
            s_bindSnapshot[i] = cur[i];
            changed = true;
        }
    }
    if (changed)
        saveNativeBindsToIni("change_detected");
}

static void applyNativeBindsFromIni(InputHandler* self)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION)
    {
        char mbuf[MAX_PATH + 96];
        sprintf_s(mbuf, sizeof(mbuf),
            "[WASDCombat] dc_native_binds_migrated old_ver=%d -> v%d (defaults kept) path=%s",
            ver, DC_NATIVE_BIND_FORMAT_VERSION, path);
        DebugLog(mbuf);
        saveNativeBindsToIni("format_migration");
        return;
    }

    int applied = 0;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int v = (int)GetPrivateProfileIntA("NativeBinds", DC_CMD_NAMES[i], -1, path);
        if (v > 0)
        {
            // bind() adds a key and does not replace, so without the unbind the
            // default and the saved key would both fire.
            self->unbind(std::string(DC_CMD_NAMES[i]));
            self->bind(DC_CMD_NAMES[i], v);
            int after = readBoundKey(DC_CMD_NAMES[i]);
            char abuf[160];
            sprintf_s(abuf, sizeof(abuf),
                "[WASDCombat] dc_native_bind_applied name=%s ini=%d bound_after=%d%s",
                DC_CMD_NAMES[i], v, after,
                (after == v) ? "" : " MISMATCH");
            DebugLog(abuf);
            ++applied;
        }
    }
    char cbuf[MAX_PATH + 96];
    sprintf_s(cbuf, sizeof(cbuf),
        "[WASDCombat] dc_native_binds_applied count=%d path=%s", applied, path);
    DebugLog(cbuf);
}

// Returns -1 when the user never rebound the command.  The version gate must
// match applyNativeBindsFromIni.
static int iniSavedNativeBind(const char* name)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION) return -1;
    int v = (int)GetPrivateProfileIntA("NativeBinds", name, -1, path);
    return (v > 0) ? v : -1;
}

// The game runs InputHandler::loadConfig before RE_Kenshi loads plugins, so this
// is called from the first mainLoop pass; the loadConfig hook only covers a
// later config reload.  Movement keys must never be registered: one command per
// key, and the vanilla camera owns W/A/S/D.
static void registerNativeCommands(InputHandler* self)
{
    if (s_nativeCommandsRegistered || !self) return;
    if (!s_processKeysHookOk)
    {
        // Without the event reader, toggle presses would be lost.
        DebugLog("[WASDCombat] dc_native_keybinds_skipped_no_event_reader");
        return;
    }
    // With a saved rebind, register with no key: Kenshi allows one command per
    // key, so claiming plain V would steal it from any vanilla command bound
    // there, every session.
    const int savedToggle = iniSavedNativeBind("dc_toggle");
    self->addCommand("dc_toggle",        0,
                     (savedToggle > 0) ? OIS::KC_UNASSIGNED : OIS::KC_V,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    char rnbuf[160];
    sprintf_s(rnbuf, sizeof(rnbuf),
        "[WASDCombat] dc_native_register defaults toggle=%s",
        (savedToggle > 0) ? "deferred(rebound)" : "V");
    DebugLog(rnbuf);
    applyNativeBindsFromIni(self);
    s_nativeCommandsRegistered = true;   // poll-thread toggle stands down
    DebugLog("[WASDCombat] dc_native_keybinds_registered");
}

static void (*s_inputLoadConfigOrig)(InputHandler*);
static void inputLoadConfig_hook(InputHandler* self)
{
    registerNativeCommands(self);
    s_inputLoadConfigOrig(self);
    if (s_nativeCommandsRegistered)
        applyNativeBindsFromIni(self);   // re-assert ours over any cfg reload
}

// controls.cfg excludes plugin commands, so persist ours here.
static void (*s_optionsSaveOrig)(OptionsWindow*);
static void optionsSave_hook(OptionsWindow* self)
{
    s_optionsSaveOrig(self);
    if (s_nativeCommandsRegistered)
        saveNativeBindsToIni("save_options");
}

static void (*s_optionsCreateOrig)(OptionsWindow*);
static void optionsCreate_hook(OptionsWindow* self)
{
    s_optionsCreateOrig(self);

    DatapanelGUI* controlsTab = nullptr;
    size_t tabCount = self->tabs->getItemCount();
    for (size_t i = 0; i < tabCount; i++)
    {
        DatapanelGUI** panel = self->tabs->getItemDataAt<DatapanelGUI*>(i, false);
        if (panel && *panel != nullptr && (*panel)->currentCategory == 0x19)   // Controls tab
        {
            controlsTab = *panel;
            break;
        }
    }
    if (controlsTab)
    {
        // No leading addSpace: it renders as a large empty gap above the row.
        controlsTab->addCustomLine(new DataPanelLine_KeyConfig(
            "dc_toggle",        "Direct Control: Toggle",        0x19));
        DebugLog("[WASDCombat] dc_controls_menu_section_added");
    }
    else
    {
        DebugLog("[WASDCombat] dc_controls_tab_not_found — bindings still work, menu rows missing");
    }
}

// Events stay in key->events for exactly one processKeys cycle.  No pause gate:
// the toggle must work while the game is paused.
static void (*s_processKeysOrig)(GameWorld* thisptr);
static void processKeys_hook(GameWorld* thisptr)
{
    s_processKeysOrig(thisptr);
    if (s_dcShutdownInProgress || !s_nativeCommandsRegistered)
        return;
    if (gui && gui->isLoadingMessageVisible())
        return;
    for (auto it = key->events.begin(); it != key->events.end(); ++it)
    {
        const std::string& n = (*it)->name;
        if (n == "dc_toggle") handleTogglePress();
    }
}
