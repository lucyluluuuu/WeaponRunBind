#include "BaseMod.h"
#include <Windows.h>
#include <XInput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <iostream>
#include <cstdint>
#include <string>
#include <MinHook.h>
#include "globals.h"
#include "config.h"

// --- template scaffolding ---
bool myCheckBox = false;
int counter = 0;

// --- weapon run state ---
static uintptr_t g_base = 0;
static void* g_pPlayer = nullptr;

// --- Bind Settings ---
static int  g_inputType = 0;
static int  g_keyboardBind = VK_RCONTROL;
static uint16_t g_xinputBind = XINPUT_GAMEPAD_RIGHT_SHOULDER;
static int    g_dinputBind = 0;
static bool s_wasBindPressed = false;

// --- Rebind Listening States ---
static bool s_listeningForDInput = false;
static bool s_listeningForXInput = false;

using MotionSwitch_t = void(__cdecl*)(void* p, int motion);
static MotionSwitch_t g_MotionSwitch = nullptr;

using TickHandler_t = void(__cdecl*)(void* p);
static TickHandler_t g_OrigTickHandler = nullptr;

// --- StartMotion Hook ---
using StartMotion_t = void(__cdecl*)(void* p, int a, int motion, int tier, int b);
static StartMotion_t g_OrigStartMotion = nullptr;

// --- DirectInput Globals ---
static LPDIRECTINPUT8 g_pDI = nullptr;
static LPDIRECTINPUTDEVICE8 g_pDIDevice = nullptr;
static DIJOYSTATE2 g_js;

// --- Helper: Check if the currently configured bind is held ---
bool IsBindHeld() {
    if (g_inputType == 0) {
        return (GetAsyncKeyState(g_keyboardBind) & 0x8000) != 0;
    }
    else if (g_inputType == 1) {
        XINPUT_STATE state = { 0 };
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            return (state.Gamepad.wButtons & g_xinputBind) != 0;
        }
    }
    else if (g_inputType == 2) {
        if (g_pDIDevice) {
            g_pDIDevice->Poll();
            HRESULT hr = g_pDIDevice->GetDeviceState(sizeof(DIJOYSTATE2), &g_js);
            if (SUCCEEDED(hr)) {
                return (g_js.rgbButtons[g_dinputBind] & 0x80) != 0;
            }
        }
    }
    return false;
}

// --- Hook: Blocks the entire run/dash family unless bind is held ---
void __cdecl Hooked_StartMotion(void* p, int a, int motion, int tier, int b) {
    // Block the "run/dash" family (0x66, 0x67, 0x68) unless bind is held
    // 0x66 = Run Start / Dash (the double-flick trigger)
    // 0x67 = Run Loop
    // 0x68 = Alternate Run Start
    // We allow 0x69 (Run Stop) to pass so the player can stop naturally.
    if (motion >= 0x66 && motion <= 0x68) {
        if (!IsBindHeld()) {
            return; // Block the motion entirely
        }
    }

    // Allow all other motions (including 0x69 stop, attacks, rolls, etc.)
    g_OrigStartMotion(p, a, motion, tier, b);
}

// --- FILE SAVE/LOAD HELPERS ---
std::string GetModDirectory() {
    char path[MAX_PATH];
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&GetModDirectory, &hm)) {
        GetModuleFileNameA(hm, path, sizeof(path));
        std::string s(path);
        return s.substr(0, s.find_last_of("\\/"));
    }
    return "";
}

void SaveSettings() {
    std::string path = GetModDirectory() + "\\WeaponRunBind.ini";
    WritePrivateProfileStringA("Settings", "InputType", std::to_string(g_inputType).c_str(), path.c_str());
    WritePrivateProfileStringA("Settings", "KeyboardBind", std::to_string(g_keyboardBind).c_str(), path.c_str());
    WritePrivateProfileStringA("Settings", "XInputBind", std::to_string(g_xinputBind).c_str(), path.c_str());
    WritePrivateProfileStringA("Settings", "DInputBind", std::to_string(g_dinputBind).c_str(), path.c_str());
}

void LoadSettings() {
    std::string path = GetModDirectory() + "\\WeaponRunBind.ini";
    g_inputType = GetPrivateProfileIntA("Settings", "InputType", 0, path.c_str());
    g_keyboardBind = GetPrivateProfileIntA("Settings", "KeyboardBind", VK_RCONTROL, path.c_str());
    g_xinputBind = (uint16_t)GetPrivateProfileIntA("Settings", "XInputBind", XINPUT_GAMEPAD_RIGHT_SHOULDER, path.c_str());
    g_dinputBind = GetPrivateProfileIntA("Settings", "DInputBind", 0, path.c_str());
    std::cout << "WeaponRunBind: Settings loaded from " << path << std::endl;
}
// --- END FILE SAVE/LOAD HELPERS ---

