// Shared by the poll thread and the native processKeys path, so loot-suspend gating is
// identical on both.
static void handleTogglePress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE || s_userWantsDC)
    {
        s_userWantsDC = false;
        s_userWantsFP = false;   // FP requires DC
        setMode(MODE_VANILLA);
        DebugLog("[WASDCombat] dc_user_intent_off_manual");
    }
    else
    {
        s_userWantsDC = true;
        setMode(MODE_FREE_MOVE);
        DebugLog("[WASDCombat] dc_user_intent_on");
    }
}

static void handleSelectPress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE)
        s_fSelectEdge = true;
}

static void handleFirstPersonPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Camera calls must run on the game thread, so only the edge is set here.
    if (s_mode == MODE_FREE_MOVE)
        s_fpToggleRequested = true;
}

static void handleSneakPress()
{
    if (s_lootUiSuspendActive)
        return;
    // A chord, so a bare C press never collides with vanilla or other mods. Final gating
    // runs on the game thread where the edge is consumed.
    if (s_mode != MODE_FREE_MOVE || !s_firstPersonActive)
        return;
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    s_sneakToggleRequested = true;
}

static void onPress(int role)
{
    if (role == KR_TOGGLE)       handleTogglePress();
    else if (role == KR_SELECT)  handleSelectPress();
    else if (role == KR_FP)      handleFirstPersonPress();
    else if (role == KR_SNEAK)   handleSneakPress();
}

struct PollKey { int role; bool prev; };
static PollKey s_keys[] =
{
    { KR_FORWARD, false }, { KR_LEFT,   false },
    { KR_BACK,    false }, { KR_RIGHT,  false },
    { KR_TOGGLE,  false }, { KR_SELECT, false },
    { KR_FP,      false }, { KR_SNEAK,  false },
};
static const int NUM_KEYS = 8;

static bool isKenshiForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI PollThread(LPVOID)
{
    while (true)
    {
        Sleep(50);
        if (!isKenshiForeground()) continue;
        // Toggle polling stands down once its native command is registered; presses then
        // arrive through processKeys.
        for (int i = 0; i < NUM_KEYS; ++i)
        {
            int role = s_keys[i].role;
            if (s_nativeCommandsRegistered && role == KR_TOGGLE)
                continue;
            bool down = (GetAsyncKeyState(s_bindVk[role]) & 0x8000) != 0;
            if (down == s_keys[i].prev) continue;
            s_keys[i].prev = down;
            bool wasWasd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
            switch (role) {
                case KR_FORWARD: s_wHeld = down; break;
                case KR_LEFT:    s_aHeld = down; break;
                case KR_BACK:    s_sHeld = down; break;
                case KR_RIGHT:   s_dHeld = down; break;
            }
            if (!wasWasd && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
                s_wasdTapStartMs = GetTickCount64();
            if (down) onPress(role);
        }

        {
            bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (rmbDown && !s_rmbPrev)
                s_rmbPressedEdge = true;
            s_rmbPrev = rmbDown;
        }

        // The 50 ms poll reliably separates the two down-edges of a normal double-click.
        {
            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lmbDown && !s_lmbPrev)
            {
                ULONGLONG nowLB = GetTickCount64();
                if (s_lastLmbDownMs > 0
                    && (nowLB - s_lastLmbDownMs) <= (ULONGLONG)GetDoubleClickTime())
                    s_lmbDoubleClickMs = nowLB;
                else
                    s_lmbDoubleClickMs = 0;       // a single click clears a stale double-click
                s_lastLmbDownMs = nowLB;
            }
            s_lmbPrev = lmbDown;
        }

        // Flips only in DC, so out-of-DC CTRL use never desyncs the toggle.
        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Not while a UI is open: CTRL+click moves stacks in trade and inventory.
            if (ctrlDown && !s_ctrlPrevPoll && s_mode == MODE_FREE_MOVE && !s_camRotateUiOpen)
                s_camRotateToggle = !s_camRotateToggle;
            s_ctrlPrevPoll = ctrlDown;
        }
    }
}
