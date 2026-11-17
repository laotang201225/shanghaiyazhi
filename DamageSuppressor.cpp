// 0627 standalone dungeon damage suppressor (CNRDActiveObject::setHP hook), Win32.
// Intercepts 0x008174E0 (vtable+0x50C, shared by 302 actor/monster vtables) to scale
// HP loss (currentHP - newHP) for local-player attacks inside combat dungeons,
// leaving town stats, real equipment, and incoming player damage untouched.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

using Word = std::uint32_t;
using Obj = Word*;
static_assert(sizeof(void*) == sizeof(Word), "0627 requires Win32");

constexpr wchar_t kLogName[] = L"gaoDamageSuppressor.log";
constexpr wchar_t kIniName[] = L"gaoDamageSuppressor.ini";
constexpr wchar_t kBootstrapMessage[] = L"GAO.0627.DamageSuppressor.Bootstrap.20261114";

constexpr std::uintptr_t kModuleRoot = 0x01A5FB4C;
constexpr std::uintptr_t kPlayerPtr = 0x01AB7CDC;

constexpr std::uintptr_t kSetHpAddr = 0x008174E0;
constexpr std::uintptr_t kSetHpResume = 0x008174E6;
constexpr BYTE kExpectedSetHpBytes[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};

using SetHpFn = int(__thiscall*)(void* self, int newHp, int flag, void* attacker, Word a6);

struct Config {
    int enabled{1};
    double suppressPercent{99.99};
    int minDamage{0};
    int fractionalAccum{1};
    int allDungeons{1};
    int onlyLocalPlayer{1};
};

struct AccumEntry {
    void* actor;
    double accum;
    DWORD lastTick;
};

constexpr size_t kAccumSlots = 1024;
constexpr size_t kAccumProbeLimit = 8;
constexpr DWORD kAccumIdleResetMs = 60000;

Config g_cfg{};
HWND g_gameWindow{};
DWORD g_uiThread{};
UINT g_bootMessage{};
wchar_t g_logPath[MAX_PATH]{};
wchar_t g_iniPath[MAX_PATH]{};

volatile LONG g_hitLogCount{0};
SRWLOCK g_accumLock = SRWLOCK_INIT;
AccumEntry g_accumTable[kAccumSlots]{};
BYTE* g_trampoline{};
SetHpFn g_origSetHp{};

template<class T> T At(std::uintptr_t va) { return reinterpret_cast<T>(va); }

template<class T> bool ReadAt(std::uintptr_t address, T& value) {
    __try {
        value = *At<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = {};
        return false;
    }
}

void LogLine(const wchar_t* text) {
    FILE* file = nullptr;
    if (g_logPath[0] && _wfopen_s(&file, g_logPath, L"a, ccs=UTF-8") == 0 && file) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        fwprintf(file, L"[%02u:%02u:%02u.%03u pid=%lu tid=%lu] %ls\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentProcessId(), GetCurrentThreadId(), text);
        fclose(file);
    }
}

void LoadConfig() {
    g_cfg.enabled = GetPrivateProfileIntW(L"main", L"enabled", 1, g_iniPath);

    wchar_t pctBuf[64]{};
    GetPrivateProfileStringW(L"main", L"suppress_percent", L"99.99", pctBuf, 64, g_iniPath);
    g_cfg.suppressPercent = wcstod(pctBuf, nullptr);
    if (g_cfg.suppressPercent < 0.0) g_cfg.suppressPercent = 0.0;
    if (g_cfg.suppressPercent > 99.999999) g_cfg.suppressPercent = 99.999999;

    g_cfg.minDamage = GetPrivateProfileIntW(L"main", L"min_damage", 0, g_iniPath);
    if (g_cfg.minDamage < 0) g_cfg.minDamage = 0;

    g_cfg.fractionalAccum = GetPrivateProfileIntW(L"main", L"fractional_accum", 1, g_iniPath);
    g_cfg.allDungeons = GetPrivateProfileIntW(L"main", L"all_dungeons", 1, g_iniPath);
    g_cfg.onlyLocalPlayer = GetPrivateProfileIntW(L"main", L"only_local_player", 1, g_iniPath);

    wchar_t buf[256]{};
    swprintf_s(buf, L"config: enabled=%d suppress_percent=%.6f min_damage=%d fractional_accum=%d all_dungeons=%d only_local=%d",
               g_cfg.enabled, g_cfg.suppressPercent, g_cfg.minDamage, g_cfg.fractionalAccum,
               g_cfg.allDungeons, g_cfg.onlyLocalPlayer);
    LogLine(buf);
}

int CurrentGameModule() {
    Obj root{}, mid{};
    int module = -1;
    if (!ReadAt(kModuleRoot, root) || !root ||
        !ReadAt(reinterpret_cast<std::uintptr_t>(root + 5), mid) || !mid ||
        !ReadAt(reinterpret_cast<std::uintptr_t>(mid + 10), module)) {
        return -1;
    }
    return module;
}

bool IsCombatDungeonModule(int module) {
    if (module == 3) return true;
    if (!g_cfg.allDungeons) return false;
    switch (module) {
    case 7:
    case 9:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x10:
        return true;
    default:
        return false;
    }
}

