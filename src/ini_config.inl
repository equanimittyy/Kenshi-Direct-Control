// Kenshi's InputHandler allows one command per physical key, and vanilla camera panning owns
// W/A/S/D. Registering DC movement commands there stole the keys from the camera and persisted
// the theft into controls.cfg, so movement keys are always VK-polled and dc_move_* commands must
// never be registered. Only the toggle sits on a free key and is registered natively.
static volatile bool s_nativeCommandsRegistered = false;
static void watchNativeBindChanges();
static void registerNativeCommands(InputHandler* self);

struct VkName { const char* name; int vk; };
static const VkName s_vkNames[] =
{
    { "VK_SPACE",   VK_SPACE   }, { "VK_TAB",      VK_TAB      },
    { "VK_RETURN",  VK_RETURN  }, { "VK_BACK",     VK_BACK     },
    { "VK_SHIFT",   VK_SHIFT   }, { "VK_LSHIFT",   VK_LSHIFT   }, { "VK_RSHIFT",   VK_RSHIFT   },
    { "VK_CONTROL", VK_CONTROL }, { "VK_LCONTROL", VK_LCONTROL }, { "VK_RCONTROL", VK_RCONTROL },
    { "VK_MENU",    VK_MENU    }, { "VK_LMENU",    VK_LMENU    }, { "VK_RMENU",    VK_RMENU    },
    { "VK_CAPITAL", VK_CAPITAL },
    { "VK_UP",      VK_UP      }, { "VK_DOWN",     VK_DOWN     },
    { "VK_LEFT",    VK_LEFT    }, { "VK_RIGHT",    VK_RIGHT    },
    { "VK_HOME",    VK_HOME    }, { "VK_END",      VK_END      },
    { "VK_PRIOR",   VK_PRIOR   }, { "VK_NEXT",     VK_NEXT     },
    { "VK_INSERT",  VK_INSERT  }, { "VK_DELETE",   VK_DELETE   },
    { "VK_NUMPAD0", VK_NUMPAD0 }, { "VK_NUMPAD1",  VK_NUMPAD1  }, { "VK_NUMPAD2", VK_NUMPAD2 },
    { "VK_NUMPAD3", VK_NUMPAD3 }, { "VK_NUMPAD4",  VK_NUMPAD4  }, { "VK_NUMPAD5", VK_NUMPAD5 },
    { "VK_NUMPAD6", VK_NUMPAD6 }, { "VK_NUMPAD7",  VK_NUMPAD7  }, { "VK_NUMPAD8", VK_NUMPAD8 },
    { "VK_NUMPAD9", VK_NUMPAD9 },
    { "VK_MULTIPLY", VK_MULTIPLY }, { "VK_ADD",     VK_ADD     },
    { "VK_SUBTRACT", VK_SUBTRACT }, { "VK_DECIMAL", VK_DECIMAL }, { "VK_DIVIDE", VK_DIVIDE },
    { "VK_OEM_1", VK_OEM_1 }, { "VK_OEM_2", VK_OEM_2 }, { "VK_OEM_3", VK_OEM_3 },
    { "VK_OEM_4", VK_OEM_4 }, { "VK_OEM_5", VK_OEM_5 }, { "VK_OEM_6", VK_OEM_6 },
    { "VK_OEM_7", VK_OEM_7 }, { "VK_OEM_8", VK_OEM_8 }, { "VK_OEM_102", VK_OEM_102 },
    { "VK_OEM_PLUS",  VK_OEM_PLUS  }, { "VK_OEM_COMMA",  VK_OEM_COMMA  },
    { "VK_OEM_MINUS", VK_OEM_MINUS }, { "VK_OEM_PERIOD", VK_OEM_PERIOD },
};
static const int NUM_VK_NAMES = sizeof(s_vkNames) / sizeof(s_vkNames[0]);