const char* GetXInputButtonName(uint16_t btn) {
    switch (btn) {
    case XINPUT_GAMEPAD_A: return "A";
    case XINPUT_GAMEPAD_B: return "B";
    case XINPUT_GAMEPAD_X: return "X";
    case XINPUT_GAMEPAD_Y: return "Y";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB (L1)";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB (R1)";
    case XINPUT_GAMEPAD_LEFT_THUMB: return "L3";
    case XINPUT_GAMEPAD_RIGHT_THUMB: return "R3";
    case XINPUT_GAMEPAD_START: return "Start";
    case XINPUT_GAMEPAD_BACK: return "Back";
    case XINPUT_GAMEPAD_DPAD_UP: return "D-Pad Up";
    case XINPUT_GAMEPAD_DPAD_DOWN: return "D-Pad Down";
    case XINPUT_GAMEPAD_DPAD_LEFT: return "D-Pad Left";
    case XINPUT_GAMEPAD_DPAD_RIGHT: return "D-Pad Right";
    default: return "Unknown";
    }
}

BOOL CALLBACK EnumDevicesCallback(const DIDEVICEINSTANCE* pdidInstance, VOID* pContext) {
    LPDIRECTINPUTDEVICE8* ppDevice = (LPDIRECTINPUTDEVICE8*)pContext;

    if (g_pDI->CreateDevice(pdidInstance->guidInstance, ppDevice, NULL) == DI_OK) {
        if ((*ppDevice)->SetDataFormat(&c_dfDIJoystick2) == DI_OK) {
            if ((*ppDevice)->SetCooperativeLevel(NULL, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK) {
                if ((*ppDevice)->Acquire() == DI_OK) {
                    std::cout << "DirectInput: Found device: " << pdidInstance->tszProductName << std::endl;
                    return DIENUM_STOP;
                }
            }
        }
        (*ppDevice)->Release();
        *ppDevice = nullptr;
    }
    return DIENUM_CONTINUE;
}

void __cdecl Hooked_TickHandler(void* p) {
    g_pPlayer = p;
    g_OrigTickHandler(p);
}

void BaseMod::OnAttach() {
    std::cout << "WeaponRunBind attach, mhfo-hd base = " << std::hex << mhfdll_addy << std::endl;

    g_base = (uintptr_t)mhfdll_addy;
    g_MotionSwitch = (MotionSwitch_t)(g_base + 0x31B690);

    if (MH_Initialize() != MH_OK) {
        std::cout << "MinHook initialization failed!" << std::endl;
        return;
    }

    // Hook the tick handler to capture player pointer
    void* target = (void*)(g_base + 0x31A410);
    if (MH_CreateHook(target, &Hooked_TickHandler, (void**)&g_OrigTickHandler) != MH_OK) {
        std::cout << "MinHook CreateHook (Tick) failed!" << std::endl;
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        std::cout << "MinHook EnableHook (Tick) failed!" << std::endl;
        return;
    }

    // Hook the StartMotion wrapper to block weapon runs unless bind is held
    void* startMotionTarget = (void*)(g_base + 0xA80A00);
    if (MH_CreateHook(startMotionTarget, &Hooked_StartMotion, (void**)&g_OrigStartMotion) != MH_OK) {
        std::cout << "MinHook CreateHook (StartMotion) failed!" << std::endl;
        return;
    }
    if (MH_EnableHook(startMotionTarget) != MH_OK) {
        std::cout << "MinHook EnableHook (StartMotion) failed!" << std::endl;
        return;
    }

    // Initialize DirectInput
    if (DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&g_pDI, NULL) == DI_OK) {
        std::cout << "DirectInput: Enumerating devices..." << std::endl;
        g_pDI->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCallback, &g_pDIDevice, DIEDFL_ATTACHEDONLY);

        if (!g_pDIDevice) {
            std::cout << "DirectInput: No game controller found!" << std::endl;
        }
    }

    // LOAD SAVED SETTINGS
    LoadSettings();
}

void BaseMod::OnDetach() {
    MH_DisableHook((void*)(g_base + 0x31A410));
    MH_DisableHook((void*)(g_base + 0xA80A00));
    MH_Uninitialize();

    if (g_pDIDevice) {
        g_pDIDevice->Unacquire();
        g_pDIDevice->Release();
        g_pDIDevice = nullptr;
    }
    if (g_pDI) {
        g_pDI->Release();
        g_pDI = nullptr;
    }
}

void BaseMod::OnImGUIInit() {}