bool SafeGetCurrentHp(void* actor, int& outHp) {
    outHp = 0;
    if (!actor) return false;
    Word vtable = 0, fnGetHp = 0;
    if (!ReadAt(reinterpret_cast<std::uintptr_t>(actor), vtable) || !vtable ||
        !ReadAt(vtable + 0x2A4, fnGetHp) || !fnGetHp) {
        return false;
    }
    __try {
        using GetHpFn = int(__thiscall*)(void*);
        outHp = reinterpret_cast<GetHpFn>(fnGetHp)(actor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* GetOwnerOrSelf(void* actor) {
    if (!actor) return nullptr;
    void* cur = actor;
    for (int depth = 0; depth < 4; ++depth) {
        Word vtable = 0, fnGetParent = 0;
        if (!ReadAt(reinterpret_cast<std::uintptr_t>(cur), vtable) || !vtable ||
            !ReadAt(vtable + 0x32C, fnGetParent) || !fnGetParent) {
            break;
        }
        void* parent = nullptr;
        __try {
            using Fn = void*(__thiscall*)(void*);
            parent = reinterpret_cast<Fn>(fnGetParent)(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return cur;
}

int AccumulateDamage(void* actor, int curHp, double exactDamage, double& outRemain) {
    outRemain = 0.0;
    if (!actor || exactDamage <= 0.0) return 0;

    const DWORD now = GetTickCount();
    const size_t baseIdx =
        static_cast<size_t>((reinterpret_cast<std::uintptr_t>(actor) >> 4) * 2654435761u) &
        (kAccumSlots - 1);

    AcquireSRWLockExclusive(&g_accumLock);

    AccumEntry* target = nullptr;
    AccumEntry* oldest = &g_accumTable[baseIdx];
    for (size_t probe = 0; probe < kAccumProbeLimit; ++probe) {
        AccumEntry& entry = g_accumTable[(baseIdx + probe) & (kAccumSlots - 1)];
        if (entry.actor == actor) {
            if ((now - entry.lastTick) > kAccumIdleResetMs) {
                entry.accum = 0.0;
            }
            target = &entry;
            break;
        }
        if (entry.actor == nullptr) {
            target = &entry;
            target->actor = actor;
            target->accum = 0.0;
            break;
        }
        if ((now - entry.lastTick) > (now - oldest->lastTick)) {
            oldest = &entry;
        }
    }

    if (!target) {
        target = oldest;
        target->actor = actor;
        target->accum = 0.0;
    }

    const double total = target->accum + exactDamage;
    int intDamage = static_cast<int>(total);
    if (intDamage < 0) intDamage = 0;

    target->accum = total - static_cast<double>(intDamage);
    target->lastTick = now;
    outRemain = target->accum;

    if (curHp - intDamage <= 0) {
        target->actor = nullptr;
        target->accum = 0.0;
    }

    ReleaseSRWLockExclusive(&g_accumLock);
    return intDamage;
}

int __fastcall Hook_SetHp(void* self, void*, int newHp, int flag, void* attacker, Word a6) {
    int adjustedNewHp = newHp;
    int curHp = 0;

    if (g_cfg.enabled && g_cfg.suppressPercent > 0.0 && SafeGetCurrentHp(self, curHp)) {
        const int rawDamage = curHp - newHp;
        const int module = CurrentGameModule();
        if (rawDamage > 0 && IsCombatDungeonModule(module)) {
            void* localPlayer = nullptr;
            ReadAt(kPlayerPtr, localPlayer);
            if (localPlayer && self != localPlayer) {
                void* topOwner = GetOwnerOrSelf(attacker);
                const bool isLocalAttack = (attacker == localPlayer || topOwner == localPlayer);
                if (!g_cfg.onlyLocalPlayer || isLocalAttack) {
                    const double scale = (100.0 - g_cfg.suppressPercent) / 100.0;
                    const double exactDamage = static_cast<double>(rawDamage) * scale;
                    int newDamage = 0;
                    double remain = 0.0;

                    if (g_cfg.fractionalAccum) {
                        newDamage = AccumulateDamage(self, curHp, exactDamage, remain);
                    } else {
                        newDamage = static_cast<int>(exactDamage + 0.5);
                        if (newDamage < g_cfg.minDamage) {
                            newDamage = g_cfg.minDamage;
                        }
                    }

                    adjustedNewHp = curHp - newDamage;

                    const LONG seq = InterlockedIncrement(&g_hitLogCount);
                    if (seq <= 40) {
                        wchar_t buf[256]{};
                        swprintf_s(buf, L"setHP#%ld mod=%d cur=%d reqNew=%d rawDmg=%d exact=%.4f rem=%.4f -> newDmg=%d adjNew=%d atk=%p top=%p lp=%p def=%p",
                                   seq, module, curHp, newHp, rawDamage, exactDamage, remain, newDamage, adjustedNewHp,
                                   attacker, topOwner, localPlayer, self);
                        LogLine(buf);
                    }
                } else {
                    const LONG seq = InterlockedIncrement(&g_hitLogCount);
                    if (seq <= 20) {
                        wchar_t buf[256]{};
                        swprintf_s(buf, L"skip#%ld non-local cur=%d rawDmg=%d atk=%p top=%p lp=%p def=%p",
                                   seq, curHp, rawDamage, attacker, topOwner, localPlayer, self);
                        LogLine(buf);
                    }
                }
            }
        }
    }

    return g_origSetHp(self, adjustedNewHp, flag, attacker, a6);
}

bool InstallSetHpHook() {
    BYTE actual[6]{};
    for (int i = 0; i < 6; ++i) {
        if (!ReadAt(kSetHpAddr + i, actual[i])) {
            LogLine(L"setHP hook: failed to read prologue");
            return false;
        }
    }
    if (std::memcmp(actual, kExpectedSetHpBytes, 6) != 0) {
        wchar_t buf[160]{};
        swprintf_s(buf, L"setHP hook: prologue mismatch (%02X %02X %02X %02X %02X %02X)",
                   actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
        LogLine(buf);
        return false;
    }

    g_trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampoline) {
        LogLine(L"setHP hook: VirtualAlloc failed");
        return false;
    }
    std::memcpy(g_trampoline, kExpectedSetHpBytes, 6);
    g_trampoline[6] = 0xE9;
    const std::int32_t backRel = static_cast<std::int32_t>(
        kSetHpResume - (reinterpret_cast<std::uintptr_t>(g_trampoline) + 11));
    std::memcpy(g_trampoline + 7, &backRel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, 16);
    g_origSetHp = reinterpret_cast<SetHpFn>(g_trampoline);

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kSetHpAddr), 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogLine(L"setHP hook: VirtualProtect failed");
        return false;
    }
    BYTE patch[6]{};
    patch[0] = 0xE9;
    const std::int32_t rel = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(&Hook_SetHp) - (kSetHpAddr + 5));
    std::memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;
    std::memcpy(reinterpret_cast<void*>(kSetHpAddr), patch, 6);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kSetHpAddr), 6);
    VirtualProtect(reinterpret_cast<void*>(kSetHpAddr), 6, oldProt, &oldProt);

    LogLine(L"hook SetHp installed at 0x008174E0");
    return true;
}

LRESULT CALLBACK Bootstrap(int code, WPARAM w, LPARAM l) {
    if (code >= 0 && GetCurrentThreadId() == g_uiThread &&
        reinterpret_cast<const CWPSTRUCT*>(l)->message == g_bootMessage) {
        LoadConfig();
        InstallSetHpHook();
    }
    return CallNextHookEx(nullptr, code, w, l);
}

BOOL CALLBACK FindGameWindow(HWND window, LPARAM state) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    RECT bounds{};
    if (pid == GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window, GW_OWNER) &&
        GetClientRect(window, &bounds) && bounds.right >= 600 && bounds.bottom >= 420) {
        *reinterpret_cast<HWND*>(state) = window;
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI Start(void* module) {
    wchar_t exe[MAX_PATH]{};
    const auto main = GetModuleHandleW(nullptr);
    if (!GetModuleFileNameW(main, exe, MAX_PATH) || !wcsrchr(exe, L'\\') ||
        _wcsicmp(wcsrchr(exe, L'\\') + 1, L"dnf.exe") != 0 ||
        reinterpret_cast<std::uintptr_t>(main) != 0x00400000) {
        return 0;
    }

    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(module), &pinned)) {
        return 0;
    }

    if (GetModuleFileNameW(pinned, g_logPath, MAX_PATH)) {
        wcscpy_s(g_iniPath, g_logPath);
        if (const auto name = wcsrchr(g_logPath, L'\\')) {
            wcscpy_s(name + 1, MAX_PATH - (name + 1 - g_logPath), kLogName);
        }
        if (const auto iniName = wcsrchr(g_iniPath, L'\\')) {
            wcscpy_s(iniName + 1, MAX_PATH - (iniName + 1 - g_iniPath), kIniName);
        }
    }

    LogLine(L"start");
    for (int i = 0; i < 300; ++i) {
        HWND window{};
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window) {
            g_gameWindow = window;
            g_uiThread = GetWindowThreadProcessId(window, nullptr);
            LogLine(L"game window found, dispatching to UI thread");
            g_bootMessage = RegisterWindowMessageW(kBootstrapMessage);
            HHOOK boot = g_bootMessage ? SetWindowsHookExW(WH_CALLWNDPROC, &Bootstrap, nullptr, g_uiThread) : nullptr;
            if (boot) {
                DWORD_PTR result{};
                if (!SendMessageTimeoutW(window, g_bootMessage, 0, 0,
                                         SMTO_BLOCK | SMTO_ABORTIFHUNG, 5000, &result)) {
                    LogLine(L"UI dispatch timeout or error");
                }
                UnhookWindowsHookEx(boot);
            } else {
                LogLine(L"UI hook failed");
            }
            return 0;
        }
        Sleep(100);
    }

    LogLine(L"game window not found");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, &Start, module, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