static int parseKeyName(const char* raw)
{
    char s[32];
    int  n = 0;
    for (const char* p = raw; *p && n < 31; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)toupper((unsigned char)*p);
    }
    s[n] = '\0';
    if (n == 0) return -1;

    char prefixed[36];
    sprintf_s(prefixed, sizeof(prefixed), "VK_%s", s);
    for (int i = 0; i < NUM_VK_NAMES; ++i)
        if (strcmp(s, s_vkNames[i].name) == 0
            || strcmp(prefixed, s_vkNames[i].name) == 0)
            return s_vkNames[i].vk;

    const char* f = s;
    if (strncmp(f, "VK_", 3) == 0) f += 3;

    if (f[0] == 'F' && f[1] >= '0' && f[1] <= '9')
    {
        int fn = atoi(f + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + fn - 1;
    }
    if (strlen(f) == 1 &&
        ((f[0] >= 'A' && f[0] <= 'Z') || (f[0] >= '0' && f[0] <= '9')))
        return f[0];
    if (s[0] == '0' && s[1] == 'X')
    {
        long v = strtol(s, nullptr, 16);
        if (v > 0 && v < 256) return (int)v;
    }
    {
        char* end = nullptr;
        long  v   = strtol(s, &end, 10);
        if (end != s && *end == '\0' && v > 0 && v < 256) return (int)v;
    }
    return -1;
}

// Unknown values return the default, so a typo cannot silently turn a feature off.
static bool parseBool(const char* raw, bool dflt)
{
    char s[16];
    int  n = 0;
    for (const char* p = raw; *p && n < 15; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)tolower((unsigned char)*p);
    }
    s[n] = '\0';
    if (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "on"))
        return true;
    if (!strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "off"))
        return false;
    return dflt;
}

static void getConfigPath(char* out, size_t cap)
{
    out[0] = '\0';
    if (s_thisModule &&
        GetModuleFileNameA(s_thisModule, out, (DWORD)cap) > 0)
    {
        char* slash = strrchr(out, '\\');
        if (slash) { *(slash + 1) = '\0'; }
        else       { out[0] = '\0'; }
    }
    strcat_s(out, cap, "WASDCombatPlugin.ini");
}

static void writeDefaultConfig(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // The template ships player-facing settings only; the loader also reads many
    // advanced [FirstPerson] keys, each with a safe default.
    static const char tmpl[] =
        "[Keybinds]\r\n"
        "; Valid names: letters, digits, F1..F24, SPACE, TAB, SHIFT, CONTROL,\r\n"
        "; arrow keys, NUMPAD0..9, OEM_1..8.  Invalid entries use the default.\r\n"
        "ToggleDC        = V\r\n"
        "MoveForward     = W\r\n"
        "MoveBackward    = S\r\n"
        "MoveLeft        = A\r\n"
        "MoveRight       = D\r\n"
        "SelectControl   = F\r\n"
        "FirstPerson     = P\r\n"
        "; Sneak is the chord SHIFT + this key, first-person only.\r\n"
        "SneakToggle     = C\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; true = camera faces your character when their inventory opens.\r\n"
        "; false = keep moving with WASD while looting/trading.\r\n"
        "InventoryFaceCam = true\r\n"
        "; Cap WASD speed at the character's real max speed (injuries,\r\n"
        "; encumbrance, shackles).  Mult scales it (1.0 = exactly vanilla).\r\n"
        "WasdSpeedCap     = true\r\n"
        "WasdSpeedMult    = 1.0\r\n"
        "\r\n"
        "[FirstPerson]\r\n"
        "; First-person view (press P while Direct Control is on).\r\n"
        "Sensitivity      = 1.0\r\n"
        "; Field of view in degrees (50-110).\r\n"
        "FOV              = 90\r\n"
        "; Hide your own hair / head so they don't block the view.\r\n"
        "HideHair         = 1\r\n"
        "HideHead         = 1\r\n"
        "; Render the storeys below you inside multi-floor buildings\r\n"
        "; (0 = vanilla reveal-on-approach).\r\n"
        "FloorRevealBelow = 1\r\n";
    DWORD written = 0;
    WriteFile(h, tmpl, (DWORD)(sizeof(tmpl) - 1), &written, nullptr);
    CloseHandle(h);
}

