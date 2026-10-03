static const float FP_RAD_PER_PIXEL = 0.0030f;
static const float FP_PITCH_LIMIT   = 1.45f;   // ~83 degrees, radians

static bool isKenshiForegroundMain()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// First-person mouse-look reads a second, non-exclusive background DirectInput
// mouse, so the game keeps its own input. A 1 kHz thread accumulates the relative
// counts and each frame takes the total, which makes the look independent of the
// frame rate (GetCursorPos/SetCursorPos sampling felt sluggish at high fps).
// DirectInput8Create and the GUIDs are resolved here because the dxguid/dinput8
// import libs are not reliably on the v100 lib path.
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static const GUID DIFP_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID DIFP_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
// DIMOUSESTATE2 lX/lY/lZ are at offsets 0/4/8; a NULL pguid matches any axis object.
static DIOBJECTDATAFORMAT DIFP_odf[] = {
    { NULL, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 8, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
};
static const DIDATAFORMAT DIFP_df = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE2), 3, DIFP_odf
};
static IDirectInputDevice8A* s_diMouse      = nullptr;
static bool                  s_diReady      = false;
static volatile LONG         s_diAccX       = 0;
static volatile LONG         s_diAccY       = 0;
static HANDLE                s_diThread     = nullptr;
static volatile LONG         s_diThreadRun  = 0;

static void fpEnsureDInput()
{
    static int tried = 0;
    if (s_diReady || tried >= 600) return;   // retry through early frames, then give up
    tried++;
    HWND w = FindWindowA("OgreD3D11Wnd", nullptr);
    if (!w) w = FindWindowA("OgreD3D9Wnd", nullptr);
    if (!w) w = GetForegroundWindow();
    if (!w) return;
    if (!s_diMouse)
    {
        HMODULE dll = LoadLibraryA("dinput8.dll");
        DI8Create_t create = dll
            ? (DI8Create_t)GetProcAddress(dll, "DirectInput8Create") : nullptr;
        IDirectInput8A* di = nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                     DIFP_IID_IDirectInput8A, (void**)&di, nullptr)) || !di)
        { tried = 600; return; }
        if (FAILED(di->CreateDevice(DIFP_GUID_SysMouse, &s_diMouse, nullptr)) || !s_diMouse)
        { di->Release(); tried = 600; return; }
        di->Release();
        s_diMouse->SetDataFormat(&DIFP_df);
    }
    s_diMouse->SetCooperativeLevel(w, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (SUCCEEDED(s_diMouse->Acquire()))
    {
        s_diReady = true;
        DebugLog("[WASDCombat] dc_fp_dinput_acquired");
    }
}

static DWORD WINAPI fpDInputPollThread(void*)
{
    timeBeginPeriod(1);   // 1ms Sleep granularity for this loop
    while (InterlockedCompareExchange(&s_diThreadRun, 1, 1))
    {
        if (s_diReady && s_diMouse)
        {
            DIMOUSESTATE2 st;
            if (SUCCEEDED(s_diMouse->GetDeviceState(sizeof(st), &st)))
            {
                if (st.lX) InterlockedAdd(&s_diAccX, st.lX);
                if (st.lY) InterlockedAdd(&s_diAccY, st.lY);
            }
            else s_diMouse->Acquire();   // lost (alt-tab): re-acquire, skip this poll
        }
        Sleep(1);
    }
    return 0;
}

static void fpStartDInputThread()
{
    if (s_diThread) return;
    InterlockedExchange(&s_diThreadRun, 1);
    s_diThread = CreateThread(nullptr, 0, fpDInputPollThread, nullptr, 0, nullptr);
}

static void fpTakeMouseAccum(float* dx, float* dy)
{
    *dx = (float)InterlockedExchange(&s_diAccX, 0);
    *dy = (float)InterlockedExchange(&s_diAccY, 0);
}
