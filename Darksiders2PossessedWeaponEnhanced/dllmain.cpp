// ============================================================================
// Darksiders 2 - Possessed Weapon Enhanced - v1.0
//
// Extends the INVALID ID fix with a per-weapon prestige system.
// Uses the engine's own "equipped possessed weapon" pointer at +0x370,
// captured via a hook on the SET instruction (+0x493914).
//
// Release build: no diagnostic logs.
// ============================================================================

#include "pch.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <fstream>
#include <sstream>
#include <intrin.h>
#include "MinHook.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const int    BLOCK_THRESHOLD = 7;
const int    ATTR_TYPE_CRITICAL_DAMAGE = 0x47;
const int    ATTR_TYPE_ARCANE_CRITICAL_DAMAGE = 0x49;
const int    ATTR_TYPE_PIERCING_DAMAGE = 0x53;
const int    ATTR_TYPE_HEALTH_STEAL = 0x5D;

const uintptr_t OFFSET_PLAYER_CD = 0x890;
const uintptr_t OFFSET_PLAYER_ACD = 0x898;
const uintptr_t OFFSET_CONTAINER_EQUIPPED = 0x370;

const char* CONFIG_FILE = "Darksiders2PossessedWeaponEnhanced.ini";
const char* PRESTIGE_FILE = "Darksiders2PrestigeData.ini";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
float g_PrestigeBonusPerLevel = 0.2872f;
float g_MaxCDPercent = 500.0f;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
volatile uintptr_t g_DeathStatCaptured = 0;
void* g_CameraCave = nullptr;
uintptr_t  g_CameraHookAddr = 0;
volatile bool g_ShuttingDown = false;

CRITICAL_SECTION g_CacheCS;
volatile LONG g_CacheCSInit = 0;

std::map<uintptr_t, int> g_CachedCD;
std::map<uintptr_t, int> g_CachedACD;
std::map<int, int> g_BaselineByCD;

// Equip hook signals
volatile uintptr_t g_EquipSetPtr = 0;
volatile uintptr_t g_EquipSetContainer = 0;
volatile LONG      g_EquipSetSeq = 0;

// Active possessed weapon (from the SET hook)
volatile uintptr_t g_EquippedPossessedPtr = 0;
volatile uintptr_t g_LastSetContainer = 0;

static volatile LONG g_InsideSave = 0;

static void* g_EquipSetTrampoline = nullptr;
static unsigned char* g_EquipSetShellcode = nullptr;

// ============================================================================
// UTIL
// ============================================================================
inline float ComputeMultiplier(int prestige) {
    if (prestige <= 0) return 1.0f;
    return 1.0f + (prestige * g_PrestigeBonusPerLevel);
}

inline int ClampBoosted(int raw, float mult) {
    int boosted = (int)(raw * mult);
    if (boosted > (int)g_MaxCDPercent) boosted = (int)g_MaxCDPercent;
    return boosted;
}