void BaseMod::DrawModMenu() {
    if (ImGui::CollapsingHeader(DISPLAY_NAME.c_str())) {
        ImGui::Text("Player: %p | Motion: %02X",
            g_pPlayer,
            g_pPlayer ? *(uint8_t*)((uintptr_t)g_pPlayer + 0x14) : 0);

        ImGui::Separator();
        ImGui::Text("Weapon Run Bind Settings");

        const char* inputTypes[] = { "Keyboard", "XInput (Xbox)", "DirectInput (Generic/PS/vJoy)" };
        if (ImGui::Combo("Input Device", &g_inputType, inputTypes, IM_ARRAYSIZE(inputTypes))) {
            s_wasBindPressed = false;
            s_listeningForDInput = false;
            s_listeningForXInput = false;
            SaveSettings();
        }

        // --- KEYBOARD ---
        if (g_inputType == 0) {
            const char* kbKeys[] = { "Right Control", "Left Control", "Right Shift", "Left Shift", "Space", "Mouse 4", "Mouse 5" };
            int vkCodes[] = { VK_RCONTROL, VK_LCONTROL, VK_RSHIFT, VK_LSHIFT, VK_SPACE, VK_XBUTTON1, VK_XBUTTON2 };

            int currentKbIndex = 0;
            for (int i = 0; i < IM_ARRAYSIZE(vkCodes); i++) {
                if (vkCodes[i] == g_keyboardBind) { currentKbIndex = i; break; }
            }

            if (ImGui::Combo("Keyboard Key", &currentKbIndex, kbKeys, IM_ARRAYSIZE(kbKeys))) {
                g_keyboardBind = vkCodes[currentKbIndex];
                s_wasBindPressed = false;
                SaveSettings();
            }
        }
        // --- XINPUT ---
        else if (g_inputType == 1) {
            ImGui::Text("XInput Configuration");
            ImGui::Separator();

            if (s_listeningForXInput) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Press a button on your Xbox controller...");
                if (ImGui::Button("Cancel")) {
                    s_listeningForXInput = false;
                }
            }
            else {
                ImGui::Text("Current Bind: %s", GetXInputButtonName(g_xinputBind));
                if (ImGui::Button("Rebind Button")) {
                    s_listeningForXInput = true;
                }
            }
        }
        // --- DIRECTINPUT ---
        else if (g_inputType == 2) {
            ImGui::Text("DirectInput Configuration");

            if (g_pDIDevice) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Device: Connected");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Device: NOT FOUND");
            }

            ImGui::Separator();

            if (s_listeningForDInput) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Press a button on your controller...");
                if (ImGui::Button("Cancel")) {
                    s_listeningForDInput = false;
                }
            }
            else {
                ImGui::Text("Current Bind: Button %d", g_dinputBind + 1);
                if (ImGui::Button("Rebind Button")) {
                    s_listeningForDInput = true;
                }
            }
        }
    }
}

void BaseMod::DrawUI(bool show_menu) {}
void BaseMod::OnUpdateLobby() {}

void BaseMod::OnUpdateQuest() {
    counter++;
    if (!g_pPlayer) return;

    // --- HANDLE XINPUT REBIND LISTENING ---
    if (g_inputType == 1 && s_listeningForXInput) {
        XINPUT_STATE state = { 0 };
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            uint16_t buttons = state.Gamepad.wButtons;
            uint16_t xinputCodes[] = {
                XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
                XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
                XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT
            };
            for (int i = 0; i < IM_ARRAYSIZE(xinputCodes); i++) {
                if (buttons & xinputCodes[i]) {
                    g_xinputBind = xinputCodes[i];
                    s_listeningForXInput = false;
                    s_wasBindPressed = false;
                    SaveSettings();
                    break;
                }
            }
        }
        return;
    }

    // --- HANDLE DIRECTINPUT REBIND LISTENING ---
    if (g_inputType == 2 && s_listeningForDInput) {
        if (g_pDIDevice) {
            g_pDIDevice->Poll();
            HRESULT hr = g_pDIDevice->GetDeviceState(sizeof(DIJOYSTATE2), &g_js);
            if (SUCCEEDED(hr)) {
                for (int i = 0; i < 24; i++) {
                    if (g_js.rgbButtons[i] & 0x80) {
                        g_dinputBind = i;
                        s_listeningForDInput = false;
                        s_wasBindPressed = false;
                        SaveSettings();
                        break;
                    }
                }
            }
            else {
                g_pDIDevice->Acquire();
            }
        }
        return;
    }

    // --- NORMAL BIND LOGIC (For triggering run via key press) ---
    bool isPressed = IsBindHeld();

    if (isPressed && !s_wasBindPressed) {
        uint8_t currentMotion = *(uint8_t*)((uintptr_t)g_pPlayer + 0x14);
        // Prevent spamming if already in the run animation family
        if (currentMotion != 0x67 && currentMotion != 0x66) {
            g_MotionSwitch(g_pPlayer, 0x67);
        }
    }

    s_wasBindPressed = isPressed;
}

extern "C" {
    __declspec(dllexport) Mod* createMod() {
        return new BaseMod(NAME, DISPLAY_NAME, VERSION, REQUIRED_VERSION, HGE_ONLY);
    }
}