// Separate from loadKeybinds so enterFirstPerson can re-read it: INI edits apply on the
// next first-person toggle without a relaunch.
static void loadFirstPersonConfig(const char* path)
{
    char fb[32];
    GetPrivateProfileStringA("FirstPerson", "Sensitivity", "1.0", fb, sizeof(fb), path);
    float sens = (float)atof(fb);
    if (sens < 0.1f) sens = 0.1f;  if (sens > 5.0f) sens = 5.0f;
    s_fpSensitivityFP = sens;
    GetPrivateProfileStringA("FirstPerson", "FOV", "75", fb, sizeof(fb), path);
    float fov = (float)atof(fb);
    if (fov < 50.0f) fov = 50.0f;  if (fov > 110.0f) fov = 110.0f;
    s_fpFovDegFP = fov;
    GetPrivateProfileStringA("FirstPerson", "EyeHeight", "16.5", fb, sizeof(fb), path);
    float eye = (float)atof(fb);
    if (eye < 1.0f) eye = 1.0f;    if (eye > 40.0f) eye = 40.0f;
    s_fpEyeHeight = eye;
    GetPrivateProfileStringA("FirstPerson", "ForwardOffset", "1.2", fb, sizeof(fb), path);
    float fwd = (float)atof(fb);
    if (fwd < 0.0f) fwd = 0.0f;    if (fwd > 10.0f) fwd = 10.0f;
    s_fpFwdOffset = fwd;
    GetPrivateProfileStringA("FirstPerson", "EyeUpAdjust", "0.0", fb, sizeof(fb), path);
    float eyeUp = (float)atof(fb);
    if (eyeUp < -6.0f) eyeUp = -6.0f;  if (eyeUp > 6.0f) eyeUp = 6.0f;
    s_fpEyeUpAdjust = eyeUp;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanUp", "0.0", fb, sizeof(fb), path);
    float mlu = (float)atof(fb);
    if (mlu < 0.0f) mlu = 0.0f;    if (mlu > 12.0f) mlu = 12.0f;
    s_fpMoveLeanUp = mlu;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanForward", "0.0", fb, sizeof(fb), path);
    float mlf = (float)atof(fb);
    if (mlf < 0.0f) mlf = 0.0f;    if (mlf > 12.0f) mlf = 12.0f;
    s_fpMoveLeanFwd = mlf;
    GetPrivateProfileStringA("FirstPerson", "MoveNearClip", "0.0", fb, sizeof(fb), path);
    float mnc = (float)atof(fb);
    if (mnc < 0.0f) mnc = 0.0f;    if (mnc > 10.0f) mnc = 10.0f;
    s_fpMoveNearClip = mnc;
    GetPrivateProfileStringA("FirstPerson", "JogForward", "2.0", fb, sizeof(fb), path);
    float jf = (float)atof(fb);
    if (jf < 0.0f) jf = 0.0f;    if (jf > 20.0f) jf = 20.0f;
    s_fpJogForward = jf;
    GetPrivateProfileStringA("FirstPerson", "RunForward", "4.0", fb, sizeof(fb), path);
    float rf = (float)atof(fb);
    if (rf < 0.0f) rf = 0.0f;    if (rf > 20.0f) rf = 20.0f;
    s_fpRunForward = rf;
    GetPrivateProfileStringA("FirstPerson", "StreamUpdateDist", "2.0", fb, sizeof(fb), path);
    float sud = (float)atof(fb);
    if (sud < 0.0f) sud = 0.0f;    if (sud > 1000.0f) sud = 1000.0f;
    s_fpStreamDist = sud;
    GetPrivateProfileStringA("FirstPerson", "GrassRangeMult", "1.0", fb, sizeof(fb), path);
    float grm = (float)atof(fb);
    if (grm < 1.0f) grm = 1.0f;    if (grm > 8.0f) grm = 8.0f;
    s_fpGrassRangeMult = grm;
    s_fpCamPreOrig = GetPrivateProfileIntA("FirstPerson", "CamPreOrig", 0, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "FollowTurn", "0.08", fb, sizeof(fb), path);
    float ft = (float)atof(fb);
    if (ft < 0.0f) ft = 0.0f;    if (ft > 1.0f) ft = 1.0f;
    s_fpFollowTurn = ft;
    GetPrivateProfileStringA("FirstPerson", "FollowDelayMs", "400", fb, sizeof(fb), path);
    float fd = (float)atof(fb);
    if (fd < 0.0f) fd = 0.0f;    if (fd > 5000.0f) fd = 5000.0f;
    s_fpFollowDelayMs = fd;
    GetPrivateProfileStringA("FirstPerson", "LookSmooth", "0.4", fb, sizeof(fb), path);
    float ls = (float)atof(fb);
    if (ls < 0.0f) ls = 0.0f;    if (ls > 0.9f) ls = 0.9f;
    s_fpLookSmooth = ls;
    s_fpFoliageCenterMode = GetPrivateProfileIntA("FirstPerson", "FoliageCenterFix", 1, path);
    if (s_fpFoliageCenterMode < 0) s_fpFoliageCenterMode = 0;
    if (s_fpFoliageCenterMode > 2) s_fpFoliageCenterMode = 2;
    s_fpFreezeCamTest = GetPrivateProfileIntA("FirstPerson", "FreezeCamTest", 0, path) ? 1 : 0;
    GetPrivateProfileStringA("FirstPerson", "ActionClearForward", "2.0", fb, sizeof(fb), path);
    float acf = (float)atof(fb);
    if (acf < 0.0f) acf = 0.0f;    if (acf > 8.0f) acf = 8.0f;
    s_fpActionClearFwd = acf;
    GetPrivateProfileStringA("FirstPerson", "NeckLimit", "75", fb, sizeof(fb), path);
    float nl = (float)atof(fb);
    if (nl < 30.0f) nl = 30.0f;    if (nl > 170.0f) nl = 170.0f;
    s_fpNeckLimitRad = nl * 3.14159265f / 180.0f;
    GetPrivateProfileStringA("FirstPerson", "NearClip", "0.2", fb, sizeof(fb), path);
    float nc = (float)atof(fb);
    if (nc < 0.05f) nc = 0.05f;    if (nc > 5.0f) nc = 5.0f;
    s_fpNearClipFP = nc;
    s_fpHideHair = GetPrivateProfileIntA("FirstPerson", "HideHair", 1, path) != 0;
    s_fpHideHead = GetPrivateProfileIntA("FirstPerson", "HideHead", 1, path) != 0;
    s_fpTrueBoneEye = GetPrivateProfileIntA("FirstPerson", "TrueBoneEye", 1, path) != 0;
    s_fpRawMouse    = GetPrivateProfileIntA("FirstPerson", "RawMouse",    1, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "EyeDrop", "0.0", fb, sizeof(fb), path);
    float ed = (float)atof(fb);
    if (ed < -4.0f) ed = -4.0f;    if (ed > 8.0f) ed = 8.0f;
    s_fpEyeDrop = ed;
    GetPrivateProfileStringA("FirstPerson", "MoveForward", "0.0", fb, sizeof(fb), path);
    float mf = (float)atof(fb);
    if (mf < 0.0f) mf = 0.0f;      if (mf > 20.0f) mf = 20.0f;
    s_fpMoveForward = mf;
    GetPrivateProfileStringA("FirstPerson", "MoveSpeedRef", "30.0", fb, sizeof(fb), path);
    float msr = (float)atof(fb);
    if (msr < 1.0f) msr = 1.0f;    if (msr > 2000.0f) msr = 2000.0f;
    s_fpMoveSpeedRef = msr;
    GetPrivateProfileStringA("FirstPerson", "StairForwardReduce", "0.25", fb, sizeof(fb), path);
    float sfr = (float)atof(fb);
    if (sfr < 0.0f) sfr = 0.0f;    if (sfr > 5.0f) sfr = 5.0f;
    s_fpStairForwardReduce = sfr;
    GetPrivateProfileStringA("FirstPerson", "StairForwardMinScale", "0.30", fb, sizeof(fb), path);
    float sms = (float)atof(fb);
    if (sms < 0.0f) sms = 0.0f;    if (sms > 1.0f) sms = 1.0f;
    s_fpStairForwardMinScale = sms;
    GetPrivateProfileStringA("FirstPerson", "StairEyeLift", "0.30", fb, sizeof(fb), path);
    float sel = (float)atof(fb);
    if (sel < 0.0f) sel = 0.0f;    if (sel > 4.0f) sel = 4.0f;
    s_fpStairEyeLift = sel;
    GetPrivateProfileStringA("FirstPerson", "EnemyClearRadius", "12.0", fb, sizeof(fb), path);
    float ecr = (float)atof(fb);
    if (ecr < 0.0f) ecr = 0.0f;    if (ecr > 60.0f) ecr = 60.0f;
    s_fpEnemyClearRadius = ecr;
    GetPrivateProfileStringA("FirstPerson", "EnemyClearMinScale", "0.15", fb, sizeof(fb), path);
    float ecm = (float)atof(fb);
    if (ecm < 0.0f) ecm = 0.0f;    if (ecm > 0.95f) ecm = 0.95f;
    s_fpEnemyClearMinScale = ecm;
    GetPrivateProfileStringA("FirstPerson", "EnemyNearClip", "0.10", fb, sizeof(fb), path);
    float enc = (float)atof(fb);
    if (enc < 0.0f) enc = 0.0f;    if (enc > 5.0f) enc = 5.0f;
    s_fpEnemyNearClip = enc;
    s_fpFloorRevealBelow = GetPrivateProfileIntA("FirstPerson", "FloorRevealBelow", 1, path) != 0;
}