static bool IsPossessedWeapon(uintptr_t wp) {
    if (wp < 0x10000 || wp > 0x7FFFFFFFFFFF) return false;

    int rarity = 0;
    __try { rarity = *(int*)(wp + 0x308); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (rarity != 5) return false;

    uintptr_t attrArray = 0;
    __try { attrArray = *(uintptr_t*)(wp + 0x338); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (attrArray < 0x10000 || attrArray > 0x7FFFFFFFFFFF) return false;

    return true;
}

static void CacheCD(uintptr_t wp, int v) {
    if (InterlockedCompareExchange(&g_CacheCSInit, 1, 1) == 0) return;
    EnterCriticalSection(&g_CacheCS);
    g_CachedCD[wp] = v;
    LeaveCriticalSection(&g_CacheCS);
}

static void CacheACD(uintptr_t wp, int v) {
    if (InterlockedCompareExchange(&g_CacheCSInit, 1, 1) == 0) return;
    EnterCriticalSection(&g_CacheCS);
    g_CachedACD[wp] = v;
    LeaveCriticalSection(&g_CacheCS);
}

static bool GetCachedCD(uintptr_t wp, int* out) {
    if (!out) return false;
    if (InterlockedCompareExchange(&g_CacheCSInit, 1, 1) == 0) return false;
    EnterCriticalSection(&g_CacheCS);
    auto it = g_CachedCD.find(wp);
    bool found = (it != g_CachedCD.end());
    if (found) *out = it->second;
    LeaveCriticalSection(&g_CacheCS);
    return found;
}

static uint32_t ReadCounter(uintptr_t weaponPtr) {
    uintptr_t slotsPtr = 0;
    __try { slotsPtr = *(uintptr_t*)(weaponPtr + 0x338); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (slotsPtr < 0x10000 || slotsPtr > 0x7FFFFFFFFFFF) return 0;

    uintptr_t ep0 = 0;
    __try { ep0 = *(uintptr_t*)slotsPtr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (ep0 < 0x10000 || ep0 > 0x7FFFFFFFFFFF) return 0;

    uint32_t c0 = 0;
    __try { c0 = *(uint32_t*)(ep0 + 0xA4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return c0;
}

static int GetPrestigeFromWeapon(uintptr_t weaponPtr) {
    int cd = 0;
    if (!GetCachedCD(weaponPtr, &cd) || cd <= 0) return 0;

    auto baseIt = g_BaselineByCD.find(cd);
    if (baseIt == g_BaselineByCD.end()) return 0;

    uint32_t c0 = ReadCounter(weaponPtr);
    if (c0 == 0) return 0;

    int p = (int)c0 - baseIt->second;
    return (p > 0) ? p : 0;
}

// ============================================================================
// PERSISTENCE
// ============================================================================
static std::string TrimStr(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void LoadPrestigeData() {
    g_BaselineByCD.clear();
    std::ifstream file(PRESTIGE_FILE);
    if (!file.is_open()) {
        OutputDebugStringA("[Prestige] No data file yet.\n");
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::string t = TrimStr(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        std::istringstream iss(t);
        int cd; int baseline;
        if (iss >> cd >> baseline) {
            if (cd > 0 && cd < 10000 && baseline > 0 && baseline < 10000)
                g_BaselineByCD[cd] = baseline;
        }
    }
    file.close();
    char msg[180];
    sprintf_s(msg, "[Prestige] Loaded %d baseline records.\n", (int)g_BaselineByCD.size());
    OutputDebugStringA(msg);
}

void SavePrestigeData() {
    if (g_ShuttingDown) return;
    if (InterlockedExchange(&g_InsideSave, 1) != 0) return;
    {
        std::ofstream file(PRESTIGE_FILE);
        if (file.is_open()) {
            file << "# Darksiders 2 Possessed Weapon Enhanced\n";
            file << "# Format: <raw_cd_value> <counter_at_cap>\n";
            for (const auto& kv : g_BaselineByCD) {
                file << kv.first << " " << kv.second << "\n";
            }
            file.close();
        }
    }
    InterlockedExchange(&g_InsideSave, 0);
}

// ============================================================================
// CONFIG
// ============================================================================
void SaveConfigToFile() {
    std::ofstream file(CONFIG_FILE);
    if (!file.is_open()) return;
    file << "# Darksiders 2 - Possessed Weapon Enhanced\n";
    file << "# Auto-generated on first run. Edit values and restart the game.\n";
    file << "\n";
    file << "# Bonus per prestige level (0.2872 = +28.72% per level)\n";
    file << "PrestigeBonusPerLevel=0.2872\n";
    file << "\n";
    file << "# Absolute cap for displayed CD/ACD on tooltip (500 = 500%)\n";
    file << "MaxCDPercent=500.0\n";
    file.close();
    OutputDebugStringA("[Config] Default config written.\n");
}

void LoadConfigFromFile() {
    g_PrestigeBonusPerLevel = 0.2872f;
    g_MaxCDPercent = 500.0f;

    std::ifstream file(CONFIG_FILE);
    if (!file.is_open()) {
        OutputDebugStringA("[Config] File not found, using defaults\n");
        SaveConfigToFile();
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::string t = TrimStr(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = TrimStr(t.substr(0, eq));
        std::string val = TrimStr(t.substr(eq + 1));
        if (key == "PrestigeBonusPerLevel") {
            float v = (float)atof(val.c_str());
            if (v > 0.0f && v <= 100.0f) g_PrestigeBonusPerLevel = v;
        }
        else if (key == "MaxCDPercent") {
            float v = (float)atof(val.c_str());
            if (v > 0.0f && v <= 100000.0f) g_MaxCDPercent = v;
        }
    }
    file.close();
    char msg[240];
    sprintf_s(msg, "[Config] Loaded - Bonus=%.4f | MaxCD=%.0f%%\n",
        g_PrestigeBonusPerLevel, g_MaxCDPercent);
    OutputDebugStringA(msg);
}

// ============================================================================
// AOB SCAN
// ============================================================================
uintptr_t AOBScan(const unsigned char* pattern, size_t len) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!base) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t start = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        for (size_t i = 0; i + len < size; i++)
            if (memcmp((void*)(start + i), pattern, len) == 0) return start + i;
    }
    return 0;
}

// ============================================================================
// SHUTDOWN HOOKS
// ============================================================================
typedef VOID(WINAPI* tExitProcess)(UINT);
tExitProcess oExitProcess = nullptr;
VOID WINAPI Hooked_ExitProcess(UINT c) {
    g_ShuttingDown = true; Sleep(1500); oExitProcess(c);
}
typedef BOOL(WINAPI* tTerminateProcess)(HANDLE, UINT);
tTerminateProcess oTerminateProcess = nullptr;
BOOL WINAPI Hooked_TerminateProcess(HANDLE h, UINT c) {
    g_ShuttingDown = true; Sleep(1500); return oTerminateProcess(h, c);
}
bool InstallShutdownHooks() {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return false;
    LPVOID p1 = GetProcAddress(k, "ExitProcess");
    LPVOID p2 = GetProcAddress(k, "TerminateProcess");
    if (p1) { MH_CreateHook(p1, &Hooked_ExitProcess, (LPVOID*)&oExitProcess); MH_EnableHook(p1); }
    if (p2) { MH_CreateHook(p2, &Hooked_TerminateProcess, (LPVOID*)&oTerminateProcess); MH_EnableHook(p2); }
    return true;
}

// ============================================================================
// SAVE HOOK
// ============================================================================
typedef HANDLE(WINAPI* tCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* tCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef LONG(NTAPI* tNtCreateFile)(PHANDLE, ULONG, PVOID, PVOID, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

tCreateFileW oCreateFileW = nullptr;
tCreateFileA oCreateFileA = nullptr;
tNtCreateFile oNtCreateFile = nullptr;

static bool IsDsav(const wchar_t* wpath) {
    if (!wpath) return false;
    size_t len = wcslen(wpath);
    return (len >= 5 && _wcsicmp(wpath + len - 5, L".dsav") == 0);
}

HANDLE WINAPI Hooked_CreateFileW(LPCWSTR fn, DWORD da, DWORD sm, LPSECURITY_ATTRIBUTES sa,
    DWORD cd, DWORD fa, HANDLE ht) {
    HANDLE r = oCreateFileW(fn, da, sm, sa, cd, fa, ht);
    if (!g_ShuttingDown && r != INVALID_HANDLE_VALUE && fn && (da & GENERIC_WRITE) && IsDsav(fn))
        SavePrestigeData();
    return r;
}

HANDLE WINAPI Hooked_CreateFileA(LPCSTR fn, DWORD da, DWORD sm, LPSECURITY_ATTRIBUTES sa,
    DWORD cd, DWORD fa, HANDLE ht) {
    wchar_t wpath[MAX_PATH] = { 0 };
    if (fn) MultiByteToWideChar(CP_ACP, 0, fn, -1, wpath, MAX_PATH);
    HANDLE r = oCreateFileA(fn, da, sm, sa, cd, fa, ht);
    if (!g_ShuttingDown && r != INVALID_HANDLE_VALUE && fn && (da & GENERIC_WRITE) && IsDsav(wpath))
        SavePrestigeData();
    return r;
}

typedef struct _MY_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} MY_UNICODE_STRING;

typedef struct _MY_OBJECT_ATTRIBUTES {
    ULONG                Length;
    HANDLE               RootDirectory;
    MY_UNICODE_STRING* ObjectName;
    ULONG                Attributes;
    PVOID                SecurityDescriptor;
    PVOID                SecurityQualityOfService;
} MY_OBJECT_ATTRIBUTES;

#define MY_FILE_OPEN         1
#define MY_FILE_OPEN_IF      3

LONG NTAPI Hooked_NtCreateFile(
    PHANDLE FileHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
    PVOID IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
    PVOID EaBuffer, ULONG EaLength) {

    bool isDsav = false;
    if (ObjectAttributes) {
        MY_OBJECT_ATTRIBUTES* oa = (MY_OBJECT_ATTRIBUTES*)ObjectAttributes;
        if (oa->ObjectName && oa->ObjectName->Buffer && oa->ObjectName->Length >= 10) {
            USHORT byteLen = oa->ObjectName->Length;
            PWSTR  buf = oa->ObjectName->Buffer;
            size_t wlen = byteLen / sizeof(wchar_t);
            if (wlen >= 5) {
                if (buf[wlen - 5] == L'.' && (buf[wlen - 4] | 0x20) == L'd' &&
                    (buf[wlen - 3] | 0x20) == L's' && (buf[wlen - 2] | 0x20) == L'a' &&
                    (buf[wlen - 1] | 0x20) == L'v') {
                    isDsav = true;
                }
            }
        }
    }

    LONG status = oNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, AllocationSize, FileAttributes, ShareAccess,
        CreateDisposition, CreateOptions, EaBuffer, EaLength);

    bool isWrite = (CreateDisposition != MY_FILE_OPEN && CreateDisposition != MY_FILE_OPEN_IF);
    if (isDsav && isWrite && status >= 0 && !g_ShuttingDown)
        SavePrestigeData();

    return status;
}

bool InstallSaveHook() {
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    if (!kb) kb = LoadLibraryA("kernelbase.dll");
    if (kb) {
        LPVOID pw = GetProcAddress(kb, "CreateFileW");
        LPVOID pa = GetProcAddress(kb, "CreateFileA");
        if (pw) { MH_CreateHook(pw, &Hooked_CreateFileW, (LPVOID*)&oCreateFileW); MH_EnableHook(pw); }
        if (pa) { MH_CreateHook(pa, &Hooked_CreateFileA, (LPVOID*)&oCreateFileA); MH_EnableHook(pa); }
    }
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt) {
        LPVOID pn = GetProcAddress(nt, "NtCreateFile");
        if (pn) {
            if (MH_CreateHook(pn, &Hooked_NtCreateFile, (LPVOID*)&oNtCreateFile) == MH_OK)
                MH_EnableHook(pn);
        }
    }
    return true;
}

// ============================================================================
// CAMERA HOOK
// ============================================================================
bool InstallCameraHook() {
    uintptr_t ba = (uintptr_t)GetModuleHandleA(NULL);
    if (!ba) return false;
    const unsigned char pat[] = { 0x89, 0x42, 0x3C, 0x48, 0x8B, 0xC2, 0xC3, 0xCC, 0x48, 0x8B };
    g_CameraHookAddr = AOBScan(pat, sizeof(pat));
    if (g_CameraHookAddr == 0) return false;

    for (int64_t off = 0x10000; off < 0x70000000 && !g_CameraCave; off += 0x10000) {
        g_CameraCave = VirtualAlloc((LPVOID)(g_CameraHookAddr + off), 0x1000,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_CameraCave) break;
        uintptr_t td = g_CameraHookAddr - off;
        if (td > 0x10000)
            g_CameraCave = VirtualAlloc((LPVOID)td, 0x1000,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!g_CameraCave) return false;

    unsigned char code[160];
    int off = 0;
    int jzList[8]; int jzCount = 0;

    code[off++] = 0x48; code[off++] = 0x85; code[off++] = 0xC9;
    code[off++] = 0x74; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x48; code[off++] = 0x8B; code[off++] = 0x81;
    code[off++] = 0x70; code[off++] = 0x01; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0x48; code[off++] = 0x85; code[off++] = 0xC0;
    code[off++] = 0x74; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x48; code[off++] = 0x3D;
    code[off++] = 0x00; code[off++] = 0x00; code[off++] = 0x01; code[off++] = 0x00;
    code[off++] = 0x72; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x44; code[off++] = 0x8B; code[off++] = 0x90;
    code[off++] = 0x90; code[off++] = 0x08; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0x41; code[off++] = 0x83; code[off++] = 0xFA; code[off++] = 0x01;
    code[off++] = 0x72; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x41; code[off++] = 0x81; code[off++] = 0xFA;
    code[off++] = 0xF4; code[off++] = 0x01; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0x77; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x44; code[off++] = 0x8B; code[off++] = 0x90;
    code[off++] = 0x8C; code[off++] = 0x08; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0x41; code[off++] = 0x81; code[off++] = 0xFA;
    code[off++] = 0x10; code[off++] = 0x27; code[off++] = 0x00; code[off++] = 0x00;
    code[off++] = 0x77; jzList[jzCount++] = off; off += 1;
    code[off++] = 0x49; code[off++] = 0xBB;
    uint64_t addr = (uint64_t)&g_DeathStatCaptured;
    memcpy(&code[off], &addr, 8); off += 8;
    code[off++] = 0x49; code[off++] = 0x89; code[off++] = 0x0B;

    int skipTarget = off;
    code[off++] = 0x89; code[off++] = 0x42; code[off++] = 0x3C;
    code[off++] = 0x48; code[off++] = 0x8B; code[off++] = 0xC2;
    code[off++] = 0xE9;
    int jmpDispOff = off; off += 4;

    for (int i = 0; i < jzCount; i++)
        code[jzList[i]] = (unsigned char)(skipTarget - (jzList[i] + 1));

    memcpy(g_CameraCave, code, off);
    uint64_t target = g_CameraHookAddr + 6;
    int32_t jmpDisp = (int32_t)(target - ((uintptr_t)g_CameraCave + jmpDispOff + 4));
    memcpy((char*)g_CameraCave + jmpDispOff, &jmpDisp, 4);

    DWORD oldProtect;
    if (!VirtualProtect((void*)g_CameraHookAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    unsigned char patch[6];
    patch[0] = 0xE9;
    int32_t rel32 = (int32_t)((uintptr_t)g_CameraCave - (g_CameraHookAddr + 5));
    memcpy(patch + 1, &rel32, 4);
    patch[5] = 0x90;
    memcpy((void*)g_CameraHookAddr, patch, 6);
    VirtualProtect((void*)g_CameraHookAddr, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_CameraHookAddr, 6);
    FlushInstructionCache(GetCurrentProcess(), g_CameraCave, off);
    return true;
}

// ============================================================================
// EQUIP HOOK (+0x493914)
//
//   mov [rdi+0x370], rbx    -- stores the newly equipped possessed weapon
//
// Fires when the engine equips a possessed weapon. Captures the weaponPtr
// (rbx) and the container (rdi). We read [container+0x370] each frame to
// get the live state (handles CLEAR and item-equip recalcs transparently).
// ============================================================================
bool InstallEquipHook() {
    uintptr_t ba = (uintptr_t)GetModuleHandleA(NULL);
    if (!ba) return false;

    uintptr_t addr = ba + 0x493914;
    const unsigned char expected[] = { 0x48, 0x89, 0x9F, 0x70, 0x03, 0x00, 0x00 };
    unsigned char actual[7];
    memcpy(actual, (void*)addr, 7);
    if (memcmp(actual, expected, 7) != 0) {
        OutputDebugStringA("[EquipHook] Pattern mismatch - hook not installed.\n");
        return false;
    }

    g_EquipSetShellcode = (unsigned char*)VirtualAlloc(nullptr, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_EquipSetShellcode) return false;

    unsigned char c[128];
    int o = 0;
    c[o++] = 0x50;   // push rax
    c[o++] = 0x53;   // push rbx
    c[o++] = 0x51;   // push rcx
    c[o++] = 0x52;   // push rdx
    c[o++] = 0x9C;   // pushfq

    // mov rax, &g_EquipSetPtr ; mov [rax], rbx
    c[o++] = 0x48; c[o++] = 0xB8;
    { uint64_t a = (uint64_t)&g_EquipSetPtr; memcpy(&c[o], &a, 8); o += 8; }
    c[o++] = 0x48; c[o++] = 0x89; c[o++] = 0x18;

    // mov rax, &g_EquipSetContainer ; mov [rax], rdi
    c[o++] = 0x48; c[o++] = 0xB8;
    { uint64_t a = (uint64_t)&g_EquipSetContainer; memcpy(&c[o], &a, 8); o += 8; }
    c[o++] = 0x48; c[o++] = 0x89; c[o++] = 0x38;

    // mov rax, &g_EquipSetSeq ; lock inc dword ptr [rax]
    c[o++] = 0x48; c[o++] = 0xB8;
    { uint64_t a = (uint64_t)&g_EquipSetSeq; memcpy(&c[o], &a, 8); o += 8; }
    c[o++] = 0xF0; c[o++] = 0xFF; c[o++] = 0x00;

    c[o++] = 0x9D;   // popfq
    c[o++] = 0x5A;   // pop rdx
    c[o++] = 0x59;   // pop rcx
    c[o++] = 0x5B;   // pop rbx
    c[o++] = 0x58;   // pop rax

    // original: mov [rdi+0x370], rbx
    c[o++] = 0x48; c[o++] = 0x89; c[o++] = 0x9F;
    c[o++] = 0x70; c[o++] = 0x03; c[o++] = 0x00; c[o++] = 0x00;

    // jmp qword ptr [rip+0]
    c[o++] = 0xFF; c[o++] = 0x25;
    c[o++] = 0x00; c[o++] = 0x00; c[o++] = 0x00; c[o++] = 0x00;
    int tramp_off = o;
    memset(&c[o], 0xCC, 8); o += 8;

    memcpy(g_EquipSetShellcode, c, o);
    FlushInstructionCache(GetCurrentProcess(), g_EquipSetShellcode, o);

    MH_STATUS st = MH_CreateHook((LPVOID)addr, g_EquipSetShellcode, &g_EquipSetTrampoline);
    if (st != MH_OK) {
        OutputDebugStringA("[EquipHook] MH_CreateHook failed.\n");
        return false;
    }

    *(uint64_t*)&g_EquipSetShellcode[tramp_off] = (uint64_t)g_EquipSetTrampoline;
    FlushInstructionCache(GetCurrentProcess(), g_EquipSetShellcode, o);

    st = MH_EnableHook((LPVOID)addr);
    if (st != MH_OK) {
        OutputDebugStringA("[EquipHook] MH_EnableHook failed.\n");
        return false;
    }

    OutputDebugStringA("[EquipHook] Installed.\n");
    return true;
}

// ============================================================================
// HOOK 1: Fix INVALID ID + register baseline
// ============================================================================
typedef void(__fastcall* tAddToArray)(uintptr_t, uintptr_t);
tAddToArray oAddToArray = nullptr;

void __fastcall Hooked_AddToArray(uintptr_t pArray, uintptr_t pItem) {
    uintptr_t ra = (uintptr_t)_ReturnAddress();
    uintptr_t ba = (uintptr_t)GetModuleHandleA(NULL);
    if (ra == (ba + 0x5BE300)) {
        int* sp = (int*)(pArray + 0x08);
        if (*sp >= BLOCK_THRESHOLD) {
            uintptr_t wp = pArray - 0x338;
            int cd = 0;
            if (GetCachedCD(wp, &cd) && cd > 0) {
                if (g_BaselineByCD.find(cd) == g_BaselineByCD.end()) {
                    uint32_t c0 = ReadCounter(wp);
                    if (c0 > 0 && c0 < 10000) {
                        g_BaselineByCD[cd] = c0;
                        SavePrestigeData();
                        char msg[220];
                        sprintf_s(msg, "[BASELINE] cd=%d baseline=%u stored\n", cd, c0);
                        OutputDebugStringA(msg);
                    }
                }
            }
            return;
        }
    }
    oAddToArray(pArray, pItem);
}

// ============================================================================
// HOOK 2: UI attribute calculation (+0x2DD898)
// ============================================================================
typedef char(__fastcall* tCalcAttr)(int64_t*, uint64_t, int*, uint64_t*, int*, uint64_t*);
tCalcAttr oCalcAttr = nullptr;

char __fastcall Hooked_CalcAttr(int64_t* pArray, uint64_t p2, int* pOutType,
    uint64_t* pData, int* pOutValue, uint64_t* p6) {
    char result = oCalcAttr(pArray, p2, pOutType, pData, pOutValue, p6);
    if (result == 1 && pOutType && pOutValue) {
        uintptr_t wp = (uintptr_t)pArray;
        int at = *pOutType;

        if (at == ATTR_TYPE_PIERCING_DAMAGE || at == ATTR_TYPE_HEALTH_STEAL) {
            int dummy = 0;
            if (!GetCachedCD(wp, &dummy)) {
                CacheCD(wp, 0);
            }
        }

        if (at == ATTR_TYPE_CRITICAL_DAMAGE || at == ATTR_TYPE_ARCANE_CRITICAL_DAMAGE) {
            if (at == ATTR_TYPE_CRITICAL_DAMAGE) CacheCD(wp, *pOutValue);
            else CacheACD(wp, *pOutValue);

            int prestige = GetPrestigeFromWeapon(wp);
            if (prestige > 0) {
                float mult = ComputeMultiplier(prestige);
                *pOutValue = ClampBoosted(*pOutValue, mult);
            }
        }
    }
    return result;
}

// ============================================================================
// HOOK 3: UI apply value (+0x2DDD14)
// ============================================================================
typedef void(__fastcall* tApplyValue)(int64_t*, int, int, char*, int64_t, int64_t, int64_t);
tApplyValue oApplyValue = nullptr;

void __fastcall Hooked_ApplyValue(int64_t* param_1, int attrType, int value,
    char* pFlag, int64_t p5, int64_t p6, int64_t p7) {

    int originalValue = value;

    if (param_1 && (attrType == ATTR_TYPE_PIERCING_DAMAGE ||
        attrType == ATTR_TYPE_HEALTH_STEAL)) {
        uintptr_t wp = (uintptr_t)param_1;
        int dummy = 0;
        if (!GetCachedCD(wp, &dummy)) {
            CacheCD(wp, 0);
        }
    }

    if (attrType == ATTR_TYPE_CRITICAL_DAMAGE ||
        attrType == ATTR_TYPE_ARCANE_CRITICAL_DAMAGE) {

        uintptr_t wp = (uintptr_t)param_1;
        if (attrType == ATTR_TYPE_CRITICAL_DAMAGE) CacheCD(wp, value);
        else CacheACD(wp, value);

        int prestige = GetPrestigeFromWeapon(wp);
        if (prestige > 0) {
            float mult = ComputeMultiplier(prestige);
            value = ClampBoosted(value, mult);
        }
    }

    oApplyValue(param_1, attrType, value, pFlag, p5, p6, p7);
}

// ============================================================================
// HOOKS 4-6 (pass-through)
// ============================================================================
typedef void(__fastcall* tCombatStats)(int64_t*);
tCombatStats oCombatStats = nullptr;
void __fastcall Hooked_CombatStats(int64_t* p) { oCombatStats(p); }

typedef void(__fastcall* tIterateAttrs)(int64_t*, int64_t, uint64_t, uint64_t);
tIterateAttrs oIterateAttrs = nullptr;
void __fastcall Hooked_IterateAttrs(int64_t* a, int64_t b, uint64_t c, uint64_t d) {
    oIterateAttrs(a, b, c, d);
}

typedef void(__fastcall* tApplyAttribute)(int64_t*, uint64_t, int64_t*);
tApplyAttribute oApplyAttribute = nullptr;
void __fastcall Hooked_ApplyAttribute(int64_t* a, uint64_t b, int64_t* c) {
    oApplyAttribute(a, b, c);
}

// ============================================================================
// CD MODIFIER THREAD
//
// Reads the live field [container+0x370] every tick. Only boosts if the
// field still holds the last SET pointer (i.e., the engine hasn't cleared it).
// ============================================================================
DWORD WINAPI CDModifierThread(LPVOID) {
    Sleep(10000);
    const int BASE = 100;

    int vanillaField = BASE;
    int lastWritten = -1;
    LONG lastSetSeq = 0;

    while (!g_ShuttingDown) {
        Sleep(16);
        if (g_ShuttingDown) break;

        uintptr_t captured = g_DeathStatCaptured;
        if (captured == 0) continue;

        uintptr_t ps = 0;
        __try { ps = *(uintptr_t*)(captured + 0x170); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (ps < 0x10000 || ps > 0x7FFFFFFFFFFF) continue;

        // Process new SET events
        LONG curSetSeq = g_EquipSetSeq;
        if (curSetSeq != lastSetSeq) {
            lastSetSeq = curSetSeq;
            uintptr_t newPtr = g_EquipSetPtr;
            uintptr_t container = g_EquipSetContainer;

            if (newPtr != 0 && IsPossessedWeapon(newPtr)) {
                g_EquippedPossessedPtr = newPtr;
                g_LastSetContainer = container;
            }
        }

        // Read the live field. If the engine cleared it (or the container
        // is stale), liveVal won't match — no boost.
        uintptr_t wp = 0;
        if (g_LastSetContainer != 0) {
            uintptr_t liveVal = 0;
            __try { liveVal = *(uintptr_t*)(g_LastSetContainer + OFFSET_CONTAINER_EQUIPPED); }
            __except (EXCEPTION_EXECUTE_HANDLER) { liveVal = 0; }

            if (liveVal != 0 && liveVal == g_EquippedPossessedPtr) {
                wp = liveVal;
            }
        }

        int rawCD = 0;
        int prestige = 0;
        if (wp != 0) {
            GetCachedCD(wp, &rawCD);
            prestige = GetPrestigeFromWeapon(wp);
        }

        int cur = 0;
        __try { cur = *(int*)(ps + OFFSET_PLAYER_CD); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        if (lastWritten != -1 && cur != lastWritten) {
            vanillaField = cur;
        }

        int boostDelta = 0;
        if (prestige > 0) {
            float mult = ComputeMultiplier(prestige);
            int boosted = ClampBoosted(rawCD, mult);
            boostDelta = boosted - rawCD;
        }

        int targetBase = vanillaField + boostDelta;
        if (targetBase < BASE) targetBase = BASE;

        if (cur != targetBase) {
            __try {
                *(int*)(ps + OFFSET_PLAYER_CD) = targetBase;
                lastWritten = targetBase;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        else {
            lastWritten = targetBase;
        }
    }
    return 0;
}

// ============================================================================
// INIT
// ============================================================================
DWORD WINAPI InitThread(LPVOID) {
    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("[Enhanced] MH_Initialize failed.\n");
        return 1;
    }

    InitializeCriticalSection(&g_CacheCS);
    InterlockedExchange(&g_CacheCSInit, 1);

    Sleep(3000);
    LoadConfigFromFile();
    LoadPrestigeData();

    uintptr_t ba = (uintptr_t)GetModuleHandleA(NULL);
    if (!ba) return 1;

    LPVOID t1 = (LPVOID)(ba + 0x149DA8);
    MH_CreateHook(t1, &Hooked_AddToArray, (LPVOID*)&oAddToArray);
    MH_EnableHook(t1);

    LPVOID t2 = (LPVOID)(ba + 0x2DD898);
    MH_CreateHook(t2, &Hooked_CalcAttr, (LPVOID*)&oCalcAttr);
    MH_EnableHook(t2);

    LPVOID t3 = (LPVOID)(ba + 0x2DDD14);
    MH_CreateHook(t3, &Hooked_ApplyValue, (LPVOID*)&oApplyValue);
    MH_EnableHook(t3);

    LPVOID t4 = (LPVOID)(ba + 0x68F984);
    MH_CreateHook(t4, &Hooked_CombatStats, (LPVOID*)&oCombatStats);
    MH_EnableHook(t4);

    LPVOID t5 = (LPVOID)(ba + 0x493CDC);
    if (MH_CreateHook(t5, &Hooked_IterateAttrs, (LPVOID*)&oIterateAttrs) == MH_OK)
        MH_EnableHook(t5);

    LPVOID t6 = (LPVOID)(ba + 0x6EAA70);
    if (MH_CreateHook(t6, &Hooked_ApplyAttribute, (LPVOID*)&oApplyAttribute) == MH_OK)
        MH_EnableHook(t6);

    InstallCameraHook();
    InstallSaveHook();
    InstallShutdownHooks();

    Sleep(5000);
    InstallEquipHook();

    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)CDModifierThread, NULL, 0, NULL);

    OutputDebugStringA("[Enhanced] v1.0 loaded.\n");
    return 0;
}

// ============================================================================
// DLL MAIN
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitThread, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        g_ShuttingDown = true;
        break;
    }
    return TRUE;
}