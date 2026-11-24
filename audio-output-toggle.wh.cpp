// ==WindhawkMod==
// @id              audio-output-toggle
// @name            Audio Output Toggle
// @description     Press a hotkey to flip the default audio output between two devices (e.g. Speaker and Headphone)
// @version         1.0
// @include         explorer.exe
// @compilerOptions -lole32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Audio Output Toggle

Press the hotkey to switch the default sound output between two devices.
Music, games and calls all move together.

- If the current output is the first device, it switches to the second.
- Otherwise (second device, or anything else) it switches to the first.

Device names only need to match part of the name shown in
Settings → System → Sound (e.g. `Speaker` matches `Speaker (Realtek(R) Audio)`).

A device that is unplugged or disabled can't be picked; the mod then does nothing.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hotkey: Ctrl+Win+Up
  $name: Hotkey
  $description: Ctrl, Alt, Shift, Win plus one key (A-Z, 0-9, F1-F24, Up, Down, Left, Right), joined with +
- deviceA: Speaker
  $name: First device
  $description: Part of the output's name
- deviceB: Headphone
  $name: Second device
  $description: Part of the output's name
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <string>

// GUIDs defined here so we don't depend on which ones the MinGW headers/libs export.
static const CLSID kClsidDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIidDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const CLSID kClsidPolicyConfig = {0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
static const IID kIidPolicyConfig = {0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};
static const PROPERTYKEY kPkeyFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

// Undocumented interface every audio switcher uses to set the default device.
// Only SetDefaultEndpoint is called; the slots before it just keep the vtable layout.
struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Slot1() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot2() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot3() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot4() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot5() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot6() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot7() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot8() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot9() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot10() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
};

static UINT g_mods, g_vk;
static std::wstring g_hotkey, g_devA, g_devB;
static HANDLE g_thread, g_stopEvent;

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return !needle.empty() && Lower(haystack).find(Lower(needle)) != std::wstring::npos;
}

static bool ParseHotkey(std::wstring s, UINT& mods, UINT& vk) {
    s.erase(std::remove(s.begin(), s.end(), L' '), s.end());
    s = Lower(s);
    mods = MOD_NOREPEAT;
    vk = 0;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(L'+', pos);
        if (end == std::wstring::npos) end = s.size();
        std::wstring t = s.substr(pos, end - pos);
        pos = end + 1;
        if (t == L"ctrl" || t == L"control") mods |= MOD_CONTROL;
        else if (t == L"alt") mods |= MOD_ALT;
        else if (t == L"shift") mods |= MOD_SHIFT;
        else if (t == L"win") mods |= MOD_WIN;
        else if (t == L"up") vk = VK_UP;
        else if (t == L"down") vk = VK_DOWN;
        else if (t == L"left") vk = VK_LEFT;
        else if (t == L"right") vk = VK_RIGHT;
        else if (t.size() == 1 && iswalnum(t[0])) vk = towupper(t[0]);
        else if (t.size() >= 2 && t.size() <= 3 && t[0] == L'f' && _wtoi(t.c_str() + 1) >= 1 && _wtoi(t.c_str() + 1) <= 24)
            vk = VK_F1 + _wtoi(t.c_str() + 1) - 1;
        else return false;
    }
    return vk != 0;
}

static std::wstring FriendlyName(IMMDevice* dev) {
    std::wstring name;
    IPropertyStore* props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

static void ToggleOutput() {
    IMMDeviceEnumerator* en;
    if (FAILED(CoCreateInstance(kClsidDeviceEnumerator, nullptr, CLSCTX_ALL, kIidDeviceEnumerator, (void**)&en))) return;

    std::wstring current, idA, idB;
    IMMDevice* dev;
    LPWSTR id;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        if (SUCCEEDED(dev->GetId(&id))) { current = id; CoTaskMemFree(id); }
        dev->Release();
    }

    IMMDeviceCollection* list;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        UINT n = 0;
        list->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            if (FAILED(list->Item(i, &dev))) continue;
            std::wstring name = FriendlyName(dev);
            if (SUCCEEDED(dev->GetId(&id))) {
                if (idA.empty() && Contains(name, g_devA)) idA = id;
                else if (idB.empty() && Contains(name, g_devB)) idB = id;
                CoTaskMemFree(id);
            }
            dev->Release();
        }
        list->Release();
    }
    en->Release();

    const std::wstring& target = (current == idA) ? idB : idA;
    if (target.empty()) {
        Wh_Log(L"Target device not found or not active (first: %s, second: %s)", g_devA.c_str(), g_devB.c_str());
        return;
    }

    IPolicyConfig* pc;
    if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig, (void**)&pc))) {
        for (ERole role : {eConsole, eMultimedia, eCommunications}) pc->SetDefaultEndpoint(target.c_str(), role);
        pc->Release();
    }
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Several explorer.exe processes can exist; only the first one gets the hotkey, which is fine.
    if (!RegisterHotKey(nullptr, 1, g_mods, g_vk)) {
        Wh_Log(L"Couldn't register %s (error %u) - another app or Windows may own it", g_hotkey.c_str(), GetLastError());
    } else {
        // Waiting on the stop event too means Stop() can never miss a wake-up.
        while (MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                if (msg.message == WM_HOTKEY) ToggleOutput();
        }
        UnregisterHotKey(nullptr, 1);
    }
    CoUninitialize();
    return 0;
}

static std::wstring StringSetting(PCWSTR key) {
    PCWSTR v = Wh_GetStringSetting(key);
    std::wstring s = v;
    Wh_FreeStringSetting(v);
    return s;
}

static void Start() {
    g_hotkey = StringSetting(L"hotkey");
    g_devA = StringSetting(L"deviceA");
    g_devB = StringSetting(L"deviceB");
    if (!ParseHotkey(g_hotkey, g_mods, g_vk)) {
        Wh_Log(L"Can't read hotkey '%s'", g_hotkey.c_str());
        return;
    }
    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
}

static void Stop() {
    if (!g_thread) return;
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = g_stopEvent = nullptr;
}

BOOL Wh_ModInit() {
    Start();
    return TRUE;
}

void Wh_ModUninit() {
    Stop();
}

void Wh_ModSettingsChanged() {
    Stop();
    Start();
}