static void loadKeybinds()
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        writeDefaultConfig(path);
        char cbuf[MAX_PATH + 64];
        sprintf_s(cbuf, sizeof(cbuf),
            "[WASDCombat] dc_keybinds_default_config_created path=%s", path);
        DebugLog(cbuf);
    }

    for (int r = 0; r < KR_COUNT; ++r)
    {
        char buf[32] = "";
        GetPrivateProfileStringA("Keybinds", KR_INI_KEY[r], KR_DEFAULT[r],
                                 buf, sizeof(buf), path);
        int vk = parseKeyName(buf);
        if (vk <= 0)
        {
            char ibuf[128];
            sprintf_s(ibuf, sizeof(ibuf),
                "[WASDCombat] dc_keybind_invalid name=%s fallback=%s",
                buf, KR_DEFAULT[r]);
            DebugLog(ibuf);
            strcpy_s(buf, sizeof(buf), KR_DEFAULT[r]);
            vk = parseKeyName(buf);
        }
        s_bindVk[r] = vk;
        strcpy_s(s_bindCfgStr[r], sizeof(s_bindCfgStr[r]), buf);
    }

    for (int i = 0; i < KR_COUNT; ++i)
        for (int j = i + 1; j < KR_COUNT; ++j)
            if (s_bindVk[i] == s_bindVk[j])
            {
                char dbuf[128];
                sprintf_s(dbuf, sizeof(dbuf),
                    "[WASDCombat] dc_keybind_duplicate %s and %s share key 0x%02X",
                    KR_INI_KEY[i], KR_INI_KEY[j], s_bindVk[i]);
                DebugLog(dbuf);
            }

    {
        char sbuf[16] = "";
        GetPrivateProfileStringA("Settings", "InventoryFaceCam", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingInventoryFaceCam = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "WasdSpeedCap", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingWasdSpeedCap = parseBool(sbuf, true);

        char mbuf[16] = "";
        GetPrivateProfileStringA("Settings", "WasdSpeedMult", "1.0",
                                 mbuf, sizeof(mbuf), path);
        float wsm = (float)atof(mbuf);
        if (wsm < 0.1f) wsm = 0.1f;   if (wsm > 5.0f) wsm = 5.0f;
        s_settingWasdSpeedMult = wsm;
    }

    loadFirstPersonConfig(path);

    char lbuf[420];
    sprintf_s(lbuf, sizeof(lbuf),
        "[WASDCombat] dc_keybinds_loaded toggle=%s(0x%02X) forward=%s(0x%02X)"
        " back=%s(0x%02X) left=%s(0x%02X) right=%s(0x%02X)"
        " select=%s(0x%02X) inventoryFaceCam=%d",
        s_bindCfgStr[KR_TOGGLE],  s_bindVk[KR_TOGGLE],
        s_bindCfgStr[KR_FORWARD], s_bindVk[KR_FORWARD],
        s_bindCfgStr[KR_BACK],    s_bindVk[KR_BACK],
        s_bindCfgStr[KR_LEFT],    s_bindVk[KR_LEFT],
        s_bindCfgStr[KR_RIGHT],   s_bindVk[KR_RIGHT],
        s_bindCfgStr[KR_SELECT],  s_bindVk[KR_SELECT],
        s_settingInventoryFaceCam ? 1 : 0);
    DebugLog(lbuf);

    char fpbuf[240];
    sprintf_s(fpbuf, sizeof(fpbuf),
        "[WASDCombat] dc_firstperson_cfg key=%s(0x%02X) fov=%.0f sens=%.2f"
        " neckLimitDeg=%.0f hideHead=%d hideHair=%d sneak=SHIFT+%s(0x%02X)"
        " enemyClearR=%.1f",
        s_bindCfgStr[KR_FP], s_bindVk[KR_FP], s_fpFovDegFP, s_fpSensitivityFP,
        s_fpNeckLimitRad * 180.0f / 3.14159265f,
        s_fpHideHead ? 1 : 0, s_fpHideHair ? 1 : 0,
        s_bindCfgStr[KR_SNEAK], s_bindVk[KR_SNEAK],
        s_fpEnemyClearRadius);
    DebugLog(fpbuf);
}
