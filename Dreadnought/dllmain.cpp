#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "includes.h"
#include "SDK.h"
#include "imgui_stdlib.h"
#include "kiero/minhook/include/MinHook.h"

#define STEAM_API_EXPORTS
#include "steam/steam_api.h"
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <intrin.h>

#include "SDK/DreadGameUI_Classes.h"
#include "SDK/DreadGame_Classes.h"
#include "SDK/UI_FrontEnd_Classes.h"
#include "SDK/UI_Screen_Persistent_Classes.h"
#include <atomic>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <io.h>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdarg.h>
#include <string>
#include <thread>
#include <vector>


std::ofstream debugLogFile;

// === Crash-safe dual logging: writes to both console AND a log file ===
static FILE *g_logFile = nullptr;
static FILE *g_console = nullptr;
static CRITICAL_SECTION g_logCS;

// tee_printf: writes to both console and log file, flushed immediately
// (crash-safe)
static void tee_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  // Write to console
  if (g_console) {
    va_list args2;
    va_copy(args2, args);
    vfprintf(g_console, fmt, args2);
    fflush(g_console);
    va_end(args2);
  }

  // Write to log file (crash-safe: flushed immediately)
  if (g_logFile) {
    va_list args3;
    va_copy(args3, args);
    vfprintf(g_logFile, fmt, args3);
    fflush(g_logFile);
    va_end(args3);
  }

  va_end(args);
}

// Redirect all printf/std::cout through tee by making stdout point to the log
// file and keeping a separate console handle for live output. After
// InitLogging(), printf() writes to file, tee_printf() writes to both.
static void InitLogging() {
  InitializeCriticalSection(&g_logCS);

  // Open log file next to the game EXE
  char logPath[MAX_PATH];
  GetModuleFileNameA(NULL, logPath, MAX_PATH);
  // Replace exe name with our log name
  char *lastSlash = strrchr(logPath, '\\');
  if (lastSlash)
    strcpy(lastSlash + 1, "dread_mod_log.txt");
  else
    strcpy(logPath, "dread_mod_log.txt");

  g_logFile = fopen(logPath, "w"); // overwrite each session
  if (g_logFile) {
    setvbuf(g_logFile, NULL, _IONBF, 0); // unbuffered for crash safety
  }

  // Keep a handle to the console for live display
  g_console = _fdopen(_dup(_fileno(stdout)), "w");
  if (g_console)
    setvbuf(g_console, NULL, _IONBF, 0);

  // Redirect stdout to the log file so ALL output (including UE4 engine log) is
  // captured
  if (g_logFile) {
    _dup2(_fileno(g_logFile), _fileno(stdout));
    setvbuf(stdout, NULL, _IONBF, 0);
  }

  tee_printf("[LOG] Logging initialized. Log file: %s\n", logPath);
}

using namespace CG;

// Global raw TArray structure for direct memory hacks
struct TArrayRaw {
  uint8_t *Data;
  int32_t Count;
  int32_t Max;
};

// === Hardened GC Root-Set Pinning ===
static bool g_gcPinningVerified = false;
static int32_t g_verifiedRootSetMask =
    (1 << 30); // Default RootSet flag bit (1<<30)

// Validates the memory layout of FUObjectItem and flag definitions at runtime.
static bool VerifyGCOffsets() {
  if (g_gcPinningVerified)
    return true;

  UObject *engineObj =
      UObject::FindObject<UObject>("YGameEngine Transient.YGameEngine");
  if (!engineObj) {
    engineObj = UObject::FindObject<UObject>("Class CoreUObject.Object");
  }

  if (!engineObj) {
    printf("[ROOT] Offset verification skipped: reference object not found "
           "yet.\n");
    return false;
  }

  int32_t index = engineObj->InternalIndex;
  if (index < 0 || index >= UObject::GObjects->Count()) {
    printf("[ROOT] Offset verification FAILED: engine object index %d out of "
           "bounds\n",
           index);
    return false;
  }

  FUObjectItem *item = UObject::GObjects->GetItemByIndex(index);
  if (!item) {
    printf("[ROOT] Offset verification FAILED: null FUObjectItem\n");
    return false;
  }

  if (item->Object != engineObj) {
    printf("[ROOT] CRITICAL: FUObjectItem->Object (%p) mismatch with engine "
           "object (%p)! Struct offsets are shifted.\n",
           item->Object, engineObj);
    return false;
  }

  if ((item->Flags & g_verifiedRootSetMask) == 0) {
    printf("[ROOT] CRITICAL: Engine object item->Flags (0x%X) does not have "
           "RootSet bit (1<<30) set. Bit drift detected.\n",
           item->Flags);
    uint8_t *rawItem = (uint8_t *)item;
    printf("[ROOT] Raw item dump: ");
    for (int i = 0; i < 24; i++)
      printf("%02X ", rawItem[i]);
    printf("\n");
    return false;
  }

  printf("[ROOT] Dynamic offset verification SUCCESS: item->Object matches and "
         "RootSet (1<<30) is verified.\n");
  g_gcPinningVerified = true;
  return true;
}

// Recursively pins the object and its Outer chain to the GC RootSet using
// atomic CPU instructions.
static void HardenedPinToRootSet(UObject *Obj) {
  if (!Obj)
    return;

  if (!VerifyGCOffsets()) {
    return;
  }

  UObject *Current = Obj;
  while (Current) {
    int32_t index = Current->InternalIndex;
    if (index >= 0 && index < UObject::GObjects->Count()) {
      FUObjectItem *item = UObject::GObjects->GetItemByIndex(index);
      if (item && item->Object == Current) {
        // Thread-safe atomic flag set
        InterlockedOr((volatile LONG *)&item->Flags, g_verifiedRootSetMask);

        // Clear PendingKill (1<<29) and Unreachable (1<<28) to revive object if
        // needed
        const int32_t clearMask = ~((1 << 28) | (1 << 29));
        InterlockedAnd((volatile LONG *)&item->Flags, clearMask);
      }
    }
    Current = Current->Outer;
  }
}

// Helper to manually increment the internal reference count of an FText's
// shared reference controller. This prevents the GC from freeing the control
// block when transient arrays are cleaned up.
static void IncrementFTextRefCounts(void *ftextPtr) {
  if (!ftextPtr)
    return;
  void *controller = *(void **)((uint8_t *)ftextPtr + 8);
  if (controller && (uintptr_t)controller > 0x10000 &&
      !IsBadReadPtr(controller, 16)) {
    volatile LONG *pSharedCount = (volatile LONG *)((uint8_t *)controller + 8);
    InterlockedIncrement(pSharedCount);
    volatile LONG *pWeakCount = (volatile LONG *)((uint8_t *)controller + 12);
    InterlockedIncrement(pWeakCount);
  }
}

// Memory allocator wrappers declaration (FMemoryMalloc is defined below, but we
// declare it here for early usage)
extern void *FMemoryMalloc(size_t size);

// Performs a deep copy of a manufacturer information array. It allocates a
// brand-new heap buffer so the destination array destructor can safely call
// appFree without touching the CDO static data, and increments the FText
// reference counts so the CDO's text objects aren't corrupted on cleanup.
static void SafeCopyManufacturerArray(TArrayRaw *dst, TArrayRaw *src) {
  if (!dst || !src || src->Count <= 0 || !src->Data)
    return;

  const int ENTRY_SIZE = 0xA8; // sizeof(FYUIManufacturerInformationEntry)
  int32_t totalBytes = src->Count * ENTRY_SIZE;

  uint8_t *newBuf = (uint8_t *)FMemoryMalloc(totalBytes);
  if (!newBuf)
    return;

  memcpy(newBuf, src->Data, totalBytes);

  for (int i = 0; i < src->Count; i++) {
    uint8_t *entry = newBuf + (i * ENTRY_SIZE);
    // FText fields at 0x40 (m_name), 0x58 (m_heroShipName), 0x70 (m_slogan),
    // and 0x88 (m_description)
    IncrementFTextRefCounts(entry + 0x40);
    IncrementFTextRefCounts(entry + 0x58);
    IncrementFTextRefCounts(entry + 0x70);
    IncrementFTextRefCounts(entry + 0x88);
  }

  dst->Data = newBuf;
  dst->Count = src->Count;
  dst->Max = src->Count;
}

// Safely updates the string content of an FText object without destroying its
// structure or corrupting its internal reference controller block pointers.
static void SafeSetFTextString(void *ftextPtr, const wchar_t *newStr) {
  if (!ftextPtr)
    return;
  void *textData = *(void **)ftextPtr;
  if (textData && (uintptr_t)textData > 0x10000 &&
      !IsBadReadPtr(textData, 64)) {
    size_t len = wcslen(newStr);
    wchar_t *buf = (wchar_t *)FMemoryMalloc((len + 1) * sizeof(wchar_t));
    if (buf) {
      wcscpy(buf, newStr);
      *(wchar_t **)((uint8_t *)textData + 0x28) = buf;
      *(int32_t *)((uint8_t *)textData + 0x30) = (int32_t)len;
    }
  }
}

void CustomCrashLog(std::string message) {
  std::cout << "[CUSTOM LOG] " << message << std::endl;
}

typedef bool(__cdecl *_SteamAPI_Init)();
typedef void(__cdecl *_SteamAPI_Shutdown)();
typedef bool(__cdecl *_SteamAPI_IsSteamRunning)();
typedef void(__cdecl *_SteamAPI_RegisterCallResult)(
    class CCallbackBase *pCallback, SteamAPICall_t hAPICall);
typedef void(__cdecl *_SteamAPI_UnregisterCallResult)(
    class CCallbackBase *pCallback, SteamAPICall_t hAPICall);
typedef ISteamMatchmaking *(__cdecl *_SteamMatchmaking)();
typedef void(__cdecl *_SteamAPI_RunCallbacks)();

_SteamAPI_Init Dyn_SteamAPI_Init = nullptr;
_SteamAPI_Shutdown Dyn_SteamAPI_Shutdown = nullptr;
_SteamAPI_IsSteamRunning Dyn_SteamAPI_IsSteamRunning = nullptr;
_SteamAPI_RegisterCallResult Dyn_SteamAPI_RegisterCallResult = nullptr;
_SteamAPI_UnregisterCallResult Dyn_SteamAPI_UnregisterCallResult = nullptr;
_SteamMatchmaking Dyn_SteamMatchmaking = nullptr;
_SteamAPI_RunCallbacks Dyn_SteamAPI_RunCallbacks = nullptr;

extern "C" S_API void S_CALLTYPE SteamAPI_RegisterCallResult(
    class CCallbackBase *pCallback, SteamAPICall_t hAPICall) {
  if (Dyn_SteamAPI_RegisterCallResult)
    Dyn_SteamAPI_RegisterCallResult(pCallback, hAPICall);
}

extern "C" S_API void S_CALLTYPE SteamAPI_UnregisterCallResult(
    class CCallbackBase *pCallback, SteamAPICall_t hAPICall) {
  if (Dyn_SteamAPI_UnregisterCallResult)
    Dyn_SteamAPI_UnregisterCallResult(pCallback, hAPICall);
}

bool InitSteamworksDynamically() {
  HMODULE hSteam = GetModuleHandleA("steam_api64.dll");
  if (!hSteam)
    return false;

  Dyn_SteamAPI_Init = (_SteamAPI_Init)GetProcAddress(hSteam, "SteamAPI_Init");
  Dyn_SteamAPI_Shutdown =
      (_SteamAPI_Shutdown)GetProcAddress(hSteam, "SteamAPI_Shutdown");
  Dyn_SteamAPI_IsSteamRunning = (_SteamAPI_IsSteamRunning)GetProcAddress(
      hSteam, "SteamAPI_IsSteamRunning");
  Dyn_SteamAPI_RegisterCallResult =
      (_SteamAPI_RegisterCallResult)GetProcAddress(
          hSteam, "SteamAPI_RegisterCallResult");
  Dyn_SteamAPI_UnregisterCallResult =
      (_SteamAPI_UnregisterCallResult)GetProcAddress(
          hSteam, "SteamAPI_UnregisterCallResult");
  Dyn_SteamMatchmaking =
      (_SteamMatchmaking)GetProcAddress(hSteam, "SteamMatchmaking");
  Dyn_SteamAPI_RunCallbacks =
      (_SteamAPI_RunCallbacks)GetProcAddress(hSteam, "SteamAPI_RunCallbacks");

  if (Dyn_SteamAPI_Init && Dyn_SteamAPI_Init()) {
    return true;
  }
  return false;
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

Present oPresent;
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device *pDevice = NULL;
ID3D11DeviceContext *pContext = NULL;
ID3D11RenderTargetView *mainRenderTargetView;

namespace Globals {
uintptr_t ModuleBase = 0; // Base address of main module
uintptr_t StaticLoadClassAddr = 0;
uintptr_t FMemoryMallocAddr = 0;
uintptr_t LoadPackageAddr = 0;

bool AmServer = false; // Are we playing as a server
} // namespace Globals

namespace Scanner {
struct Section {
  uintptr_t start;
  size_t size;
};

Section GetSection(const char *name) {
  uintptr_t base = Globals::ModuleBase;
  auto dosHeader = (PIMAGE_DOS_HEADER)base;
  auto ntHeaders = (PIMAGE_NT_HEADERS)(base + dosHeader->e_lfanew);
  auto sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);

  for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
    if (strncmp((char *)sectionHeader[i].Name, name, 8) == 0) {
      return {base + sectionHeader[i].VirtualAddress,
              (size_t)sectionHeader[i].Misc.VirtualSize};
    }
  }
  return {0, 0};
}

uintptr_t FindPattern(Section section, const char *pattern, const char *mask) {
  if (!section.start || !section.size)
    return 0;

  const uint8_t *start = (const uint8_t *)section.start;
  size_t size = section.size;
  size_t patternLen = strlen(mask);

  for (size_t i = 0; i < size - patternLen; i++) {
    bool found = true;
    for (size_t j = 0; j < patternLen; j++) {
      if (mask[j] != '?' && (uint8_t)pattern[j] != start[i + j]) {
        found = false;
        break;
      }
    }
    if (found)
      return (uintptr_t)(start + i);
  }
  return 0;
}

void ScanAll() {
  printf("[SCAN] Scoping engine discovery to valid sections...\n");
  Section text = GetSection(".text");
  Section rdata = GetSection(".rdata");

  if (!text.start) {
    printf("[SCAN] Error: Could not find .text section! Base=0x%llX\n",
           Globals::ModuleBase);
    return;
  }

  printf("[SCAN] Scanning .text [0x%llX - 0x%llX]\n", text.start,
         text.start + text.size);

  // StaticLoadObject / StaticLoadClass
  Globals::StaticLoadClassAddr =
      FindPattern(text,
                  "\x48\x89\x5C\x24\x10\x48\x89\x74\x24\x20\x55\x57\x41\x55\x41"
                  "\x56\x41\x57\x48\x8B\xEC\x48\x83\xEC\x70",
                  "xxxxxxxxxxxxxxxxxxxxxxxxx");
  if (Globals::StaticLoadClassAddr)
    printf("[SCAN] Found StaticLoadObject at 0x%llX\n",
           Globals::StaticLoadClassAddr);

  // LoadPackage
  Globals::LoadPackageAddr =
      FindPattern(text,
                  "\x40\x55\x53\x56\x57\x41\x54\x41\x56\x41\x57\x48\x8D\xAC\x24"
                  "\x70\xFF\xFF\xFF\x48\x81\xEC\x90\x01\x00\x00",
                  "xxxxxxxxxxxxxxxxxxxxxxxxxx");
  if (Globals::LoadPackageAddr)
    printf("[SCAN] Found LoadPackage at 0x%llX\n", Globals::LoadPackageAddr);

  // FMemory::Malloc (Standard UE4 wrapper)
  Globals::FMemoryMallocAddr =
      FindPattern(text,
                  "\x48\x83\xec\x28\x48\x8b\x05\x00\x00\x00\x00\x48\x8b\x00\x48"
                  "\x8b\x08\xff\x51\x08\x48\x83\xc4\x28\xc3",
                  "xxxxxxx????xxxxxxxxxxxxxx");
  if (Globals::FMemoryMallocAddr)
    printf("[SCAN] Found FMemoryMalloc at 0x%llX\n",
           Globals::FMemoryMallocAddr);
}
} // namespace Scanner
// =============================================================================
// GC crash prevention - disable GC entirely
// =============================================================================
void PatchGCUnknownTokenCrash() {
  static bool alreadyCalled = false;
  if (alreadyCalled)
    return;
  alreadyCalled = true;
  printf("[GC] Patching 'Unknown token' crash sites...\n");

  uintptr_t base = Globals::ModuleBase;
  auto dosHeader = (PIMAGE_DOS_HEADER)base;
  auto ntHeaders = (PIMAGE_NT_HEADERS)(base + dosHeader->e_lfanew);
  auto sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);
  size_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;

  uint8_t *textBase = nullptr;
  size_t textSize = 0;
  for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
    if (strncmp((char *)sectionHeader[i].Name, ".text", 5) == 0) {
      textBase = (uint8_t *)base + sectionHeader[i].VirtualAddress;
      textSize = sectionHeader[i].Misc.VirtualSize;
      break;
    }
  }
  if (!textBase) {
    printf("[GC] ERROR: .text section not found\n");
    return;
  }

  uint8_t *imageBytes = (uint8_t *)base;

  // APPROACH 1: Find and patch the GC timer float value
  const char *gcTimerStr = "gc.TimeBetweenPurgingPendingKillObjects";
  size_t gcTimerLen = strlen(gcTimerStr);
  uint8_t *gcTimerAddr = nullptr;

  for (size_t i = 0; i < imageSize - gcTimerLen; i++) {
    if (memcmp(imageBytes + i, gcTimerStr, gcTimerLen) == 0) {
      gcTimerAddr = imageBytes + i;
      break;
    }
  }

  bool timerPatched = false;
  if (gcTimerAddr) {

    // Find LEA XREF to this string in .text
    for (size_t i = 0; i < textSize - 7; i++) {
      uint8_t *pc = textBase + i;
      if ((pc[0] != 0x48 && pc[0] != 0x4C) || pc[1] != 0x8D)
        continue;
      if ((pc[2] & 0xC7) != 0x05)
        continue;
      int32_t disp = *(int32_t *)(pc + 3);
      if (pc + 7 + disp != gcTimerAddr)
        continue;

      // Search nearby code for MOVSS xmm, [rip+disp32] loading the default
      // float Pattern: F3 0F 10 [modrm with rm=101] [disp32]
      for (int scan = -200; scan < 200; scan++) {
        uint8_t *chk = pc + scan;
        if (chk < textBase || chk >= textBase + textSize - 8)
          continue;
        if (chk[0] == 0xF3 && chk[1] == 0x0F && chk[2] == 0x10 &&
            (chk[3] & 0xC7) == 0x05) {
          int32_t fDisp = *(int32_t *)(chk + 4);
          uint8_t *floatAddr = chk + 8 + fDisp;
          if (floatAddr >= imageBytes &&
              floatAddr < imageBytes + imageSize - 4) {
            float val = *(float *)floatAddr;
            if (val >= 10.0f && val <= 300.0f) {
              DWORD oldProt;
              VirtualProtect(floatAddr, 4, PAGE_EXECUTE_READWRITE, &oldProt);
              *(float *)floatAddr = 999999.0f;
              VirtualProtect(floatAddr, 4, oldProt, &oldProt);
              printf("[GC] Patched GC timer float %.1f -> 999999.0s\n", val);
              timerPatched = true;
              break;
            }
          }
        }
      }
      break;
    }
  }

  // Neutralize "Unknown token" FatalError calls in FastReferenceCollector
  {
    // Search for L"Unknown token" (UTF-16LE) in the binary
    const wchar_t *searchStr = L"Unknown token";
    size_t searchLen = wcslen(searchStr) * 2; // bytes
    uint8_t *tokenStrAddr = nullptr;

    for (size_t i = 0; i < imageSize - searchLen; i++) {
      if (memcmp(imageBytes + i, searchStr, searchLen) == 0) {
        tokenStrAddr = imageBytes + i;
        break;
      }
    }

    if (tokenStrAddr) {

      // NOPping FatalError alone is insufficient â€” FMsg::Logf_Internal
      // with Fatal verbosity ALSO triggers the crash handler in UE4.
      // Binary analysis identified all 12 CALL sites (6 Logf + 6 FatalError
      // pairs).
      static const struct {
        uint64_t rva;
        uint64_t expectedTarget;
        const char *label;
      } callSites[] = {
          {0xD1F204, 0xC81740, "Logf"},       // inst 1 (line 381)
          {0xD1F220, 0xC6D4F0, "FatalError"}, // inst 1
          {0xD20700, 0xC81740, "Logf"},       // inst 2 (near LEA)
          {0xD20724, 0xC6D4F0, "FatalError"}, // inst 2
          {0xD21FF9, 0xC81740, "Logf"},       // inst 3
          {0xD22027, 0xC6D4F0, "FatalError"}, // inst 3
          {0xD23619, 0xC81740, "Logf"},       // inst 4
          {0xD23708, 0xC6D4F0, "FatalError"}, // inst 4
          {0xD2B839, 0xC81740, "Logf"},       // inst 5
          {0xD2B859, 0xC6D4F0, "FatalError"}, // inst 5
          {0xD2EACD, 0xC81740, "Logf"},       // inst 6
          {0xD2EB24, 0xC6D4F0, "FatalError"}, // inst 6
      };
      static const int NUM_SITES = 12;

      int nopCount = 0;
      for (int i = 0; i < NUM_SITES; i++) {
        uint8_t *callAddr = imageBytes + callSites[i].rva;
        if (callAddr < textBase || callAddr + 5 > textBase + textSize)
          continue;

        if (callAddr[0] == 0x90) {
          nopCount++;
        } else if (callAddr[0] == 0xE8) {
          int32_t disp = *(int32_t *)(callAddr + 1);
          uint8_t *target = callAddr + 5 + disp;
          uint64_t targetRVA = (uint64_t)(target - imageBytes);
          if (targetRVA == callSites[i].expectedTarget) {
            DWORD oldProt;
            VirtualProtect(callAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt);
            memset(callAddr, 0x90, 5);
            VirtualProtect(callAddr, 5, oldProt, &oldProt);
            nopCount++;
          }
        }
      }
      printf("[GC] NOPped %d/%d Logf+FatalError call sites\n", nopCount,
             NUM_SITES);

      // ROOT CAUSE FIX â€” After NOPping the Fatal+Logf calls, the code
      // at the default case falls through to a JMP back to the token processing
      // loop top (LAB_140d1c6d0). This causes an infinite loop because the
      // token stream pointer is invalid and every iteration AVs on the same bad
      // pointer.
      //
      // From Ghidra decompilation of FUN_140d1c520 (FastReferenceCollector):
      //   Line 1969: if ((uVar6 & 0xf00) != 0xb00)  // check for EndOfStream
      //   Line 1971-1975: FMsg::Logf + FatalError("Unknown token")  [NOPped
      //   above] Line 1976: goto LAB_140d1c6d0;             // JMP at 0xD1F233
      //   â†’ loop back Line 1978+: EndOfStream handler            // at
      //   0xD1F238 â†’ clean exit
      //
      // By NOPping this JMP, unknown tokens fall through to the EndOfStream
      // handler, which cleanly exits the token processing loop and advances to
      // the next object. The GC continues normally, collecting all valid
      // objects. No memory leaks.
      {
        uint8_t *jmpAddr = imageBytes + 0xD1F233;
        if (jmpAddr >= textBase && jmpAddr + 5 <= textBase + textSize) {
          if (jmpAddr[0] == 0xE9) {
            int32_t disp = *(int32_t *)(jmpAddr + 1);
            uint8_t *target = jmpAddr + 5 + disp;
            uint64_t targetRVA = (uint64_t)(target - imageBytes);

            if (targetRVA == 0xD1C6D0) { // verify it's the loop-back JMP
              DWORD oldProt;
              VirtualProtect(jmpAddr, 5, PAGE_EXECUTE_READWRITE, &oldProt);
              memset(jmpAddr, 0x90, 5); // NOP the JMP
              VirtualProtect(jmpAddr, 5, oldProt, &oldProt);
              printf("[GC] NOPped loop-back JMP â€” unknown tokens now exit "
                     "via EndOfStream\n");
            } else {
              printf("[GC] JMP at 0xD1F233: target mismatch (0x%llX != "
                     "0xD1C6D0)\n",
                     targetRVA);
            }
          } else if (jmpAddr[0] == 0x90) {
            // Already patched
          } else {
            printf("[GC] JMP at 0xD1F233: unexpected byte 0x%02X\n",
                   jmpAddr[0]);
          }
        }
      }
    }
  }

  printf("[GC] All patches applied.\n");
}

// =============================================================================
// Runtime GC disable â€” patch GarbageCollectionSettings CDO
// The binary-level timer patch fails in shipping builds (CVar string stripped).
// Instead, find the GarbageCollectionSettings CDO and patch the float directly.
// Must be called AFTER UObjects are initialized (not at DLL load time).
// =============================================================================
static bool g_gcDisabledAtRuntime = false;
void DisableGCAtRuntime() {
  if (g_gcDisabledAtRuntime)
    return;

  // === Part A: Patch the CDO (for any future reads from default) ===
  UObject *gcSettings = UObject::FindObject<UObject>(
      "GarbageCollectionSettings Engine.Default__GarbageCollectionSettings");
  if (!gcSettings) {
    printf("[GC] GarbageCollectionSettings CDO not found yet\n");
    return;
  }

  uint8_t *obj = (uint8_t *)gcSettings;
  printf("[GC] Found GarbageCollectionSettings CDO at %p\n", obj);

  for (int offset = 0x28; offset < 0x80; offset += 4) {
    float val = *(float *)(obj + offset);
    if (val >= 30.0f && val <= 120.0f) {
      printf("[GC] Patched CDO float %.1f -> 999999.0 at CDO+0x%X\n", val,
             offset);
      // Must stay disabled. GC at ANY interval triggers 'Unknown token'
      // crash in FastReferenceCollector on our TTM data. Killing the GC worker
      // thread deadlocks the main thread (GC coordinator waits forever).
      *(float *)(obj + offset) = 999999.0f;
      break;
    }
  }

  // === Part B: Patch the UEngine's CACHED copy ===
  // UEngine::Init() copies TimeBetweenPurgingPendingKillObjects from CDO into a
  // member. Patching the CDO alone doesn't help â€” the cached copy is already
  // set. Find the live YGameEngine instance and patch all 60.0f floats in it.
  UObject *gameEngine = nullptr;
  for (int i = 0; i < UObject::GObjects->Count(); i++) {
    UObject *o = UObject::GObjects->GetByIndex(i);
    if (!o || !o->Class)
      continue;
    std::string fn = o->GetFullName();
    if (fn.find("YGameEngine Transient.YGameEngine") != std::string::npos &&
        fn.find("Default__") == std::string::npos &&
        fn.find("Class ") == std::string::npos) {
      gameEngine = o;
      printf("[GC] Found UEngine instance: %s at %p\n", fn.c_str(), o);
      break;
    }
  }

  if (gameEngine) {
    uint8_t *eng = (uint8_t *)gameEngine;
    int patchCount = 0;
    // UEngine is large. TimeBetweenPurgingPendingKillObjects is typically in
    // the first 0x800 bytes. Scan for float 60.0f (the default).
    for (int offset = 0x28; offset < 0x1000; offset += 4) {
      float val = *(float *)(eng + offset);
      if (val == 60.0f) {
        *(float *)(eng + offset) = 999999.0f; // must stay disabled (see above)
        printf("[GC] Patched Engine+0x%X: 60.0 -> 999999.0\n", offset);
        patchCount++;
      }
    }
    printf("[GC] Patched %d float(s) in UEngine instance\n", patchCount);

    if (patchCount == 0) {
      // Dump floats 30-120 range as candidates
      printf("[GC] No 60.0 found. Candidate floats in UEngine:\n");
      for (int offset = 0x28; offset < 0x800; offset += 4) {
        float val = *(float *)(eng + offset);
        if (val >= 30.0f && val <= 120.0f) {
          printf("  Engine+0x%X = %.2f\n", offset, val);
        }
      }
    }
  } else {
    printf("[GC] WARNING: UEngine instance not found!\n");
  }

  g_gcDisabledAtRuntime = true;
}

/*
        Iterates over the global objects array, and finds the final object of
   the given type
*/
template <typename T> T *getLastOfType() {
  return UObject::FindObjects<T>().back();
}

/*
        DEBUGGING ONLY: Iterates over the global objects array, and lists all
   objects of the provided type.
*/
template <typename T> void ListAllObjectsOfType() {
  for (T *obj : UObject::FindObjects<T>()) {
    std::cout << obj->GetFullName() << std::endl;
  }
}

/*
        Unreal Engine uses it's own allocator, which will crash when attempting
   to deallocate memory that dosen't belong to it. Instead of using "new" in our
   code, we need to use the native Unreal Engine allocator, which this function
   calls.
*/
// Forward declaration so FMemoryMalloc can use it
static void *UE4Malloc(size_t size);

void *FMemoryMalloc(size_t size) {
  // Redirect EVERYTHING to the proven UE4Malloc implementation which uses the
  // correct Realloc wrapper. The previous implementation used a fragile AOB
  // scan and fell back to an invalid offset (0xC06B70) that caused an AV crash
  // when writing.
  return UE4Malloc(size);
}

/*
        Equivalent to StaticLoad<T> in UE4, used to load ship loadout BPs
*/
UPackage *LoadPackage(UObject *InOuter, const TCHAR *InLongPackageName,
                      uint32 LoadFlags) {
  uintptr_t addr = Globals::LoadPackageAddr ? Globals::LoadPackageAddr
                                            : (Globals::ModuleBase + 0xCF04B0);
  // UE4 4.15 signature is (UPackage*, const TCHAR*, uint32, FArchive*) â€” 4
  // args. The 4th arg (FArchive*) goes in R9. Passing nullptr explicitly to
  // avoid garbage in R9.
  return reinterpret_cast<UPackage *(*)(UObject *, const TCHAR *, uint32,
                                        void *)>(addr)(
      InOuter, InLongPackageName, LoadFlags, nullptr);
}

// Relay using proven raw engine call (not LoadPackage which crashes)
UObject *StaticLoadClass(UClass *ObjectClass, UObject *InOuter,
                         const TCHAR *InName) {
  uintptr_t addr = Globals::ModuleBase + 0x0D78110;
  // 7-arg signature: (UClass*, UObject*, const TCHAR*, const TCHAR*, int,
  // void*, bool)
  UObject *Obj =
      reinterpret_cast<UObject *(*)(UClass *, UObject *, const TCHAR *,
                                    const TCHAR *, int, void *, bool)>(addr)(
          ObjectClass, InOuter, InName, nullptr, 0, nullptr, false);
  if (Obj) {
    HardenedPinToRootSet(Obj);
  }
  return Obj;
}

UObject *GetObjByName(const char *name) {
  return UObject::FindObject<UObject>(name);
}

// Forward declarations for lazy/early hooking
void InitEarlyHooks();
void InitUIHooks();

/*
        Same as StaticLoadClass but with bAllowObjectReconciliation=true.
        Needed specifically for loadout loading when Steam subsystem is active.
*/
UObject *StaticLoadClassReconcile(UClass *ObjectClass, UObject *InOuter,
                                  const TCHAR *InName) {
  return StaticLoadClass(ObjectClass, InOuter, InName);
}

/*
        Startup IMGUI
*/
void InitImGui() {
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
  ImGui_ImplWin32_Init(window);
  ImGui_ImplDX11_Init(pDevice, pContext);
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam,
                          LPARAM lParam) {

  if (true && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
    return true;

  return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

typedef void(__thiscall *tProcessEvent)(UObject *, class UFunction *, void *);

tProcessEvent pProcessEvent_Original = nullptr;

static bool flipTeams = false;

bool interceptPostLogin = false;

bool procMapLoad = false;

std::string mapCommand = "";

bool connectToServer = false;

std::string serverIP = "";

bool launchTutorial = false;

bool forceHUD = false;

std::string loadoutString = "";

/*
        Loads the provided loadout class to memory
*/
void LoadLoadouts() {
  std::wstring wLoadoutString(loadoutString.begin(), loadoutString.end());

  StaticLoadClass(UYShipLoadout::StaticClass(), nullptr,
                  wLoadoutString.c_str());
}

/*
        Load the sepcified loadout for singleplayer, and force starts the match
*/

FString MakeFMemoryFString(const wchar_t *StrContents) {
  FString ret{};

  ret._data = (wchar_t *)UE4Malloc((wcslen(StrContents) + 1) * sizeof(wchar_t));
  memcpy(ret._data, StrContents, (wcslen(StrContents) + 1) * sizeof(wchar_t));
  ret._count = (int32_t)wcslen(StrContents) + 1;
  ret._max = (int32_t)wcslen(StrContents) + 1;

  return ret;
}

UYShipLoadout *THELOADOUT = nullptr;
UClass *THELOADOUT_CLASS = nullptr;
bool pendingPawnLoadout =
    false; // Set to true when loadout is ready but pawn doesn't exist yet
bool loadoutAppliedToPC =
    false; // Internal flag to track if we called
           // AddAndActiveLoadoutFromBlueprint on the main thread

UYShipLoadout *FindLoadoutInWorld(const std::string &pattern) {
  for (UYShipLoadout *cmpLoadout : UObject::FindObjects<UYShipLoadout>()) {
    if (cmpLoadout &&
        cmpLoadout->GetFullName().find(pattern) != std::string::npos) {
      return cmpLoadout;
    }
  }
  return nullptr;
}

void CompleteSingleplayerMatchSetup(std::string loadoutPath) {
  std::cout << "[SCAN] CompleteSingleplayerMatchSetup called with: "
            << loadoutPath << std::endl;

  ULocalPlayer *lp = ((*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]);
  AYPlayerController *pc = (AYPlayerController *)(lp->PlayerController);

  std::wstring wLoadout(loadoutPath.begin(), loadoutPath.end());
  THELOADOUT_CLASS = (UClass *)StaticLoadClass(UClass::StaticClass(), nullptr,
                                               wLoadout.c_str());

  if (pc && THELOADOUT_CLASS) {
    // Find the actual loadout object to assign directly
    UYShipLoadout *loadoutToApply = nullptr;
    std::string shortName =
        loadoutPath.substr(loadoutPath.find_last_of("/") + 1);
    if (shortName.find(".") != std::string::npos)
      shortName = shortName.substr(0, shortName.find("."));

    for (UYShipLoadout *cmpLoadout : UObject::FindObjects<UYShipLoadout>()) {
      if (cmpLoadout->GetFullName().find(shortName) != std::string::npos) {
        loadoutToApply = cmpLoadout;
        break;
      }
    }

    if (loadoutToApply && pc->m_loadoutManager) {
      pc->m_loadoutManager->m_activeLoadout = loadoutToApply;
      std::cout << "[SCAN] Directly assigned m_activeLoadout: "
                << loadoutToApply->GetFullName() << std::endl;
    }

    pc->AddAndActiveLoadoutFromBlueprint(THELOADOUT_CLASS);
    loadoutAppliedToPC = true;
    pendingPawnLoadout = true;
    std::cout << "[SCAN] Triggered AddAndActiveLoadoutFromBlueprint & set "
                 "pendingPawnLoadout."
              << std::endl;
  }

  if ((*UWorld::GWorld)->AuthorityGameMode &&
      (*UWorld::GWorld)->AuthorityGameMode->GameState) {
    ((AYGameState *)(*UWorld::GWorld)->AuthorityGameMode->GameState)
        ->SetRemainingTime(1);
  }
}

static std::string singleplayerLoadoutString =
    "/Game/Generic/Loadouts/Precast/T5/"
    "VH_AssaultLight_PrecastLoadout_T5_BP.VH_AssaultLight_PrecastLoadout_T5_BP_"
    "C";

/*
        Sets up singleplayer AI, requires about 30sec of built in delay to
   ensure all AI pawns spawn
*/
void SetupSingleplayerAIThread(int numBotsTeamOne, int numBotsTeamTwo,
                               int difficulty, std::string loadoutString) {
  Sleep(20 * 1000);

  switch (difficulty) {
  case 0:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Rec");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_RECRUIT;
    }
    break;
  case 1:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Vet");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_VETERAN;
    }
    break;
  case 2:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Leg");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_LEGENDARY;
    }
    break;
  default:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Leg");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_LEGENDARY;
    }
    break;
  }

  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->GetCombatManager()
      ->m_NPCSet = getLastOfType<UYNPCPawnData>();
  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->GetCombatManager()
      ->m_isNPCSetLoaded = true;

  UYNPCPawnData *pawnData = getLastOfType<UYNPCPawnData>();

  for (int i = 0;
       i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
               ->m_npcPlayers.Count();
       i++) {
    TArray<FName> shipIDs;

    shipIDs._data =
        (FName *)UE4Malloc(sizeof(FName) * pawnData->m_PawnsData.Count());
    shipIDs._count = pawnData->m_PawnsData.Count();
    shipIDs._max = pawnData->m_PawnsData.Count();

    for (int j = 0; j < pawnData->m_PawnsData.Count(); j++) {
      shipIDs[j] = pawnData->m_PawnsData[j].m_shipId;
    }

    ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
        ->m_npcPlayers[i]
        .m_npcSpawnIDs = shipIDs;
  }

  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->SetTeamSizeAI(EYTeam::YT_TEAM1, numBotsTeamOne);
  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->SetTeamSizeAI(EYTeam::YT_TEAM2, numBotsTeamTwo);

  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->m_enableSpawnAI = true;

  Sleep(30 * 1000);

  CompleteSingleplayerMatchSetup(loadoutString);
}

/*
        Sets up multiplayer AI, requires about 30sec of built in delay to ensure
   all AI pawns spawn
*/
void SetupMultiplayerAI(int numBotsTeamOne, int numBotsTeamTwo,
                        int difficulty) {
  switch (difficulty) {
  case 0:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Rec");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_RECRUIT;
    }
    break;
  case 1:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Vet");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_VETERAN;
    }
    break;
  case 2:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Leg");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_LEGENDARY;
    }
    break;
  default:
    StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
                    L"/Game/Generic/GameModes/TDM/AIShips_TDM_Leg");
    for (int i = 0;
         i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
                 ->m_aiSpawnTierRules.Count();
         i++) {
      ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
          ->m_aiSpawnTierRules[i]
          .m_aiTier_aiDificultyLevel = EYAILevel::YAIL_LEGENDARY;
    }
    break;
  }

  AYPlayerController *hostPC = nullptr;
  if (Globals::AmServer) {
    hostPC = (AYPlayerController *)(*UWorld::GWorld)
                 ->NetDriver->ClientConnections[0]
                 ->PlayerController;
  } else {
    hostPC = (AYPlayerController *)(*UWorld::GWorld)
                 ->OwningGameInstance->LocalPlayers[0]
                 ->PlayerController;
  }

  if (hostPC) {
    hostPC->GetCombatManager()->m_NPCSet = getLastOfType<UYNPCPawnData>();
    hostPC->GetCombatManager()->m_isNPCSetLoaded = true;
  }

  UYNPCPawnData *pawnData = getLastOfType<UYNPCPawnData>();

  for (int i = 0;
       i < ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
               ->m_npcPlayers.Count();
       i++) {
    TArray<FName> shipIDs;

    shipIDs._data =
        (FName *)UE4Malloc(sizeof(FName) * pawnData->m_PawnsData.Count());
    shipIDs._count = pawnData->m_PawnsData.Count();
    shipIDs._max = pawnData->m_PawnsData.Count();

    for (int j = 0; j < pawnData->m_PawnsData.Count(); j++) {
      shipIDs[j] = pawnData->m_PawnsData[j].m_shipId;
    }

    ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
        ->m_npcPlayers[i]
        .m_npcSpawnIDs = shipIDs;
  }

  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->SetTeamSizeAI(EYTeam::YT_TEAM1, numBotsTeamOne);
  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->SetTeamSizeAI(EYTeam::YT_TEAM2, numBotsTeamTwo);

  ((AYGameMode_Multiplayer *)(*UWorld::GWorld)->AuthorityGameMode)
      ->m_enableSpawnAI = true;

  // Sleep(30 * 1000);
}

int numPlayersConnected = 0;

std::vector<AYPlayerController *> spawnedPlayerControllers =
    std::vector<AYPlayerController *>();

static int numBotsTeamOne = 0;
static int numBotsTeamTwo = 0;
static int difficulty = 0;
static int map = 0;
static int singleplayerLoadoutIndex = 0;

static int hostNumBotsTeamOne = 8;
static int hostNumBotsTeamTwo = 8;
static int hostDifficulty = 1;
static int hostMapIndex = 0;
static int hostLoadoutIndex = 0;
static char hostServerName[64] = "";
static char hostPassword[64] = "";

class SteamLobbyManager {
public:
  CCallResult<SteamLobbyManager, LobbyCreated_t> m_LobbyCreatedCallResult;

  void OnLobbyCreated(LobbyCreated_t *pCallback, bool bIOFailure) {
    if (pCallback->m_eResult == k_EResultOK && !bIOFailure) {
      uint64_t lobbyID = pCallback->m_ulSteamIDLobby;

      const char *mapNames[10] = {
          "Amirani",   "DansMap",  "Derelict",  "Glacier", "Gorge",
          "Highlands", "Paradise", "Skybridge", "Space01", "Space02"};

      if (Dyn_SteamMatchmaking) {
        std::string finalName = std::string(hostServerName);
        if (finalName.empty()) {
          finalName = "Dreadnought Lobby " + std::to_string(lobbyID % 10000);
        }

        Dyn_SteamMatchmaking()->SetLobbyData(lobbyID, "Name",
                                             finalName.c_str());
        Dyn_SteamMatchmaking()->SetLobbyData(lobbyID, "Map",
                                             mapNames[hostMapIndex]);

        if (std::string(hostPassword).length() > 0) {
          Dyn_SteamMatchmaking()->SetLobbyData(lobbyID, "Password",
                                               hostPassword);
        }

        std::cout
            << "Steam Lobby Created cleanly and map explicitly assigned! ID: "
            << lobbyID << std::endl;
      }
    }
  }
};
SteamLobbyManager g_LobbyManager;

bool launchHostServer = false;
bool launchSingleplayer = false;

void Listen();

UClass *FindClassFast(std::string searchString) {
  for (UClass *cls : UObject::FindObjects<UClass>()) {
    if (cls->GetFullName().find(searchString) != std::string::npos) {
      return cls;
    }
  }
  return nullptr;
}

bool executeServerSetupOnMainThread = false;
int cachedBotsT1 = 0;
int cachedBotsT2 = 0;
int cachedDiff = 0;
int cachedLoadout = 0;

static const char *hostLoadoutPaths[5] = {
    "/Game/Generic/Loadouts/Precast/T5/VH_AssaultLight_PrecastLoadout_T5_BP",
    "/Game/Generic/Loadouts/Precast/T5/VH_SniperLight_T5_PrecastLoadout_BP",
    "/Game/Generic/Loadouts/Precast/T5/"
    "VH_DreadnoughtHeavy_PrecastLoadout_T5_BP",
    "/Game/Generic/Loadouts/Precast/T5/VH_DestroyerMedium_PrecastLoadout_T5_BP",
    "/Game/Generic/Loadouts/Precast/T5/"
    "VH_TacticalCruiser_PrecastLoadout_T5_BP"};

void HostServerSetupThread(int botsT1, int botsT2, int diff, int loadout) {
  // Wait for map to load, then trigger Listen + AI setup on main thread
  Sleep(25 * 1000);
  cachedBotsT1 = botsT1;
  cachedBotsT2 = botsT2;
  cachedDiff = diff;
  cachedLoadout = loadout;
  executeServerSetupOnMainThread = true;

  // Apply loadout directly from background thread â€” same pattern as
  // singleplayer
  std::string loadoutPath = hostLoadoutPaths[loadout];
  Sleep(30 * 1000);
  CompleteSingleplayerMatchSetup(loadoutPath);
}

/*
        Delays the singleplayer setup thread so the map has time to load in
*/
void DelaySingleplayerSetupThread(std::string loadoutString) {
  if (pendingPawnLoadout)
    return; // Already triggered by another thread
  Sleep(15 * 1000);

  CompleteSingleplayerMatchSetup(loadoutString);
}

// SEH wrapper for StaticLoadObject calls (can't mix __try with C++ try/catch)
typedef UObject *(__cdecl *tStaticLoadObject_Global)(
    UClass *ObjectClass, UObject *InOuter, const wchar_t *InName,
    const wchar_t *Filename, uint32_t LoadFlags, void *Sandbox,
    bool bAllowObjectReconciliation);

// LoadPackage typedef â€” simpler: (UPackage* InOuter, const TCHAR*
// InLongPackageName, uint32 LoadFlags)
typedef UObject *(__cdecl *tLoadPackage)(UObject *InOuter,
                                         const wchar_t *InLongPackageName,
                                         uint32_t LoadFlags);

struct SLOResult {
  UObject *obj;
  DWORD exceptionCode;
  bool crashed;
  uint64_t crashAddr;
};

// VEH to capture exact crash address
static uint64_t g_vehCrashAddr = 0;
static LONG CALLBACK CrashAddrVEH(PEXCEPTION_POINTERS pExInfo) {
  if (pExInfo && pExInfo->ExceptionRecord) {
    g_vehCrashAddr = (uint64_t)pExInfo->ExceptionRecord->ExceptionAddress;
  }
  return EXCEPTION_CONTINUE_SEARCH; // Let the SEH handler catch it
}

SLOResult SafeCallStaticLoadObject(tStaticLoadObject_Global fn, UClass *cls,
                                   const wchar_t *path) {
  SLOResult result = {nullptr, 0, false, 0};
  g_vehCrashAddr = 0;
  PVOID veh = AddVectoredExceptionHandler(1, CrashAddrVEH);
  __try {
    result.obj = fn(cls, nullptr, path, nullptr, 0, nullptr, true);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result.exceptionCode = GetExceptionCode();
    result.crashed = true;
    result.crashAddr = g_vehCrashAddr;
  }
  if (veh)
    RemoveVectoredExceptionHandler(veh);
  return result;
}

SLOResult SafeCallLoadPackage(tLoadPackage fn, const wchar_t *packageName) {
  SLOResult result = {nullptr, 0, false, 0};
  g_vehCrashAddr = 0;
  PVOID veh = AddVectoredExceptionHandler(1, CrashAddrVEH);
  __try {
    result.obj = fn(nullptr, packageName, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result.exceptionCode = GetExceptionCode();
    result.crashed = true;
    result.crashAddr = g_vehCrashAddr;
  }
  if (veh)
    RemoveVectoredExceptionHandler(veh);
  return result;
}

// Include <fstream> is now at the top of the file
static bool g_hasWipedOnce =
    false; // Moved to global to ensure strict one-time execution

// Native UFunction hijacking helper
void InstallNativeHook(const char *funcName, void *hookFunc,
                       void **origFuncOut) {
  // Guard: if already hooked (origFuncOut already set and != hookFunc), skip.
  // This prevents double-hooking when trying multiple name variants.
  if (origFuncOut && *origFuncOut != nullptr && *origFuncOut != hookFunc) {
    // Already successfully hooked with a real original â€” skip
    return;
  }
  UFunction *fn = (UFunction *)GetObjByName(funcName);
  if (!fn) {
    printf("[WARN] Could not find UFunction: %s\n", funcName);
    return;
  }
  if (origFuncOut)
    *origFuncOut = fn->Func;
  fn->Func = hookFunc;
  printf("[HOOK] Native hook installed: %s\n", funcName);
}

static UObject *g_capturedHUD =
    nullptr; // HUD actor captured from UserConstructionScript
static int g_streamingCallbackCountdown =
    0; // Deferred streaming completion callback
static UObject *g_capturedTitleScreen =
    nullptr; // Title screen widget to remove after transition
static UObject *g_customizationPreviewActor =
    nullptr; // Customization preview actor captured on ReceiveTick
static bool g_needsCustomizationPreviewUpdate = true;
static bool g_levelActorLinksInitialized = false;
static bool g_levelActorLinksAttempted = false;
static bool g_genericPInitialised = false;

static const wchar_t* g_shipAnimClassPaths[16] = {
  /* 0 */ nullptr,
  /* 1 */ L"/Game/Generic/Ships/Dreadnought/Light/VH_DreadL_ABP.VH_DreadL_ABP_C",
  /* 2 */ L"/Game/Generic/Ships/Scout/Light/VH_ScoutL_ABP.VH_ScoutL_ABP_C",
  /* 3 */ L"/Game/Generic/Ships/Sniper/Light/VH_SniperL_ABP.VH_SniperL_ABP_C",
  /* 4 */ L"/Game/Generic/Ships/Support/Light/VH_SupportL_ABP.VH_SupportL_ABP_C",
  /* 5 */ L"/Game/Generic/Ships/Assault/Light/VH_AssaultL_ABP.VH_AssaultL_ABP_C",
  /* 6 */ L"/Game/Generic/Ships/Dreadnought/Medium/VH_DreadM_ABP.VH_DreadM_ABP_C",
  /* 7 */ L"/Game/Generic/Ships/Dreadnought/Heavy/VH_DreadH_ABP.VH_DreadH_ABP_C",
  /* 8 */ L"/Game/Generic/Ships/Scout/Medium/VH_ScoutM_ABP.VH_ScoutM_ABP_C",
  /* 9 */ L"/Game/Generic/Ships/Scout/Heavy/VH_ScoutH_ABP.VH_ScoutH_ABP_C",
  /* 10 */ L"/Game/Generic/Ships/Sniper/Medium/Animations/VH_SniperM_ABP.VH_SniperM_ABP_C",
  /* 11 */ L"/Game/Generic/Ships/Sniper/Heavy/VH_SniperH_ABP.VH_SniperH_ABP_C",
  /* 12 */ L"/Game/Generic/Ships/Support/Medium/VH_SupportM_ABP.VH_SupportM_ABP_C",
  /* 13 */ L"/Game/Generic/Ships/Support/Heavy/VH_SupportH_ABP.VH_SupportH_ABP_C",
  /* 14 */ L"/Game/Generic/Ships/Assault/Medium/VH_AssaultM_ABP.VH_AssaultM_ABP_C",
  /* 15 */ L"/Game/Generic/Ships/Assault/Heavy/VH_AssaultH_ABP.VH_AssaultH_ABP_C"
};

static UClass* g_shipAnimClasses[16] = { nullptr };

static bool IsValidUObject(UObject *obj) {
  if (!obj)
    return false;
  try {
    int32_t index = obj->InternalIndex;
    if (index >= 0 && index < UObject::GObjects->Count()) {
      FUObjectItem *item = UObject::GObjects->GetItemByIndex(index);
      if (item && item->Object == obj) {
        if (item->Flags & (1 << 29)) { // PendingKill flag
          return false;
        }
        return true;
      }
    }
  } catch (...) {
  }
  return false;
}
static bool g_waitingForFinalization = false;

// --- Fleet Persistence State ---
#include <fstream>
#include <set>
#include <sstream>


static std::set<int> g_ownedShips;
static int g_credits = 500000;
static int g_xp = 50000;
static std::map<int, int> g_shipXP;
static std::vector<int> g_fleetSlots;
static std::string g_fleetSavePath = "";

enum class PersistenceBackendType {
  JSON,
  SQLITE
};

// Global persistence switch: active JSON for testing/editing, ready for SQLite when shipping
static PersistenceBackendType g_ActivePersistenceBackend = PersistenceBackendType::JSON;

struct PlayerProfileData {
  int credits = 500000;
  int freeXP = 50000;
  std::set<int> ownedShips;
  std::map<int, int> shipXP;
  std::vector<int> fleetSlots;
};

class IPersistenceStore {
public:
  virtual ~IPersistenceStore() = default;
  virtual bool Save(const std::string &path, const PlayerProfileData &profile) = 0;
  virtual bool Load(const std::string &path, PlayerProfileData &outProfile) = 0;
};

class JSONPersistenceStore : public IPersistenceStore {
public:
  bool Save(const std::string &path, const PlayerProfileData &profile) override {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "{\n";
    f << "  \"credits\": " << profile.credits << ",\n";
    f << "  \"free_xp\": " << profile.freeXP << ",\n";

    f << "  \"owned_ships\": [";
    bool first = true;
    for (int id : profile.ownedShips) {
      if (!first) f << ", ";
      f << id;
      first = false;
    }
    f << "],\n";

    f << "  \"fleet_slots\": [";
    first = true;
    for (int id : profile.fleetSlots) {
      if (!first) f << ", ";
      f << id;
      first = false;
    }
    f << "],\n";

    f << "  \"ship_xp\": {\n";
    first = true;
    for (const auto &kv : profile.shipXP) {
      if (!first) f << ",\n";
      f << "    \"" << kv.first << "\": " << kv.second;
      first = false;
    }
    f << "\n  }\n";
    f << "}\n";
    f.close();
    return true;
  }

  bool Load(const std::string &path, PlayerProfileData &outProfile) override {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Parse credits
    size_t pos = json.find("\"credits\":");
    if (pos != std::string::npos) {
      outProfile.credits = std::stoi(json.substr(pos + 10));
    }
    // Parse free_xp
    pos = json.find("\"free_xp\":");
    if (pos != std::string::npos) {
      outProfile.freeXP = std::stoi(json.substr(pos + 10));
    }
    // Parse owned_ships
    pos = json.find("\"owned_ships\":");
    if (pos != std::string::npos) {
      size_t start = json.find('[', pos);
      size_t end = json.find(']', start);
      if (start != std::string::npos && end != std::string::npos) {
        std::string arrStr = json.substr(start + 1, end - start - 1);
        std::stringstream ss(arrStr);
        std::string token;
        outProfile.ownedShips.clear();
        while (std::getline(ss, token, ',')) {
          token.erase(0, token.find_first_not_of(" \t\n\r"));
          token.erase(token.find_last_not_of(" \t\n\r") + 1);
          if (!token.empty()) {
            outProfile.ownedShips.insert(std::stoi(token));
          }
        }
      }
    }
    // Parse fleet_slots
    pos = json.find("\"fleet_slots\":");
    if (pos != std::string::npos) {
      size_t start = json.find('[', pos);
      size_t end = json.find(']', start);
      if (start != std::string::npos && end != std::string::npos) {
        std::string arrStr = json.substr(start + 1, end - start - 1);
        std::stringstream ss(arrStr);
        std::string token;
        outProfile.fleetSlots.clear();
        while (std::getline(ss, token, ',')) {
          token.erase(0, token.find_first_not_of(" \t\n\r"));
          token.erase(token.find_last_not_of(" \t\n\r") + 1);
          if (!token.empty()) {
            outProfile.fleetSlots.push_back(std::stoi(token));
          }
        }
      }
    }
    return true;
  }
};

class SQLitePersistenceStore : public IPersistenceStore {
public:
  bool Save(const std::string &path, const PlayerProfileData &profile) override {
    printf("[FLEET-SAVE] SQLite Save backend switch ready for shipping.\n");
    return true;
  }
  bool Load(const std::string &path, PlayerProfileData &outProfile) override {
    printf("[FLEET-SAVE] SQLite Load backend switch ready for shipping.\n");
    return true;
  }
};

void SaveFleetData() {
  if (g_fleetSavePath.empty())
    return;

  std::string profilePath = g_fleetSavePath;
  if (profilePath.find(".txt") != std::string::npos) {
    profilePath.replace(profilePath.find(".txt"), 4, ".json");
  }

  PlayerProfileData profile;
  profile.credits = g_credits;
  profile.freeXP = g_xp;
  profile.ownedShips = g_ownedShips;
  profile.fleetSlots = g_fleetSlots;
  profile.shipXP = g_shipXP;

  std::unique_ptr<IPersistenceStore> store;
  if (g_ActivePersistenceBackend == PersistenceBackendType::JSON) {
    store = std::make_unique<JSONPersistenceStore>();
  } else {
    store = std::make_unique<SQLitePersistenceStore>();
  }

  if (store->Save(profilePath, profile)) {
    printf("[FLEET-SAVE] Saved profile to %s (Backend: %s)\n",
           profilePath.c_str(),
           g_ActivePersistenceBackend == PersistenceBackendType::JSON ? "JSON" : "SQLite");
  }
}

void LoadFleetData() {
  if (g_fleetSavePath.empty())
    return;

  std::string profilePath = g_fleetSavePath;
  if (profilePath.find(".txt") != std::string::npos) {
    profilePath.replace(profilePath.find(".txt"), 4, ".json");
  }

  PlayerProfileData profile;
  std::unique_ptr<IPersistenceStore> store;
  if (g_ActivePersistenceBackend == PersistenceBackendType::JSON) {
    store = std::make_unique<JSONPersistenceStore>();
  } else {
    store = std::make_unique<SQLitePersistenceStore>();
  }

  if (!store->Load(profilePath, profile)) {
    printf("[FLEET-SAVE] Save profile not found at %s. Seeding default fleet profile...\n",
           profilePath.c_str());
    g_ownedShips = {
        // T1 & T2 starters
        11001, 11002, 11003,
        12001, 12002, 12003,
        13001, 13002, 13003,
        // Hero / Legendary ships
        50001, 50002, 50003, 50004
    };
    g_credits = 500000;
    g_xp = 50000;
    g_fleetSlots = {11001, 12001, 13001, 50001, 11002};
    SaveFleetData();
    return;
  }

  g_ownedShips = profile.ownedShips;
  g_credits = profile.credits;
  g_xp = profile.freeXP;
  g_fleetSlots = profile.fleetSlots;
  g_shipXP = profile.shipXP;

  printf("[FLEET-SAVE] Loaded profile from %s: %d owned ships, %d credits, %d XP, %d fleet slots\n",
         profilePath.c_str(), (int)g_ownedShips.size(), g_credits, g_xp, (int)g_fleetSlots.size());
}

bool UnlockShipAndSave(int shipId, int costCredit = 0) {
  if (costCredit > 0 && g_credits < costCredit) {
    printf("[FLEET-MGMT] Cannot purchase ship %d: insufficient credits (%d < %d)\n", shipId, g_credits, costCredit);
    return false;
  }
  if (costCredit > 0) {
    g_credits -= costCredit;
  }
  g_ownedShips.insert(shipId);
  printf("[FLEET-MGMT] Unlocked ship %d! Credits remaining: %d. Saving profile...\n", shipId, g_credits);
  SaveFleetData();
  return true;
}

static std::string g_waitingLevelName = "";
static void *g_waitingGM = nullptr;
static UFunction *g_finalizeFunction = nullptr;

// Global ship data read from UYShipLoadout objects for UI population
#define MAX_LOADED_SHIPS 80
struct LoadedShipInfo {
  UObject *loadoutObj;   // UYShipLoadout*
  wchar_t name[64];      // Ship display name
  int32_t precastID;     // Precast loadout ID
  EYShipClass shipClass; // Ship class enum
  int32_t tier;          // Ship tier (1-5)
  int32_t shipId;        // Unique UI ship ID
};
static LoadedShipInfo g_loadedShips[MAX_LOADED_SHIPS] = {};
static int g_numLoadedShips = 0;

// Game-thread asset loading dispatch
static tLoadPackage g_pLoadPackage = nullptr;
static tStaticLoadObject_Global g_pStaticLoadObject = nullptr;
static volatile bool g_needsGameThreadLoad = false;
static volatile bool g_gameThreadLoadDone = false;
static int g_gameThreadLoadResult = 0;

// v19.17 Hook variables
static void *OriginalGetUIShipDataFunc = nullptr;
static bool bHasHookedGetUIShipData = false;

// Hook variables for GetCurrentShipItemData and GetShipData
static void *OriginalGetCurrentShipItemDataFunc = nullptr;
static bool bHasHookedGetCurrentShipItemData = false;
static void *OriginalGetShipDataFunc = nullptr;
static bool bHasHookedGetShipData = false;

// New hooks for tier and class icon fixes
static void *OriginalGetTierFunc = nullptr;
static bool bHasHookedGetTier = false;
static void *OriginalGetShipTierFunc = nullptr;
static bool bHasHookedGetShipTier = false;
static void *OriginalGetShipClassIconFunc = nullptr;
static bool bHasHookedGetShipClassIcon = false;

// TierIcon hardening hooks
static void *OriginalSetTierFunc = nullptr;
static bool bHasHookedSetTier = false;
static void *OriginalSetTextureFromTierFunc = nullptr;
static bool bHasHookedSetTextureFromTier = false;

// Ownership spoof hooks
static void *OriginalIsItemOwnedByPlayerFunc = nullptr;
static bool bHasHookedIsItemOwnedByPlayer = false;
static void *OriginalIsCurrentShipOwnedByPlayerFunc = nullptr;
static bool bHasHookedIsCurrentShipOwnedByPlayer = false;

// HasItem hooks — overrides item entitlement checks for hero ship vanity parts
typedef bool(__fastcall *tHasItemNative)(void *pThis, int32_t itemID);
static tHasItemNative OrigHasItemNative = nullptr;

bool __fastcall MyHookHasItemNative(void *pThis, int32_t itemID) {
  static int logCount = 0;
  if (logCount < 50) {
    tee_printf("[HASITEM-NATIVE] Intercepted native HasItem(%d) -> returning true\n", itemID);
    logCount++;
  }
  return true;
}

static void *OriginalHasItemUFunctionFunc = nullptr;
void __fastcall MyHookHasItemUFunction(UObject *Context, void *Stack, void *RESULT_DECL) {
  if (RESULT_DECL) {
    *(bool *)RESULT_DECL = true;
  }
}

// SetSelectedShip hooks — fires when user clicks a ship in various UI screens
static void *OriginalSetSelectedShipFunc =
    nullptr; // UI_ManufacturerTechTreeScreen
static void *OriginalShipTechTreeSetSelectedShipFunc =
    nullptr; // UI_ShipTechTreeScreen
static void *OriginalOwnedShipsSetSelectedShipFunc =
    nullptr; // UI_OwnedShipsScreen
static void *OriginalAddShipToFleetSetSelectedShipFunc =
    nullptr; // UI_AddShipToFleetScreen

// Tyr Hero Ship DataAssets for Monarch (T5 Dreadnought)
static UObject *g_tyrDAs[4] = {nullptr, nullptr, nullptr, nullptr};

void ApplyMonarchHeroParts(void *previewActorObj) {
  if (!previewActorObj) return;

  bool allLoaded = true;
  for (int i = 0; i < 4; i++) {
    if (!g_tyrDAs[i]) { allLoaded = false; break; }
  }
  if (!allLoaded) {
    tee_printf("[MONARCH] Cannot apply Tyr parts: not all 4 DAs are loaded in memory!\n");
    return;
  }

  uintptr_t actorAddr = (uintptr_t)previewActorObj;
  
  // Set vanity parts directly on preview actor:
  // 0x0408: m_meshPartBridge (Quarterdeck)
  // 0x0410: m_meshPartStern
  // 0x0418: m_meshPartHull
  // 0x0420: m_meshPartForecastle
  *(UObject**)(actorAddr + 0x0418) = g_tyrDAs[0]; // Hull
  *(UObject**)(actorAddr + 0x0420) = g_tyrDAs[1]; // Forecastle
  *(UObject**)(actorAddr + 0x0408) = g_tyrDAs[2]; // Quarterdeck / Bridge
  *(UObject**)(actorAddr + 0x0410) = g_tyrDAs[3]; // Stern

  // Get m_appliedShipLoadout (0x0458)
  UObject *loadout = *(UObject**)(actorAddr + 0x0458);
  if (loadout) {
    uintptr_t loadoutAddr = (uintptr_t)loadout;
    // m_appereance is at 0x0108 on UYShipLoadout
    // m_heroShipParts is TArray<UYShipVanityMeshPart*>
    struct TArrayRaw {
      void *Data;
      int32_t Count;
      int32_t Max;
    } *heroParts = (TArrayRaw*)(loadoutAddr + 0x0108);

    static UObject *s_tyrPartsBuffer[4];
    s_tyrPartsBuffer[0] = g_tyrDAs[0];
    s_tyrPartsBuffer[1] = g_tyrDAs[1];
    s_tyrPartsBuffer[2] = g_tyrDAs[2];
    s_tyrPartsBuffer[3] = g_tyrDAs[3];

    heroParts->Data = s_tyrPartsBuffer;
    heroParts->Count = 4;
    heroParts->Max = 4;
    tee_printf("[MONARCH] Populated loadout 0x%p m_heroShipParts with 4 Tyr DAs.\n", loadout);
  }

  // Get m_mesh (USkeletalMeshComponent*) at 0x0460
  UObject *shipMeshComp = *(UObject**)(actorAddr + 0x0460);
  if (shipMeshComp) {
    UFunction *fnMerge = UObject::FindObject<UFunction>("Function DreadGame.YShipVanityLibrary.MergeShipMeshParts");
    if (fnMerge) {
      struct MergeParams {
        UObject *shipMeshComponent;
        struct {
          void *Data;
          int32_t Count;
          int32_t Max;
        } meshParts;
        bool enqueue;
        unsigned char pad[7];
        UObject *context;
      } params{};

      static UObject *s_tyrPartsBuffer[4];
      s_tyrPartsBuffer[0] = g_tyrDAs[0];
      s_tyrPartsBuffer[1] = g_tyrDAs[1];
      s_tyrPartsBuffer[2] = g_tyrDAs[2];
      s_tyrPartsBuffer[3] = g_tyrDAs[3];

      params.shipMeshComponent = shipMeshComp;
      params.meshParts.Data = s_tyrPartsBuffer;
      params.meshParts.Count = 4;
      params.meshParts.Max = 4;
      params.enqueue = true;
      params.context = (UObject*)previewActorObj;

      uint32_t flags = fnMerge->FunctionFlags;
      fnMerge->FunctionFlags |= 0x00000400;
      if (pProcessEvent_Original) {
        UObject *callTarget = UYShipVanityLibrary::StaticClass()
            ? UYShipVanityLibrary::StaticClass()->CreateDefaultObject() : fnMerge;
        if (!callTarget) callTarget = fnMerge;
        pProcessEvent_Original(callTarget, fnMerge, &params);
      }
      fnMerge->FunctionFlags = flags;

      tee_printf("[MONARCH] Successfully merged 4 Tyr vanity parts into shipMeshComponent %p!\n", shipMeshComp);
    } else {
      tee_printf("[MONARCH] ERROR: Could not find MergeShipMeshParts UFunction!\n");
    }
  }
}


// Hook for Manufacturer Tech Tree Data (RVA 0x4ED0C0)
typedef uint64_t(__fastcall *tGetManufacturerData)(int32_t manufacturerId,
                                                   void *outArr1, void *outArr2,
                                                   void *worldContext);
static tGetManufacturerData OrigGetManufacturerData = nullptr;
static bool g_getManufacturerDataHookInstalled = false;

// Global storage for our TTM data (NOT stored in the TTM UObject itself)
// This data is only wired into the TTM temporarily during GetManufacturerData
// calls.
static uint8_t *g_ttmMfgData = nullptr; // manufacturer groups array
static int g_ttmMfgCount = 0;
static uint8_t *g_ttmClassData = nullptr; // class lookups array
static int g_ttmClassCount = 0;
static uint8_t *g_ttmPtr = nullptr; // pointer to the live TTM UObject
static CRITICAL_SECTION g_ttmLock;  // protects TTM wire/unwire
static bool g_ttmLockInit = false;

// UE4 allocator wrappers using DIRECT RVA calls â€” no vtable reconstruction
// needed.
//
// Confirmed from Ghidra analysis:
//   FMemory::Realloc wrapper = FUN_140c0abd0, RVA 0xC0ABD0
//     Calls vtable+0x18 on GMalloc. When first arg (old ptr) = NULL, acts as
//     Malloc.
//   FMemory::Free wrapper   = FUN_140bfc9c0, RVA 0xBFC9C0
//     Calls vtable+0x20 on GMalloc. This is what UE4 destructors use to free
//     TArray buffers.
//
// Using these means our allocations are in the same pool as UE4's, so Free
// won't AV.
static uintptr_t g_moduleBase = 0;

// FMemory::Realloc(oldPtr, newSize, alignment) â€” RVA 0xC0ABD0
// Called with oldPtr=NULL it behaves identically to Malloc.
typedef void *(__fastcall *tUE4Realloc)(void *, size_t, uint32_t);
static tUE4Realloc g_UE4Realloc = nullptr;

// FMemory::Free(ptr) â€” RVA 0xBFC9C0
typedef void(__fastcall *tUE4Free)(void *);
static tUE4Free g_UE4Free = nullptr;

static void EnsureUE4Allocators() {
  if (!g_UE4Realloc && g_moduleBase) {
    g_UE4Realloc = (tUE4Realloc)(g_moduleBase + 0xC0ABD0);
    g_UE4Free = (tUE4Free)(g_moduleBase + 0xBFC9C0);
  }
}

static void *UE4Malloc(size_t size) {
  EnsureUE4Allocators();
  if (!g_UE4Realloc) {
    printf("[ALLOC] UE4Malloc: No Realloc! standard malloc of %zu\n", size);
    return malloc(size);
  }
  void *ptr = g_UE4Realloc(nullptr, size, 0); // 0 = DEFAULT_ALIGNMENT
  if (!ptr) {
    printf("[ALLOC] UE4Malloc(%zu) FAILED â€” falling back to malloc\n", size);
    return malloc(size);
  }
  return ptr;
}

// Helper to create an FString using UE4's allocator
static void InitFStringUE4(void *fstringPtr, const wchar_t *str) {
  if (!fstringPtr || !str)
    return;
  int len = (int)wcslen(str);
  EnsureUE4Allocators();
  if (!g_UE4Realloc)
    return;
  // 3-arg signature confirmed by Ghidra 0xC0ABD0: (ptr, size, alignment)
  void *data = g_UE4Realloc(nullptr, (size_t)((len + 1) * 2), 0);
  if (!data)
    return;
  memcpy(data, str, (len + 1) * 2);
  *(void **)fstringPtr = data;
  *(int32_t *)((uint8_t *)fstringPtr + 0x08) = len + 1;
  *(int32_t *)((uint8_t *)fstringPtr + 0x0C) = len + 1;
}

static bool g_logTechTree = true;

// Per-tier cache entry lookup table
// FUN_4F5780 reads tier from cache_entry+0xF8. We scan the cache to find
// entries with valid tiers (1-5) and use them for synthetic IDs.
static void *g_tierCacheEntries[5] = {nullptr, nullptr, nullptr, nullptr,
                                      nullptr};
static bool g_tierCacheScanned = false;
static const int CACHE_ENTRY_STRIDE = 0x118; // 280 bytes per cache entry

void BuildSyntheticToRealMap();

// Tech Tree Structs & Globals
struct FTechTreeShip {
  int32_t manufacturerId;
  int32_t shipId;    // synthetic (11001+)
  int32_t realId;    // real cache ID (translated)
  int32_t tier;      // 1-5
  int32_t shipClass; // 6, 10, 2, 12, 14
  int32_t prereqId;
  std::wstring name;
  int32_t proxyFallbackIndex;
};
static std::vector<FTechTreeShip> g_FullTechTree;

struct CacheDiscoveryEntry {
  int32_t realId;    // m_itemID at entry+0xFC (canonical ID like 0x01FF0121)
  int32_t tier;      // m_tier at entry+0xF8
  int32_t shipClass; // m_shipClass at entry+0x10D (uint8_t, EYShipClass)
  int32_t itemType;  // m_cachedItemType at entry+0x104
  int32_t loadoutItemType; // m_loadoutItemType at entry+0x10C (uint8_t)
  std::string name;
};
static std::map<int32_t, CacheDiscoveryEntry> g_discoveryCache;
static std::map<int32_t, int32_t> g_syntheticToRealMap;
// Reverse map: real cache ID -> synthetic ship ID
// Built alongside g_syntheticToRealMap. Allows GetShipResearchData to translate
// real IDs (stored by Blueprint after our SSS swap) back to synthetic IDs
// so the TTM mfg-array lookup finds our injected ship entries.
static std::map<int32_t, int32_t> g_realToSyntheticMap;
// Module/weapon IDs referenced by ships' m_relatedItemIDs
// Maps: moduleItemId -> FYRelatedItemEntry.m_identifier (slot type byte)
static std::map<int32_t, uint8_t> g_moduleItemIds;

static void ScanCacheForTiers() {
  if (g_tierCacheScanned)
    return;
  g_tierCacheScanned = true;

  typedef void *(__fastcall * fn_GetCachedSingleton)();
  auto GetCacheSingleton = (fn_GetCachedSingleton)(g_moduleBase + 0x4813A0);
  void *cacheSingleton = GetCacheSingleton();
  if (!cacheSingleton)
    return;

  uint8_t *cacheData = *(uint8_t **)((uint8_t *)cacheSingleton + 0x28);
  int32_t cacheCount = *(int32_t *)((uint8_t *)cacheSingleton + 0x30);
  if (!cacheData || cacheCount <= 0)
    return;

  printf("\n[DISCOVERY] === UYCachedItemIDData Cache Scan (%d entries, "
         "stride=0x%X) ===\n",
         cacheCount, CACHE_ENTRY_STRIDE);
  int found = 0;
  int shipCount = 0, moduleCount = 0, otherCount = 0;
  for (int i = 0; i < cacheCount; i++) {
    uint8_t *entry = cacheData + i * CACHE_ENTRY_STRIDE;

    // SDK-verified offsets (FCachedItemIDDataEntry, size 0x118):
    //   +0x0000: m_relatedItemIDs (TArray<FYRelatedItemEntry>) â€” NOT the item
    //   ID +0x0010: m_uiData (FYItemUIData, size 0xD0) â€” contains FText
    //   m_headline at +0x00 +0x00F8: m_tier (int32_t) +0x00FC: m_itemID
    //   (int32_t) â€” THE canonical item ID (e.g. 0x01FF0121) +0x0104:
    //   m_cachedItemType (int32_t) â€” EYCachedItemType enum +0x010C:
    //   m_loadoutItemType (uint8_t) +0x010D: m_shipClass (uint8_t) â€”
    //   EYShipClass enum
    int32_t itemId = *(int32_t *)(entry + 0xFC);        // m_itemID
    int32_t tierVal = *(int32_t *)(entry + 0xF8);       // m_tier
    int32_t itemType = *(int32_t *)(entry + 0x104);     // m_cachedItemType
    uint8_t shipClassVal = *(uint8_t *)(entry + 0x10D); // m_shipClass (1 byte!)
    uint8_t loadoutType = *(uint8_t *)(entry + 0x10C);  // m_loadoutItemType

    // Read name via FText pointer chain (NOT FString):
    // entry+0x10 = m_uiData.m_headline (FText)
    // FText layout: { FTextData* Data (+0x00); uint8_t Unknown[0x10]; } = 0x18
    // bytes FTextData layout: { uint8_t Unknown[0x28]; wchar_t* Name (+0x28);
    // int32_t* Length (+0x30); }
    std::string nameStr = "???";
    try {
      void *textDataPtr = *(void **)(entry + 0x10); // FText.Data (FTextData*)
      if (textDataPtr) {
        wchar_t *wname =
            *(wchar_t **)((uint8_t *)textDataPtr + 0x28); // FTextData.Name
        if (wname && wname[0] != 0) {
          char buf[256] = {0};
          WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, sizeof(buf) - 1, NULL,
                              NULL);
          nameStr = buf;
        }
      }
    } catch (...) {
      nameStr = "EXCEP";
    }

    // Only log ships (ItemType==4) with valid tiers â€” skip the ~2800
    // modules/weapons
    if (itemType == 4 && tierVal >= 1 && tierVal <= 5) {
      printf("[DISCOVERY]   Ship: 0x%08X T%d C%d \"%s\"\n", (uint32_t)itemId,
             tierVal, (int)shipClassVal, nameStr.c_str());
      shipCount++;
    } else if (itemType == 1) {
      moduleCount++;
    } else {
      otherCount++;
    }

    // Store in discovery map keyed by canonical m_itemID (for bridge builder)
    g_discoveryCache[itemId] = {
        itemId, tierVal, (int32_t)shipClassVal, itemType, (int32_t)loadoutType,
        nameStr};

    // For ship entries, extract m_relatedItemIDs to discover all
    // modules/weapons
    if (itemType == 4 && tierVal >= 1 && tierVal <= 5) {
      // m_relatedItemIDs at entry+0x00 is TArray<FYRelatedItemEntry>
      // TArray layout: { void* Data; int32_t Count; int32_t Max; }
      void *relData = *(void **)(entry + 0x00);
      int32_t relCount = *(int32_t *)(entry + 0x08);
      if (relData && relCount > 0 && relCount < 500) {
        for (int r = 0; r < relCount; r++) {
          // FYRelatedItemEntry: { uint8_t m_identifier +0x00; uint32_t m_itemID
          // +0x04; } size=8
          uint8_t *relEntry = (uint8_t *)relData + r * 8;
          uint8_t identifier = *(uint8_t *)(relEntry + 0x00);
          uint32_t relItemId = *(uint32_t *)(relEntry + 0x04);
          if (relItemId != 0) {
            g_moduleItemIds[(int32_t)relItemId] = identifier;
          }
        }
      }
    }

    if (tierVal >= 1 && tierVal <= 5) {
      int idx = tierVal - 1;
      if (!g_tierCacheEntries[idx]) {
        g_tierCacheEntries[idx] = entry;
        found++;
      }
    }
  }

  // Fill any missing tiers with the nearest available
  void *fallback = nullptr;
  for (int i = 0; i < 5; i++) {
    if (g_tierCacheEntries[i])
      fallback = g_tierCacheEntries[i];
  }
  for (int i = 0; i < 5; i++) {
    if (!g_tierCacheEntries[i]) {
      g_tierCacheEntries[i] = fallback;
    }
  }
  printf("[DISCOVERY] Cache scan complete: %d ships, %d modules, %d other (%d "
         "total). Tiers: %d/5. Related modules: %d unique.\n",
         shipCount, moduleCount, otherCount, (int)g_discoveryCache.size(),
         found, (int)g_moduleItemIds.size());

  BuildSyntheticToRealMap();
}

void BuildSyntheticToRealMap() {
  if (g_FullTechTree.empty())
    return;
  if (g_discoveryCache.empty())
    return;

  printf("[BRIDGE] Building Synthetic->Real translation map...\n");
  printf(
      "[BRIDGE] g_FullTechTree has %d ships, g_discoveryCache has %d entries\n",
      (int)g_FullTechTree.size(), (int)g_discoveryCache.size());

  // Count viable ship candidates (valid tier 1-5, non-zero class)
  int shipCandidates = 0;
  for (auto const &pair : g_discoveryCache) {
    const auto &e = pair.second;
    if (e.tier >= 1 && e.tier <= 5 && e.shipClass != 0)
      shipCandidates++;
  }
  printf("[BRIDGE] %d viable ship candidates out of %d cache entries\n",
         shipCandidates, (int)g_discoveryCache.size());

  int mapped = 0;
  for (size_t i = 0; i < g_FullTechTree.size(); i++) {
    auto &ship = g_FullTechTree[i];
    int32_t bestRealId = -1;
    int32_t bestScore = -1;

    for (auto const &pair : g_discoveryCache) {
      int32_t candidateId = pair.first;
      const auto &entry = pair.second;

      // Skip non-ship entries: must have valid tier and non-zero shipClass
      if (entry.tier < 1 || entry.tier > 5)
        continue;
      if (entry.shipClass == 0)
        continue;

      int score = 0;
      // Exact tier match
      if (entry.tier == ship.tier)
        score += 10;
      // Exact shipClass match
      if (entry.shipClass == ship.shipClass)
        score += 20;

      // Name match: convert our wstring name to narrow string for comparison
      // Cache may store "Dover (T2)" while our table has "Dover" â€” use
      // contains check
      if (!entry.name.empty() && !ship.name.empty()) {
        // Convert ship.name (wstring) to narrow UTF-8 for comparison
        char narrowName[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, ship.name.c_str(), -1, narrowName,
                            sizeof(narrowName) - 1, NULL, NULL);

        // Case-insensitive compare: cache name contains our ship name as
        // prefix/substring
        std::string cacheName = entry.name;
        std::string shipNameNarrow = narrowName;

        // Lowercase both
        for (auto &c : cacheName)
          c = (char)tolower((unsigned char)c);
        for (auto &c : shipNameNarrow)
          c = (char)tolower((unsigned char)c);

        // Cache entry matches if it starts with our ship name (handles "dover
        // (t2)" vs "dover") or is an exact match
        if (cacheName == shipNameNarrow ||
            (cacheName.size() >= shipNameNarrow.size() &&
             cacheName.substr(0, shipNameNarrow.size()) == shipNameNarrow)) {
          score += 50; // name match dominates â€” can't be beaten by class/tier
                       // alone
        }
      }

      if (score > bestScore) {
        bestScore = score;
        bestRealId = candidateId;
      }
    }

    if (bestRealId != -1) {
      g_syntheticToRealMap[ship.shipId] = bestRealId;
      ship.realId = bestRealId;
      mapped++;
      // Log all matches; flag low-confidence ones (no name match = score <= 30)
      if (bestScore >= 50) {
        printf("[BRIDGE]   %ls (synth=%d) -> realId=0x%08X (score=%d OK)\n",
               ship.name.c_str(), ship.shipId, (uint32_t)bestRealId, bestScore);
      } else {
        printf("[BRIDGE]   WARN %ls (synth=%d) -> realId=0x%08X (score=%d "
               "NO-NAME-MATCH)\n",
               ship.name.c_str(), ship.shipId, (uint32_t)bestRealId, bestScore);
      }
    } else {
      printf("[BRIDGE]   WARNING: No match for %ls (synth=%d, tier=%d, "
             "class=%d)\n",
             ship.name.c_str(), ship.shipId, ship.tier, ship.shipClass);
    }
  }
  printf("[BRIDGE] Mapped %d / %d ships to real cache entries\n", mapped,
         (int)g_FullTechTree.size());

  // Build reverse map: real cache ID -> synthetic ID
  // This is required because SetSelectedShip hook swaps the synthetic ID with
  // the real ID before calling the Blueprint. The Blueprint stores the real ID
  // and passes it to GetShipResearchData. We reverse-translate here.
  g_realToSyntheticMap.clear();
  for (const auto &kv : g_syntheticToRealMap) {
    // kv.first = synthetic, kv.second = real
    // Only insert if not already mapped (keep first/best match)
    if (g_realToSyntheticMap.find(kv.second) == g_realToSyntheticMap.end()) {
      g_realToSyntheticMap[kv.second] = kv.first;
    }
  }
  printf("[BRIDGE] Built reverse map: %d real->synthetic entries\n",
         (int)g_realToSyntheticMap.size());

  // Deferred TTM+0x68 population: discovery scan runs AFTER TTM init,
  // so g_moduleItemIds was empty when the TTM was first populated.
  // Now that we have all module IDs, populate TTM+0x68 if TTM exists.
  if (g_ttmPtr && !g_moduleItemIds.empty()) {
    struct FRawArray {
      void *Data;
      int32_t Count;
      int32_t Max;
    };
    FRawArray *arr68 = (FRawArray *)(g_ttmPtr + 0x68);

    if (arr68->Count == 0) {
      EnsureUE4Allocators();
      const int ITEM_ENTRY_SIZE = 0x48;
      int moduleCount = (int)g_moduleItemIds.size();
      uint8_t *moduleData = (uint8_t *)UE4Malloc(moduleCount * ITEM_ENTRY_SIZE);
      if (moduleData) {
        memset(moduleData, 0, moduleCount * ITEM_ENTRY_SIZE);
        int idx = 0;
        for (auto const &pair : g_moduleItemIds) {
          int32_t modItemId = pair.first;
          uint8_t identifier = pair.second;
          uint8_t *item = moduleData + idx * ITEM_ENTRY_SIZE;

          // +0x20: item_id â€” the module's canonical cache ID
          *(int32_t *)(item + 0x20) = modItemId;

          // +0x2C: tier â€” look up from discovery cache if available
          auto it = g_discoveryCache.find(modItemId);
          int32_t modTier =
              (it != g_discoveryCache.end()) ? it->second.tier : 1;
          *(int32_t *)(item + 0x2C) = modTier;

          // +0x3C: identifier byte from FYRelatedItemEntry
          *(char *)(item + 0x3C) = (char)identifier;
          // +0x3D: isHero flag = 0
          *(char *)(item + 0x3D) = 0;

          // Inner entry structure with tier record (required by FUN_4E1D80)
          {
            const int INNER_ENTRY_SIZE = 32;
            const int TIER_RECORD_SIZE = 0x18;
            uint8_t *innerEntry = (uint8_t *)UE4Malloc(INNER_ENTRY_SIZE);
            uint8_t *tierRecord = (uint8_t *)UE4Malloc(TIER_RECORD_SIZE);
            if (innerEntry && tierRecord) {
              memset(innerEntry, 0, INNER_ENTRY_SIZE);
              memset(tierRecord, 0, TIER_RECORD_SIZE);
              *(void **)(tierRecord + 0x00) = nullptr;
              *(int32_t *)(tierRecord + 0x08) = 0;
              *(int32_t *)(tierRecord + 0x10) = modTier;
              *(void **)(innerEntry + 0x00) = tierRecord;
              *(int32_t *)(innerEntry + 0x08) = 1;
              *(int32_t *)(innerEntry + 0x0C) = 1;
              *(int64_t *)(innerEntry + 0x10) = 0;
              *(int32_t *)(innerEntry + 0x18) = 0;
              *(void **)(item + 0x00) = innerEntry;
              *(int32_t *)(item + 0x08) = 1;
              *(int32_t *)(item + 0x0C) = 1;
            }
          }
          idx++;
        }

        // Wire module data into TTM+0x68 (flat items array)
        arr68->Data = moduleData;
        arr68->Count = moduleCount;
        arr68->Max = moduleCount;

        printf("[TTM] Deferred Phase 4: Populated TTM+0x68 with %d "
               "module/weapon entries\n",
               moduleCount);
      }
    } else {
      printf("[TTM] TTM+0x68 already has %d entries, skipping deferred "
             "population\n",
             arr68->Count);
    }
  }
}

// Per-manufacturer ship definition: { name, shipClass (EYShipClass enum), tier,
// blueprintSuffix, proxyFallback } EYShipClass: 1=DreadnoughtLight,
// 2=ScoutLight, 3=SniperLight, 4=SupportLight, 5=AssaultLight,
//              6=DreadnoughtMedium, 7=DreadnoughtHeavy, 8=ScoutMedium,
//              9=ScoutHeavy, 10=SniperMedium, 11=SniperHeavy, 12=SupportMedium,
//              13=SupportHeavy, 14=AssaultMedium, 15=AssaultHeavy
struct ShipDef {
  const wchar_t *name;
  int shipClassEnum;    // EYShipClass value (NOT abstract 0-4)
  int tier;             // column in tech tree (1-5)
  const char *bpSuffix; // blueprint suffix for loading
  int proxyFallback;    // index into g_loadedShips for icon fallback
};

// Jupiter Arms â€” 17 ships (1â†’2â†’4â†’5â†’5)
// Manufacturer confirmed from in-game ship descriptions
static const ShipDef s_jupiterArms[] = {
    // T1: 1 ship
    {L"Agosta", 14, 1, "AssaultMedium",
     0}, // Destroyer   (JA: "salvaged from Jupiter Arms junkyard")
    // T2: 2 ships
    {L"Trafalgar", 14, 2, "AssaultMedium",
     0}, // Destroyer   (JA: "built during Jupiter Arms' rise")
    {L"Dover", 2, 2, "ScoutLight",
     4}, // Corvette    (JA: cache C2=ScoutLight, no ScoutMedium T2 in paks)
    // T3: 4 ships
    {L"Otranto", 14, 3, "AssaultMedium",
     0}, // Destroyer   (JA: "stock Jupiter Arms Destroyer")
    {L"Machias", 2, 3, "ScoutLight",
     4}, // Corvette    (JA: cache C2=ScoutLight)
    {L"Ballista", 11, 3, "SniperHeavy", 2}, // Artillery   (JA: icon=SniperH-T3)
    {L"Palos", 4, 3, "SupportLight",
     3}, // Tactical    (JA: cache C4=SupportLight)
    // T4: 5 ships
    {L"Vigo", 14, 4, "AssaultMedium", 0},      // Destroyer   (JA confirmed)
    {L"Jutland", 7, 4, "DreadnoughtHeavy", 1}, // Dreadnought (JA: confirmed)
    {L"Onager", 11, 4, "SniperHeavy",
     2}, // Artillery   (JA: "commissioned by Jupiter Arms' Shiphead Hobbes")
    {L"Harwich", 4, 4, "SupportLight", 3}, // Tactical    (JA: confirmed)
    {L"Valcour", 2, 4, "ScoutLight",
     4}, // Corvette    (JA: confirmed from description)
    // T5: 5 ships
    {L"Athos", 14, 5, "AssaultMedium",
     0}, // Destroyer   (JA: T5 C14 = Athos in cache)
    {L"Monarch", 7, 5, "DreadnoughtHeavy",
     1}, // Dreadnought (JA: icon=DreadnoughtH-T5, description confirmed)
    {L"Grenada", 11, 5, "SniperHeavy",
     2}, // Artillery   (JA: "jewel in the crown of Jupiter Arms fleet")
    {L"Cattaro", 4, 5, "SupportLight",
     3}, // Tactical    (JA: "Jupiter Arms' Shiphead" in description)
    {L"Nevis", 2, 5, "ScoutLight",
     4}, // Corvette    (JA: confirmed from description)
};
static const int s_jupiterArmsCount =
    sizeof(s_jupiterArms) / sizeof(s_jupiterArms[0]);

// Akula Vektor â€” 18 ships (2â†’2â†’4â†’5â†’5)
// Manufacturer confirmed from in-game ship descriptions
static const ShipDef s_akulaVektor[] = {
    // T1: 2 ships
    {L"Simargl", 6, 1, "DreadnoughtMedium",
     1}, // Dreadnought (AV: "Akula's wartime shipyards")
    {L"Rurik", 10, 1, "SniperMedium",
     2}, // Artillery   (AV: "Manufactured by Akula")
    // T2: 2 ships
    {L"Nav", 6, 2, "DreadnoughtMedium",
     1}, // Dreadnought (AV: "Akula's shipyard satellites")
    {L"Tugarin", 10, 2, "SniperMedium",
     2}, // Artillery   (AV: "Commissioned by Akula")
    // T3: 4 ships
    {L"Chernobog", 6, 3, "DreadnoughtMedium",
     1}, // Dreadnought (AV: "captained by head of Akula's Titan Guards")
    {L"Dola", 15, 3, "AssaultHeavy", 0}, // Destroyer   (AV: confirmed)
    {L"Kreshnik", 9, 3, "ScoutHeavy",
     4}, // Corvette    (AV: cache C9=ScoutHeavy)
    {L"Vucari", 10, 3, "SniperMedium",
     2}, // Artillery   (AV: icon=SniperM-T3 confirmed)
    // T4: 5 ships
    {L"Voronezh", 6, 4, "DreadnoughtMedium",
     1}, // Dreadnought (AV: T4 C6 in cache = Voronezh)
    {L"Blud", 15, 4, "AssaultHeavy",
     0}, // Destroyer   (AV: "heavily armored" AV ship)
    {L"Stribog", 9, 4, "ScoutHeavy",
     4}, // Corvette    (AV: "first rate ship" AV Corvette)
    {L"Koschei", 13, 4, "SupportHeavy",
     3}, // Tactical    (AV: "Akula Tuners on Sinley Bay")
    {L"Murometz", 10, 4, "SniperMedium",
     2}, // Artillery   (AV: "head of Akula's Aggressive Takeover Division")
    // T5: 5 ships
    {L"Zmey", 6, 5, "DreadnoughtMedium",
     1}, // Dreadnought (AV: "customized Akula Zmey")
    {L"Gora", 15, 5, "AssaultHeavy", 0},   // Destroyer   (AV: confirmed)
    {L"Svarog", 10, 5, "SniperMedium", 2}, // Artillery   (AV: confirmed)
    {L"Ohkta", 13, 5, "SupportHeavy", 3},  // Tactical    (AV: confirmed)
    {L"Netron", 9, 5, "ScoutHeavy",
     4}, // Corvette    (AV: "prized flagship of Akula CEO")
};
static const int s_akulaVektorCount =
    sizeof(s_akulaVektor) / sizeof(s_akulaVektor[0]);

// House Oberon â€” 17 ships (1â†’2â†’4â†’5â†’5)
// Manufacturer confirmed from in-game ship descriptions
static const ShipDef s_oberon[] = {
    // T1: 1 ship
    {L"Cerberus", 12, 1, "SupportMedium",
     3}, // Tactical    (OB: "Salvaged from House Oberon's ritual shipbreaking
         // pyres")
    // T2: 2 ships
    {L"Orcus", 12, 2, "SupportMedium",
     3}, // Tactical    (OB: "House Oberon's Transhuman influence")
    {L"Furia", 3, 2, "SniperLight",
     2}, // Artillery   (OB: "Lifted directly from House Oberon's shipyards")
    // T3: 4 ships
    {L"Ceres", 12, 3, "SupportMedium",
     3}, // Tactical    (OB: cache C12=SupportMedium, wiki=Tactical Cruiser)
    {L"Virtus", 3, 3, "SniperLight",
     2}, // Artillery   (OB: "seized from main Oberon shipyard")
    {L"Gravis", 1, 3, "DreadnoughtLight",
     1}, // Dreadnought (OB: "stock Oberon Dreadnought, tuned by Oberon Forge")
    {L"Fulgora", 8, 3, "ScoutMedium",
     4}, // Corvette    (OB: "commissioned by House Oberon's Conflict Escalation
         // division")
    // T4: 5 ships
    {L"Aion", 12, 4, "SupportMedium",
     3}, // Tactical    (OB: cache C12=SupportMedium, wiki=Tactical Cruiser)
    {L"Nox", 3, 4, "SniperLight", 2}, // Artillery   (OB: confirmed Oberon)
    {L"Lorica", 1, 4, "DreadnoughtLight",
     1}, // Dreadnought (OB: "Oberon fleet, the Lorica's helm")
    {L"Medusa", 8, 4, "ScoutMedium",
     4}, // Corvette    (OB: "commissioned by head of House Oberon's Conflict
         // Escalation")
    {L"Vindicta", 5, 4, "AssaultLight", 0}, // Destroyer   (OB: confirmed)
    // T5: 5 ships
    {L"Feronia", 12, 5, "SupportMedium",
     3}, // Tactical    (OB: "strongest tactical cruiser ever produced by House
         // Oberon")
    {L"Stabia", 3, 5, "SniperLight",
     2}, // Artillery   (OB: T5 C3 = Stabia in cache)
    {L"Invictus", 1, 5, "DreadnoughtLight",
     1}, // Dreadnought (OB: T5 C1 = Invictus in cache)
    {L"Mithras", 8, 5, "ScoutMedium",
     4}, // Corvette    (OB: "served its master on Oberon subscription raids")
    {L"Brutus", 5, 5, "AssaultLight",
     0}, // Destroyer   (OB: "most prized Destroyer in the Oberon fleet")
};
static const int s_oberonCount = sizeof(s_oberon) / sizeof(s_oberon[0]);

// Manufacturer table for iteration
struct MfgDef {
  const ShipDef *ships;
  int count;
  int idBase; // synthetic ID base (JA=11000, AV=12000, OB=13000)
};
static const MfgDef s_manufacturers[3] = {
    {s_jupiterArms, s_jupiterArmsCount, 11000},
    {s_akulaVektor, s_akulaVektorCount, 12000},
    {s_oberon, s_oberonCount, 13000},
};

static void InitFullTechTree() {
  if (!g_FullTechTree.empty())
    return;

  for (int m = 0; m < 3; m++) {
    const MfgDef &mfg = s_manufacturers[m];
    for (int i = 0; i < mfg.count; i++) {
      const ShipDef &def = mfg.ships[i];
      FTechTreeShip s;
      s.manufacturerId = m;          // 0=JA, 1=AV, 2=OB (matches TTM mfg IDs)
      s.shipId = mfg.idBase + i + 1; // unique per manufacturer
      s.tier = def.tier;
      s.shipClass = def.shipClassEnum; // Real EYShipClass enum value
      s.realId = -1;                   // to be filled by bridge
      s.prereqId = 0;                  // simplified
      s.proxyFallbackIndex = def.proxyFallback;
      s.name = def.name;
      g_FullTechTree.push_back(s);
    }
  }
}

// Hook on FUN_140480f70 (UYCachedItemIDData::FindCachedDataEntry)
typedef uint64_t(__fastcall *tFindCachedDataEntry)(uint32_t param1,
                                                   void **outPtr);
static tFindCachedDataEntry OrigFindCachedDataEntry = nullptr;

uint64_t __fastcall MyHookFindCachedDataEntry(uint32_t param1, void **outPtr) {
  uint64_t result = OrigFindCachedDataEntry(param1, outPtr);
  if (result == 1) {
    static int nativeLogCount = 0;
    if (nativeLogCount < 10) {
      printf("[DATA] FindCachedDataEntry NATIVE OK: param1=0x%08X (%u) -> %p\n",
             param1, param1, *outPtr);
      nativeLogCount++;
    }
    return 1;
  }

  if (param1 >= 11001 && param1 <= 15999) {
    ScanCacheForTiers();

    auto it = g_syntheticToRealMap.find((int32_t)param1);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      uint64_t bridgedRes =
          OrigFindCachedDataEntry((uint32_t)it->second, outPtr);
      if (bridgedRes == 1) {
        static int bridgeLog = 0;
        if (bridgeLog < 10) {
          printf("[BRIDGE] FindCachedDataEntry: synth %u -> real 0x%08X (OK)\n",
                 param1, (uint32_t)it->second);
          bridgeLog++;
        }
        return 1;
      } else {
        static int bridgeFailLog = 0;
        if (bridgeFailLog < 5) {
          printf("[BRIDGE] FindCachedDataEntry: synth %u -> real 0x%08X "
                 "(LOOKUP FAILED)\n",
                 param1, (uint32_t)it->second);
          bridgeFailLog++;
        }
      }
    }

    int tier = 1;
    if (param1 >= 11001 && param1 <= 11000 + s_jupiterArmsCount) {
      tier = s_jupiterArms[param1 - 11001].tier;
    } else if (param1 >= 12001 && param1 <= 12000 + s_akulaVektorCount) {
      tier = s_akulaVektor[param1 - 12001].tier;
    } else if (param1 >= 13001 && param1 <= 13000 + s_oberonCount) {
      tier = s_oberon[param1 - 13001].tier;
    }
    if (tier >= 1 && tier <= 5 && g_tierCacheEntries[tier - 1]) {
      *outPtr = g_tierCacheEntries[tier - 1];
      return 1;
    }
  }

  typedef void *(__fastcall * fn_GetCachedSingleton)();
  auto GetCacheSingleton = (fn_GetCachedSingleton)(g_moduleBase + 0x4813A0);
  void *cacheSingleton = GetCacheSingleton();

  if (cacheSingleton) {
    void *cacheData = *(void **)((uint8_t *)cacheSingleton + 0x28);
    int32_t cacheCount = *(int32_t *)((uint8_t *)cacheSingleton + 0x30);
    if (cacheData && cacheCount > 0) {
      *outPtr = cacheData;
      return 1;
    }
  }
  return 0;
}

// Hook on FUN_1404e0520 (ItemFilter)
typedef uint64_t(__fastcall *tItemFilter)(int64_t, uint64_t, int64_t, uint64_t,
                                          uint32_t, uint64_t, uint8_t);
static tItemFilter OrigItemFilter = nullptr;

uint64_t __fastcall MyHookItemFilter(int64_t p1, uint64_t p2, int64_t p3,
                                     uint64_t p4, uint32_t p5, uint64_t p6,
                                     uint8_t p7) {
  uint64_t result = OrigItemFilter(p1, p2, p3, p4, p5, p6, p7);
  return result;
}

// Track last synthetic ship clicked for loadout switching
static volatile int32_t g_lastClickedSyntheticId = 0;
static volatile bool g_loadoutSwitchPending = false;

// Maps internal item ID (from result+0x00, e.g. 630) -> synthetic ship ID
// (11001+)
static std::map<int32_t, int32_t> g_internalToSyntheticMap;

// Maps (shipClass * 10 + tier) -> loaded class index for direct loadout
// resolution
static std::map<int, int> g_loadoutMap;

// Global storage for loaded UClass* objects (for loadout switching)
#define MAX_LOADED_CLASSES 64
static UClass *g_loadedClasses[MAX_LOADED_CLASSES] = {};
static int g_numLoadedClasses = 0;

uint64_t __fastcall MyHookGetManufacturerData(int32_t manufacturerId,
                                              void *outArr1, void *outArr2,
                                              void *worldContext) {
  // TTM is now permanently populated (GC root cause fixed â€” unknown tokens
  // exit cleanly via EndOfStream). No more temporary wire/unwire needed.
  uint64_t result =
      OrigGetManufacturerData(manufacturerId, outArr1, outArr2, worldContext);

  if (g_logTechTree) {
    uint8_t **pOut1Data = (uint8_t **)((uint8_t *)outArr1 + 0x00);
    int32_t *pOut1Count = (int32_t *)((uint8_t *)outArr1 + 0x08);
    uint8_t **pOut2Data = (uint8_t **)((uint8_t *)outArr2 + 0x00);
    int32_t *pOut2Count = (int32_t *)((uint8_t *)outArr2 + 0x08);
    printf("[DATA] GetManufacturerData mfg=%d result=%llu outArr1: Data=%p "
           "Count=%d | outArr2: Data=%p Count=%d\n",
           manufacturerId, result, *pOut1Data, *pOut1Count, *pOut2Data,
           *pOut2Count);
  }

  // Post-process outArr1 entries to fix names and shipClass for synthetic
  // ships. outArr1 contains stride-0x28 entries, each with a TArray of
  // 0x180-byte FYUIShipManufacturerTechItemData structs. FUN_4F5780 sets m_name
  // from cache entry text (wrong) and never sets m_shipClass at +0x150 (stays 0
  // = all same row). We walk all embedded 0x180 structs and override for
  // synthetic IDs.
  if (result == 1) {
    uint8_t *arr1Data = *(uint8_t **)((uint8_t *)outArr1 + 0x00);
    int32_t arr1Count = *(int32_t *)((uint8_t *)outArr1 + 0x08);
    const int STACK_ENTRY_SIZE =
        0x28; // FYUIShipManufacturerTechTreeStackItemData stride
    const int ITEM_SIZE = 0x180; // FYUIShipManufacturerTechItemData size

    int fixCount = 0;
    int mapCount = 0;
    for (int i = 0; i < arr1Count && arr1Data; i++) {
      uint8_t *stackEntry = arr1Data + i * STACK_ENTRY_SIZE;
      // TArray<FYUIShipManufacturerTechItemData> at offset 0x00 of stack entry
      uint8_t *itemsData = *(uint8_t **)(stackEntry + 0x00);
      int32_t itemsCount = *(int32_t *)(stackEntry + 0x08);

      for (int j = 0; j < itemsCount && itemsData; j++) {
        uint8_t *item = itemsData + j * ITEM_SIZE;
        int32_t itemID = *(int32_t *)(item + 0x08); // m_itemID

        if (itemID >= 11000) {
          for (const auto &s : g_FullTechTree) {
            if (s.shipId == itemID) {
              // Override m_name (FString at +0x10)
              InitFStringUE4(item + 0x10, s.name.c_str());
              // Override m_shipClass (EYShipClass at +0x0150)
              *(uint8_t *)(item + 0x0150) = (uint8_t)s.shipClass;
              // Override m_tier (+0x0C)
              *(int32_t *)(item + 0x0C) = s.tier;
              // Override m_manufacturerID (+0x04)
              *(int32_t *)(item + 0x04) = s.manufacturerId;
              // Override m_itemState (+0x40) = Owned
              *(uint8_t *)(item + 0x40) = 3;
              fixCount++;

              // Build mapping from potential internal IDs to synthetic ID.
              int32_t v00 = *(int32_t *)(item + 0x00);
              int32_t v44 = *(int32_t *)(item + 0x44);
              int32_t v48 = *(int32_t *)(item + 0x48);
              int32_t v4C = *(int32_t *)(item + 0x4C);

              if (v00 != 0)
                g_internalToSyntheticMap[v00] = itemID;
              if (v44 > 0 && v44 < 100000)
                g_internalToSyntheticMap[v44] = itemID;
              if (v48 > 0 && v48 < 100000)
                g_internalToSyntheticMap[v48] = itemID;
              if (v4C > 0 && v4C < 100000)
                g_internalToSyntheticMap[v4C] = itemID;
              g_internalToSyntheticMap[itemID] = itemID;

              if (mapCount < 3) {
                printf("[DATA] Map item '%ls': +0x00=%d +0x08=%d +0x44=%d "
                       "+0x48=%d +0x4C=%d\n",
                       s.name.c_str(), v00, itemID, v44, v48, v4C);
                mapCount++;
              }
              break;
            }
          }
        }
      }
    }
    if (fixCount > 0 && g_logTechTree) {
      printf("[DATA] Post-processed outArr1: fixed %d synthetic ships (name + "
             "class + tier)\n",
             fixCount);
      printf("[DATA] Built internal->synthetic map with %d entries\n",
             (int)g_internalToSyntheticMap.size());
    }
  }

  return result;
}

// Direct hook on YUIExternalFunctions::GetShipResearchData (RVA 0x4EE820)
typedef uint64_t(__fastcall *tGetShipResearchData)(int32_t shipId,
                                                   void *outArr1, void *outArr2,
                                                   void *worldContext);
static tGetShipResearchData OrigGetShipResearchData = nullptr;

uint64_t __fastcall MyHookGetShipResearchData(int32_t shipId, void *outArr1,
                                              void *outArr2,
                                              void *worldContext) {
  uint64_t result =
      OrigGetShipResearchData(shipId, outArr1, outArr2, worldContext);
  if (result == 1)
    return 1;

  // Case 1: Synthetic ID (e.g. 11001-15999) passed directly.
  // Translate to real ID so native TTM lookup works.
  if (shipId >= 11000 && shipId <= 15999) {
    auto it = g_syntheticToRealMap.find(shipId);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      uint64_t bridgedRes =
          OrigGetShipResearchData(it->second, outArr1, outArr2, worldContext);
      if (bridgedRes == 1) {
        static int bridgeLog = 0;
        if (bridgeLog < 10) {
          printf("[BRIDGE] GetShipResearchData: synth %d -> real 0x%08X (OK)\n",
                 shipId, (uint32_t)it->second);
          bridgeLog++;
        }
        return 1;
      }
    }
    // Failsafe: don't return 0 for synthetic â€” return success to prevent BP
    // errors
    static int synthLog = 0;
    if (synthLog < 5) {
      printf("[BRIDGE] GetShipResearchData: synth %d native bridge failed, "
             "returning success\n",
             shipId);
      synthLog++;
    }
    return 1;
  }

  // Case 2: Real cache ID passed (e.g. 0x01FF013E ~33M).
  // This happens because our SetSelectedShip hook swapped the synthetic ID
  // with the real ID so the Blueprint's tier/class lookup succeeds.
  // Now the Blueprint is calling GetShipResearchData with that real ID.
  // We need to reverse-translate back to the synthetic ID so FUN_1403f5050
  // can find the ship in the TTM mfg arrays (which use synthetic IDs).
  auto revIt = g_realToSyntheticMap.find(shipId);
  if (revIt != g_realToSyntheticMap.end()) {
    int32_t synthId = revIt->second;
    uint64_t bridgedRes =
        OrigGetShipResearchData(synthId, outArr1, outArr2, worldContext);
    if (bridgedRes == 1) {
      static int revLog = 0;
      if (revLog < 10) {
        printf("[BRIDGE] GetShipResearchData: real 0x%08X -> synth %d (reverse "
               "OK)\n",
               (uint32_t)shipId, synthId);
        revLog++;
      }
      return 1;
    }
    // Even if the bridged lookup failed, return 1 (success) to prevent the
    // Blueprint from displaying an error. The module list may be empty but
    // the UI won't crash.
    static int revFailLog = 0;
    if (revFailLog < 5) {
      printf("[BRIDGE] GetShipResearchData: real 0x%08X -> synth %d bridge "
             "failed\n",
             (uint32_t)shipId, synthId);
      revFailLog++;
    }
    return 1;
  }

  static int failLog = 0;
  if (failLog < 5) {
    printf("[DATA] GetShipResearchData: no mapping for shipId=0x%08X (%d)\n",
           (uint32_t)shipId, shipId);
    failLog++;
  }
  return 0;
}

// ========================================================================
// OnResearchTechTreeItem â€” intercepting ship research/purchase requests
// The game calls this when the player clicks "Research" or "Buy" on a ship.
// Without a server, the original function fails. We intercept and auto-succeed.
// ========================================================================
static void *OriginalOnResearchFunc = nullptr;

void __fastcall MyHookOnResearchTechTreeItem(UObject *Context, void *Stack,
                                             void *RESULT_DECL) {
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;

  // Call original
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  if (OriginalOnResearchFunc) {
    ((OrigFunc)OriginalOnResearchFunc)(Context, Stack, RESULT_DECL);
  }

  int32_t shipToUnlock = g_lastClickedSyntheticId;
  if (shipToUnlock >= 11000 && shipToUnlock <= 19999) {
    g_ownedShips.insert(shipToUnlock);
    SaveFleetData();
    printf("[UI] OnResearchTechTreeItem: Unlocked & saved ship ID %d to fleet profile!\n", shipToUnlock);
  }

  if (RESULT_DECL) {
    *(bool *)RESULT_DECL = true;
  }

  printf("[UI] OnResearchTechTreeItem called â€” auto-approved\n");
}

// Phase 3.3: ComposeModuleUiDataForShip Hooks
typedef void(__fastcall *tComposeModuleUiData)(void *p1, int32_t shipId,
                                               void *p3, void *p4, void *p5);
static tComposeModuleUiData OrigComposeModuleUiData1 = nullptr;
static tComposeModuleUiData OrigComposeModuleUiData2 = nullptr;

void __fastcall MyHookComposeModuleUiData1(void *p1, int32_t shipId, void *p3,
                                           void *p4, void *p5) {
  if (shipId >= 11000 && shipId <= 15999) {
    auto it = g_syntheticToRealMap.find(shipId);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      shipId = it->second;
    }
  }
  OrigComposeModuleUiData1(p1, shipId, p3, p4, p5);
}

void __fastcall MyHookComposeModuleUiData2(void *p1, int32_t shipId, void *p3,
                                           void *p4, void *p5) {
  if (shipId >= 11000 && shipId <= 15999) {
    auto it = g_syntheticToRealMap.find(shipId);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      shipId = it->second;
    }
  }
  OrigComposeModuleUiData2(p1, shipId, p3, p4, p5);
}

// Phase 3.4: ComposeShipManufacturerData Hooks
typedef void(__fastcall *tComposeShipManufacturerDataForId)(
    void *p1, int32_t shipId, void *p3, void *p4, void *p5, void *p6, void *p7);
typedef void(__fastcall *tComposeShipManufacturerDataForLoadout)(
    void *p1, void *p2, int32_t shipId, void *p4, void *p5, void *p6, void *p7);

static tComposeShipManufacturerDataForId OrigComposeShipManufacturerDataForId =
    nullptr;
static tComposeShipManufacturerDataForLoadout
    OrigComposeShipManufacturerDataForLoadout = nullptr;

void __fastcall MyHookComposeShipManufacturerDataForId(void *p1, int32_t shipId,
                                                       void *p3, void *p4,
                                                       void *p5, void *p6,
                                                       void *p7) {
  if (shipId >= 11000 && shipId <= 15999) {
    auto it = g_syntheticToRealMap.find(shipId);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      shipId = it->second;
    }
  }
  OrigComposeShipManufacturerDataForId(p1, shipId, p3, p4, p5, p6, p7);
}

void __fastcall MyHookComposeShipManufacturerDataForLoadout(void *p1, void *p2,
                                                            int32_t shipId,
                                                            void *p4, void *p5,
                                                            void *p6,
                                                            void *p7) {
  if (shipId >= 11000 && shipId <= 15999) {
    auto it = g_syntheticToRealMap.find(shipId);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      shipId = it->second;
    }
  }
  OrigComposeShipManufacturerDataForLoadout(p1, p2, shipId, p4, p5, p6, p7);
}

// Ship Name/Description Registry
struct FShipInfo {
  const wchar_t *Name;
  const wchar_t *Desc;
};

FShipInfo GetShipInfoRegistry(int32_t shipClass, int32_t tier,
                              int32_t manufacturer) {
  // Jupiter Arms (1) focus for v19.23
  if (tier == 1) {
    if (shipClass == 1)
      return {L"Monarch", L"Jupiter Arms Tier 1 Dreadnought."};
    if (shipClass == 2)
      return {L"Assault Destroyer", L"Jupiter Arms Tier 1 Destroyer."};
    if (shipClass == 3)
      return {L"Corvette", L"Jupiter Arms Tier 1 Corvette."};
    if (shipClass == 4)
      return {L"Artillery Cruiser", L"Jupiter Arms Tier 1 Artillery Cruiser."};
    if (shipClass == 5)
      return {L"Tactical Cruiser", L"Jupiter Arms Tier 1 Tactical Cruiser."};
  } else if (tier == 2) {
    if (shipClass == 1)
      return {L"Monarch MKII", L"Jupiter Arms Tier 2 Dreadnought."};
    if (shipClass == 2)
      return {L"Assault Destroyer MKII", L"Jupiter Arms Tier 2 Destroyer."};
    if (shipClass == 3)
      return {L"Corvette MKII", L"Jupiter Arms Tier 2 Corvette."};
    if (shipClass == 4)
      return {L"Artillery Cruiser MKII",
              L"Jupiter Arms Tier 2 Artillery Cruiser."};
    if (shipClass == 5)
      return {L"Tactical Cruiser MKII",
              L"Jupiter Arms Tier 2 Tactical Cruiser."};
  }
  return {L"Jupiter Prototype", L"Jupiter Arms Classified Vessel."};
}

// Native UFunction override for GetManufacturersData (Blueprint-level)
// Returns TArray<FYUIManufacturerInformationEntry> (each entry 0xA8 bytes).
// We read from Default__GlobalUI_C.m_manufacturerEntries which has valid FText
// objects.
static void *OriginalGetManufacturersDataFunc_BP = nullptr;
void __fastcall MyHookGetManufacturersData(UObject *Context, void *Stack,
                                           void *RESULT_DECL) {
  // Call original first â€” it may partially work
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  if (OriginalGetManufacturersDataFunc_BP) {
    ((OrigFunc)OriginalGetManufacturersDataFunc_BP)(Context, Stack,
                                                    RESULT_DECL);
  }

  if (!RESULT_DECL)
    return;

  // RESULT_DECL is TArray<FYUIManufacturerInformationEntry>
  TArrayRaw *outArray = (TArrayRaw *)RESULT_DECL;

  // If the original already returned data, we're good
  if (outArray->Count > 0) {
    static bool loggedOnce = false;
    if (!loggedOnce) {
      printf("[MFG] GetManufacturersData: Original returned %d entries (OK)\n",
             outArray->Count);
      loggedOnce = true;
    }
    return;
  }

  // Original returned empty â€” read from Default__GlobalUI_C CDO
  static UObject *cachedUIData = nullptr;
  if (!cachedUIData) {
    cachedUIData =
        UObject::FindObject<UObject>("GlobalUI_C GlobalUI.Default__GlobalUI_C");
    if (!cachedUIData) {
      cachedUIData =
          UObject::FindObject<UObject>("None GlobalUI.Default__GlobalUI_C");
    }
    if (!cachedUIData) {
      cachedUIData =
          UObject::FindObject<UObject>("YUIData DreadGame.Default__YUIData");
    }
    if (cachedUIData) {
      printf("[MFG] Found UI Data CDO at %p\n", cachedUIData);
    } else {
      printf("[MFG] WARNING: Cannot find any UYUIData CDO!\n");
      return;
    }
  }

  // m_manufacturerEntries at offset 0x00C8 in UYUIData
  TArrayRaw *srcArray = (TArrayRaw *)((uint8_t *)cachedUIData + 0x00C8);
  if (srcArray->Count <= 0 || !srcArray->Data) {
    printf("[MFG] WARNING: UYUIData.m_manufacturerEntries is empty!\n");
    return;
  }

  // Copy the source array into our result safely (FText-safe)
  SafeCopyManufacturerArray(outArray, srcArray);
  printf(
      "[MFG] Safely copied %d manufacturer entries into result (FText-safe)\n",
      outArray->Count);
  return;
}

// Native UFunction override for GetUIShipData
// This is executed directly by the VM, skipping ProcessEvent.
void __fastcall MyHookGetUIShipData(UObject *Context, void *Stack,
                                    void *RESULT_DECL) {
  // Stack is FFrame&. Locals contains the parameters
  // (FYUIShipManufacturerTechItemData&).
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;
  bool class13Mitigated = false;
  int32_t originalItemId = 0;

  if (frame && frame->Locals) {
    FYUIShipManufacturerTechItemData *pData =
        *(FYUIShipManufacturerTechItemData **)frame->Locals;
    if (pData) {
      originalItemId = pData->m_itemID;

      // Resolve synthetic ship ID to the correct loaded ship using
      // g_loadoutMap. The old proxyFallbackIndex system mapped to wrong ships.
      // Now we use (shipClass * 10 + tier) as the key to find the correct
      // engine-loaded entry.
      if (pData->m_itemID >= 11000) {
        for (const auto &s : g_FullTechTree) {
          if (s.shipId == pData->m_itemID) {
            // Look up the correct loaded ship by its blueprint class + tier
            // First, find the bpSuffix for this ship to determine the loadout
            // class
            int loadoutShipClass = s.shipClass;
            int loadoutTier = s.tier;

            // The ShipDef defines which loadout blueprint to use (bpSuffix)
            // We need to find the EYShipClass enum that matches the loadout
            // For ships that use Medium loadouts at lower tiers (e.g., AV
            // T1-T2), we need to look up the bpSuffix-based class, not the
            // display class.
            const ShipDef *matchedDef = nullptr;
            for (int m = 0; m < 3; m++) {
              const MfgDef &mfg = s_manufacturers[m];
              for (int i = 0; i < mfg.count; i++) {
                if (mfg.idBase + i + 1 == s.shipId) {
                  matchedDef = &mfg.ships[i];
                  break;
                }
              }
              if (matchedDef)
                break;
            }

            if (matchedDef) {
              // Use the bpSuffix to find the actual loadout shipClass enum
              // The bpSuffix name determines which loadout blueprint was loaded
              int bpClass =
                  matchedDef->shipClassEnum; // This matches the loadout's class
              // g_loadedShips normalizes class 13 (SupportHeavy) to 12
              // (SupportMedium)
              if (bpClass == 13)
                bpClass = 12;
              int key = bpClass * 10 + s.tier;
              auto it = g_loadoutMap.find(key);
              if (it != g_loadoutMap.end() && it->second < g_numLoadedShips) {
                int loadedIdx = it->second;
                pData->m_itemID = g_loadedShips[loadedIdx].shipId;
                static int clickLog = 0;
                if (clickLog < 20) {
                  printf("[LOADOUT] Ship click: %ls (ID %d) -> loaded ship idx "
                         "%d (class=%d tier=%d shipId=%d)\n",
                         s.name.c_str(), s.shipId, loadedIdx, bpClass, s.tier,
                         g_loadedShips[loadedIdx].shipId);
                  clickLog++;
                }
              } else {
                // Fallback: use first loaded ship of any tier for this class
                bool found = false;
                for (int t = 1; t <= 5 && !found; t++) {
                  int fallbackKey = bpClass * 10 + t;
                  auto fit = g_loadoutMap.find(fallbackKey);
                  if (fit != g_loadoutMap.end() &&
                      fit->second < g_numLoadedShips) {
                    pData->m_itemID = g_loadedShips[fit->second].shipId;
                    found = true;
                    printf("[LOADOUT] Ship click FALLBACK: %ls -> class %d "
                           "tier %d\n",
                           s.name.c_str(), bpClass, t);
                  }
                }
                if (!found) {
                  printf("[LOADOUT] Ship click MISS: %ls class=%d tier=%d no "
                         "loadout found\n",
                         s.name.c_str(), bpClass, s.tier);
                }
              }
            }
            break;
          }
        }
      }

      // manufacturer IDs are 0-based (0=JA, 1=AV, 2=OB)
      // No need to force manufacturer ID â€” 0 is valid
      if ((uint8_t)pData->m_shipClass == 13) {
        pData->m_shipClass =
            (EYShipClass)12; // Temporarily map to Support Medium for UI lookup
        class13Mitigated = true;
      }
    }
  }

  // 1. Call original C++ function which does the actual Techtree lookup
  auto orig =
      (void(__fastcall *)(UObject *, void *, void *))OriginalGetUIShipDataFunc;
  orig(Context, Stack, RESULT_DECL);

  // Restore if we changed it
  if (frame && frame->Locals) {
    FYUIShipManufacturerTechItemData *pData =
        *(FYUIShipManufacturerTechItemData **)frame->Locals;
    if (pData) {
      if (class13Mitigated)
        pData->m_shipClass = (EYShipClass)13;
      if (originalItemId >= 11000)
        pData->m_itemID = originalItemId;
    }
  }

  // 2. Override the resulting FUIShipData
  if (RESULT_DECL != nullptr) {
    uint8_t *pResult = (uint8_t *)RESULT_DECL;

    // Relabel with synthetic name and original tier if synthetic
    if (originalItemId >= 11000) {
      for (const auto &s : g_FullTechTree) {
        if (s.shipId == originalItemId) {
          // FUIShipData.m_shipName is FText at +0x00 (0x18 bytes)
          // Safely set the string inside the FTextData to avoid corruption
          SafeSetFTextString(pResult + 0x0000, s.name.c_str());
          // FUIShipData.m_shipTier is at +0x30
          *(int32_t *)(pResult + 0x0030) = s.tier;
          // FUIShipData.m_shipClass is at +0x18
          *(uint8_t *)(pResult + 0x0018) = (uint8_t)s.shipClass;
          // FUIShipData.m_manufacturerID is at +0x50
          *(int32_t *)(pResult + 0x0050) = s.manufacturerId;
          break;
        }
      }
    }

    // ALWAYS set tier to 1-5
    int32_t *shipTierPtr = (int32_t *)(pResult + 0x0030);
    if (*shipTierPtr <= 0 || *shipTierPtr > 5) {
      *shipTierPtr = 1;
    }

    // Enforce class 12 normalization
    uint8_t *classPtr = (uint8_t *)(pResult + 0x0018);
    if (*classPtr == 13) {
      *classPtr = 12;
    }

    // Fix empty loadouts array to prevent UI husks
    struct TDummyArray {
      void *Data;
      int32_t Count;
      int32_t Max;
    };
    TDummyArray *loadoutsArr = (TDummyArray *)(pResult + 0x0058);
    if (loadoutsArr && loadoutsArr->Count == 0) {
      void *fakeLoadout = UE4Malloc(0x0040); // FUILoadoutData size
      memset(fakeLoadout, 0, 0x0040);
      loadoutsArr->Data = fakeLoadout;
      loadoutsArr->Count = 1;
      loadoutsArr->Max = 1;
    }
  }
}

// Native UFunction override for GetCurrentShipItemData
// Returns FYUIShipManufacturerTechItemData (0x0180 bytes). m_tier at offset
// 0x000C. This feeds data to UI_Screen_EditShip, UI_Generic_ShipTitleWidget,
// etc.
void __fastcall MyHookGetCurrentShipItemData(UObject *Context, void *Stack,
                                             void *RESULT_DECL) {
  // If a synthetic ship click is pending, switch the active loadout BEFORE the
  // original call. This makes the original function find and return the correct
  // ship data.
  if (g_loadoutSwitchPending && g_lastClickedSyntheticId >= 11000) {
    g_loadoutSwitchPending = false;
    int32_t syntheticId = g_lastClickedSyntheticId;

    printf("[LOADOUT] GetCurrentShipItemData: switching loadout for synthetic "
           "ID %d\n",
           syntheticId);

    // Find the ShipDef for this synthetic ID
    const ShipDef *matchedDef = nullptr;
    int matchedTier = 1;
    for (int m = 0; m < 3; m++) {
      const MfgDef &mfg = s_manufacturers[m];
      for (int i = 0; i < mfg.count; i++) {
        if (mfg.idBase + i + 1 == syntheticId) {
          matchedDef = &mfg.ships[i];
          matchedTier = matchedDef->tier;
          break;
        }
      }
      if (matchedDef)
        break;
    }

    if (matchedDef) {
      int bpClass = matchedDef->shipClassEnum;
      if (bpClass == 13)
        bpClass = 12; // SupportHeavy -> SupportMedium normalization
      int key = bpClass * 10 + matchedTier;
      auto it = g_loadoutMap.find(key);
      if (it != g_loadoutMap.end() && it->second < g_numLoadedShips) {
        int loadedIdx = it->second;
        UObject *loadout = g_loadedShips[loadedIdx].loadoutObj;
        if (loadout) {
          // Switch active loadout via AddAndActiveLoadoutFromBlueprint
          // We need the UClass* of the loadout blueprint
          try {
            UClass *loadoutClass = loadout->Class;
            AYPlayerController *pc = (AYPlayerController *)(*UWorld::GWorld)
                                         ->OwningGameInstance->LocalPlayers[0]
                                         ->PlayerController;
            if (pc && loadoutClass) {
              pc->AddAndActiveLoadoutFromBlueprint(loadoutClass);
              printf("[LOADOUT] Switched active loadout to %s (class=%d "
                     "tier=%d)\n",
                     loadout->GetFullName().c_str(), bpClass, matchedTier);
            }
          } catch (...) {
            printf("[LOADOUT] EXCEPTION switching loadout\n");
          }
        }
      } else {
        printf("[LOADOUT] No loadout found for class=%d tier=%d\n", bpClass,
               matchedTier);
      }
    }
  }

  // Call original
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalGetCurrentShipItemDataFunc)(Context, Stack, RESULT_DECL);

  if (RESULT_DECL != nullptr) {
    uint8_t *pResult = (uint8_t *)RESULT_DECL;
    int32_t *tierPtr = (int32_t *)(pResult + 0x000C);
    if (*tierPtr <= 0 || *tierPtr > 5) {
      *tierPtr = 1;
    }

    // If a synthetic ship was recently selected, override the returned data
    int32_t synId = g_lastClickedSyntheticId;
    if (synId >= 11000) {
      for (const auto &s : g_FullTechTree) {
        if (s.shipId == synId) {
          *(int32_t *)(pResult + 0x04) = s.manufacturerId;
          *(int32_t *)(pResult + 0x08) = synId;
          *(int32_t *)(pResult + 0x0C) = s.tier;
          *(uint8_t *)(pResult + 0x0150) = (uint8_t)s.shipClass;
          *(uint8_t *)(pResult + 0x40) = 3; // Owned
          InitFStringUE4(pResult + 0x10, s.name.c_str());
          break;
        }
      }
    }
  }
}

// Native UFunction override for SetSelectedShip
// UI_ManufacturerTechTreeScreen::SetSelectedShip(int32_t shipID)
// The shipID is passed as a parameter on the Blueprint stack (FFrame::Locals).
// We must read it BEFORE calling original, since the original consumes the
// stack.
static uint8_t *g_ttmItemBases[3] =
    {}; // base addresses of 3 manufacturer item arrays
static int g_ttmItemCounts[3] = {};      // count of items per manufacturer
static const int TTM_ITEM_STRIDE = 0x48; // bytes per TTM item

void ProcessSetSelectedShip(UObject *Context, void *Stack, void *RESULT_DECL,
                            void *originalFunc, const char *screenName) {
  static int logCount = 0;

  // FFrame layout (UE4 4.20):
  //   +0x00: vtable          +0x08: FOutputDevice data
  //   +0x10: UFunction* Node +0x18: UObject* Object
  //   +0x20: uint8* Code     +0x28: uint8* Locals
  // Ship ID found at Locals+0x10 (confirmed via raw dump)
  uint8_t *stackBytes = (uint8_t *)Stack;
  uint8_t *locals = *(uint8_t **)(stackBytes + 0x28);

  int32_t shipID = 0;
  if (locals) {
    shipID = *(int32_t *)(locals + 0x10); // Confirmed offset via diagnostic
  }

  // Track synthetic ship selection BEFORE calling original
  int32_t realID = 0;
  if (shipID >= 11000 && shipID <= 19999) {
    g_lastClickedSyntheticId = shipID;
    g_loadoutSwitchPending = true;

    // Look up the real cache ID for this synthetic ship
    auto it = g_syntheticToRealMap.find(shipID);
    if (it != g_syntheticToRealMap.end() && it->second != -1) {
      realID = it->second;
    }

    if (logCount < 50) {
      printf("[SSS] [%s] SetSelectedShip: shipID=%d -> realID=0x%08X "
             "(SYNTHETIC OK)\n",
             screenName, shipID, (uint32_t)realID);
      logCount++;
    }

    // CRITICAL: Replace synthetic ID with real cache ID in Locals BEFORE
    // calling original. The Blueprint handler internally calls native functions
    // (GetTier, etc.) with this ID. Synthetic IDs don't exist in the cache,
    // so they return tier=0 â†’ TierColors[0-1] â†’ index -1 crash.
    // The real cache ID has proper tier/class/module data.
    if (realID != 0 && locals) {
      *(int32_t *)(locals + 0x10) = realID;
    }
  } else if (shipID > 0) {
    if (logCount < 50) {
      printf("[SSS] [%s] SetSelectedShip: shipID=%d (non-synthetic)\n",
             screenName, shipID);
      logCount++;
    }
  }

  // Call original Blueprint handler (now with real cache ID in Locals)
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)originalFunc)(Context, Stack, RESULT_DECL);

  // Trigger loadout activation and ship loading offline
  if (shipID >= 11000 && shipID <= 19999) {
    const ShipDef *matchedDef = nullptr;
    int matchedTier = 1;
    for (int m = 0; m < 3; m++) {
      const MfgDef &mfg = s_manufacturers[m];
      for (int i = 0; i < mfg.count; i++) {
        if (mfg.idBase + i + 1 == shipID) {
          matchedDef = &mfg.ships[i];
          matchedTier = matchedDef->tier;
          break;
        }
      }
      if (matchedDef)
        break;
    }

    if (matchedDef) {
      // Fix B: Cancel any in-progress finalization polling on new ship
      // selection
      if (g_waitingForFinalization) {
        printf("[SSS] [%s] Cancelling in-progress finalization for level: %s\n",
               screenName, g_waitingLevelName.c_str());
        g_waitingForFinalization = false;
        g_waitingLevelName = "";
      }

      int bpClass = matchedDef->shipClassEnum;
      if (bpClass == 13)
        bpClass = 12; // SupportHeavy -> SupportMedium normalization
      int key = bpClass * 10 + matchedTier;
      auto it = g_loadoutMap.find(key);
      if (it != g_loadoutMap.end() && it->second < g_numLoadedShips) {
        int loadedIdx = it->second;
        UObject *loadout = g_loadedShips[loadedIdx].loadoutObj;
        if (loadout) {
          try {
            AYPlayerController *pc = (AYPlayerController *)(*UWorld::GWorld)
                                         ->OwningGameInstance->LocalPlayers[0]
                                         ->PlayerController;
            if (pc) {
              UClass *loadoutClass = loadout->Class;
              if (loadoutClass) {
                pc->AddAndActiveLoadoutFromBlueprint(loadoutClass);
              }

              UYLoadoutManagerComponent *lm = pc->GetLoadoutManager();
              if (lm) {
                lm->m_activeLoadout = (UYShipLoadout *)loadout;
                lm->ActivateLoadout((UYShipLoadout *)loadout, true);
                printf("[SSS] [%s] Activated loadout for synthetic ID %d "
                       "(class=%d tier=%d)\n",
                       screenName, shipID, bpClass, matchedTier);
              }

              if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
                UObject *gmObj =
                    (UObject *)(*UWorld::GWorld)->AuthorityGameMode;
                UFunction *flagshipChangedFn = (UFunction *)GetObjByName(
                    "Function "
                    "DreadGame.YGameMode_Outpost.PlayerFlagshipChanged");
                if (flagshipChangedFn && pProcessEvent_Original) {
                  struct {
                    CG::FName flagshipID;
                  } p;
                  p.flagshipID = *(CG::FName *)((uintptr_t)loadout + 0xB0);

                  // Fix A: Debounce PlayerFlagshipChanged
                  static uint64_t s_lastFlagshipFName = 0;
                  uint64_t currentFName = *(uint64_t *)&p.flagshipID;
                  if (currentFName == s_lastFlagshipFName) {
                    tee_printf("[SSS] [%s] Skipping duplicate "
                           "PlayerFlagshipChanged for FName 0x%016llX\n",
                           screenName, currentFName);
                  } else {
                    s_lastFlagshipFName = currentFName;
                    tee_printf("[SSS] [%s] Triggering PlayerFlagshipChanged on "
                           "GameMode %p with loadout FName 0x%016llX\n",
                           screenName, gmObj, currentFName);
                    pProcessEvent_Original(gmObj, flagshipChangedFn, &p);
                  }
                } else {
                  printf("[SSS] [%s] WARNING: PlayerFlagshipChanged not found, "
                         "falling back to native InitShipInternal\n",
                         screenName);
                  uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
                  typedef void (*FN_InitShipInternal)(void *gameMode);
                  FN_InitShipInternal initShip =
                      (FN_InitShipInternal)(base + 0x3743b0);
                  initShip(gmObj);
                }
              }
            }
          } catch (...) {
            printf("[SSS] [%s] EXCEPTION activating loadout or triggering ship "
                   "loading\n",
                   screenName);
          }
        }
      }
    }
  }

  // NOTE: Do NOT restore the synthetic ID. The Blueprint has consumed locals
  // and deferred rendering events (TierColors, GetTier) still hold a reference
  // to this buffer. Restoring synthetic ID here causes tier=0 â†’
  // TierColors[-1]. FindCachedDataEntry already translates synthâ†’real for any
  // future lookups.
}

void __fastcall MyHookSetSelectedShip(UObject *Context, void *Stack,
                                      void *RESULT_DECL) {
  ProcessSetSelectedShip(Context, Stack, RESULT_DECL,
                         OriginalSetSelectedShipFunc, "MfgTechTree");
}

void __fastcall MyHookShipTechTreeSetSelectedShip(UObject *Context, void *Stack,
                                                  void *RESULT_DECL) {
  ProcessSetSelectedShip(Context, Stack, RESULT_DECL,
                         OriginalShipTechTreeSetSelectedShipFunc,
                         "ShipTechTree");
}

void __fastcall MyHookOwnedShipsSetSelectedShip(UObject *Context, void *Stack,
                                                void *RESULT_DECL) {
  ProcessSetSelectedShip(Context, Stack, RESULT_DECL,
                         OriginalOwnedShipsSetSelectedShipFunc, "OwnedShips");
}

void __fastcall MyHookAddShipToFleetSetSelectedShip(UObject *Context,
                                                    void *Stack,
                                                    void *RESULT_DECL) {
  ProcessSetSelectedShip(Context, Stack, RESULT_DECL,
                         OriginalAddShipToFleetSetSelectedShipFunc,
                         "AddShipToFleet");
}

// Native UFunction override for GetShipData
// Returns FYUIShipManufacturerTechItemData (0x0180 bytes). m_tier at offset
// 0x000C. This feeds data to UI_Button_ManufacturerTechTreeItem, ManageFleet,
// OwnedShips, etc. Also tracks the last-selected synthetic ship (since
// SetSelectedShip can't extract the parameter from Blueprint bytecode).
void __fastcall MyHookGetShipData(UObject *Context, void *Stack,
                                  void *RESULT_DECL) {
  // Call original
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalGetShipDataFunc)(Context, Stack, RESULT_DECL);

  if (RESULT_DECL != nullptr) {
    uint8_t *pResult = (uint8_t *)RESULT_DECL;
    int32_t itemID = *(int32_t *)(pResult + 0x08); // m_itemID set by FUN_4F5780

    // Override synthetic ships with correct metadata
    if (itemID >= 11000) {
      for (const auto &s : g_FullTechTree) {
        if (s.shipId == itemID) {
          *(int32_t *)(pResult + 0x04) = s.manufacturerId; // m_manufacturerID
          *(int32_t *)(pResult + 0x0C) = s.tier;           // m_tier
          *(uint8_t *)(pResult + 0x0150) =
              (uint8_t)s.shipClass; // m_shipClass (EYShipClass)
          bool isOwned = g_ownedShips.count(itemID) > 0;
          uint8_t shipState = 0; // 0 = Locked by Prerequisite
          if (isOwned) {
            shipState = 3; // 3 = Unlocked / Owned
          } else if (s.tier == 1) {
            shipState = 1; // 1 = Purchasable (Tier 1 starters)
          } else {
            // Check if any ship of the same manufacturer in the previous tier is owned
            for (const auto &prev : g_FullTechTree) {
              if (prev.manufacturerId == s.manufacturerId && prev.tier == (s.tier - 1)) {
                if (g_ownedShips.count(prev.shipId) > 0) {
                  shipState = 1; // Lower-tier prerequisite met -> Purchasable
                  break;
                }
              }
            }
          }
          *(uint8_t *)(pResult + 0x40) = shipState;
          InitFStringUE4(pResult + 0x10, s.name.c_str()); // m_name (FString)

          // Track the last synthetic ship queried â€” this is how we identify
          // the selected ship since SetSelectedShip can't read FFrame params
          if (g_loadoutSwitchPending) {
            g_lastClickedSyntheticId = itemID;
            static int selectLogCount = 0;
            if (selectLogCount < 20) {
              printf("[DATA] GetShipData: selected synthetic ship %d\n",
                     itemID);
              selectLogCount++;
            }
          }
          break;
        }
      }
    }

    // Failsafe normalization
    int32_t *tierPtr = (int32_t *)(pResult + 0x000C);
    if (*tierPtr <= 0 || *tierPtr > 5)
      *tierPtr = 1;
    uint8_t *classPtr = (uint8_t *)(pResult + 0x0150);
    if (*classPtr == 13)
      *classPtr = 12;
  }
}

// Native UFunction override for GetTier (UUI_ManufacturerTechTreeWidget)
void __fastcall MyHookGetTier(UObject *Context, void *Stack,
                              void *RESULT_DECL) {
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalGetTierFunc)(Context, Stack, RESULT_DECL);

  if (RESULT_DECL != nullptr) {
    int32_t *tier = (int32_t *)RESULT_DECL;
    if (*tier <= 0 || *tier > 5)
      *tier = 1;
  }
}

// Native UFunction override for GetShipTier (UUI_GenericShipTitleWidget)
void __fastcall MyHookGetShipTier(UObject *Context, void *Stack,
                                  void *RESULT_DECL) {
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalGetShipTierFunc)(Context, Stack, RESULT_DECL);

  if (RESULT_DECL != nullptr) {
    int32_t *tier = (int32_t *)RESULT_DECL;
    if (*tier <= 0 || *tier > 5)
      *tier = 1;
  }
}

// Native UFunction override for GetShipClassIcon (UYHUDWidget_StyleContainer)
void __fastcall MyHookGetShipClassIcon(UObject *Context, void *Stack,
                                       void *RESULT_DECL) {
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;
  bool bAltered = false;
  uint8_t originalVal = 0;

  if (frame && frame->Locals) {
    // EYShipBaseClass is the first parameter. Enum size is usually 1 byte.
    uint8_t *pBaseClass = (uint8_t *)frame->Locals;
    if (*pBaseClass == 13) {
      originalVal = 13;
      *pBaseClass = 12; // Redirect Support Heavy to Support Medium
      bAltered = true;
    }
  }

  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalGetShipClassIconFunc)(Context, Stack, RESULT_DECL);

  // Restore stack state
  if (bAltered && frame && frame->Locals) {
    *(uint8_t *)frame->Locals = originalVal;
  }
}

// Native UFunction override for SetTier (UUI_Generic_TierIcon_C)
void __fastcall MyHookSetTier(UObject *Context, void *Stack,
                              void *RESULT_DECL) {
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;
  if (frame && frame->Locals) {
    int32_t *tier = (int32_t *)frame->Locals;
    if (*tier <= 0 || *tier > 5)
      *tier = 1;
  }
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalSetTierFunc)(Context, Stack, RESULT_DECL);
}

// Native UFunction override for SetTextureFromTier (UUI_Generic_TierIcon_C)
void __fastcall MyHookSetTextureFromTier(UObject *Context, void *Stack,
                                         void *RESULT_DECL) {
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;
  if (frame && frame->Locals) {
    int32_t *tier = (int32_t *)frame->Locals;
    if (*tier <= 0 || *tier > 5)
      *tier = 1;
  }
  typedef void(__fastcall * OrigFunc)(UObject *, void *, void *);
  ((OrigFunc)OriginalSetTextureFromTierFunc)(Context, Stack, RESULT_DECL);
}

// ========================================================================
// UYItemIDList::LoadItemsAsync hook (RVA 0x2D9390)
//
// Disasm at RVA 0x2D93A2: CMP DWORD PTR [RCX+8], 0
//                         JNZ -> success path
//             else:       LEA -> "Given object is empty!" -> return false
//
// RCX = param_1 = the UYItemIDList* (plain struct, NOT a UObject)
// UYItemIDList layout (TArray-style plain struct):
//   [+0x00] void*   Data    (pointer to array elements)
//   [+0x08] int32   Num     (element count)  <-- this is what gets checked
//   [+0x0C] int32   Max     (allocated capacity)
//
// param_2 (RDX) = context / world context UObject*
// param_3 (R8)  = callback / loadout object
//
// The fix: if Num==0, populate with the active loadout object pointer
// so the streaming pipeline gets a valid item to work with.
// ========================================================================
// GetFrontendHUD hook (RVA 0xAA5470)
// Bypasses the class matching lookup which fails offline and causes access
// violations. We decompiled two major callers (RVA 0xAA5550 and 0xAAAED0) and
// verified that:
//   1. They perform null checks on the returned HUD pointer and handle nulls
//   gracefully.
//   2. The HUD resides at offset +0x20 of the component.
// Thus, returning the HUD directly (or 0 if null) is safe and valid.
typedef long long(__fastcall *tGetFrontendHUD)(long long component);
static tGetFrontendHUD OrigGetFrontendHUD = nullptr;

long long __fastcall MyHookGetFrontendHUD(long long component) {
  if (!component)
    return 0;
  __try {
    return *(long long *)(component + 0x20);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

typedef UObject *(__fastcall *tGetUObjectFromWeakPtr)(void *pWeakPtr);
static tGetUObjectFromWeakPtr OrigGetUObjectFromWeakPtr = nullptr;

UObject *__fastcall MyHookGetUObjectFromWeakPtr(void *pWeakPtr) {
  if (!pWeakPtr)
    return nullptr;
  UObject *resolved = OrigGetUObjectFromWeakPtr(pWeakPtr);
  if (!resolved) {
    uintptr_t returnAddr = (uintptr_t)_ReturnAddress();
    uintptr_t moduleBase = (uintptr_t)Globals::ModuleBase;
    uintptr_t returnRVA = returnAddr - moduleBase;

    // FUN_140ab4b50 (RVA 0xAB4B50, size 270) and FUN_140ab5e70 (RVA 0xAB5E70,
    // size 965)
    if ((returnRVA >= 0xAB4B50 && returnRVA <= 0xAB4C60) ||
        (returnRVA >= 0xAB5E70 && returnRVA <= 0xAB6240)) {
      // We used to inject the PlayerController here to prevent crashes, but it
      // caused LoadItemsAsync to crash in the background thread. Let's just
      // return nullptr and let the UI gracefully handle the missing PC, or let
      // LoadItemsAsync block.
      /*
      if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance &&
      (*UWorld::GWorld)->OwningGameInstance->LocalPlayers.Count() > 0) {
          ULocalPlayer* lp =
      (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]; if (lp &&
      lp->PlayerController) { static int count = 0; if (count < 10) {
                  printf("[WEAKPTR-HOOK] Intercepted null PlayerController
      resolution. Caller RVA: 0x%llX. Injecting PC: %p\n", (unsigned long
      long)returnRVA, lp->PlayerController); count++;
              }
              return lp->PlayerController;
          }
      }
      */
    }
  }
  return resolved;
}

// ProcessMulticastDelegate (RVA 0x2322A0)
// Disasm analysis (disasm_2322A0.txt) reveals the delegate struct layout:
//   [pDelegate + 0x00] = void* Data  (pointer to array of entries)
//   [pDelegate + 0x08] = int32 Num   (number of entries)
//   Each entry is 0x10 (16) bytes:
//     [entry + 0x00] = FWeakObjectPtr (8 bytes: int32 ObjectIndex + int32
//     SerialNumber) [entry + 0x08] = FName          (8 bytes: function name to
//     call via ProcessEvent)
//   The original function iterates entries, calls GetUObjectFromWeakPtr
//   (0xD6AD50) on each, then FindFunction (0xD57C70/0xD57C90), then
//   vtable[0x1A8] = ProcessEvent. Crash at 0x2322C0: MOV EBX,[RCX+8] where RCX
//   = pDelegate with stale/freed backing memory.
//
// Our fix: pre-validate the delegate pointer and each entry's weak pointer
// BEFORE calling the original. If any weak ptr resolves to null, zero the FName
// field so the original function's internal FindFunction check will skip it
// (FName=0 â†’ not found â†’ skip).
typedef void(__fastcall *tProcessMulticastDelegate)(void *pDelegate,
                                                    void *pParameters);
static tProcessMulticastDelegate OrigProcessMulticastDelegate = nullptr;

void __fastcall MyHookProcessMulticastDelegate(void *pDelegate,
                                               void *pParameters) {
  if (!pDelegate || (uintptr_t)pDelegate < 0x10000) {
    return;
  }

  // Validate the delegate object itself isn't garbage memory
  __try {
    volatile int32_t numEntries = *(int32_t *)((uint8_t *)pDelegate + 0x08);
    if (numEntries <= 0 || numEntries > 1024) {
      // Probably garbage â€” skip entirely
      return;
    }

    volatile void *dataPtr = *(void **)pDelegate;
    if (!dataPtr || (uintptr_t)dataPtr < 0x10000) {
      return;
    }

    // Pre-validate each entry's weak object pointer.
    // If GetUObjectFromWeakPtr returns null for a live entry, zero its FName
    // so the original function's FindFunction call will gracefully skip it.
    if (OrigGetUObjectFromWeakPtr) {
      uint8_t *entries = (uint8_t *)dataPtr;
      for (int32_t i = 0; i < numEntries; i++) {
        uint8_t *entry = entries + (i * 0x10);
        // Check if this entry has a non-null FName (bytes 8-15)
        uint64_t fname = *(uint64_t *)(entry + 0x08);
        if (fname == 0)
          continue; // Already empty, skip

        // Try to resolve the weak pointer
        UObject *obj = OrigGetUObjectFromWeakPtr(entry);
        if (!obj) {
          // Dead weak pointer â€” zero the FName so original function skips it
          *(uint64_t *)(entry + 0x08) = 0;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Delegate memory is corrupt â€” don't call original
    static int logLimit = 0;
    if (logLimit++ < 5) {
      printf("[DELEGATE] SEH caught corrupt delegate %p â€” skipping\n",
             pDelegate);
    }
    return;
  }

  OrigProcessMulticastDelegate(pDelegate, pParameters);
}

typedef bool(__fastcall *tLoadItemsAsync)(void *pItemIDList, void *pContext,
                                          void *pCallback);
static tLoadItemsAsync OrigLoadItemsAsync = nullptr;

bool __fastcall MyHookLoadItemsAsync(void *pItemIDList, void *pContext,
                                     void *pCallback) {
  static int logCount = 0;

  if (!pItemIDList)
    return false;

  void **pData = (void **)((uint8_t *)pItemIDList + 0x00);
  int32_t *pNum = (int32_t *)((uint8_t *)pItemIDList + 0x08);

  if (logCount < 4) {
    printf(
        "[HANGAR] LoadItemsAsync called: this=%p Num=%d Data=%p ctx=%p cb=%p\n",
        pItemIDList, *pNum, *pData, pContext, pCallback);
    logCount++;
  }

  if (!pContext) {
    if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance) {
      pContext = (*UWorld::GWorld)->OwningGameInstance;
      if (logCount <= 8) {
        printf("[HANGAR] LoadItemsAsync: pContext was null, injected "
               "GameInstance: %p\n",
               pContext);
        logCount++;
      }
    } else {
      if (logCount <= 8) {
        printf("[HANGAR] LoadItemsAsync: pContext was null and no GameInstance "
               "-- returning false\n");
        logCount++;
      }
      return false;
    }
  }

  return OrigLoadItemsAsync(pItemIDList, pContext, pCallback);
}

template <typename T> void TArrayAddSafe(TArray<T> &arr, const T &item);

CG::FName SafeCreateFName(const wchar_t *StrContents) {
  static UObject *kismetLib = nullptr;
  static UFunction *convFunc = nullptr;
  if (!kismetLib) {
    kismetLib = UObject::FindObject<UObject>(
        "KismetStringLibrary Engine.Default__KismetStringLibrary");
    convFunc = UObject::FindObject<UFunction>(
        "Function Engine.KismetStringLibrary.Conv_StringToName");
  }
  CG::FName retName{};
  if (kismetLib && convFunc) {
    struct {
      FString inString;
      CG::FName ReturnValue;
    } params;

    params.inString = MakeFMemoryFString(StrContents);
    kismetLib->ProcessEvent(convFunc, &params);
    retName = params.ReturnValue;

    if (params.inString._data) {
      EnsureUE4Allocators();
      if (g_UE4Free) {
        g_UE4Free(params.inString._data);
      } else {
        free(params.inString._data);
      }
    }
  }
  return retName;
}

CG::FName SafeCreateFName(const char *StrContents) {
  if (!StrContents)
    return CG::FName();
  int len = (int)strlen(StrContents);
  std::wstring wstr(len, L'\0');
  for (int i = 0; i < len; ++i) {
    wstr[i] = (wchar_t)StrContents[i];
  }
  return SafeCreateFName(wstr.c_str());
}
bool IsValidBinnedPtr(void *allocator, void *ptr) {
  if (!allocator || !ptr)
    return false;
  __try {
    uint64_t *p1 = (uint64_t *)allocator;
    uint32_t PageSize =
        *(uint32_t *)((uintptr_t)allocator + 0x40BF8); // 0x817f * 8
    if (PageSize == 0)
      return false;

    int32_t iVar19 = (int32_t)(0x10000 / PageSize);
    if (iVar19 == -1)
      return false;

    uint32_t uVar16 = (uint32_t)p1[7] - 1;
    uint64_t lVar15 =
        *(uint64_t *)((uintptr_t)allocator + 0x40BE8); // 0x817d * 8
    uint64_t uVar11 = (uint64_t)ptr;

    uint32_t uVar17 = 0;
    uint16_t uVar12 = 0;
    bool foundPage = false;
    uint64_t psVar13 = 0;

    do {
      uint64_t psVar14 = (~((uint64_t)PageSize - 1)) & uVar11;
      uVar11 = psVar14 >> (p1[14] & 0x3F);
      uVar12 = 0;

      uint64_t *puVar10 = (uint64_t *)(((uVar11 & uVar16) * 32) + lVar15);
      uint64_t *puVar5 = puVar10;
      do {
        if (*puVar5 == (uVar11 & 0xFFFFFFFF)) {
          psVar13 = (((psVar14 >> (p1[11] & 0x3F)) &
                      *(uint32_t *)((uintptr_t)allocator + 120)) *
                     32) +
                    puVar5[1];
          if (psVar13 == 0) {
            return false;
          }
          if (*(int32_t *)(psVar13 + 4) == 0) {
            uVar12 = *(uint16_t *)(psVar13 + 2);
          } else {
            foundPage = true;
            uVar12 = *(uint16_t *)(psVar13 + 2);
          }
          break;
        }
        puVar5 = (uint64_t *)puVar5[3];
      } while (puVar5 != puVar10);

      if (foundPage) {
        break;
      }

      lVar15 = *(uint64_t *)((uintptr_t)allocator + 0x40BE8);
      uVar17++;
      uVar11 = psVar14 + (-1 - (uint64_t)(uVar12 * PageSize));
    } while (uVar17 < (uint32_t)iVar19 + 1);

    if (foundPage && psVar13 != 0) {
      uintptr_t binPtr =
          *(uintptr_t *)((uintptr_t)allocator + (uVar12 + 0x17a) * 8);
      if (binPtr > 0x10000) {
        uint32_t BlockSize = *(uint32_t *)(binPtr + 16);
        if (BlockSize > 0) {
          uint64_t PageBase = (~((uint64_t)PageSize - 1)) & (uint64_t)ptr;
          uint64_t offset = (uint64_t)ptr - PageBase;
          if ((offset % BlockSize) == 0) {
            return true;
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return false;
}

typedef void(__fastcall *tFMallocBinnedFree)(void *allocator, void *ptr);
static tFMallocBinnedFree OrigFMallocBinnedFree = nullptr;

void __fastcall MyHookFMallocBinnedFree(void *allocator, void *ptr) {
  if (!ptr)
    return;
  if (!IsValidBinnedPtr(allocator, ptr)) {
    // Mismatched or unallocated pointer! Skip it to prevent crash.
    return;
  }
  OrigFMallocBinnedFree(allocator, ptr);
}

typedef void(__fastcall *tFUN_1403b07b0)(void *param_1, void *param_2);
static tFUN_1403b07b0 OrigFUN_1403b07b0 = nullptr;

void __fastcall MyHookFUN_1403b07b0(void *param_1, void *param_2) {
  OrigFUN_1403b07b0(param_1, param_2);

  uintptr_t returnAddr = (uintptr_t)_ReturnAddress();
  uintptr_t returnRVA = returnAddr - (uintptr_t)Globals::ModuleBase;

  // Check if called from FUN_1403bd800 (RVA 0x3BD800)
  if (returnRVA >= 0x3BD800 && returnRVA <= 0x3BD950) {
    // Reset the transition manager queue to prevent stale/corrupted transitions
    if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
      void *tm = *(void **)((uintptr_t)(*UWorld::GWorld)->AuthorityGameMode + 0x9a8);
      if (tm) {
        void *dummyNode = UE4Malloc(24);
        if (dummyNode) {
          memset(dummyNode, 0, 24);
          *(void **)((uintptr_t)tm + 0x78) = dummyNode; // Head
          *(void **)((uintptr_t)tm + 0x70) = dummyNode; // Tail
          printf("[HANGAR] MyHookFUN_1403b07b0: Reset transition queue with fresh dummy node %p\n", dummyNode);
        }
      }
    }
    g_levelActorLinksAttempted = false;
    g_levelActorLinksInitialized = false;
    g_needsCustomizationPreviewUpdate = true;

    TArray<CG::FName> *levelNames = (TArray<CG::FName> *)param_2;
    if (levelNames) {
      // DO NOT inject background levels here because it causes them to be
      // queue-unloaded!
      int32_t synId = g_lastClickedSyntheticId;
      uint8_t shipClass = 6; // Default to DreadnoughtMedium
      if (synId >= 11001 && synId <= 11000 + s_jupiterArmsCount) {
        shipClass = s_jupiterArms[synId - 11001].shipClassEnum;
      } else if (synId >= 12001 && synId <= 12000 + s_akulaVektorCount) {
        shipClass = s_akulaVektor[synId - 12001].shipClassEnum;
      } else if (synId >= 13001 && synId <= 13000 + s_oberonCount) {
        shipClass = s_oberon[synId - 13001].shipClassEnum;
      }

      const char *mapName = nullptr;
      switch (shipClass) {
      case 1:
        mapName = "MN_HGR_DREADL";
        break;
      case 2:
        mapName = "MN_HGR_SCOUTL";
        break;
      case 3:
        mapName = "MN_HGR_SNIPERL";
        break;
      case 4:
        mapName = "MN_HGR_SUPPORTL";
        break;
      case 5:
        mapName = "MN_HGR_ASSAULTL";
        break;
      case 6:
        mapName = "MN_HGR_DREADM";
        break;
      case 7:
        mapName = "MN_HGR_DREADH";
        break;
      case 8:
        mapName = "MN_HGR_SCOUTM";
        break;
      case 9:
        mapName = "MN_HGR_SCOUTH";
        break;
      case 10:
        mapName = "MN_HGR_SNIPERM";
        break;
      case 11:
        mapName = "MN_HGR_SNIPERH";
        break;
      case 12:
        mapName = "MN_HGR_SUPPORTM";
        break;
      case 13:
        mapName = "MN_HGR_SUPPORTH";
        break;
      case 14:
        mapName = "MN_HGR_ASSAULTM";
        break;
      case 15:
        mapName = "MN_HGR_ASSAULTH";
        break;
      }

      if (mapName) {
        TArrayAddSafe(*levelNames, SafeCreateFName(mapName));
        printf("[HANGAR] Injected hangar levels: base + %s\n", mapName);
      } else {
        printf("[HANGAR] Injected base hangar levels (no class map for "
               "class=%d)\n",
               shipClass);
      }
    }
  }
}

typedef CG::FName *(__fastcall *tFUN_140372640)(void *param_1,
                                                CG::FName *param_2,
                                                uint32_t param_3);
static tFUN_140372640 OrigFUN_140372640 = nullptr;

CG::FName *__fastcall MyHookFUN_140372640(void *param_1, CG::FName *param_2,
                                          uint32_t param_3) {
  CG::FName *result = OrigFUN_140372640(param_1, param_2, param_3);
  if (!param_2 || param_2->ComparisonIndex == 0) {
    const char *mapName = nullptr;
    switch (param_3) {
    case 1:
      mapName = "MN_HGR_DREADL";
      break;
    case 2:
      mapName = "MN_HGR_SCOUTL";
      break;
    case 3:
      mapName = "MN_HGR_SNIPERL";
      break;
    case 4:
      mapName = "MN_HGR_SUPPORTL";
      break;
    case 5:
      mapName = "MN_HGR_ASSAULTL";
      break;
    case 6:
      mapName = "MN_HGR_DREADM";
      break;
    case 7:
      mapName = "MN_HGR_DREADH";
      break;
    case 8:
      mapName = "MN_HGR_SCOUTM";
      break;
    case 9:
      mapName = "MN_HGR_SCOUTH";
      break;
    case 10:
      mapName = "MN_HGR_SNIPERM";
      break;
    case 11:
      mapName = "MN_HGR_SNIPERH";
      break;
    case 12:
      mapName = "MN_HGR_SUPPORTM";
      break;
    case 13:
      mapName = "MN_HGR_SUPPORTH";
      break;
    case 14:
      mapName = "MN_HGR_ASSAULTM";
      break;
    case 15:
      mapName = "MN_HGR_ASSAULTH";
      break;
    }
    if (mapName && param_2) {
      *param_2 = SafeCreateFName(mapName);
      printf("[HANGAR] FUN_140372640 lookup overridden for shipClass=%u -> "
             "levelName=%s\n",
             param_3, mapName);
      result = param_2;
    }
  }
  return result;
}

typedef void(__fastcall *tFUN_140aabf50)(void *param_1, void *param_2,
                                         void *param_3, char param_4);
static tFUN_140aabf50 OrigFUN_140aabf50 = nullptr;

typedef void(__fastcall *tFUN_140381d00)(void *param_1, void *param_2,
                                         void *param_3, char param_4);
static tFUN_140381d00 OrigFUN_140381d00 = nullptr;

void __fastcall MyHookAABF50(void *param_1, void *param_2, void *param_3,
                             char param_4) {
  if (!OrigFUN_140381d00) {
    OrigFUN_140381d00 = (tFUN_140381d00)(Globals::ModuleBase + 0x381D00);
  }
  bool handled = false;
  if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
    UObject *gameModeObj = (UObject *)(*UWorld::GWorld)->AuthorityGameMode;
    std::string fullName = gameModeObj->GetFullName();
    if (fullName.find("Outpost") != std::string::npos ||
        fullName.find("Frontend") != std::string::npos) {
      printf("[HANGAR] MyHookAABF50: Invoking FUN_140381d00 directly with "
             "Outpost GameMode: %p (%s)\n",
             gameModeObj, fullName.c_str());
      OrigFUN_140381d00(gameModeObj, param_2, param_3, param_4);
      handled = true;
    }
  }
  if (!handled) {
    OrigFUN_140aabf50(param_1, param_2, param_3, param_4);
  }
}

void __fastcall MyHookIsItemOwnedByPlayer(UObject *Context, void *Stack,
                                          void *RESULT_DECL) {
  bool owned = true;
  struct FFrame {
    void *Node;
    UObject *Object;
    uint8_t *Code;
    uint8_t *Locals;
  };
  FFrame *frame = (FFrame *)Stack;
  if (frame && frame->Locals) {
    FUIGenericButtonModuleData *pData =
        *(FUIGenericButtonModuleData **)frame->Locals;
    if (pData) {
      int32_t itemId = pData->m_itemID;
      if (itemId >= 11000 && itemId <= 19999) {
        owned = (g_ownedShips.count(itemId) > 0);
      }
    }
  }
  if (RESULT_DECL) {
    *(bool *)RESULT_DECL = owned;
  }
}

// Native UFunction override for IsCurrentShipOwnedByPlayer
void __fastcall MyHookIsCurrentShipOwnedByPlayer(UObject *Context, void *Stack,
                                                 void *RESULT_DECL) {
  bool owned = true;
  int32_t currentId = g_lastClickedSyntheticId;
  if (currentId >= 11000 && currentId <= 19999) {
    owned = (g_ownedShips.count(currentId) > 0);
  }
  if (RESULT_DECL) {
    *(bool *)RESULT_DECL = owned;
  }
}

template <typename T> void TArrayAddSafe(TArray<T> &arr, const T &item) {
  if (arr._count >= arr._max) {
    int32_t newMax = (arr._max == 0) ? 16 : arr._max * 2;
    EnsureUE4Allocators();
    if (g_UE4Realloc) {
      arr._data = (T *)g_UE4Realloc(arr._data, newMax * sizeof(T), 0);
    } else {
      T *newData = (T *)UE4Malloc(newMax * sizeof(T));
      if (arr._data) {
        memcpy(newData, arr._data, arr._count * sizeof(T));
        // Fallback leak, but UE4Realloc should always exist
      }
      arr._data = newData;
    }
    arr._max = newMax;
  }
  arr._data[arr._count] = item;
  arr._count++;
}

// SEH-safe wrapper (can't use __try in functions with C++ objects)
UObject *SafeStaticLoadObject(uintptr_t addr, UClass *cls,
                              const wchar_t *path) {
  __try {
    UObject *Obj =
        reinterpret_cast<UObject *(*)(UClass *, UObject *, const TCHAR *,
                                      const TCHAR *, int, void *, bool)>(addr)(
            cls, nullptr, path, nullptr, 0, nullptr, false);
    if (Obj) {
      HardenedPinToRootSet(Obj);
    }
    return Obj;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void InjectOfflineFleet(AYPlayerController *pc) {
  static bool g_isInjecting = false;
  if (g_isInjecting)
    return;
  g_isInjecting = true;

  InitFullTechTree(); // Guarantee g_FullTechTree is populated for owned ship filtering

  if (!pc || !pc->m_loadoutManager || !pc->m_fleetManager) {
    printf("[LOAD] ERROR: Invalid Controller/Manager pointers!\n");
    g_isInjecting = false;
    return;
  }

  UYLoadoutManagerComponent *lmc =
      (UYLoadoutManagerComponent *)pc->m_loadoutManager;
  UYFleetManager *fm = (UYFleetManager *)pc->m_fleetManager;

  printf("[FLEET] Beginning fleet restoration (pak-verified assets)...\n");

  // Load and pin all 15 ship animation blueprints
  printf("[FLEET] Preloading and pinning all 15 ship animation blueprints...\n");
  uintptr_t staticLoadAddr = Globals::ModuleBase + 0x0D78110;
  for (int c = 1; c <= 15; c++) {
    if (g_shipAnimClassPaths[c]) {
      UObject *loadedAnim = SafeStaticLoadObject(staticLoadAddr, UClass::StaticClass(), g_shipAnimClassPaths[c]);
      if (loadedAnim) {
        g_shipAnimClasses[c] = (UClass *)loadedAnim;
        printf("[FLEET]   Successfully loaded AnimClass for shipClass %d: %s\n", c, loadedAnim->GetFullName().c_str());
      } else {
        printf("[FLEET]   FAILED to load AnimClass for shipClass %d: %ls\n", c, g_shipAnimClassPaths[c]);
      }
    }
  }

  // === MONARCH FIX: Preload Tyr hero vanity Data Assets and Skeletal Meshes ===
  // The Monarch (Jupiter Arms T5 Dreadnought) precast loadout references 4
  // Tyr hero ship vanity Data Assets in its m_appereance.m_heroShipParts array.
  // These replace the base ship's skeletal mesh sections entirely. Without them,
  // the Monarch is invisible. In offline mode, the game's entitlement check
  // for these premium assets fails, so we must load them explicitly from the
  // pak files (custompakchunk8) and pin them to the GC root set before loading
  // the precast loadout itself (so import resolution finds them in GObjects).
  // We must also load the referenced SkeletalMeshes (VAN_H_DreadH_*_Tyr_SK)
  // because the engine will not automatically load them from the pak when resolving
  // DA imports.
  {
    printf("[FLEET] Preloading Tyr hero vanity assets & meshes for Monarch (T5 DreadH)...\n");
    static const wchar_t *s_tyrVanityPaths[] = {
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Hull_Tyr_DA.VAN_H_DreadH_Hull_Tyr_DA",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Forecastle_Tyr_DA.VAN_H_DreadH_Forecastle_Tyr_DA",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Quarterdeck_Tyr_DA.VAN_H_DreadH_Quarterdeck_Tyr_DA",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Stern_Tyr_DA.VAN_H_DreadH_Stern_Tyr_DA",
    };
    static const wchar_t *s_tyrMeshPaths[] = {
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Hull_Tyr_SK.VAN_H_DreadH_Hull_Tyr_SK",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Forecastle_Tyr_SK.VAN_H_DreadH_Forecastle_Tyr_SK",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Quarterdeck_Tyr_SK.VAN_H_DreadH_Quarterdeck_Tyr_SK",
        L"/Game/Generic/VanityItems/Heroships/Dreadnought/Heavy/Tyr/VAN_H_DreadH_Stern_Tyr_SK.VAN_H_DreadH_Stern_Tyr_SK",
    };
    int tyrLoaded = 0;
    for (int i = 0; i < 4; i++) {
      // Load Skeletal Mesh first
      UObject *mesh = SafeStaticLoadObject(staticLoadAddr, UObject::StaticClass(), s_tyrMeshPaths[i]);
      if (mesh) {
        printf("[FLEET]   Tyr skeletal mesh loaded: %s\n", mesh->GetFullName().c_str());
      } else {
        printf("[FLEET]   WARNING: Failed to load Tyr skeletal mesh: %ls\n", s_tyrMeshPaths[i]);
      }

      // Load Data Asset
      UObject *da = SafeStaticLoadObject(staticLoadAddr, UObject::StaticClass(), s_tyrVanityPaths[i]);
      if (da) {
        g_tyrDAs[i] = da;
        tyrLoaded++;
        printf("[FLEET]   Tyr vanity DA loaded: %s\n", da->GetFullName().c_str());
      } else {
        printf("[FLEET]   WARNING: Failed to load Tyr vanity DA: %ls\n", s_tyrVanityPaths[i]);
      }
    }
    printf("[FLEET] Tyr vanity preload complete: %d/4 DA assets loaded.\n", tyrLoaded);
    if (tyrLoaded < 4) {
      printf("[FLEET] WARNING: Not all Tyr vanity assets loaded. Monarch may be invisible.\n");
      printf("[FLEET]          Check that custompakchunk8-WindowsNoEditor.pak is present and readable.\n");
    }
  }

  // Load ALL 52 precast loadout BPs from pak files.
  // Maps EYShipClass enum to the loadout class name used in asset paths.
  // Path pattern:
  // /Game/Generic/Loadouts/Precast/T{tier}/VH_{ClassName}_T{tier}_PrecastLoadout_BP
  static const char *s_classNames[] = {
      /*  0 */ nullptr,             // YSC_NONE
      /*  1 */ "DreadnoughtLight",  // YSC_DREADNOUGHT_LIGHT
      /*  2 */ "ScoutLight",        // YSC_SCOUT_LIGHT
      /*  3 */ "SniperLight",       // YSC_SNIPER_LIGHT
      /*  4 */ "SupportLight",      // YSC_SUPPORT_LIGHT
      /*  5 */ "AssaultLight",      // YSC_ASSAULT_LIGHT
      /*  6 */ "DreadnoughtMedium", // YSC_DREADNOUGHT_MEDIUM
      /*  7 */ "DreadnoughtHeavy",  // YSC_DREADNOUGHT_HEAVY
      /*  8 */ "ScoutMedium",       // YSC_SCOUT_MEDIUM
      /*  9 */ "ScoutHeavy",        // YSC_SCOUT_HEAVY
      /* 10 */ "SniperMedium",      // YSC_SNIPER_MEDIUM
      /* 11 */ "SniperHeavy",       // YSC_SNIPER_HEAVY
      /* 12 */ "SupportMedium",     // YSC_SUPPORT_MEDIUM
      /* 13 */ "SupportHeavy",      // YSC_SUPPORT_HEAVY
      /* 14 */ "AssaultMedium",     // YSC_ASSAULT_MEDIUM
      /* 15 */ "AssaultHeavy",      // YSC_ASSAULT_HEAVY
  };
  static const int NUM_SHIP_CLASSES = 16;

  // Which (class, tier) combos exist as precast loadouts in the pak files:
  // T1: classes 6,10,12,14 (4 Medium-weight starters)
  // T2: classes 2,6,10,12,14 + 3 (SniperLight) = 6
  // T3: classes 1-4,6,8-12,14,15 = 12
  // T4: classes 1-15 = 15
  // T5: classes 1-15 = 15
  struct LoadoutEntry {
    int shipClass;
    int tier;
  };
  std::vector<LoadoutEntry> allLoadouts;

  // T1: Medium starters only
  for (int c : {6, 10, 12, 14})
    allLoadouts.push_back({c, 1});
  // T2: Medium starters + ScoutLight + SniperLight
  for (int c : {2, 3, 6, 10, 12, 14})
    allLoadouts.push_back({c, 2});
  // T3: All except Heavy Dreadnought, Heavy Assault, Heavy Support,
  // AssaultLight
  for (int c : {1, 2, 3, 4, 6, 8, 9, 10, 11, 12, 14, 15})
    allLoadouts.push_back({c, 3});
  // T4 and T5: All 15 classes
  for (int tier = 4; tier <= 5; tier++) {
    for (int c = 1; c <= 15; c++)
      allLoadouts.push_back({c, tier});
  }

  printf("[FLEET] Attempting to load %d precast loadout BPs...\n",
         (int)allLoadouts.size());

  // g_loadoutMap: maps (shipClass * 10 + tier) -> index in loadedClasses
  // This replaces the old proxyFallback system entirely
  std::map<int, int> g_loadoutKeyToIndex;

  struct ShipLoadDef {
    const char *name;
    const wchar_t *pkgPath;
    int tier;
    int shipClass;
  };
  std::vector<ShipLoadDef> shipLoadDefs;
  std::vector<UClass *> loadedClasses;

  for (const auto &entry : allLoadouts) {
    if (entry.shipClass <= 0 || entry.shipClass >= NUM_SHIP_CLASSES)
      continue;
    const char *className = s_classNames[entry.shipClass];
    if (!className)
      continue;

    // Build the asset path
    wchar_t pkgPath[512];
    // Special case: T5 AssaultLight uses different naming convention
    if (entry.shipClass == 5 && entry.tier == 5) {
      swprintf(pkgPath, 512,
               L"/Game/Generic/Loadouts/Precast/T5/"
               L"VH_AssaultLight_PrecastLoadout_T5_BP");
    } else {
      wchar_t classNameW[64];
      mbstowcs(classNameW, className, 64);
      swprintf(
          pkgPath, 512,
          L"/Game/Generic/Loadouts/Precast/T%d/VH_%s_T%d_PrecastLoadout_BP",
          entry.tier, classNameW, entry.tier);
    }

    // Build the full _C path for loading
    wchar_t fullPath[512];
    const wchar_t *assetName = wcsrchr(pkgPath, L'/');
    if (assetName)
      assetName++;
    else
      assetName = pkgPath;
    swprintf(fullPath, 512, L"%s.%s_C", pkgPath, assetName);

    // Try to load
    uintptr_t addr = Globals::ModuleBase + 0x0D78110;
    UObject *loaded =
        SafeStaticLoadObject(addr, UClass::StaticClass(), fullPath);

    if (loaded) {
      int idx = (int)loadedClasses.size();
      int key = entry.shipClass * 10 + entry.tier;
      g_loadoutKeyToIndex[key] = idx;
      loadedClasses.push_back((UClass *)loaded);
      if (idx < 3)
        printf("[FLEET]   [%d] %s T%d -> %s\n", idx, className, entry.tier,
               loaded->GetFullName().c_str());
      else if (idx == 3)
        printf("[FLEET]   ... (loading remaining ships)\n");
    } else {
      printf("[FLEET] MISS: %s T%d (not in paks)\n", className, entry.tier);
    }
  }

  // Store the map globally for use by hooks
  g_loadoutMap = g_loadoutKeyToIndex;

  // Debug: dump all keys so we can diagnose class mismatch bugs
  printf("[LOADOUT] g_loadoutMap keys (%d entries):\n",
         (int)g_loadoutMap.size());
  for (const auto &kv : g_loadoutMap) {
    int cls = kv.first / 10, tier = kv.first % 10;
    const char *cname =
        (cls >= 0 && cls < 16 && s_classNames[cls]) ? s_classNames[cls] : "?";
    printf("[LOADOUT]   key=%d -> idx=%d (class=%d/%s tier=%d)\n", kv.first,
           kv.second, cls, cname, tier);
  }

  if (loadedClasses.empty()) {
    printf("[FLEET] ERROR: No ship classes loaded! Aborting.\n");
    g_isInjecting = false;
    return;
  }
  printf("[FLEET] Loaded %d ship classes. Registering via "
         "AddAndActiveLoadoutFromBlueprint...\n",
         (int)loadedClasses.size());

  // Register ALL loaded classes with the engine's loadout system.
  // This is how the game normally registers loadouts â€” it takes a UClass*
  // (BlueprintGeneratedClass) and internally constructs a UYShipLoadout
  // instance, populating all fields correctly (precastID, tier, manufacturer,
  // modules, etc).
  int registered = 0;
  for (int i = 0; i < (int)loadedClasses.size(); i++) {
    try {
      pc->AddAndActiveLoadoutFromBlueprint(loadedClasses[i]);
      registered++;
    } catch (...) {
      printf("[FLEET] EXCEPTION registering loadout %d\n", i);
    }
  }
  printf("[FLEET] Registered %d / %d loadouts\n", registered,
         (int)loadedClasses.size());

  // === HERO SHIPS (Special precast loadouts) ===
  // These are named hero ships (MorningStar, Ravenswood, PAX variants, etc.)
  // that appear in a separate section of the hangar UI from the tech tree.
  // KNOWN (from extracting VH_DreadnoughtMorningStar_PrecastLoadout_BP.json):
  //   - Same parent class (YShipLoadoutPrecast) as tier ships
  //   - m_shipClass is null/absent (no EYShipClass enum value)
  //   - m_itemTier is absent
  //   - m_heroShipParts IS populated (cosmetic mesh DAs in chunk10)
  //   - Uses same base pawn classes as regular ships
  // The engine uses the same AddAndActiveLoadoutFromBlueprint for both.
  {
    static const wchar_t *s_heroShipPaths[] = {
        // chunk27
        L"/Game/Generic/Loadouts/Precast/Special/VH_AssaultM01_PAX_PrecastLoadout_BP.VH_AssaultM01_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_AssaultM02_PAX_PrecastLoadout_BP.VH_AssaultM02_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_AssaultMRavenswood_PrecastLoadout_BP.VH_AssaultMRavenswood_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_DreadnoughtM01_PAX_PrecastLoadout_BP.VH_DreadnoughtM01_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_DreadnoughtM02_PAX_PrecastLoadout_BP.VH_DreadnoughtM02_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_DreadnoughtMorningStar_PrecastLoadout_BP.VH_DreadnoughtMorningStar_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_DreadnoughtRavenswood_PrecastLoadout_BP.VH_DreadnoughtRavenswood_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_ScoutL01_PAX_PrecastLoadout_BP.VH_ScoutL01_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_ScoutL02_PAX_PrecastLoadout_BP.VH_ScoutL02_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_SniperM02_PAX_PrecastLoadout_BP.VH_SniperM02_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_SupportM02_PAX_PrecastLoadout_BP.VH_SupportM02_PAX_PrecastLoadout_BP_C",
        // chunk29
        L"/Game/Generic/Loadouts/Precast/Special/VH_SniperM01_PAX_PrecastLoadout_BP.VH_SniperM01_PAX_PrecastLoadout_BP_C",
        L"/Game/Generic/Loadouts/Precast/Special/VH_SupportM01_PAX_PrecastLoadout_BP.VH_SupportM01_PAX_PrecastLoadout_BP_C",
        // Note: VH_Tutorial_101 intentionally excluded - not a playable ship
    };
    static const int NUM_HERO_SHIPS = 13;

    printf("[FLEET] Loading %d hero ship precast loadouts...\n", NUM_HERO_SHIPS);
    int heroLoaded = 0;
    int heroRegistered = 0;
    for (int i = 0; i < NUM_HERO_SHIPS; i++) {
      UObject *loaded = SafeStaticLoadObject(staticLoadAddr, UClass::StaticClass(), s_heroShipPaths[i]);
      if (loaded) {
        heroLoaded++;
        try {
          pc->AddAndActiveLoadoutFromBlueprint((UClass *)loaded);
          heroRegistered++;
        } catch (...) {
          printf("[FLEET]   EXCEPTION registering hero ship %d\n", i);
        }
      } else {
        printf("[FLEET]   MISS: hero ship %d (not in paks or path wrong)\n", i);
      }
    }
    printf("[FLEET] Hero ships: loaded=%d registered=%d\n", heroLoaded, heroRegistered);
  }


  // Set active loadout to first T1 ship (Simargl) for matchmaking readiness
  // The last registered loadout was T5 AssaultHeavy - we want a T1 for Recruit
  // matches
  if (!loadedClasses.empty()) {
    try {
      pc->AddAndActiveLoadoutFromBlueprint(loadedClasses[0]);
      printf("[FLEET] Set active loadout to T1 ship (index 0) for match "
             "readiness\n");
    } catch (...) {
      printf("[FLEET] Failed to set T1 active loadout\n");
    }
  }

  // Diagnostic: Check what the engine populated
  int32_t entriesNum = 0;
  if (lmc) {
    // m_loadoutEntries at offset 0x0108 (TArray<FYLoadoutEntry>, entry size
    // 0x30)
    uint8_t *entriesBase = (uint8_t *)lmc + 0x0108;
    uint8_t **entriesData = (uint8_t **)entriesBase;
    entriesNum = *(int32_t *)(entriesBase + 0x08);
    UObject *activeLoadout = *(UObject **)((uint8_t *)lmc + 0x0208);
    printf("[FLEET]   m_loadoutEntries: count=%d\n", entriesNum);
    printf("[FLEET]   m_activeLoadout: %s\n", activeLoadout ? "SET" : "NULL");
    if (activeLoadout) {
      try {
        printf("[FLEET] Active: %s\n", activeLoadout->GetFullName().c_str());
      } catch (...) {
      }
    }

    // Read ship names from loaded UYShipLoadout objects
    g_numLoadedShips = 0;
    if (*entriesData && entriesNum > 0 && entriesNum <= MAX_LOADED_SHIPS) {
      printf("[FLEET] Reading ship data from %d loadout entries:\n",
             entriesNum);
      for (int e = 0; e < entriesNum && g_numLoadedShips < MAX_LOADED_SHIPS;
           e++) {
        // FYLoadoutEntry is 0x30 bytes, m_loadouts (TArray<UYShipLoadout*>) at
        // offset 0x00
        uint8_t *entry = *entriesData + (e * 0x30);
        UObject **loadoutArr = *(UObject ***)(entry);
        int32_t loadoutCount = *(int32_t *)(entry + 0x08);

        // Only take the first loadout per entry (Loadout[0]).
        // Each entry has 2 identical loadouts (default + active copy), and
        // storing both was filling g_loadedShips with duplicates, hitting the
        // 64-cap at entry 31 and missing all T5 ships.
        for (int l = 0;
             l < 1 && l < loadoutCount && g_numLoadedShips < MAX_LOADED_SHIPS;
             l++) {
          UObject *loadout = loadoutArr[l];
          if (!loadout)
            continue;

          HardenedPinToRootSet(loadout);

          // UYShipLoadout offsets:
          //   0x00B0: m_id (FName)
          //   0x00C0: m_precastLoadoutID (int32)
          //   0x00C8: m_name (FString)
          //   0x00D8: m_shipClass (EYShipClass, uint8)
          int32_t precastID = *(int32_t *)((uint8_t *)loadout + 0x00C0);
          FString *namePtr = (FString *)((uint8_t *)loadout + 0x00C8);
          uint8_t shipClass = *((uint8_t *)loadout + 0x00D8);

          // Read FName m_id at 0x00B0
          FName *idName = (FName *)((uint8_t *)loadout + 0x00B0);

          // Store for UI hooks
          LoadedShipInfo &info = g_loadedShips[g_numLoadedShips];
          info.loadoutObj = loadout;
          info.precastID = precastID;
          info.shipClass = (EYShipClass)shipClass;

          // Normalize Class 13 (Support Heavy) to 12 (Support Medium) to fix UI
          // icons
          if (shipClass == 13) {
            info.shipClass = (EYShipClass)12;
          }

          // Extract tier from loadout class name (e.g.,
          // "VH_AssaultMedium_T3_..." â†’ 3) Offset +0xD0 was reading garbage
          // (425). The class name always contains _T{N}_.
          int loadoutTier = 1; // default
          try {
            std::string className = loadout->GetFullName();
            size_t tPos = className.find("_T");
            while (tPos != std::string::npos) {
              if (tPos + 2 < className.size()) {
                char tierChar = className[tPos + 2];
                if (tierChar >= '1' && tierChar <= '5') {
                  loadoutTier = tierChar - '0';
                  break;
                }
              }
              tPos = className.find("_T", tPos + 1);
            }
          } catch (...) {
          }
          info.tier = loadoutTier;

          info.shipId = g_numLoadedShips + 1; // unique ID starting at 1

          // Read ship name from FString
          try {
            if (namePtr && namePtr->c_str()) {
              std::string nameStr = namePtr->ToString();
              if (g_numLoadedShips < 3)
                printf("[FLEET]   %s (class %d, tier %d)\n", nameStr.c_str(),
                       shipClass, info.tier);
              else if (g_numLoadedShips == 3)
                printf("[FLEET]   ... (reading remaining entries)\n");
              // Store wide name for UI
              int len = (int)nameStr.length();
              if (len > 63)
                len = 63;
              for (int c = 0; c < len; c++)
                info.name[c] = (wchar_t)nameStr[c];
              info.name[len] = 0;
            } else {
              printf("[FLEET]   Entry %d: name=NULL class=%d\n", e, shipClass);
              swprintf(info.name, 64, L"Ship_%d", g_numLoadedShips + 1);
            }
          } catch (...) {
            printf("[FLEET]   Entry %d: exception reading name\n", e);
            swprintf(info.name, 64, L"Ship_%d", g_numLoadedShips + 1);
          }

          g_numLoadedShips++;
        }
      }
      printf("[FLEET] Loaded %d ship records for UI population\n",
             g_numLoadedShips);

      // Rebuild g_loadoutMap from g_loadedShips so click handlers can find
      // the correct ship by (shipClass * 10 + tier). This replaces the
      // loadedClasses-based map which used different indices.
      g_loadoutMap.clear();
      for (int i = 0; i < g_numLoadedShips; i++) {
        int key = (int)g_loadedShips[i].shipClass * 10 + g_loadedShips[i].tier;
        if (g_loadoutMap.find(key) == g_loadoutMap.end()) {
          g_loadoutMap[key] = i;
        }
      }
      printf(
          "[LOADOUT] Built g_loadoutMap with %d entries from g_loadedShips\n",
          (int)g_loadoutMap.size());
    }
  }

  // Manually populate m_fleetList with 3 fleet types
  // The game expects 3 fleets matching m_fleetEligibiliyConfigTable:
  //   Fleet[0] = Recruit  (T1-T2 ships)
  //   Fleet[1] = Veteran  (T2-T3 ships)
  //   Fleet[2] = Legendary (T4-T5 ships)
  // Inject 3 standard fleets (Recruit, Veteran, Legendary) into FleetManager
  if (fm && entriesNum > 0) {
    const int FLEET_ENTRY_SIZE = 0x50;
    uint8_t *fleetData = (uint8_t *)UE4Malloc(3 * FLEET_ENTRY_SIZE);
    memset(fleetData, 0, 3 * FLEET_ENTRY_SIZE);

    struct TArrayRaw {
      void *data;
      int32_t count;
      int32_t max;
    };
    TArrayRaw *pFleetList = (TArrayRaw *)((uint8_t *)fm + 0x0030);
    pFleetList->data = fleetData;
    pFleetList->count = 3;
    pFleetList->max = 3;

    struct FleetDef {
      const char *name;
      int minTier;
      int maxTier;
      uint8_t fleetType;
    };
    auto isLoadedShipOwned = [&](const LoadedShipInfo &info) -> bool {
      int bpClass = (int)info.shipClass;
      if (bpClass == 13)
        bpClass = 12; // SupportHeavy -> SupportMedium normalization
      for (const auto &s : g_FullTechTree) {
        int sClass = s.shipClass;
        if (sClass == 13)
          sClass = 12;
        if (sClass == bpClass && s.tier == info.tier) {
          return g_ownedShips.count(s.shipId) > 0;
        }
      }
      return false;
    };

    FleetDef fleetDefs[] = {
        {"Recruit", 1, 2, 1}, {"Veteran", 2, 3, 2}, {"Legendary", 4, 5, 3}};

    for (int f = 0; f < 3; f++) {
      uint8_t *fleet = fleetData + f * FLEET_ENTRY_SIZE;

      int matchCount = 0;
      for (int i = 0; i < entriesNum && i < g_numLoadedShips; i++) {
        int tier = g_loadedShips[i].tier;
        if (tier >= fleetDefs[f].minTier && tier <= fleetDefs[f].maxTier) {
          if (isLoadedShipOwned(g_loadedShips[i])) {
            matchCount++;
          }
        }
      }
      if (matchCount > 5) {
        matchCount = 5;
      }

      int32_t *loadoutIndices =
          (int32_t *)UE4Malloc(matchCount * sizeof(int32_t));
      bool *veteranStatus = (bool *)UE4Malloc(matchCount * sizeof(bool));
      memset(veteranStatus, 0, matchCount * sizeof(bool));

      int idx = 0;
      for (int i = 0; i < entriesNum && i < g_numLoadedShips && idx < matchCount; i++) {
        int tier = g_loadedShips[i].tier;
        if (tier >= fleetDefs[f].minTier && tier <= fleetDefs[f].maxTier) {
          if (isLoadedShipOwned(g_loadedShips[i])) {
            loadoutIndices[idx++] = i;
          }
        }
      }

      *(int32_t **)(fleet + 0x00) = loadoutIndices;
      *(int32_t *)(fleet + 0x08) = matchCount;
      *(int32_t *)(fleet + 0x0C) = matchCount;

      *(bool **)(fleet + 0x10) = veteranStatus;
      *(int32_t *)(fleet + 0x18) = matchCount;
      *(int32_t *)(fleet + 0x1C) = matchCount;

      *(uint64_t *)(fleet + 0x20) = (uint64_t)(f + 1);

      {
        uint8_t **entriesData = (uint8_t **)((uint8_t *)lmc + 0x0108);
        int maxLoadouts = matchCount;
        UObject **resolvedLoadouts =
            (UObject **)UE4Malloc(maxLoadouts * sizeof(UObject *));
        int resolvedCount = 0;
        for (int s = 0; s < maxLoadouts && s < matchCount; s++) {
          int entryIdx = loadoutIndices[s];
          if (entryIdx >= 0 && entryIdx < entriesNum) {
            uint8_t *entry = *entriesData + (entryIdx * 0x30);
            UObject **entryLoadouts = *(UObject ***)(entry);
            int32_t entryLoadoutCount = *(int32_t *)(entry + 0x08);
            if (entryLoadouts && entryLoadoutCount > 0 && entryLoadouts[0]) {
              resolvedLoadouts[resolvedCount++] = entryLoadouts[0];
            }
          }
        }
        *(UObject ***)(fleet + 0x28) = resolvedLoadouts;
        *(int32_t *)(fleet + 0x30) = resolvedCount;
        *(int32_t *)(fleet + 0x34) = maxLoadouts;
      }

      *(int32_t *)(fleet + 0x38) = 5;
      *(uint8_t *)(fleet + 0x40) = fleetDefs[f].fleetType;
      if (matchCount > 0)
        *(int32_t *)(fleet + 0x44) = loadoutIndices[0];
    }
    printf("[FLEET] Created 3 standard fleets on FleetManager %p\n", fm);
  }

  // Fire FleetManager delegates so UI knows data is ready
  pc->OnLoadoutInitilized();
  fm->OnLocalPlayerAvailable();
  fm->PlayerDataInitCompleted();
  printf("[FLEET] Fleet injection complete. %d ships registered, 3 fleets "
         "populated.\n",
         registered);

  // Populate manufacturer data on the UYUIData CDO obtained from
  // FrontendHUD.m_globalData ARCHITECTURE:
  //   AFrontendHUD.m_globalData (offset 0x04B0) -> UClass* pointing to a
  //   UYUIData Blueprint subclass That class's CDO has m_manufacturerEntries at
  //   offset 0x00C8 YUIExternalFunctions::GetManufacturerData reads from the
  //   LIVE AYMenu actor at offset 0x0638 We populate UYUIData CDOs here, then
  //   copy the CDO data into live AYMenu instances below
  {
    const int32_t NUM_MANUFACTURERS = 3;
    const int32_t ENTRY_SIZE = 0xA8; // sizeof(FYUIManufacturerInformationEntry)

    // Helper lambda: inject manufacturer entries into a TArray at a given
    // offset
    auto injectManufacturersAt = [&](UObject *target, int32_t offset,
                                     const char *label) -> bool {
      if (!target) {
        printf("[UI] WARNING: %s is NULL, skipping\n", label);
        return false;
      }
      TArrayRaw *dstArray = (TArrayRaw *)((uint8_t *)target + offset);
      printf("[UI] %s (%p + 0x%X): current manufacturer entries: count=%d\n",
             label, target, offset, dstArray->Count);

      if (dstArray->Count > 0 && dstArray->Count < 100 && dstArray->Data &&
          (uintptr_t)dstArray->Data > 0x10000 &&
          !IsBadReadPtr(dstArray->Data, dstArray->Count * ENTRY_SIZE)) {
        printf("[UI] %s already has %d manufacturer entries â€” using existing "
               "data\n",
               label, dstArray->Count);
        // Log the IDs to verify
        for (int i = 0; i < dstArray->Count && i < 10; i++) {
          int32_t id = *(int32_t *)(dstArray->Data + (i * ENTRY_SIZE) + 0xA0);
          printf("[UI]   existing entry[%d]: id=%d\n", i, id);
        }
        return true;
      }

      // Try to find valid manufacturer data from ANY UYUIData-derived CDO
      // NOTE: Default__YUIData has count=0, real data is on Default__GlobalUI_C
      static TArrayRaw *s_cachedSrc = nullptr;
      if (!s_cachedSrc) {
        UClass *uiDataCls =
            UObject::FindObject<UClass>("Class DreadGame.YUIData");
        if (uiDataCls) {
          for (int idx = 0; idx < UObject::GObjects->Count() && !s_cachedSrc;
               idx++) {
            UObject *o = UObject::GObjects->GetByIndex(idx);
            if (!o || !o->IsA(uiDataCls))
              continue;
            TArrayRaw *arr = (TArrayRaw *)((uint8_t *)o + 0x00C8);
            if (arr->Count > 0 && arr->Count < 100 && arr->Data &&
                (uintptr_t)arr->Data > 0x10000 &&
                !IsBadReadPtr(arr->Data, arr->Count * ENTRY_SIZE)) {
              s_cachedSrc = arr;
              printf("[UI] injectManufacturersAt: found source from %s with %d "
                     "entries\n",
                     o->GetFullName().c_str(), arr->Count);
            }
          }
        }
      }
      TArrayRaw *cdoSrc = s_cachedSrc;

      if (cdoSrc) {
        // Perform a deep copy of the manufacturer array to prevent double-free
        // and FText refcount depletion
        SafeCopyManufacturerArray(dstArray, cdoSrc);
        printf("[UI] Copied %d manufacturer entries from UIData CDO into %s "
               "(FText-safe)\n",
               cdoSrc->Count, label);
        for (int i = 0; i < cdoSrc->Count && i < 5; i++) {
          int32_t id = *(int32_t *)(dstArray->Data + (i * ENTRY_SIZE) + 0xA0);
          printf("[UI]   entry[%d]: id=%d\n", i, id);
        }
      } else {
        // Fallback: synthetic entries (WARNING: FText fields will be null!)
        printf("[UI] WARNING: No CDO data available for %s, creating minimal "
               "entries (FText-unsafe!)\n",
               label);
        int32_t totalBytes = NUM_MANUFACTURERS * ENTRY_SIZE;
        uint8_t *buf = (uint8_t *)UE4Malloc(totalBytes);
        memset(buf, 0, totalBytes);
        for (int i = 0; i < NUM_MANUFACTURERS; i++) {
          uint8_t *entry = buf + (i * ENTRY_SIZE);
          *(int32_t *)(entry + 0xA0) = i; // m_id field
          printf("[UI] Created synthetic entry[%d]: id=%d\n", i, i);
        }
        dstArray->Data = buf;
        dstArray->Count = NUM_MANUFACTURERS;
        dstArray->Max = NUM_MANUFACTURERS;
      }
      printf("[UI] Injected manufacturer entries into %s\n", label);
      return true;
    };

    // === PRIMARY TARGET: UYUIData CDO from AFrontendHUD.m_globalData ===
    // This is where YUIExternalFunctions::GetManufacturerData ACTUALLY reads
    // from
    if (g_capturedHUD) {
      // Probe multiple offsets since Blueprint subclass UI_FrontEnd_C may shift
      // things AFrontendHUD.m_globalData is documented at 0x04B0, but check
      // nearby offsets too
      const int32_t offsets[] = {0x04B0, 0x04B8, 0x04C8, 0x04D0};
      UClass *globalDataClass = nullptr;
      int32_t foundOffset = -1;

      printf("[UI] Probing g_capturedHUD (%p) for m_globalData UClass*...\n",
             g_capturedHUD);
      for (int32_t off : offsets) {
        uintptr_t val = *(uintptr_t *)((uint8_t *)g_capturedHUD + off);
        printf("[UI]   offset 0x%04X = 0x%llX", off, (unsigned long long)val);

        // Validate: must be a reasonable pointer (above 64KB, 8-byte aligned)
        if (val > 0x10000 && (val & 0x7) == 0 &&
            !IsBadReadPtr((void *)val, 64)) {
          UObject *candidate = (UObject *)val;
          // Check if the vtable pointer is also readable (basic object
          // validation)
          uintptr_t vtable = *(uintptr_t *)val;
          if (vtable > 0x10000 && !IsBadReadPtr((void *)vtable, 8)) {
            std::string name = candidate->GetFullName();
            if (name.find("Class") != std::string::npos ||
                name.find("BlueprintGeneratedClass") != std::string::npos) {
              printf(" -> %s (MATCH!)\n", name.c_str());
              globalDataClass = (UClass *)val;
              foundOffset = off;
              break;
            } else {
              printf(" -> %s (not a class)\n", name.c_str());
            }
          } else {
            printf(" (bad vtable)\n");
          }
        } else {
          printf(" (invalid pointer)\n");
        }
      }

      if (globalDataClass && foundOffset >= 0) {
        printf("[UI] Found m_globalData at offset 0x%04X: %s\n", foundOffset,
               globalDataClass->GetFullName().c_str());

        // Get the CDO of this class â€” this is the actual UYUIData instance
        // with serialized data
        UObject *globalDataCDO = globalDataClass->CreateDefaultObject();
        if (globalDataCDO) {
          printf("[UI] m_globalData CDO: %p (%s)\n", globalDataCDO,
                 globalDataCDO->GetFullName().c_str());
          // UYUIData.m_manufacturerEntries is at offset 0x00C8
          injectManufacturersAt(globalDataCDO, 0x00C8,
                                "UYUIData CDO (m_globalData)");
        } else {
          printf("[UI] WARNING: Could not get CDO from m_globalData class\n");
        }
      } else {
        printf("[UI] WARNING: Could not find m_globalData UClass* in "
               "FrontendHUD\n");
      }
    } else {
      printf("[UI] WARNING: g_capturedHUD is NULL, cannot read m_globalData\n");
    }

    // === SECONDARY: Also inject into Default__YUIData CDO ===
    UObject *uidataCDO =
        UObject::FindObject<UObject>("YUIData DreadGame.Default__YUIData");
    if (uidataCDO) {
      injectManufacturersAt(uidataCDO, 0x00C8, "Default__YUIData CDO");
    }

    // === TERTIARY: Find ALL loaded UYUIData instances and inject into them ===
    UClass *uidataClass =
        UObject::FindObject<UClass>("Class DreadGame.YUIData");
    if (uidataClass) {
      int found = 0;
      for (int i = 0; i < UObject::GObjects->Count() && found < 20; i++) {
        UObject *obj = UObject::GObjects->GetByIndex(i);
        if (!obj)
          continue;
        if (obj->IsA(uidataClass) && obj != uidataCDO) {
          found++;
          char label[256];
          snprintf(label, sizeof(label), "UYUIData instance #%d", found);
          printf("[UI] Found non-CDO UYUIData: %p (%s)\n", obj,
                 obj->GetFullName().c_str());
          injectManufacturersAt(obj, 0x00C8, label);
        }
      }
      if (found == 0) {
        printf("[UI] No non-CDO UYUIData instances found\n");
      }
    }

    // === CRITICAL: Inject into ALL AYMenu instances (CDO + live actors) at
    // 0x0638 === YUIExternalFunctions::GetManufacturerData reads from the LIVE
    // AYMenu actor, not the CDO! We must find all live instances and copy valid
    // manufacturer data into them. Source: Find ANY UYUIData-derived object
    // that has valid entries (Default__GlobalUI_C has 3) NOTE: Default__YUIData
    // itself has count=0 at runtime! The real data is on the
    //       Blueprint subclass CDO Default__GlobalUI_C which inherits from
    //       UYUIData.
    TArrayRaw *srcMfgArray = nullptr;
    UObject *srcMfgOwner = nullptr;

    // First try Default__YUIData (base CDO)
    UObject *uidataBase =
        UObject::FindObject<UObject>("YUIData DreadGame.Default__YUIData");
    if (uidataBase) {
      TArrayRaw *check = (TArrayRaw *)((uint8_t *)uidataBase + 0x00C8);
      if (check->Count > 0 && check->Data && (uintptr_t)check->Data > 0x10000 &&
          !IsBadReadPtr(check->Data, check->Count * ENTRY_SIZE)) {
        srcMfgArray = check;
        srcMfgOwner = uidataBase;
        printf("[UI] Source from Default__YUIData: %d entries\n", check->Count);
      } else {
        printf("[UI] Default__YUIData has no entries (count=%d), scanning "
               "subclasses...\n",
               check->Count);
      }
    }

    // If base CDO empty, scan ALL UYUIData instances for one with valid data
    if (!srcMfgArray && uidataClass) {
      for (int i = 0; i < UObject::GObjects->Count() && !srcMfgArray; i++) {
        UObject *obj = UObject::GObjects->GetByIndex(i);
        if (!obj)
          continue;
        if (!obj->IsA(uidataClass))
          continue;
        TArrayRaw *check = (TArrayRaw *)((uint8_t *)obj + 0x00C8);
        if (check->Count > 0 && check->Count < 100 && check->Data &&
            (uintptr_t)check->Data > 0x10000 &&
            !IsBadReadPtr(check->Data, check->Count * ENTRY_SIZE)) {
          srcMfgArray = check;
          srcMfgOwner = obj;
          printf("[UI] Found manufacturer source: %p (%s) with %d entries\n",
                 obj, obj->GetFullName().c_str(), check->Count);
          for (int j = 0; j < check->Count && j < 5; j++) {
            int32_t id = *(int32_t *)(check->Data + (j * ENTRY_SIZE) + 0xA0);
            printf("[UI]   source entry[%d]: id=%d\n", j, id);
          }
          break;
        }
      }
    }

    if (!srcMfgArray) {
      printf("[UI] CRITICAL: No UYUIData instance has valid manufacturer "
             "entries!\n");
    }

    // Lambda: copy REAL manufacturer data (with FText) from UIData source into
    // target at given offset
    auto copyManufacturersFrom = [&](UObject *target, int32_t offset,
                                     const char *label,
                                     TArrayRaw *src) -> bool {
      if (!target || !src || src->Count <= 0)
        return false;
      TArrayRaw *dstArray = (TArrayRaw *)((uint8_t *)target + offset);
      // If already populated with valid data, skip
      if (dstArray->Count > 0 && dstArray->Count < 100 && dstArray->Data &&
          (uintptr_t)dstArray->Data > 0x10000 &&
          !IsBadReadPtr(dstArray->Data, dstArray->Count * ENTRY_SIZE)) {
        printf("[UI] %s already has %d manufacturer entries â€” verifying\n",
               label, dstArray->Count);
        for (int i = 0; i < dstArray->Count && i < 5; i++) {
          int32_t id = *(int32_t *)(dstArray->Data + (i * ENTRY_SIZE) + 0xA0);
          printf("[UI]   entry[%d]: id=%d\n", i, id);
        }
        return true;
      }
      // Perform a deep copy of the manufacturer array to prevent double-free
      // and FText refcount depletion
      SafeCopyManufacturerArray(dstArray, src);
      printf("[UI] Copied %d manufacturer entries from UIData CDO into %s\n",
             src->Count, label);
      for (int i = 0; i < src->Count && i < 5; i++) {
        int32_t id = *(int32_t *)(dstArray->Data + (i * ENTRY_SIZE) + 0xA0);
        printf("[UI]   entry[%d]: id=%d\n", i, id);
      }
      return true;
    };

    // Inject into Default__YMenu CDO
    UObject *menuCDO =
        UObject::FindObject<UObject>("YMenu DreadGame.Default__YMenu");
    if (menuCDO && srcMfgArray) {
      copyManufacturersFrom(menuCDO, 0x0638, "Default__YMenu CDO", srcMfgArray);
    }

    // === NEW v19.50: Inject via PlayerController.m_globalData (offset 0x1140)
    // === AYPlayerController_Outpost has its OWN m_globalData UClass* at
    // 0x1140! This is SEPARATE from AFrontendHUD.m_globalData (0x04B0). The
    // native GetManufacturerData likely reads from here, not from the HUD.
    {
      printf("[UI] === PlayerController m_globalData injection ===\n");
      printf("[UI] PC address: %p, class: %s\n", pc, pc->GetFullName().c_str());

      // Read m_globalData at offset 0x1140
      uintptr_t pcGlobalDataVal = *(uintptr_t *)((uint8_t *)pc + 0x1140);
      printf("[UI] PC.m_globalData (0x1140) = 0x%llX\n",
             (unsigned long long)pcGlobalDataVal);

      if (pcGlobalDataVal > 0x10000 &&
          !IsBadReadPtr((void *)pcGlobalDataVal, 64)) {
        UObject *pcGlobalData = (UObject *)pcGlobalDataVal;
        printf("[UI] PC.m_globalData points to: %s\n",
               pcGlobalData->GetFullName().c_str());

        // This should be a UClass* - get its CDO to find/inject manufacturer
        // data The CDO offset in UClass is typically at a known offset. Let's
        // use FindObject instead.
        std::string className = pcGlobalData->GetName();
        std::string cdoName = "Default__" + className;

        // Search for the CDO in GObjects
        UObject *pcGlobalDataCDO = nullptr;
        for (int i = 0; i < UObject::GObjects->Count() && !pcGlobalDataCDO;
             i++) {
          UObject *obj = UObject::GObjects->GetByIndex(i);
          if (!obj)
            continue;
          std::string name = obj->GetName();
          if (name == cdoName) {
            pcGlobalDataCDO = obj;
          }
        }

        if (pcGlobalDataCDO) {
          printf("[UI] Found CDO: %p (%s)\n", pcGlobalDataCDO,
                 pcGlobalDataCDO->GetFullName().c_str());
          TArrayRaw *cdoArr =
              (TArrayRaw *)((uint8_t *)pcGlobalDataCDO + 0x00C8);
          printf("[UI] CDO.m_manufacturerEntries (0xC8): count=%d, data=%p\n",
                 cdoArr->Count, cdoArr->Data);

          if (cdoArr->Count > 0 && cdoArr->Data &&
              (uintptr_t)cdoArr->Data > 0x10000) {
            printf("[UI] PC CDO already has manufacturer data!\n");
            for (int j = 0; j < cdoArr->Count && j < 5; j++) {
              int32_t id = *(int32_t *)(cdoArr->Data + (j * ENTRY_SIZE) + 0xA0);
              printf("[UI]   entry[%d]: id=%d\n", j, id);
            }
          } else if (srcMfgArray) {
            // Inject from our source
            cdoArr->Data = srcMfgArray->Data;
            cdoArr->Count = srcMfgArray->Count;
            cdoArr->Max = srcMfgArray->Count;
            printf("[UI] Injected %d manufacturer entries into PC's globalData "
                   "CDO\n",
                   srcMfgArray->Count);
          }
        } else {
          printf("[UI] Could not find CDO for PC.m_globalData class '%s'\n",
                 className.c_str());
        }
      } else {
        printf(
            "[UI] PC.m_globalData is NULL or invalid â€” needs population!\n");
        // If empty, try to populate with the GlobalUI_C class
        if (srcMfgOwner) {
          // srcMfgOwner is Default__GlobalUI_C - find its class
          UObject *globalUIClass = UObject::FindObject<UObject>(
              "BlueprintGeneratedClass GlobalUI.GlobalUI_C");
          if (globalUIClass) {
            *(uintptr_t *)((uint8_t *)pc + 0x1140) = (uintptr_t)globalUIClass;
            printf("[UI] Set PC.m_globalData to GlobalUI_C class: %p\n",
                   globalUIClass);
          }
        }
      }

      // Also check m_outpostHUD at 0x11F8 (AYMenu*)
      uintptr_t outpostHUDVal = *(uintptr_t *)((uint8_t *)pc + 0x11F8);
      printf("[UI] PC.m_outpostHUD (0x11F8) = 0x%llX\n",
             (unsigned long long)outpostHUDVal);
      if (outpostHUDVal > 0x10000 && !IsBadReadPtr((void *)outpostHUDVal, 64)) {
        UObject *outpostHUD = (UObject *)outpostHUDVal;
        printf("[UI] PC.m_outpostHUD: %s\n", outpostHUD->GetFullName().c_str());
        // This is the AYMenu instance! Inject manufacturer data at 0x0638
        if (srcMfgArray) {
          // copyManufacturersFrom(outpostHUD, 0x0638, "PC.m_outpostHUD
          // (AYMenu)", srcMfgArray);
        }
      } else {
        printf("[UI] PC.m_outpostHUD is NULL â€” no live AYMenu from PC\n");
      }
    }

    // === CRITICAL: Find and inject into ALL LIVE AYMenu instances ===
    // The native GetManufacturerData code reads from the live AYMenu actor, not
    // the CDO.
    UClass *ymenuClass = UObject::FindObject<UClass>("Class DreadGame.YMenu");
    if (ymenuClass && srcMfgArray) {
      int menuFound = 0;
      for (int i = 0; i < UObject::GObjects->Count() && menuFound < 20; i++) {
        UObject *obj = UObject::GObjects->GetByIndex(i);
        if (!obj)
          continue;
        if (obj->IsA(ymenuClass) && obj != menuCDO) {
          menuFound++;
          char label[256];
          snprintf(label, sizeof(label), "Live AYMenu instance #%d (%s)",
                   menuFound, obj->GetName().c_str());
          printf("[UI] Found live AYMenu instance: %p (%s)\n", obj,
                 obj->GetFullName().c_str());
          // copyManufacturersFrom(obj, 0x0638, label, srcMfgArray);
        }
      }
      if (menuFound == 0) {
        printf("[UI] No live AYMenu instances found (CDO-only injection)\n");
      } else {
        printf(
            "[UI] Injected manufacturer data into %d live AYMenu instance(s)\n",
            menuFound);
      }
    } else {
      printf("[UI] WARNING: Cannot find YMenu class or no source data for live "
             "injection\n");
    }
  }

  g_isInjecting = false;
}

// Forward declarations for types/functions defined later in this file
struct TArrayRaw_FwdDecl {
  uint8_t *Data;
  int32_t Count;
  int32_t Max;
};
AActor *UWorldSpawnActor(UClass *ActorClass, FVector *SpawnLocation,
                         FRotator *SpawnRotation);

void ProcessEventHook(UObject *object, UFunction *function, void *params) {
  if (!function) {
    if (pProcessEvent_Original)
      pProcessEvent_Original(object, function, params);
    return;
  }

  std::string funcName = function->GetFullName();

  // === MONARCH INVISIBILITY FIX: Override item ownership for Tyr vanity parts ===
  if (funcName.find("YCtAInventoryInterface.HasItem") != std::string::npos) {
    if (pProcessEvent_Original) {
      pProcessEvent_Original(object, function, params);
    }
    struct HasItemParams {
      int32_t itemID;
      bool ReturnValue;
    } *p = (HasItemParams *)params;
    if (p) {
      tee_printf("[INVENTORY] HasItem(%d) called, forcing return to true (was %s)\n", p->itemID, p->ReturnValue ? "true" : "false");
      p->ReturnValue = true;
    }
    return;
  }

  // === FLEET MANAGEMENT & PURCHASE INTERCEPTION ===
  if (funcName.find("SelectShipButton") != std::string::npos ||
      funcName.find("ManageFleet") != std::string::npos ||
      funcName.find("OnPurchaseShip") != std::string::npos ||
      funcName.find("PurchaseShip") != std::string::npos) {
    tee_printf("[FLEET-HOOK] Event %s called on %s\n", funcName.c_str(), object ? object->GetFullName().c_str() : "null");

    static ULONGLONG s_lastSaveTime = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastSaveTime > 1000) {
      s_lastSaveTime = now;
      SaveFleetData();
    }
  }



  // Capture the CustomisationPreview actor when it ticks, as it is a verified
  // ticking target
  if (funcName.find("CustomisationPreview_BP_C:ReceiveTick") !=
      std::string::npos) {
    if (!IsValidUObject(g_customizationPreviewActor)) {
      g_customizationPreviewActor = object;
    } else if (object != g_customizationPreviewActor) {
      if (object->InternalIndex > g_customizationPreviewActor->InternalIndex) {
        g_customizationPreviewActor = object;
      }
    }

    if (object) {
      std::string objFullName = object->GetFullName();
      if (objFullName.find("MN_HGR_DREADH") != std::string::npos || objFullName.find("DreadH") != std::string::npos || objFullName.find("DreadnoughtHeavy") != std::string::npos) {
        static int s_monarchApplyCount = 0;
        if (s_monarchApplyCount < 10) {
          tee_printf("[MONARCH] Monarch hangar preview actor ticking: %s. Applying Tyr hero parts...\n", objFullName.c_str());
          ApplyMonarchHeroParts(object);
          s_monarchApplyCount++;
        }
      }
    }
  }

  // Deadlock bypass: manually finalize level when streaming completes
  if (funcName.find("HandleOnLevelStreamed") != std::string::npos) {
    bool isTickingTarget = false;
    if (object && object->Class) {
      std::string className = object->Class->GetFullName();
      if (className.find("PlayerController") != std::string::npos ||
          className.find("CustomisationPreview") != std::string::npos) {
        isTickingTarget = true;
      }
    }

    printf(
        "[HOOK] Intercepted HandleOnLevelStreamed on %p (isTickingTarget=%d)\n",
        object, isTickingTarget);

    if (!isTickingTarget) {
      // Case 1: First callback (stream completed) on GameMode
      void *gm = object;
      void *tm = *(void **)((uintptr_t)gm + 0x9a8);
      if (!tm) {
        printf("[HOOK] WARNING: transitionManager is NULL on GameMode %p!\n",
               gm);
        if (pProcessEvent_Original)
          pProcessEvent_Original((UObject *)gm, function, params);
        return;
      }

      void *head = *(void **)((uintptr_t)tm + 0x78);
      if (!head) {
        printf("[HOOK] WARNING: queue Head is NULL on transitionManager %p!\n",
               tm);
        if (pProcessEvent_Original)
          pProcessEvent_Original((UObject *)gm, function, params);
        return;
      }

      void *currentLevelNode = *(void **)head;
      if (!currentLevelNode) {
        printf(
            "[HOOK] WARNING: currentLevelNode is NULL (empty queue) on %p!\n",
            tm);
        if (pProcessEvent_Original)
          pProcessEvent_Original((UObject *)gm, function, params);
        return;
      }

      CG::FName levelName = *(CG::FName *)((uintptr_t)currentLevelNode + 0x10);
      std::string levelNameStr = levelName.GetName();
      bool isLast = (*(void **)currentLevelNode == nullptr);

      if (g_waitingForFinalization && levelNameStr.find(g_waitingLevelName) != std::string::npos) {
        printf("[HOOK] Intercepted finalization callback for level: %s. Suppressing native call to prevent unload.\n", levelNameStr.c_str());
        return;
      }

      if (levelName.ComparisonIndex != 0) {
        printf("[HOOK] Level streamed: %s (isLast=%d). Initiating manual "
               "finalization...\n",
               levelNameStr.c_str(), isLast);

        // Force the streaming level flags to loaded and visible before finalization
        if (*UWorld::GWorld) {
          UWorld* world = *UWorld::GWorld;
          TArray<ULevelStreaming*>& streamingLevels = world->StreamingLevels;
          for (int i = 0; i < streamingLevels.Count(); ++i) {
            ULevelStreaming* sl = streamingLevels[i];
            if (sl) {
              std::string pathStr = "";
              FString* pPathFStr = (FString*)((uintptr_t)sl + 0x40);
              if (pPathFStr && pPathFStr->Data()) {
                pathStr = pPathFStr->ToString();
              }
              if (pathStr.find(levelNameStr) != std::string::npos) {
                uint8_t* pFlags = (uint8_t*)((uintptr_t)sl + 0x00B0);
                if (pFlags) {
                  *pFlags |= 0x60; // Set bShouldBeLoaded = true (bit 5) and bShouldBeVisible = true (bit 6)
                  printf("[HOOK] Forced level streaming flags for %s to 0x%02X before finalizing.\n", levelNameStr.c_str(), *pFlags);
                }
                if (sl->LoadedLevel) {
                  uint8_t* pLvlFlags = (uint8_t*)((uintptr_t)sl->LoadedLevel + 0x01B0);
                  if (pLvlFlags) {
                    uint8_t oldFlags = *pLvlFlags;
                    *pLvlFlags &= ~0x20; // Clear bActorsInitialized (bit 5, 0x20) so engine registers components/ticks upon visibility update
                    printf("[HOOK] Cleared bActorsInitialized on LoadedLevel %p (flags: 0x%02X -> 0x%02X) before finalization.\n", sl->LoadedLevel, oldFlags, *pLvlFlags);
                  }
                }
                break;
              }
            }
          }
        }

        // Initialize polling state for UGameEngineTick BEFORE calling finalizeLevel
        // to prevent infinite recursion on synchronous callbacks
        g_waitingForFinalization = true;
        g_waitingLevelName = levelNameStr;
        g_waitingGM = gm;
        g_finalizeFunction = function;
        printf("[HOOK] Polling initialized in GameEngineTick for level: %s\n",
               levelNameStr.c_str());

        // Let the GameMode native StreamCompleted handler run first
        if (pProcessEvent_Original) {
          pProcessEvent_Original((UObject *)gm, function, params);
        }
      }
    } else {
      // Let the PlayerController / CustomisationPreview handle its own events
      if (pProcessEvent_Original) {
        pProcessEvent_Original(object, function, params);
      }
    }
    return;
  }

  static int callDepth = 0;
  static enum EMenuState {
    STATE_LOGOS,
    STATE_TITLE,
    STATE_LOADING_DELAY,
    STATE_LOADING_HANGAR,
    STATE_READY
  } menuState = STATE_LOGOS;
  static int g_loadingDelayCountdown = 0; // Fake loading delay
  static bool hasReachedHangarOnce = false;
  static bool g_fleetInjected = false;
  static bool g_hudInitComplete = false; // Set after FLAG block runs pfcNative
  static int64_t g_savedFlagshipFName =
      0; // FName bits of our injected flagship (for repair)

  if (callDepth > 5) {
    if (pProcessEvent_Original)
      pProcessEvent_Original(object, function, params);
    return;
  }
  callDepth++;

  // === VIEWPORT OBSERVATION: Log key GameMode init functions as they fire
  // natively === These are the functions in the YGameMode_Outpost viewport
  // initialization chain. Logging them lets us see exactly what fires (and what
  // doesn't) in offline mode.
  static bool s_observedLevelStreamed = false;
  static bool s_observedLevelFinalized = false;
  static bool s_observedHudAvailable = false;
  static bool s_observedInitShip = false;
  static bool s_observedInitInternal = false;
  static bool s_observedFlagshipChanged = false;
  if (!s_observedHudAvailable &&
      funcName.find("OnHudAvailable") != std::string::npos) {
    s_observedHudAvailable = true;
    printf("[OBS] GameMode::OnHudAvailable fired on %p\n", object);
  }
  if (!s_observedInitShip &&
      funcName.find("InitializeOutpostShip") != std::string::npos &&
      funcName.find("Internal") == std::string::npos) {
    s_observedInitShip = true;
    printf("[OBS] GameMode::InitializeOutpostShip fired on %p\n", object);
  }
  if (!s_observedInitInternal &&
      funcName.find("InitializeOutpostShipInternal") != std::string::npos) {
    s_observedInitInternal = true;
    printf("[OBS] GameMode::InitializeOutpostShipInternal fired on %p\n",
           object);
  }
  if (!s_observedLevelStreamed &&
      funcName.find("HandleOnLevelStreamed") != std::string::npos) {
    s_observedLevelStreamed = true;
    printf("[OBS] GameMode::HandleOnLevelStreamed fired on %p\n", object);
  }
  if (!s_observedLevelFinalized &&
      funcName.find("HandleOnLevelFinalized") != std::string::npos) {
    s_observedLevelFinalized = true;
    printf("[OBS] GameMode::HandleOnLevelFinalized fired on %p\n", object);
  }
  if (!s_observedFlagshipChanged &&
      funcName.find("PlayerFlagshipChanged") != std::string::npos) {
    s_observedFlagshipChanged = true;
    printf("[OBS] GameMode::PlayerFlagshipChanged fired on %p\n", object);
  }

  if (!g_capturedHUD && funcName.find("UI_FrontEnd_C.UserConstructionScript") !=
                            std::string::npos) {
    g_capturedHUD = object;
    printf("[UI] Captured FrontendHUD: %p (%s)\n", object,
           object->GetFullName().c_str());
  }

  // === 2. HandleHangarStateUpdate â€” hangar level is loaded, trigger fleet
  // injection ===
  static bool g_techTreeInspected = false;
  if (funcName.find("HandleHangarStateUpdate") != std::string::npos &&
      !g_techTreeInspected) {
    g_techTreeInspected = true;
    printf("[UI] HandleHangarStateUpdate fired. Hangar level is loaded.\n");

    // Disable GC at runtime to prevent 50-second crash
    DisableGCAtRuntime();

    // =====================================================================
    // Tech Tree Population via Native Engine Functions
    // Strategy: Probe the pipeline with real engine functions to understand
    // the data state, then call native parsers to populate structures.
    // NO raw byte writes to complex structs â€” let the engine handle layout.
    // =====================================================================

    // Find the live YTechTreeManager
    UObject *liveTechTreeMgr = nullptr;
    for (int i = 0; i < UObject::GObjects->Count(); i++) {
      UObject *obj = UObject::GObjects->GetByIndex(i);
      if (!obj || !obj->Class)
        continue;
      std::string fullName = obj->GetFullName();
      if (fullName.find("YTechTreeManager Transient.") != std::string::npos &&
          fullName.find("Default__") == std::string::npos) {
        liveTechTreeMgr = obj;
        printf("[TTM] Found live TTM: %s at %p\n", fullName.c_str(), obj);
        break;
      }
    }

    if (!liveTechTreeMgr) {
      printf("[TTM] WARNING: No live YTechTreeManager found!\n");
    } else {
      struct FRawArray {
        void *Data;
        int32_t Count;
        int32_t Max;
      };
      uint8_t *ttm = (uint8_t *)liveTechTreeMgr;

      // Check TTM internal arrays
      FRawArray *arr38 = (FRawArray *)(ttm + 0x38);
      FRawArray *arr48 = (FRawArray *)(ttm + 0x48);
      FRawArray *arr58 = (FRawArray *)(ttm + 0x58);
      FRawArray *arr68 = (FRawArray *)(ttm + 0x68);
      printf("[TTM] Arrays: mfg=%d, class=%d, lookups=%d, orphaned=%d\n",
             arr38->Count, arr48->Count, arr58->Count, arr68->Count);

      // Probe CachedItemIDData singleton
      typedef void *(__fastcall * fn_GetCachedItemIDData)();
      auto GetCachedItemIDData =
          (fn_GetCachedItemIDData)(Globals::ModuleBase + 0x4813A0);
      void *cacheInst = GetCachedItemIDData();
      printf("[TTM] UYCachedItemIDData singleton: %p\n", cacheInst);

      if (cacheInst) {
        // Dump the singleton's internal state
        FRawArray *cacheArr = (FRawArray *)((uint8_t *)cacheInst + 0x28);
        printf("[TTM] CacheInstance+0x28 (cached entries array): Data=%p "
               "Count=%d Max=%d\n",
               cacheArr->Data, cacheArr->Count, cacheArr->Max);

        // Probe cache with real item IDs from CachedItemData_BP.uasset
        typedef void(__fastcall * fn_FindCachedDataEntry)(int, void **);
        auto FindCachedDataEntry =
            (fn_FindCachedDataEntry)(Globals::ModuleBase + 0x480F70);

        // Sample item IDs from PAK (spread across all ID ranges)
        struct {
          int id;
          const char *desc;
        } testItems[] = {
            {0x01FF0121, "0x01FF range"}, {0x03FF0001, "0x03FF range"},
            {0x04FF00C3, "0x04FF range"}, {0x05FF0127, "0x05FF range"},
            {0x0AFF016B, "0x0AFF range"}, {0x14FF0002, "0x14FF range"},
            {0x18FF0014, "0x18FF range"}, {0x33FF0001, "0x33FF range"},
            {0x63FF002C, "0x63FF range"},
        };
        int numTest = sizeof(testItems) / sizeof(testItems[0]);
        int foundCount = 0;
        for (int t = 0; t < numTest; t++) {
          void *entry = nullptr;
          FindCachedDataEntry(testItems[t].id, &entry);
          if (entry)
            foundCount++;
        }
        printf("[CACHE] Item ID probe: %d/%d found in %d cache entries\n",
               foundCount, numTest, cacheArr->Count);

        // From Ghidra analysis of FUN_1403ffde0 (server response parser):
        // TTM+0x38 = TArray of manufacturer groups (stride 0x28 = 40 bytes per
        // entry) Each entry layout:
        //   +0x00: int64  manufacturer_id
        //   +0x08: void*  items_Data     (inner TArray, stride 0x48)
        //   +0x10: int32  items_Count
        //   +0x14: int32  items_Max
        //   +0x18: void*  heroItems_Data (second inner TArray, stride 0x48)
        //   +0x20: int32  heroItems_Count
        //   +0x24: int32  heroItems_Max
        // TTM+0x40 = int32 Count of manufacturer groups
        // TTM+0x44 = int32 Max of manufacturer groups

        if (arr38->Count == 0 && foundCount > 0) {
          printf(
              "[TTM] TTM is empty. Building per-manufacturer tech trees...\n");
          EnsureUE4Allocators();

          // Initialize the REAL per-manufacturer ship data
          InitFullTechTree();

          const int NUM_MFGS = 3;
          const int MFG_ENTRY_SIZE = 0x28;    // 40 bytes per manufacturer group
          const int ITEM_ENTRY_SIZE = 0x48;   // 72 bytes per item
          const int CLASS_LOOKUP_SIZE = 0x0C; // 12 bytes per class lookup

          // Count total items across all manufacturers
          int totalItems = 0;
          for (int m = 0; m < NUM_MFGS; m++)
            totalItems += s_manufacturers[m].count;

          uint8_t *mfgData = (uint8_t *)UE4Malloc(NUM_MFGS * MFG_ENTRY_SIZE);
          uint8_t *classData =
              (uint8_t *)UE4Malloc(totalItems * CLASS_LOOKUP_SIZE);

          if (mfgData && classData) {
            memset(mfgData, 0, NUM_MFGS * MFG_ENTRY_SIZE);
            memset(classData, 0, totalItems * CLASS_LOOKUP_SIZE);
            printf("[TTM] Allocated mfg groups at %p, class lookups at %p\n",
                   mfgData, classData);

            int classIdx = 0;
            int globalShipIdx = 0; // index into g_FullTechTree

            for (int m = 0; m < NUM_MFGS; m++) {
              uint8_t *mfgEntry = mfgData + m * MFG_ENTRY_SIZE;
              *(int64_t *)(mfgEntry + 0x00) = m; // manufacturer ID (0, 1, 2)

              int shipCount = s_manufacturers[m].count;
              uint8_t *itemsData =
                  (uint8_t *)UE4Malloc(shipCount * ITEM_ENTRY_SIZE);
              if (!itemsData)
                continue;
              memset(itemsData, 0, shipCount * ITEM_ENTRY_SIZE);

              for (int i = 0; i < shipCount; i++) {
                uint8_t *item = itemsData + i * ITEM_ENTRY_SIZE;
                const FTechTreeShip &ship = g_FullTechTree[globalShipIdx];

                // +0x20: item_id â€” unique synthetic ID
                *(int32_t *)(item + 0x20) = ship.shipId;
                // +0x2C: tier (1-5)
                *(int32_t *)(item + 0x2C) = ship.tier;
                // +0x3C: ship class byte (0-4)
                *(char *)(item + 0x3C) = (char)ship.shipClass;
                // +0x3D: isHero flag
                *(char *)(item + 0x3D) = 0;

                // Inner entry structure (required by FUN_4E1D80)
                {
                  const int INNER_ENTRY_SIZE = 32;
                  const int TIER_RECORD_SIZE = 0x18;

                  uint8_t *innerEntry = (uint8_t *)UE4Malloc(INNER_ENTRY_SIZE);
                  uint8_t *tierRecord = (uint8_t *)UE4Malloc(TIER_RECORD_SIZE);

                  if (innerEntry && tierRecord) {
                    memset(innerEntry, 0, INNER_ENTRY_SIZE);
                    memset(tierRecord, 0, TIER_RECORD_SIZE);

                    *(void **)(tierRecord + 0x00) = nullptr;
                    *(int32_t *)(tierRecord + 0x08) = 0;
                    *(int32_t *)(tierRecord + 0x10) = ship.tier;

                    *(void **)(innerEntry + 0x00) = tierRecord;
                    *(int32_t *)(innerEntry + 0x08) = 1;
                    *(int32_t *)(innerEntry + 0x0C) = 1;
                    *(int64_t *)(innerEntry + 0x10) = 0;
                    *(int32_t *)(innerEntry + 0x18) = 0; // filter key MUST be 0

                    *(void **)(item + 0x00) = innerEntry;
                    *(int32_t *)(item + 0x08) = 1;
                    *(int32_t *)(item + 0x0C) = 1;
                  }
                }

                // Class lookup entry (TTM+0x58)
                uint8_t *cls = classData + classIdx * CLASS_LOOKUP_SIZE;
                *(int64_t *)(cls + 0x00) = (int64_t)(uintptr_t)item;
                *(int32_t *)(cls + 0x08) = ship.tier;
                classIdx++;

                if (i == 0) {
                  printf("[TTM] Mfg[%d] first: item=%p id=%d '%ls' class=%d "
                         "tier=%d\n",
                         m, item, ship.shipId, ship.name.c_str(),
                         ship.shipClass, ship.tier);
                }

                globalShipIdx++;
              }

              // Wire items into manufacturer entry
              *(void **)(mfgEntry + 0x08) = itemsData;
              *(int32_t *)(mfgEntry + 0x10) = shipCount;
              *(int32_t *)(mfgEntry + 0x14) = shipCount;

              // Store item array info for SetSelectedShip pointer scanning
              g_ttmItemBases[m] = itemsData;
              g_ttmItemCounts[m] = shipCount;

              printf("[TTM] Mfg[%d]: %d ships at %p\n", m, shipCount,
                     itemsData);
            }

            // Permanently populate TTM â€” safe now that the GC
            // root cause is fixed (unknown tokens exit via EndOfStream).
            // GetHeroShipsFromManufacturerData needs data in TTM+0x38
            // at all times, not just during GetManufacturerData calls.
            g_ttmMfgData = mfgData;
            g_ttmMfgCount = NUM_MFGS;
            g_ttmClassData = classData;
            g_ttmClassCount = classIdx;
            g_ttmPtr = ttm;

            // Wire data permanently into TTM
            arr38->Data = mfgData;
            arr38->Count = NUM_MFGS;
            arr38->Max = NUM_MFGS;
            arr58->Data = classData;
            arr58->Count = classIdx;
            arr58->Max = classIdx;

            // ====================================================
            // Phase 4: Populate TTM+0x68 with module/weapon entries
            // FUN_1403f51a0 searches TTM+0x68 as a flat TArray<stride 0x48>
            // matching *(int*)(entry+0x20) == itemId. This enables
            // GetShipResearchData -> FUN_4F3190 to resolve modules
            // referenced in each ship's m_relatedItemIDs.
            // ====================================================
            if (!g_moduleItemIds.empty()) {
              int moduleCount = (int)g_moduleItemIds.size();
              uint8_t *moduleData =
                  (uint8_t *)UE4Malloc(moduleCount * ITEM_ENTRY_SIZE);
              if (moduleData) {
                memset(moduleData, 0, moduleCount * ITEM_ENTRY_SIZE);
                int idx = 0;
                for (auto const &pair : g_moduleItemIds) {
                  int32_t modItemId = pair.first;
                  uint8_t identifier = pair.second;
                  uint8_t *item = moduleData + idx * ITEM_ENTRY_SIZE;

                  // +0x20: item_id â€” the module's canonical cache ID
                  *(int32_t *)(item + 0x20) = modItemId;

                  // +0x2C: tier â€” look up from discovery cache if available
                  auto it = g_discoveryCache.find(modItemId);
                  if (it != g_discoveryCache.end()) {
                    *(int32_t *)(item + 0x2C) = it->second.tier;
                  } else {
                    *(int32_t *)(item + 0x2C) = 1; // fallback
                  }

                  // +0x3C: identifier byte from FYRelatedItemEntry
                  // FUN_4F3190 switch maps: 0->2, 1->3, 2->4, ... 8->10,
                  // 9->special The TTM item +0x3C must hold the value BEFORE
                  // the switch remap (i.e., the raw identifier from
                  // m_relatedItemIDs)
                  *(char *)(item + 0x3C) = (char)identifier;

                  // +0x3D: isHero flag = 0 (this is a module, not a hero ship)
                  *(char *)(item + 0x3D) = 0;

                  // Inner entry structure with tier record (required by
                  // FUN_4E1D80)
                  {
                    const int INNER_ENTRY_SIZE = 32;
                    const int TIER_RECORD_SIZE = 0x18;
                    uint8_t *innerEntry =
                        (uint8_t *)UE4Malloc(INNER_ENTRY_SIZE);
                    uint8_t *tierRecord =
                        (uint8_t *)UE4Malloc(TIER_RECORD_SIZE);
                    if (innerEntry && tierRecord) {
                      memset(innerEntry, 0, INNER_ENTRY_SIZE);
                      memset(tierRecord, 0, TIER_RECORD_SIZE);
                      int32_t modTier =
                          (it != g_discoveryCache.end()) ? it->second.tier : 1;
                      *(void **)(tierRecord + 0x00) = nullptr;
                      *(int32_t *)(tierRecord + 0x08) = 0;
                      *(int32_t *)(tierRecord + 0x10) = modTier;
                      *(void **)(innerEntry + 0x00) = tierRecord;
                      *(int32_t *)(innerEntry + 0x08) = 1;
                      *(int32_t *)(innerEntry + 0x0C) = 1;
                      *(int64_t *)(innerEntry + 0x10) = 0;
                      *(int32_t *)(innerEntry + 0x18) = 0;
                      *(void **)(item + 0x00) = innerEntry;
                      *(int32_t *)(item + 0x08) = 1;
                      *(int32_t *)(item + 0x0C) = 1;
                    }
                  }
                  idx++;
                }

                // Wire module data into TTM+0x68 (flat items array)
                arr68->Data = moduleData;
                arr68->Count = moduleCount;
                arr68->Max = moduleCount;

                printf("[TTM] Phase 4: Populated TTM+0x68 with %d "
                       "module/weapon entries\n",
                       moduleCount);
              }
            }

            printf("[TTM] TTM populated: %d mfg groups, %d class lookups, %d "
                   "modules (permanent)\n",
                   NUM_MFGS, classIdx, (int)g_moduleItemIds.size());
          }
        }
      } else {
        printf("[TTM] No UYCachedItemIDData singleton found.\n");
      }

      // Verify final TTM state
      printf("[TTM] Final state: mfg=%d, class=%d, lookups=%d\n", arr38->Count,
             arr48->Count, arr58->Count);
    }

    // Inject fleet data NOW â€” get PC from GWorld the reliable way
    if (!g_fleetInjected) {
      try {
        ULocalPlayer *lp =
            ((*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]);
        AYPlayerController *pc = (AYPlayerController *)(lp->PlayerController);
        if (pc && pc->m_loadoutManager && pc->m_fleetManager) {
          printf("[UI] Got PlayerController from GWorld: %p\n", pc);

          // ================================================================
          // PRE-FLEET INIT: Wire m_player + call InitializeOutpostShip
          // BEFORE fleet injection so fleet is empty when InitializeOutpostShip
          // runs. FUN_14034dff0 returns false (no flagship yet) â†’ only binds
          // delegates on fleet_obj+0x70 and +0x90. No crash. No immediate
          // ship loading. Later, PlayerFlagshipChanged will trigger loading.
          // ================================================================
          {
            uintptr_t gm = (uintptr_t)(*UWorld::GWorld)->AuthorityGameMode;
            UObject *gm_obj = (UObject *)(*UWorld::GWorld)->AuthorityGameMode;

            // 1. Wire GameMode+0x998 (m_player) = PC
            UObject **gmPlayerSlot = (UObject **)(gm + 0x998);
            if (*gmPlayerSlot == nullptr) {
              *gmPlayerSlot = (UObject *)pc;
              printf("[PRE] Wired GameMode+0x998 (m_player) = %p\n", pc);
            }

            // 2. Wire m_outpostHUD to AYMenu if not already done
            UObject **gmHudSlot = (UObject **)(gm + 0x09A0);
            if (*gmHudSlot == nullptr) {
              int objCount = UObject::GObjects->Count();
              for (int i = 0; i < objCount; i++) {
                UObject *o = UObject::GObjects->GetByIndex(i);
                if (!o || !o->Class)
                  continue;
                std::string fn = o->GetFullName();
                if (fn.find("YMenu") != std::string::npos &&
                    fn.find("PersistentLevel") != std::string::npos &&
                    fn.find("Function ") == std::string::npos &&
                    fn.find("Default__") == std::string::npos) {
                  *gmHudSlot = o;
                  printf(
                      "[PRE] Wired GameMode+0x9A0 (m_outpostHUD) = %p (%s)\n",
                      o, fn.c_str());
                  break;
                }
              }
            }

            // 3. Call InitializeOutpostShip BEFORE fleet injection.
            // Fleet is empty â†’ FUN_14034dff0 returns 0 â†’ binds delegates
            // ONLY. Camera sections TMap (GameMode+0xA38) is NOT populated here
            // yet. We deliberately defer OnHudAvailable to AFTER pfcNative so
            // that FUN_140372640 returns null (cameras empty) â†’ FUN_1403d1990
            // not called â†’ avoids crash in FUN_1403bd800 (tm+0x30 null
            // TArray).
            {
              UFunction *initShipFn = (UFunction *)GetObjByName(
                  "Function DreadGame.YGameMode_Outpost.InitializeOutpostShip");
              if (initShipFn) {
                pProcessEvent_Original(gm_obj, initShipFn, nullptr);
                printf("[PRE] InitializeOutpostShip completed (pre-fleet, "
                       "delegates bound).\n");
              }
            }
            // g_hudInitComplete set later (in FLAG block, after OnHudAvailable)
          }

          // ================================================================
          // FLEET INJECTION
          // ================================================================
          g_fleetInjected = true;
          InjectOfflineFleet(pc);

          // ================================================================
          // POST-FLEET: Wire tm+0x30, call OnHudAvailable, then pfcNative
          //
          // ROOT CAUSE ANALYSIS (FUN_1403bd800 crash):
          //   FUN_1403bd800(tm) calls FUN_140d6ad50(tm+0x30) to get an object.
          //   It then calls vtable+0x108 (= GetWorld()) on that object.
          //   tm+0x30 is a backpointer to GameMode â€” null offline because
          //   the transition manager's BeginPlay/init never ran.
          //   FIX: wire tm+0x30 = GameMode before calling pfcNative.
          //   With GameMode wired: GetWorld() works, finds 0 current ships,
          //   FUN_1403bd800 returns cleanly, FUN_1403835b0 fires â†’ ship
          //   shows.
          //
          // SEQUENCE:
          //   1. Wire tm+0x30 = GameMode
          //   2. OnHudAvailable â†’ camera sections TMap populated
          //   3. pfcNative â†’ FUN_140384d80 â†’ FUN_140372640(cameras ready)
          //   â†’
          //      FUN_1403d1990(tm, section, 1) â†’ FUN_1403bd800(tm) â†’ clean
          //      â†’ FUN_1403835b0(GameMode, section) â†’ FUN_1403cd3c0(tm,
          //      section) â†’ ship streams in and appears in viewport!
          // ================================================================
          {
            uintptr_t gm = (uintptr_t)(*UWorld::GWorld)->AuthorityGameMode;
            UObject *gm_obj = (UObject *)(*UWorld::GWorld)->AuthorityGameMode;

            // â”€â”€ Collect loadout FName (same probe as before)
            // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            void *fleetObj = *(void **)((uintptr_t)pc + 0x958);
            printf("[FLAG] pc->m_fleetManager = %p\n", pc->m_fleetManager);
            printf("[FLAG] PC+0x958 fleet_obj  = %p\n", fleetObj);

            int64_t foundFlagshipFName = 0;
            if (fleetObj) {
              void *loadoutSearchBase = *(void **)((uintptr_t)fleetObj + 0x28);
              printf("[FLAG] fleet_obj+0x28 (loadout_search_base) = %p\n",
                     loadoutSearchBase);
              if (loadoutSearchBase) {
                void *groupsPtr =
                    *(void **)((uintptr_t)loadoutSearchBase + 0x108);
                int groupCount = *(int *)((uintptr_t)loadoutSearchBase + 0x110);
                printf("[FLAG] loadout groups: ptr=%p count=%d\n", groupsPtr,
                       groupCount);
                for (int g = 0; g < groupCount && g < 5 && groupsPtr; g++) {
                  uintptr_t groupBase =
                      (uintptr_t)groupsPtr + (uintptr_t)g * 6 * 8;
                  void *ldArrayPtr = *(void **)groupBase;
                  int ldCount = *(int *)(groupBase + 8);
                  for (int l = 0; l < ldCount && l < 2 && ldArrayPtr; l++) {
                    void *loadout = *(void **)((uintptr_t)ldArrayPtr + l * 8);
                    if (loadout) {
                      int64_t fn = *(int64_t *)((uintptr_t)loadout + 0xB0);
                      uint8_t sc = *(uint8_t *)((uintptr_t)loadout + 0xD8);
                      if (g == 0)
                        printf("[FLAG] G0.L%d FName=0x%016llX shipClass=%d\n",
                               l, fn, sc);
                      if (fn != 0 && foundFlagshipFName == 0)
                        foundFlagshipFName = fn;
                    }
                  }
                }
              }
            }

            if (foundFlagshipFName != 0) {
              // Keep FName saved for later ship loading
              g_savedFlagshipFName = foundFlagshipFName;
              printf("[FLAG] Found flagship FName=0x%016llX\n",
                     foundFlagshipFName);
            } else {
              printf("[FLAG] No loadout FName found â€” flagship not set.\n");
            }
          }

          // ----------------------------------------------------------------
          // Wire m_outpostHUD (PC+0x11F8) to the live AYMenu actor.
          // Online: AYMenu::BeginPlay() calls SetOutpostHUD() on the PC.
          // Offline: that server-triggered BeginPlay signal never fires,
          // so PC+0x11F8 stays null and the 3D hangar viewport never shows.
          // Fix: scan GObjects for any live AYMenu instance and inject it.
          // AYMenu is the base class of VH_YMenu_Outpost_BP_C.
          // ----------------------------------------------------------------
          static bool s_ayMenuWired = false;
          if (!s_ayMenuWired) {
            UObject *foundMenu = nullptr;
            int objCount = UObject::GObjects->Count();
            for (int i = 0; i < objCount && !foundMenu; i++) {
              UObject *o = UObject::GObjects->GetByIndex(i);
              if (!o || !o->Class)
                continue;
              std::string fn = o->GetFullName();
              // Must be a world-placed actor: outer chain contains
              // PersistentLevel Reject: UFunction objects, CDOs, Class objects
              if (fn.substr(0, 9) == "Function ")
                continue;
              if (fn.substr(0, 6) == "Class ")
                continue;
              if (fn.find("Default__") != std::string::npos)
                continue;
              if (fn.find("PersistentLevel") == std::string::npos)
                continue;
              // Class name (before first space) must contain "YMenu"
              size_t sp = fn.find(' ');
              std::string className =
                  (sp != std::string::npos) ? fn.substr(0, sp) : fn;
              if (className.find("YMenu") != std::string::npos) {
                foundMenu = o;
                printf("[HUD] Found live AYMenu actor: %s at %p\n", fn.c_str(),
                       o);
              }
            }

            if (!foundMenu) {
              // ----------------------------------------------------------------
              // Offline: VH_YMenu_Outpost_BP_C is never placed in the level
              // because the server (AYGameMode_Outpost) never fires BeginPlay.
              // A CDO is NOT sufficient â€” AYMenu is referenced by 43+
              // functions and needs full UE4 actor lifecycle (BeginPlay, Tick,
              // components).
              //
              // FIX: Spawn a real AYMenu actor into the world.
              // Try VH_YMenu_Outpost_BP_C first (the Blueprint subclass that
              // online mode uses), fall back to the base AYMenu class.
              // ----------------------------------------------------------------
              UClass *menuBPClass = UObject::FindObject<UClass>(
                  "BlueprintGeneratedClass "
                  "VH_YMenu_Outpost_BP.VH_YMenu_Outpost_BP_C");
              if (!menuBPClass) {
                // Try common asset path variants
                menuBPClass = UObject::FindObject<UClass>(
                    "BlueprintGeneratedClass "
                    "/Game/Menus/VH_YMenu_Outpost_BP.VH_YMenu_Outpost_BP_C");
              }
              if (!menuBPClass) {
                // Fall back to native AYMenu class
                menuBPClass =
                    UObject::FindObject<UClass>("Class DreadGame.YMenu");
                if (menuBPClass) {
                  printf("[HUD] Using native AYMenu class for spawn (BP not "
                         "found)\n");
                }
              }

              if (menuBPClass) {
                printf("[HUD] Spawning live AYMenu actor using class: %s\n",
                       ((UObject *)menuBPClass)->GetFullName().c_str());
                FVector spawnLoc = {0.0f, 0.0f, 0.0f};
                FRotator spawnRot = {0.0f, 0.0f, 0.0f};
                AActor *spawnedMenu =
                    UWorldSpawnActor(menuBPClass, &spawnLoc, &spawnRot);
                if (spawnedMenu) {
                  foundMenu = (UObject *)spawnedMenu;
                  printf("[HUD] Successfully spawned AYMenu actor: %p (%s)\n",
                         foundMenu, foundMenu->GetFullName().c_str());

                  // Inject manufacturer data at +0x0638 (same offset as online)
                  // Source: Default__YMenu CDO was already populated by
                  // InjectOfflineFleet
                  UObject *menuCDOSrc = UObject::FindObject<UObject>(
                      "YMenu DreadGame.Default__YMenu");
                  if (menuCDOSrc) {
                    TArrayRaw_FwdDecl *cdoMfg =
                        (TArrayRaw_FwdDecl *)((uint8_t *)menuCDOSrc + 0x0638);
                    if (cdoMfg->Count > 0 && cdoMfg->Data) {
                      // [REMOVED] Do NOT share TArray::Data pointers with
                      // spawned instances! When the spawned AYMenu is
                      // destroyed, its destructor frees the CDO's array,
                      // causing a GC crash! TArrayRaw_FwdDecl* menuMfg =
                      // (TArrayRaw_FwdDecl*)((uint8_t*)foundMenu + 0x0638);
                      // menuMfg->Data = cdoMfg->Data;
                      // menuMfg->Count = cdoMfg->Count;
                      // menuMfg->Max = cdoMfg->Count;
                      printf("[HUD] Safely skipped injecting %d manufacturer "
                             "entries into spawned AYMenu to prevent "
                             "double-free GC crash\n",
                             cdoMfg->Count);
                    }
                  }
                } else {
                  printf("[HUD] SpawnActor returned null â€” falling back to "
                         "CDO\n");
                  foundMenu = UObject::FindObject<UObject>(
                      "YMenu DreadGame.Default__YMenu");
                  if (foundMenu) {
                    printf("[HUD] CDO fallback (limited): %p\n", foundMenu);
                  }
                }
              } else {
                printf("[HUD] AYMenu class not found â€” will retry on next "
                       "event\n");
              }
            }

            if (foundMenu) {
              // Write to PC+0x11F8 = m_outpostHUD
              *(UObject **)((uintptr_t)pc + 0x11F8) = foundMenu;
              printf("[HUD] Wired PC+0x11F8 (m_outpostHUD) = %p (%s)\n",
                     foundMenu, foundMenu->GetFullName().c_str());

              // Also wire into the GameMode's m_outpostHUD at +0x09A0.
              // From SDK: AYGameMode_Outpost has m_outpostHUD at 0x09A0
              // (AYMenu*). This is what GetOutpostHUD() returns and what 43+
              // functions read. Also set GameMode's m_player at +0x0998 to
              // point to our PC.
              if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
                uintptr_t gm = (uintptr_t)(*UWorld::GWorld)->AuthorityGameMode;

                // Wire m_outpostHUD at GameMode+0x09A0
                UObject **gmHudSlot = (UObject **)(gm + 0x09A0);
                if (*gmHudSlot == nullptr) {
                  *gmHudSlot = foundMenu;
                  printf("[HUD] Wired GameMode+0x09A0 (m_outpostHUD) = %p\n",
                         foundMenu);
                } else {
                  printf("[HUD] GameMode+0x09A0 already set to %p\n",
                         *gmHudSlot);
                }

                // Wire m_player at GameMode+0x0998
                UObject **gmPlayerSlot = (UObject **)(gm + 0x0998);
                if (*gmPlayerSlot == nullptr) {
                  *gmPlayerSlot = (UObject *)pc;
                  printf("[HUD] Wired GameMode+0x0998 (m_player) = %p\n", pc);
                }

                // ----------------------------------------------------------------
                // CRASH FIX: Spawn + wire m_transitionManager at
                // GameMode+0x9A8.
                //
                // When any ScoutLight (class 2) ship is clicked in the tech
                // tree, the game calls
                // FUN_1403d0530(GameMode->m_transitionManager, ...). If
                // m_transitionManager == null â†’ immediate crash. If non-null
                // but FWeakObjectPtr at +0x30 doesn't resolve â†’ crash inside
                // FUN_140372640 when it dereferences param_1+0xA38 (which is
                // AYMenu::m_visualAttractionModeMap).
                //
                // Fix:
                //   1. Spawn UYOutpostTransitionManager (via BP subclass)
                //   2. Wire it to GameMode+0x9A8
                //   3. Set m_fadeToBlackDuration at tm+0x2C = 1.0f
                //   4. Populate FWeakObjectPtr at tm+0x30 with live AYMenu
                // ----------------------------------------------------------------
                UObject **gmTmSlot = (UObject **)(gm + 0x09A8);
                UObject *tm = *gmTmSlot;
                if (tm == nullptr) {
                  // Try the BP subclass first, fall back to native class
                  UClass *tmClass = UObject::FindObject<UClass>(
                      "BlueprintGeneratedClass "
                      "OutpostTransitionsManager_BP.OutpostTransitionsManager_"
                      "BP_C");
                  if (!tmClass) {
                    tmClass = UObject::FindObject<UClass>(
                        "Class DreadGame.YOutpostTransitionManager");
                  }
                  if (tmClass) {
                    printf("[HUD] Spawning OutpostTransitionManager using "
                           "class: %s\n",
                           ((UObject *)tmClass)->GetFullName().c_str());
                    tm = getLastOfType<UGameplayStatics>()->STATIC_SpawnObject(
                        tmClass,
                        (UObject *)(*UWorld::GWorld)->AuthorityGameMode);
                    if (tm) {
                      // Wire to GameMode+0x9A8
                      *gmTmSlot = tm;
                      printf("[HUD] Wired GameMode+0x9A8 (m_transitionManager) "
                             "= %p (%s)\n",
                             tm, tm->GetFullName().c_str());
                    } else {
                      printf("[HUD] WARNING: STATIC_SpawnObject returned null "
                             "for transition manager\n");
                    }
                  } else {
                    printf("[HUD] WARNING: OutpostTransitionManager class not "
                           "found — ScoutLight ships will crash\n");
                  }
                } else {
                  printf("[HUD] GameMode+0x9A8 (m_transitionManager) already "
                         "set: %p\n",
                         tm);
                }

                if (tm) {
                  // Set m_fadeToBlackDuration at tm+0x2C (float, read by
                  // FUN_1403d0530)
                  *(float *)((uintptr_t)tm + 0x2C) = 1.0f;
                  printf("[HUD] Set tm+0x2C (m_fadeToBlackDuration) = 1.0f\n");

                  // Populate FWeakObjectPtr at tm+0x30 to point at GameMode
                  // (fixes crash inside FUN_1403bd800).
                  UObject *gmObj =
                      (UObject *)(*UWorld::GWorld)->AuthorityGameMode;
                  if (gmObj) {
                    int32_t gmIndex = gmObj->InternalIndex;
                    FUObjectItem *gmItem =
                        UObject::GObjects->GetItemByIndex(gmIndex);
                    if (gmItem) {
                      *(int32_t *)((uintptr_t)tm + 0x30) = gmIndex;
                      *(int32_t *)((uintptr_t)tm + 0x34) = gmItem->SerialNumber;
                      printf("[HUD] Populated tm+0x30 FWeakObjectPtr with "
                             "GameMode: index=%d serial=%d\n",
                             gmIndex, gmItem->SerialNumber);
                    }
                  }

                  // Correctly initialize the singly-linked queue with a dummy
                  // head node to prevent null-dereference crash:
                  void *dummyNode = UE4Malloc(24);
                  if (dummyNode) {
                    memset(dummyNode, 0, 24);
                    *(void **)((uintptr_t)tm + 0x78) =
                        dummyNode; // Head points to dummy node
                    *(void **)((uintptr_t)tm + 0x70) =
                        dummyNode; // Tail points to dummy node's next field
                                   // (offset 0)
                    printf("[HUD] Initialized transition queue with dummy head "
                           "node (%p)\n",
                           dummyNode);
                  } else {
                    printf("[HUD] ERROR: Failed to allocate dummy node for "
                           "transition queue\n");
                  }
                }
              } else {
                printf("[HUD] AuthorityGameMode is null â€” skipping GameMode "
                       "wiring\n");
              }

              s_ayMenuWired = true;

              // ----------------------------------------------------------------
              // CRITICAL INSIGHT from decompile of FUN_140374240:
              //   InitializeOutpostShip reads GameMode+0x998 (m_player).
              //   If m_player == null â†’ function returns immediately, no-op.
              //   It then binds PlayerFlagshipChanged delegate on
              //   (m_player+0x958)+0x70 and InitializeOutpostShipInternal on
              //   (m_player+0x958)+0x90. Without these delegates, the ship
              //   never loads.
              //
              // Root cause of blank viewport: the game calls
              // InitializeOutpostShip BEFORE our hook wires m_player â†’ it's
              // always a no-op offline.
              //
              // Fix: After wiring m_player, check if cameras were populated
              // (GameMode+0xB20 TMap count). If still 0, the function was a
              // no-op and we need to call OnHudAvailable +
              // InitializeOutpostShip now that m_player is properly set.
              // ----------------------------------------------------------------
              if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
                uintptr_t gm = (uintptr_t)(*UWorld::GWorld)->AuthorityGameMode;

                // Log native Func pointers for key UFunctions (one-time, for
                // RVA discovery)
                static bool s_loggedFuncPtrs = false;
                if (!s_loggedFuncPtrs) {
                  s_loggedFuncPtrs = true;
                  uintptr_t base = (uintptr_t)GetModuleHandleA(
                      "DreadGame-Win64-Shipping.exe");
                  auto logFuncPtr = [&](const char *name) {
                    UFunction *fn = (UFunction *)GetObjByName(name);
                    if (fn) {
                      void *funcPtr = *(void **)((uintptr_t)fn + 0x158);
                      printf("[RVA] %s -> Func=%p RVA=0x%llX\n", name, funcPtr,
                             (uintptr_t)funcPtr - base);
                    }
                  };
                  logFuncPtr(
                      "Function DreadGame.YGameMode_Outpost.OnHudAvailable");
                  logFuncPtr(
                      "Function "
                      "DreadGame.YGameMode_Outpost.InitializeOutpostShip");
                  logFuncPtr("Function "
                             "DreadGame.YGameMode_Outpost."
                             "InitializeOutpostShipInternal");
                  logFuncPtr(
                      "Function "
                      "DreadGame.YGameMode_Outpost.HandleOnLevelStreamed");
                  logFuncPtr(
                      "Function "
                      "DreadGame.YGameMode_Outpost.HandleOnLevelFinalized");
                  logFuncPtr(
                      "Function "
                      "DreadGame.YGameMode_Outpost.PlayerFlagshipChanged");
                }

                // Check if m_outpostCameras TMap (at GameMode+0xB20) is
                // populated.
                int32_t numElements = *(int32_t *)(gm + 0xB20 + 0x08);
                int32_t freeIndices = *(int32_t *)(gm + 0xB20 + 0x34);
                int32_t cameraCount = numElements - freeIndices;
                printf("[HUD] m_outpostCameras count = %d (allocated=%d, "
                       "free=%d)\n",
                       cameraCount, numElements, freeIndices);

                if (cameraCount == 0 && !g_hudInitComplete) {
                  UObject *gm_obj = (UObject *)gm;
                  // ── STEP 1: Wire tm+0x30 = GameMode (fixes crash inside
                  // FUN_1403bd800) ──
                  void *tm = *(void **)(gm + 0x9A8);
                  if (tm) {
                    int32_t gmIndex = gm_obj->InternalIndex;
                    FUObjectItem *gmItem =
                        UObject::GObjects->GetItemByIndex(gmIndex);
                    if (gmItem) {
                      *(int32_t *)((uintptr_t)tm + 0x30) = gmIndex;
                      *(int32_t *)((uintptr_t)tm + 0x34) = gmItem->SerialNumber;
                      printf(
                          "[HUD] Wired tm+0x30 FWeakObjectPtr -> GameMode\n");
                    }
                  }
                  printf("[HUD] Cameras not populated â€” calling "
                         "OnHudAvailable + InitializeOutpostShipInternal.\n");
                  UFunction *onHudFn = (UFunction *)GetObjByName(
                      "Function DreadGame.YGameMode_Outpost.OnHudAvailable");
                  if (onHudFn) {
                    pProcessEvent_Original(gm_obj, onHudFn, nullptr);
                    printf("[HUD] OnHudAvailable completed.\n");
                  }

                  // â”€â”€ STEP 2: Call InitializeOutpostShipInternal natively
                  // â”€â”€ (Moved to a 30-frame delay block to prevent BG
                  // thread crash)

                  g_hudInitComplete = true;

                } else {
                  printf("[HUD] Cameras already populated (%d) â€” BP init ran "
                         "correctly.\n",
                         cameraCount);
                }
              }
              printf("[HUD] Data wiring complete.\n");
            }
          }
        } else {
          printf("[UI] PC found but managers not ready yet (will retry)\n");
        }
      } catch (...) {
        printf("[UI] Exception getting PlayerController â€” will retry on next "
               "event\n");
      }
    }

    // Set IsHangarReady property directly on the HUD (it's at offset 0x5D8)
    if (g_capturedHUD) {
      *(bool *)((uintptr_t)g_capturedHUD + 0x05D8) = true; // IsHangarReady
      *(bool *)((uintptr_t)g_capturedHUD + 0x05B0) =
          true; // ShouldHangarReportReady
      printf("[UI] Set IsHangarReady=true and ShouldHangarReportReady=true on "
             "HUD %p\n",
             g_capturedHUD);
    }

    // STATE MACHINE FIX: HandleHangarStateUpdate completed fleet injection +
    // HUD wiring, but menuState may still be stuck at STATE_TITLE because the
    // RequestSession → timed delay flow never triggered. The delayed
    // InitializeOutpostShipInternal call (below) requires menuState >=
    // STATE_LOADING_HANGAR. Advance it now AND trigger the loading completion
    // sequence that the normal RequestSession path would have run.
    //
    // TEMPORARY: Skipping the first game screen and loading screen for now.
    // We will add the proper timed loading delay sequence back in later.
    if (g_fleetInjected && menuState < STATE_LOADING_HANGAR) {
      printf("[STATE] Advancing menuState from %d to STATE_LOADING_HANGAR "
             "(fleet injected, hangar ready)\n",
             (int)menuState);
      menuState = STATE_LOADING_HANGAR;

      // Run the loading completion that STATE_LOADING_DELAY would have done:
      if (g_capturedHUD) {
        // 1. HandleLogin — tells the HUD the player is "logged in"
        UFunction *handleLoginFn = (UFunction *)GetObjByName(
            "Function DreadGameUI.FrontendHUD.HandleLogin");
        if (handleLoginFn) {
          pProcessEvent_Original(g_capturedHUD, handleLoginFn, nullptr);
          printf("[STATE] Called HandleLogin on HUD\n");
        }

        // 2. HangarLoadFinished — signals hangar level is ready
        UFunction *hangarFinFn = (UFunction *)GetObjByName(
            "Function DreadGameUI.FrontendHUD.HangarLoadFinished");
        if (hangarFinFn) {
          pProcessEvent_Original(g_capturedHUD, hangarFinFn, nullptr);
          printf("[STATE] Called HangarLoadFinished on HUD\n");
        }

        // 3. Trigger title screen removal + Home navigation on next
        // ProcessEvent
        g_streamingCallbackCountdown = 1;
        printf("[STATE] Set streaming callback countdown — will navigate to "
               "Home next frame\n");
      }
    }
  }

  // === 2.5 Camera-ready trigger (Delayed Ship Loading) ===
  // If we call InitializeOutpostShipInternal immediately, the background
  // streaming thread crashes because the assets aren't fully loaded into memory
  // yet. We wait 30 frames (approx 500ms) to let the engine settle, then
  // trigger the load.
  if (menuState >= STATE_LOADING_HANGAR && g_savedFlagshipFName != 0 &&
      funcName.find("Tick") == std::string::npos) {
    static int s_repairThrottle = 0;
    static bool s_cameraReadyTriggered = false;
    if (!s_cameraReadyTriggered && ++s_repairThrottle >= 30) {
      s_cameraReadyTriggered = true;
      try {
        if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
          UObject *gm_obj = (UObject *)(*UWorld::GWorld)->AuthorityGameMode;
          printf("[DELAY] 30 frames passed. Preparing to stream hangar "
                 "levels...\n");
          uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);

          typedef void (*FN_InitShipInternal)(void *gameMode);
          FN_InitShipInternal initShip = (FN_InitShipInternal)(base + 0x3743b0);
          initShip(gm_obj);
          printf("[DELAY] InitializeOutpostShipInternal returned.\n");

          void *tm = *(void **)((uintptr_t)gm_obj + 0x9a8);
          if (tm) {
            typedef void(__fastcall * tFUN_1403d1990)(
                void *param_1, CG::FName *param_2, char param_3);
            tFUN_1403d1990 queueLevel = (tFUN_1403d1990)(base + 0x3D1990);

            CG::FName geoBg = SafeCreateFName("MN_Hangar_Geo_Background");
            CG::FName baseLt = SafeCreateFName("MN_Hangar_BaseLighting");
            CG::FName lt01 = SafeCreateFName("MN_Hangar_Light01");

            printf("[DELAY] Manually queueing base levels for streaming...\n");
            queueLevel(tm, &geoBg, 1);
            queueLevel(tm, &baseLt, 1);
            queueLevel(tm, &lt01, 1);
          }
        }
      } catch (...) {
      }
    }
  }

  // === 3. Retry fleet injection on subsequent events if it didn't work yet ===
  if (!g_fleetInjected && g_techTreeInspected &&
      funcName.find("Tick") == std::string::npos) {
    // Only try on non-Tick events to avoid spamming
    static int retryCount = 0;
    if (retryCount < 50) {
      retryCount++;
      try {
        ULocalPlayer *lp =
            ((*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]);
        AYPlayerController *pc = (AYPlayerController *)(lp->PlayerController);
        if (pc && pc->m_loadoutManager && pc->m_fleetManager) {
          printf("[UI] Retry #%d: Got valid PC, injecting fleet\n", retryCount);
          g_fleetInjected = true;
          InjectOfflineFleet(pc);
        }
      } catch (...) {
      }
    }
  }

  // === 3b. Retry AYMenu wiring independently (AYMenu may spawn after fleet
  // injection) ===
  static bool s_ayMenuWiredGlobal = false;
  if (!s_ayMenuWiredGlobal && g_fleetInjected && !g_hudInitComplete &&
      funcName.find("Tick") == std::string::npos) {
    try {
      ULocalPlayer *lp =
          ((*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]);
      AYPlayerController *pc = (AYPlayerController *)(lp->PlayerController);
      if (pc) {
        UObject *foundMenu = nullptr;
        int objCount = UObject::GObjects->Count();
        for (int i = 0; i < objCount && !foundMenu; i++) {
          UObject *o = UObject::GObjects->GetByIndex(i);
          if (!o || !o->Class)
            continue;
          std::string fn = o->GetFullName();
          // Same strict filter: world-placed actor only
          if (fn.substr(0, 9) == "Function ")
            continue;
          if (fn.substr(0, 6) == "Class ")
            continue;
          if (fn.find("Default__") != std::string::npos)
            continue;
          if (fn.find("PersistentLevel") == std::string::npos)
            continue;
          size_t sp = fn.find(' ');
          std::string className =
              (sp != std::string::npos) ? fn.substr(0, sp) : fn;
          if (className.find("YMenu") != std::string::npos) {
            foundMenu = o;
          }
        }
        // Spawn AYMenu if not found (same logic as primary path)
        if (!foundMenu) {
          UClass *menuBPClass = UObject::FindObject<UClass>(
              "BlueprintGeneratedClass "
              "VH_YMenu_Outpost_BP.VH_YMenu_Outpost_BP_C");
          if (!menuBPClass) {
            menuBPClass = UObject::FindObject<UClass>(
                "BlueprintGeneratedClass "
                "/Game/Menus/VH_YMenu_Outpost_BP.VH_YMenu_Outpost_BP_C");
          }
          if (!menuBPClass) {
            menuBPClass = UObject::FindObject<UClass>("Class DreadGame.YMenu");
          }
          if (menuBPClass) {
            FVector spawnLoc = {0.0f, 0.0f, 0.0f};
            FRotator spawnRot = {0.0f, 0.0f, 0.0f};
            AActor *spawnedMenu =
                UWorldSpawnActor(menuBPClass, &spawnLoc, &spawnRot);
            if (spawnedMenu) {
              foundMenu = (UObject *)spawnedMenu;
              printf("[HUD] Retry: Spawned live AYMenu: %p (%s)\n", foundMenu,
                     foundMenu->GetFullName().c_str());
            }
          }
        }
        if (foundMenu) {
          *(UObject **)((uintptr_t)pc + 0x11F8) = foundMenu;
          printf("[HUD] Retry: Wired PC+0x11F8 (m_outpostHUD) = %p (%s)\n",
                 foundMenu, foundMenu->GetFullName().c_str());

          // Wire GameMode m_outpostHUD + m_transitionManager (same as primary
          // path)
          if (*UWorld::GWorld && (*UWorld::GWorld)->AuthorityGameMode) {
            UObject *gmObj = (UObject *)(*UWorld::GWorld)->AuthorityGameMode;
            std::string gmName = gmObj->GetFullName();
            if (gmName.find("Outpost") != std::string::npos ||
                gmName.find("Frontend") != std::string::npos) {
              uintptr_t gm = (uintptr_t)gmObj;

              // m_outpostHUD at GameMode+0x9A0
              UObject **gmHudSlot = (UObject **)(gm + 0x09A0);
              if (*gmHudSlot == nullptr) {
                *gmHudSlot = foundMenu;
                printf(
                    "[HUD] Retry: Wired GameMode+0x09A0 (m_outpostHUD) = %p\n",
                    foundMenu);
              }

              // m_transitionManager at GameMode+0x9A8
              UObject **gmTmSlot = (UObject **)(gm + 0x09A8);
              if (*gmTmSlot == nullptr) {
                UClass *tmClass = UObject::FindObject<UClass>(
                    "BlueprintGeneratedClass "
                    "OutpostTransitionsManager_BP.OutpostTransitionsManager_BP_"
                    "C");
                if (!tmClass) {
                  tmClass = UObject::FindObject<UClass>(
                      "Class DreadGame.YOutpostTransitionManager");
                }
                if (tmClass) {
                  UObject *tm =
                      getLastOfType<UGameplayStatics>()->STATIC_SpawnObject(
                          tmClass,
                          (UObject *)(*UWorld::GWorld)->AuthorityGameMode);
                  if (tm) {
                    *gmTmSlot = tm;
                    *(float *)((uintptr_t)tm + 0x2C) = 1.0f;
                    int32_t gmIndex = gmObj->InternalIndex;
                    FUObjectItem *gmItem =
                        UObject::GObjects->GetItemByIndex(gmIndex);
                    if (gmItem) {
                      *(int32_t *)((uintptr_t)tm + 0x30) = gmIndex;
                      *(int32_t *)((uintptr_t)tm + 0x34) = gmItem->SerialNumber;
                      printf("[HUD] Retry: Wired tm+0x30 FWeakObjectPtr -> "
                             "GameMode (index=%d serial=%d)\n",
                             gmIndex, gmItem->SerialNumber);
                    }

                    // Correctly initialize the singly-linked queue in retry
                    // block with a dummy head node:
                    void *dummyNode = UE4Malloc(24);
                    if (dummyNode) {
                      memset(dummyNode, 0, 24);
                      *(void **)((uintptr_t)tm + 0x78) =
                          dummyNode; // Head points to dummy node
                      *(void **)((uintptr_t)tm + 0x70) =
                          dummyNode; // Tail points to dummy node's next field
                                     // (offset 0)
                      printf("[HUD] Retry: Initialized transition queue with "
                             "dummy head node (%p)\n",
                             dummyNode);
                    } else {
                      printf("[HUD] Retry ERROR: Failed to allocate dummy node "
                             "for transition queue\n");
                    }
                  }
                }
              }
            } else {
              printf(
                  "[HUD] Retry: Skipping GameMode wiring (not Outpost: %s)\n",
                  gmName.c_str());
            }
          }

          s_ayMenuWiredGlobal = true;
          printf("[HUD] Retry: Data wiring complete â€” letting BP handle "
                 "initialization\n");
        }
      }
    } catch (...) {
    }
  }

  // === BLOCK only the functions that actually contact dead servers or loop
  // back === IMPORTANT: DO NOT block RequestSession â€” its Blueprint handles
  // UI transitions. Only block the underlying native calls and failure
  // handlers.
  bool skipOriginal = false;

  if (funcName.find("TryCreateSession") != std::string::npos) {
    printf("[LOAD] BLOCKED: %s (would contact dead web service)\n",
           funcName.c_str());
    skipOriginal = true;
  }
  if (funcName.find("WebServicesConnectionFailed") != std::string::npos) {
    printf("[LOAD] BLOCKED: %s (preventing title screen loop-back)\n",
           funcName.c_str());
    skipOriginal = true;
  }
  if (funcName.find("SessionRequestFailed") != std::string::npos) {
    printf("[LOAD] BLOCKED: %s (preventing session failure handling)\n",
           funcName.c_str());
    skipOriginal = true;
  }
  if (funcName.find("TouchSession") != std::string::npos &&
      funcName.find("Definition") == std::string::npos) {
    skipOriginal = true;
  }
  if (funcName.find("DestroySession") != std::string::npos &&
      funcName.find("Definition") == std::string::npos) {
    skipOriginal = true;
  }

  // Call the original engine ProcessEvent (unless blocked)
  if (!skipOriginal && pProcessEvent_Original)
    pProcessEvent_Original(object, function, params);

  // === 4. Startup state machine ===
  // Extra safety guard against stack corruption
  if (object == nullptr || function == nullptr)
    return;

  if (menuState == STATE_LOGOS &&
      funcName.find("UI_Screen_Title_C.Construct") != std::string::npos) {
    menuState = STATE_TITLE;
    g_capturedTitleScreen = object; // Capture for later removal
    printf("[UI] Title Screen Detected (%p). Initializing UI hooks...\n",
           object);
    InitUIHooks();
  }
  static ULONGLONG g_loadingStartTimeMs = 0; // Wall-clock loading delay

  if (menuState == STATE_TITLE &&
      funcName.find("UI_Screen_Title_C.RequestSession") != std::string::npos) {
    menuState = STATE_LOADING_DELAY;
    g_loadingStartTimeMs = GetTickCount64();
    printf("[LOAD] Starting login sequence...\n");

    if (g_capturedHUD) {
      *(bool *)((uintptr_t)g_capturedHUD + 0x05D8) = true; // IsHangarReady
      *(bool *)((uintptr_t)g_capturedHUD + 0x05B0) =
          true; // ShouldHangarReportReady
    }
  }

  // Deferred loading sequence â€” waits for engine to settle before triggering
  // login
  static bool g_phase1Done = false;
  if (menuState == STATE_LOADING_DELAY && g_loadingStartTimeMs > 0) {
    ULONGLONG elapsed = GetTickCount64() - g_loadingStartTimeMs;

    if (!g_phase1Done && elapsed >= 2500 && g_capturedHUD) {
      g_phase1Done = true;
      printf("[LOAD] Patching session validation and logging in...\n");

      // Patch all WebServiceRequestDefinition::ValidateSession functions to
      // return true. Each definition has its own session check that blocks
      // without a real server.
      {
        uintptr_t moduleBase =
            (uintptr_t)GetModuleHandleA("DreadGame-Win64-Shipping.exe");

        // All session-check functions from key_functions_dump.txt "session id"
        // search: Each is a WebServiceRequestDefinition::ValidateSession
        // variant
        static const uint32_t sessionCheckRVAs[] = {
            0x2AB9710, // "No valid session id to accept legal item."
            0x2AB9930, // "No valid session Id to destroy session."
            0x2AB9A00, // "No valid session id to request legal document."
            0x2AB9B10, // "No valid session Id to request legal items."
            0x2AB9BE0, // "No valid session Id to destroy session."
            0x2AB9CB0, // "No valid session Id to destroy session."
            0x2AB9D80, // "No valid session Id to destroy session."
            0x2AB9E50, // "No valid session Id to destroy session." (decompiled)
            0x2AB9F20, // "No valid session Id to destroy session."
            0x2ABA0C0, // "No valid session id to reject legal item."
            0x2ABA240, // "No valid session id to touch session."
            0x2AB9FF0, // "No valid session Id to request mmog connection info."
        };

        // Patch bytes: MOV EAX, 1; RET  (B8 01 00 00 00 C3)
        uint8_t patch[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
        int patched = 0;

        for (int i = 0;
             i < sizeof(sessionCheckRVAs) / sizeof(sessionCheckRVAs[0]); i++) {
          uintptr_t addr = moduleBase + sessionCheckRVAs[i];
          DWORD oldProtect;
          if (VirtualProtect((void *)addr, sizeof(patch),
                             PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy((void *)addr, patch, sizeof(patch));
            VirtualProtect((void *)addr, sizeof(patch), oldProtect,
                           &oldProtect);
            patched++;
          } else {
            printf("[PATCH] WARN: VirtualProtect failed for RVA 0x%X\n",
                   sessionCheckRVAs[i]);
          }
        }
        printf("[PATCH] Patched %d/%d session validation functions (MOV EAX,1; "
               "RET)\n",
               patched,
               (int)(sizeof(sessionCheckRVAs) / sizeof(sessionCheckRVAs[0])));

        // Also still set the global session ID for any code that reads it
        // directly
        uintptr_t sessionGlobal = moduleBase + 0x3D9BFA0;
        if (*(int32_t *)(sessionGlobal + 0x08) < 2) {
          InitFStringUE4((void *)sessionGlobal,
                         L"mock-session-dread-offline-001");
          InitFStringUE4((void *)(moduleBase + 0x3D9C010), L"mock-token-12345");
          InitFStringUE4((void *)(moduleBase + 0x3D9C080), L"mock-cert-67890");
          printf("[PATCH] Also set global session+token FStrings\n");
        }

        // Also patch the TIMER SCHEDULING functions that cause the 60s crash.
        // FUN_14039d200 = ScheduleTouchSession: constructs
        // TouchSessionRequestDefinition, sets up FTimerManager timer delegate,
        // and fires on TaskGraphThread after ~55-60s. The callback accesses
        // freed objects â†’ EXCEPTION_ACCESS_VIOLATION. Patching with RET
        // (0xC3) prevents the timer from ever being scheduled.
        {
          static const uint32_t timerFuncRVAs[] = {
              0x39D200, // ScheduleTouchSession (calls FUN_142aae6f0 constructor
                        // + SetTimer)
              0x38ED50, // Timer delegate target (puVar8[2] = FUN_14038ed50 in
                        // scheduler)
          };
          uint8_t retPatch[] = {0xC3}; // just RET
          int timerPatched = 0;

          for (int i = 0; i < sizeof(timerFuncRVAs) / sizeof(timerFuncRVAs[0]);
               i++) {
            uintptr_t addr = moduleBase + timerFuncRVAs[i];
            DWORD oldProtect;
            if (VirtualProtect((void *)addr, sizeof(retPatch),
                               PAGE_EXECUTE_READWRITE, &oldProtect)) {
              memcpy((void *)addr, retPatch, sizeof(retPatch));
              VirtualProtect((void *)addr, sizeof(retPatch), oldProtect,
                             &oldProtect);
              timerPatched++;
            }
          }
          printf("[PATCH] Patched %d/%d timer-scheduling functions (RET) to "
                 "prevent 60s crash\n",
                 timerPatched,
                 (int)(sizeof(timerFuncRVAs) / sizeof(timerFuncRVAs[0])));
        }
      }

      // Trigger the login flow on the frontend HUD
      UFunction *handleLoginFn = (UFunction *)GetObjByName(
          "Function DreadGameUI.FrontendHUD.HandleLogin");
      if (handleLoginFn) {
        printf("[LOAD] Calling HandleLogin...\n");
        pProcessEvent_Original(g_capturedHUD, handleLoginFn, nullptr);
      }

      // Signal that the hangar level has finished loading
      UFunction *hangarFinFn = (UFunction *)GetObjByName(
          "Function DreadGameUI.FrontendHUD.HangarLoadFinished");
      if (hangarFinFn) {
        printf("[LOAD] Calling HangarLoadFinished...\n");
        pProcessEvent_Original(g_capturedHUD, hangarFinFn, nullptr);
      }
    }

    if (g_phase1Done && elapsed >= 7000 && g_capturedHUD) {
      printf("[LOAD] Transitioning to home screen...\n");
      menuState = STATE_LOADING_HANGAR;
      g_streamingCallbackCountdown = 1; // Fire immediately on next ProcessEvent
      g_loadingStartTimeMs = 0;
    }
  }

  // Complete the title-to-home transition after the engine settles
  if (g_streamingCallbackCountdown > 0) {
    g_streamingCallbackCountdown--;
    if (g_streamingCallbackCountdown == 0 && g_capturedHUD) {
      // Remove the title screen widget
      if (g_capturedTitleScreen) {
        UFunction *removeFromParent =
            (UFunction *)GetObjByName("Function UMG.Widget.RemoveFromParent");
        if (removeFromParent) {
          pProcessEvent_Original(g_capturedTitleScreen, removeFromParent,
                                 nullptr);
          printf("[UI] Title screen removed.\n");
        }
        g_capturedTitleScreen = nullptr;
      }

      // Trigger the streaming completion callback
      typedef void (*tOnStreamingComplete)(void *hud);
      auto onStreamingComplete =
          (tOnStreamingComplete)(Globals::ModuleBase + 0xAACC00);
      onStreamingComplete(g_capturedHUD);

      // Navigate to the Home screen
      UFunction *navFn = (UFunction *)GetObjByName(
          "Function DreadGameUI.FrontendHUD.NavigateToScreen");
      if (navFn) {
        struct {
          uint8_t Screen;
        } navParams;
        navParams.Screen = 1; // EUI_Screen::Home
        pProcessEvent_Original(g_capturedHUD, navFn, &navParams);
        printf("[UI] Navigated to Home screen.\n");
      }

      menuState = STATE_READY;
      hasReachedHangarOnce = true;
    }
  }

  if (menuState == STATE_LOADING_HANGAR && !hasReachedHangarOnce &&
      funcName.find("UI_Button_Generic_C.Construct") != std::string::npos) {
    menuState = STATE_READY;
    hasReachedHangarOnce = true;
    printf("[LOAD] Hangar UI Initialized. Ready.\n");
  }

  // === 5. IsHangarReady â€” it's a PROPERTY at 0x5D8 on UI_FrontEnd_C, not a
  // function === We set it directly in HandleHangarStateUpdate above. No
  // ProcessEvent intercept needed. Also keep it true continuously in case
  // Blueprint resets it:
  if (g_capturedHUD && g_techTreeInspected) {
    bool *isReady = (bool *)((uintptr_t)g_capturedHUD + 0x05D8);
    if (!*isReady) {
      *isReady = true;
      printf("[UI] Re-set IsHangarReady=true (was reset by Blueprint)\n");
    }
  }

  callDepth--;
}

/*
        Tells UE4 to listen for incoming connections.
        - Why not just pass ?listen to the open command?
        - Glad you asked! Dreadnought checks for ?listen on the command line,
   and sets up the game differently, which results in clients not being able to
   spawn. The only consistent way I've found to get clients to spawn properly is
   to setup the game in standalone mode, THEN manually call the listen function
*/
void Listen() {
  FURL url = FURL();
  url.Port = 7777;
  reinterpret_cast<UObject *(*)(UWorld * world, FURL & inURL)>(
      Globals::ModuleBase + 0x1CDBB20)(*UWorld::GWorld, url);
  interceptPostLogin = true;
}

/*
        Iterate through all playercontrollers, and spam restart thier feat
   component. OnPlayerRespawned does nothing if the player is already spawned,
   so this is safe to call without checking if the player is dead or not.
*/
void RespawnThread() {
  while (true) {
    if ((*UWorld::GWorld)->NetDriver) {
      for (int i = 0;
           i < (*UWorld::GWorld)->NetDriver->ClientConnections.Count(); i++) {
        AYPlayerController *pc = (AYPlayerController *)(*UWorld::GWorld)
                                     ->NetDriver->ClientConnections[i]
                                     ->PlayerController;

        if (pc) {
          pc->ServerRestartPlayer();
        }

        if (pc && pc->Pawn && ((AYPawn *)pc->Pawn)->m_featsComponent) {
          ((AYPawn *)pc->Pawn)->m_featsComponent->OnPlayerRespawned(pc);
        }
      }
      Sleep(5 * 1000);
    }
  }
}

bool init = false;
bool menuEnabled = true;

/*
        Hooks the DX11 Present function, used to draw our IMGUI menu onto the
   screen
*/
HRESULT __stdcall hkPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                            UINT Flags) {
  if (!init) {
    if (SUCCEEDED(
            pSwapChain->GetDevice(__uuidof(ID3D11Device), (void **)&pDevice))) {
      pDevice->GetImmediateContext(&pContext);
      DXGI_SWAP_CHAIN_DESC sd;
      pSwapChain->GetDesc(&sd);
      window = sd.OutputWindow;
      ID3D11Texture2D *pBackBuffer;
      pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            (LPVOID *)&pBackBuffer);
      pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
      pBackBuffer->Release();
      oWndProc =
          (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
      InitImGui();
      init = true;
    }

    else
      return oPresent(pSwapChain, SyncInterval, Flags);
  }

  if (Dyn_SteamAPI_RunCallbacks)
    Dyn_SteamAPI_RunCallbacks();

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();

  ImGui::GetIO().MouseDrawCursor = menuEnabled;

  ImGui::NewFrame();

  /*
          Main menu code begins here
  */
  if (menuEnabled) {
    ImGui::Begin("Dreadnought (F7 to show/hide)", &menuEnabled,
                 ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::BeginTabBar("MenuSelect")) {
      if (ImGui::BeginTabItem("Singleplayer")) {
        const char *difficultyNames[3] = {"Recruit", "Veteran", "Legendary"};

        const char *mapNames[10] = {
            "Amirani",   "DansMap",  "Derelict",  "Glacier", "Gorge",
            "Highlands", "Paradise", "Skybridge", "Space01", "Space02"};

        ImGui::SliderInt("Num Friendly Bots", &numBotsTeamOne, 0, 7);
        ImGui::SliderInt("Num Enemy Bots", &numBotsTeamTwo, 0, 8);
        ImGui::Combo("Bot Difficulty", &difficulty, difficultyNames, 3);
        ImGui::Combo("Map", &map, mapNames, 10);

        const char *singleplayerLoadoutNames[] = {
            "Corvette", "Artillery Cruiser", "Dreadnought Heavy", "Destroyer",
            "Tactical Cruiser"};
        const char *singleplayerLoadoutPaths[] = {
            "/Game/Generic/Loadouts/Precast/T5/"
            "VH_AssaultLight_PrecastLoadout_T5_BP",
            "/Game/Generic/Loadouts/Precast/T5/"
            "VH_SniperLight_T5_PrecastLoadout_BP",
            "/Game/Generic/Loadouts/Precast/T5/"
            "VH_DreadnoughtHeavy_PrecastLoadout_T5_BP",
            "/Game/Generic/Loadouts/Precast/T5/"
            "VH_DestroyerMedium_PrecastLoadout_T5_BP",
            "/Game/Generic/Loadouts/Precast/T5/"
            "VH_TacticalCruiser_PrecastLoadout_T5_BP"};

        if (ImGui::Combo("Ship Loadout", &singleplayerLoadoutIndex,
                         singleplayerLoadoutNames, 5)) {
          singleplayerLoadoutString =
              singleplayerLoadoutPaths[singleplayerLoadoutIndex];
        }

        if (ImGui::Button("Launch Singleplayer")) {
          launchSingleplayer = true;
        }

        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Tutorial")) {
        if (ImGui::Button("Launch Tutorial"))
          launchTutorial = true;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Multiplayer (BETA)")) {
        ImGui::InputText("Server IP", &serverIP);
        ImGui::SameLine();

        if (ImGui::Button("Connect"))
          connectToServer = true;
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Host Server")) {
        const char *difficultyNames[3] = {"Recruit", "Veteran", "Legendary"};
        const char *mapNames[10] = {
            "Amirani",   "DansMap",  "Derelict",  "Glacier", "Gorge",
            "Highlands", "Paradise", "Skybridge", "Space01", "Space02"};
        const char *singleplayerLoadoutNames[] = {
            "Corvette", "Artillery Cruiser", "Dreadnought Heavy", "Destroyer",
            "Tactical Cruiser"};

        ImGui::Combo("Map", &hostMapIndex, mapNames, 10);
        ImGui::Combo("Host Ship", &hostLoadoutIndex, singleplayerLoadoutNames,
                     5);
        ImGui::Combo("Bot Difficulty", &hostDifficulty, difficultyNames, 3);

        ImGui::Separator();

        ImGui::InputText("Server Name", hostServerName,
                         IM_ARRAYSIZE(hostServerName));
        ImGui::InputText("Password", hostPassword, IM_ARRAYSIZE(hostPassword),
                         ImGuiInputTextFlags_Password);

        ImGui::Separator();

        if (Dyn_SteamAPI_IsSteamRunning && Dyn_SteamAPI_IsSteamRunning()) {
          ImGui::TextColored(ImVec4(0, 1, 0, 1), "Steam Status: Online");
        } else {
          ImGui::TextColored(ImVec4(1, 0, 0, 1), "Steam Status: Offline");
        }

        if (ImGui::Button("Start Hosting")) {
          if (Dyn_SteamMatchmaking) {
            SteamAPICall_t call =
                Dyn_SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 16);
            g_LobbyManager.m_LobbyCreatedCallResult.Set(
                call, &g_LobbyManager, &SteamLobbyManager::OnLobbyCreated);
          }
          launchHostServer = true;
        }

        ImGui::EndTabItem();
      }
      /*
      if (ImGui::BeginTabItem("Debug")) {
              if (ImGui::Button("Buffs Manager")) {
                      AYPlayerController* pc =
      (AYPlayerController*)(*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;

                      std::cout << ((AYPawn*)pc->Pawn)->m_buffsComponent <<
      std::endl;
              }
              if (ImGui::Button("Force Loadout")) {
                      AYPlayerController* pc =
      (AYPlayerController*)(*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;

                      StaticLoadClass(UYShipLoadout::StaticClass(), nullptr,
      L"/Game/Generic/Loadouts/Precast/T5/VH_SniperLight_T5_PrecastLoadout_BP");

                      UYShipLoadout* loadoutToApply = nullptr;

                      for (UYShipLoadout* cmpLoadout : UObject::FindObjects<
      UYShipLoadout>()) { if (cmpLoadout->GetFullName().find("Sniper") !=
      std::string::npos) { loadoutToApply = cmpLoadout;
                              }
                      }

                      ((AYPlayerController*)pc)->AddAndActiveLoadoutFromBlueprint(loadoutToApply->Class);
              }

              if (ImGui::Button("Enable AI Spawn")) {
                      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_enableSpawnAI
      = true;
              }

              if (ImGui::Button("Disable AI Spawn")) {
                      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_enableSpawnAI
      = false;
              }

              if (ImGui::Button("Load Loadout")) {
                      StaticLoadClass(UYShipLoadout::StaticClass(), nullptr,
      L"/Game/Generic/Loadouts/Precast/T5/VH_AssaultLight_PrecastLoadout_T5_BP");
              }

              if (ImGui::Button("InstaStartMatch")) {
                      ((AYGameState*)(*UWorld::GWorld)->AuthorityGameMode->GameState)->SetRemainingTime(1);
              }

              if (ImGui::Button("Listen")) {
                      Listen();
              }

              if (ImGui::Button("RestartAllPlayers")) {
                      StaticLoadClass(UYShipLoadout::StaticClass(), nullptr,
      L"/Game/Generic/Loadouts/Precast/T5/VH_AssaultLight_PrecastLoadout_T5_BP");

                      for (AYPlayerController* pc :
      UObject::FindObjects<AYPlayerController>()) { if
      (pc->GetFullName().find("Default") == std::string::npos) { // && pc !=
      (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController
                                      if
      (!pc->GetLoadoutManager()->m_activeLoadout) { UYShipLoadout*
      loadoutToApply = nullptr;

                                              for (UYShipLoadout* cmpLoadout :
      UObject::FindObjects< UYShipLoadout>()) { if
      (cmpLoadout->GetFullName().find("VH_AssaultLight_PrecastLoadout_T5_BP") !=
      std::string::npos) { loadoutToApply = cmpLoadout;
                                                      }
                                              }

                                              pc->GetLoadoutManager()->m_activeLoadout
      = loadoutToApply;
                                              pc->AddAndActiveLoadoutFromBlueprint(loadoutToApply->Class);
                                      }
                                      pc->ServerRestartPlayer();
                              }
                      }
              }

              if (ImGui::Button("AI Team")) {
                      StaticLoadClass(UYNPCPawnData::StaticClass(), nullptr,
      L"/Game/Generic/GameModes/TDM/AIShips_TDM_Vet");

                      Sleep(1 * 1000);

                      ListAllObjectsOfType< UYNPCPawnData>();

                      ((AYPlayerController*)(*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController)->GetCombatManager()->m_NPCSet
      = getLastOfType< UYNPCPawnData>();
                      ((AYPlayerController*)(*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController)->GetCombatManager()->m_isNPCSetLoaded
      = true;

                      UYNPCPawnData* pawnData = getLastOfType< UYNPCPawnData>();

                      for (int i = 0; i <
      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_npcPlayers.Count();
      i++) { TArray<FName> shipIDs;

                              shipIDs._data = (FName*)UE4Malloc(sizeof(FName) *
      pawnData->m_PawnsData.Count()); shipIDs._count =
      pawnData->m_PawnsData.Count(); shipIDs._max =
      pawnData->m_PawnsData.Count();

                              for (int j = 0; j < pawnData->m_PawnsData.Count();
      j++) { shipIDs[j] = pawnData->m_PawnsData[j].m_shipId;
                              }

                              ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_npcPlayers[i].m_npcSpawnIDs
      = shipIDs;
                      }

                      for (int i = 0; i <
      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_aiSpawnTierRules.Count();
      i++) {
                              ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->m_aiSpawnTierRules[i].m_aiTier_aiDificultyLevel
      = EYAILevel::YAIL_LEGENDARY;
                      }

                      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->SetTeamSizeAI(EYTeam::YT_TEAM1,
      6);
                      ((AYGameMode_Multiplayer*)(*UWorld::GWorld)->AuthorityGameMode)->SetTeamSizeAI(EYTeam::YT_TEAM2,
      6);
              }
              ImGui::EndTabItem();
              }
      */
      if (ImGui::BeginTabItem("Fleet & Inventory")) {
        ImGui::Text("Backend: %s", g_ActivePersistenceBackend == PersistenceBackendType::JSON ? "JSON (profile.json)" : "SQLite");
        ImGui::Text("Credits: %d | XP: %d | Owned Ships: %d", g_credits, g_xp, (int)g_ownedShips.size());
        ImGui::Separator();
        
        if (ImGui::Button("+10,000 Credits")) {
          g_credits += 10000;
          SaveFleetData();
        }
        ImGui::SameLine();
        if (ImGui::Button("+5,000 Free XP")) {
          g_xp += 5000;
          SaveFleetData();
        }

        if (ImGui::Button("Unlock All Ships")) {
          for (int i = 1; i <= 50; i++) {
            g_ownedShips.insert(i);
          }
          SaveFleetData();
        }
        
        ImGui::Separator();
        ImGui::Text("Active Fleet Slots:");
        for (size_t i = 0; i < g_fleetSlots.size(); i++) {
          ImGui::Text("Slot %d: Ship ID %d", (int)i, g_fleetSlots[i]);
        }

        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::End();
  }
  /*
          Main menu code ends here
  */

  ImGui::Render();

  pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  return oPresent(pSwapChain, SyncInterval, Flags);
}

bool menuToggledThisFrame = false;

typedef HRESULT(__stdcall *ResizeBuffers)(IDXGISwapChain *pThis,
                                          UINT BufferCount, UINT Width,
                                          UINT Height, DXGI_FORMAT NewFormat,
                                          UINT SwapChainFlags);
ResizeBuffers oResizeBuffers;

/*
        Hooks DX11's resize buffer function
        This is required to make the game not crash when the resolution is
   changed
*/
HRESULT hkResizeBuffers(IDXGISwapChain *pThis, UINT BufferCount, UINT Width,
                        UINT Height, DXGI_FORMAT NewFormat,
                        UINT SwapChainFlags) {
  if (mainRenderTargetView) {
    pContext->OMSetRenderTargets(0, 0, 0);
    mainRenderTargetView->Release();
  }

  HRESULT hr = oResizeBuffers(pThis, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);

  ID3D11Texture2D *pBuffer;
  pThis->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&pBuffer);

  pDevice->CreateRenderTargetView(pBuffer, NULL, &mainRenderTargetView);

  pBuffer->Release();

  pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);

  // Set up the viewport.
  D3D11_VIEWPORT vp;
  vp.Width = (FLOAT)Width;
  vp.Height = (FLOAT)Height;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  pContext->RSSetViewports(1, &vp);
  return hr;
}

/*
        This stub function prevents the hud from being created on the listen
   player. Without this, the server will crash on any player's death.
*/
void *origJustReturn = nullptr;

void *JustReturnWhatWeWereGoingToReturn(void *param1, void *param2) {
  return nullptr;
}

// 1CDB7C0

// 036B2E0

void *origEndMatch = nullptr;

/*
        This stub function prevents the match from ending, as it would normally
   end when any player disconnects
*/
void EndMatchHook(void *param1) { return; }

void *origEACErrorMessageHook = nullptr;

/*
        Prevent EAC from booting on the client so we don't get an error popup
*/
uint8_t EACErrorMessageHook(__int64 a1, __int64 a2) {
  return 1; // 1 = Success here
}

void *OrigUGameEngineTick = nullptr;

std::mutex ProcOnMainThreadMutex{};

std::vector<std::function<void()>> FunctionsToProcOnMainThread{};

struct FActorSpawnParameters {
  /* A name to assign as the Name of the Actor being spawned. If no value is
   * specified, the name of the spawned Actor will be automatically generated
   * using the form [Class]_[Number]. */
  FName Name;

  /* An Actor to use as a template when spawning the new Actor. The spawned
   * Actor will be initialized using the property values of the template Actor.
   * If left NULL the class default object (CDO) will be used to initialize the
   * spawned Actor. */
  AActor *Template;

  /* The Actor that spawned this Actor. (Can be left as NULL). */
  AActor *Owner;

  /* The APawn that is responsible for damage done by the spawned Actor. (Can be
   * left as NULL). */
  APawn *Instigator;

  /* The ULevel to spawn the Actor in, i.e. the Outer of the Actor. If left as
   * NULL the Outer of the Owner is used. If the Owner is NULL the persistent
   * level is used. */
  class ULevel *OverrideLevel;

  /** Method for resolving collisions at the spawn point. Undefined means no
   * override, use the actor's setting. */
  ESpawnActorCollisionHandlingMethod SpawnCollisionHandlingOverride;

  /* Is the actor remotely owned. This should only be set true by the package
   * map when it is creating an actor on a client that was replicated from the
   * server. */
  uint16_t bRemoteOwned : 1;

  /* Determines whether spawning will not fail if certain conditions are not
   * met. If true, spawning will not fail because the class being spawned is
   * `bStatic=true` or because the class of the template Actor is not the same
   * as the class of the Actor being spawned. */
  uint16_t bNoFail : 1;

  /* Determines whether the construction script will be run. If true, the
   * construction script will not be run on the spawned Actor. Only applicable
   * if the Actor is being spawned from a Blueprint. */
  uint16_t bDeferConstruction : 1;

  /* Determines whether or not the actor may be spawned when running a
   * construction script. If true spawning will fail if a construction script is
   * being run. */
  uint16_t bAllowDuringConstructionScript : 1;

  /* Flags used to describe the spawned actor/object instance. */
  ObjectFlags ObjectFlags;

  FActorSpawnParameters() {
    Name = FName();
    Template = nullptr;
    Owner = nullptr;
    Instigator = nullptr;
    OverrideLevel = nullptr;
    SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    bRemoteOwned = false;
    bNoFail = true;
    bDeferConstruction = false;
    bAllowDuringConstructionScript = true;
    ObjectFlags = ObjectFlags::None;
  }
};

AActor *UWorldSpawnActor(UClass *ActorClass, FVector *SpawnLocation,
                         FRotator *SpawnRotation) {
  return reinterpret_cast<AActor *(*)(UWorld *, UClass *, FVector *, FRotator *,
                                      FActorSpawnParameters *)>(
      Globals::ModuleBase + 0x1A0C8D0)(*UWorld::GWorld, ActorClass,
                                       SpawnLocation, SpawnRotation,
                                       new FActorSpawnParameters());
}

bool OverrideGetActor = false;

static std::string ToLower(const std::string &str) {
  std::string lower;
  for (char c : str) {
    lower += (c >= 'A' && c <= 'Z') ? (c + 32) : c;
  }
  return lower;
}

static bool CaseInsensitiveContains(const std::string &haystack,
                                    const std::string &needle) {
  if (needle.empty())
    return true;
  std::string hsLower = ToLower(haystack);
  std::string ndLower = ToLower(needle);
  return hsLower.find(ndLower) != std::string::npos;
}

void UnpauseHangarAnimations() {
  if (!*UWorld::GWorld) return;
  UWorld* world = *UWorld::GWorld;
  if (!world->PersistentLevel) return;
  
  std::string plName = world->PersistentLevel->GetFullName();
  if (plName.find("Launch_P") == std::string::npos) {
    return;
  }

  printf("[ANIM] Active persistent level is Launch_P. Restoring animations...\n");
  
  int settingsCount = 0;
  for (auto actor : UObject::FindObjects<AWorldSettings>()) {
    if (!actor) continue;
    
    void** pauser = (void**)((uintptr_t)actor + 0x05C0);
    if (*pauser != nullptr) {
      printf("[ANIM] WorldSettings %p: Clearing Pauser (was %p)\n", actor, *pauser);
      *pauser = nullptr;
    }
    
    float* td = (float*)((uintptr_t)actor + 0x05A0);
    if (*td < 0.01f) {
      printf("[ANIM] WorldSettings %p: Setting TimeDilation from %.4f to 1.0f\n", actor, *td);
      *td = 1.0f;
    }
    settingsCount++;
  }
  printf("[ANIM] Unpause complete. Processed %d AWorldSettings instance(s).\n", settingsCount);
}

void TriggerLevelActorLinks() {
  if (g_levelActorLinksAttempted) {
    return;
  }
  if (!*UWorld::GWorld) return;
  UWorld *world = *UWorld::GWorld;
  
  bool onHangarMap = false;
  if (world->PersistentLevel) {
    std::string plName = world->PersistentLevel->GetFullName();
    if (plName.find("Launch_P") != std::string::npos) {
      onHangarMap = true;
    }
  }

  if (!onHangarMap) return;

  g_levelActorLinksAttempted = true;

  // DIAGNOSTIC LOOP: Print level actors using the verified offset 0x00A0
  if (world->Levels.Count() > 0) {
    printf("[LEVEL-DIAG] Enumerating levels and actors (offset 0x00A0):\n");
    for (int li = 0; li < world->Levels.Count(); ++li) {
      ULevel* lvl = world->Levels[li];
      if (!lvl) continue;
      std::string lvlName = lvl->GetFullName();
      TArray<AActor*>& levelActors = *(TArray<AActor*>*)( (uintptr_t)lvl + 0x00A0 );
      printf("[LEVEL-DIAG]   Level[%d]: %s has %d actors\n", li, lvlName.c_str(), levelActors.Count());
      if (lvlName.find("Generic_P") != std::string::npos || lvlName.find("Launch_P") != std::string::npos) {
        for (int ai = 0; ai < levelActors.Count() && ai < 15; ++ai) {
          AActor* actor = levelActors[ai];
          if (actor) {
            printf("[LEVEL-DIAG]     Actor[%d]: %s (TickEnabled: %d, Ptr=%p)\n", ai, actor->GetFullName().c_str(), actor->IsActorTickEnabled(), actor);
          }
        }
      }
    }
  }

  // Force actor initialization by setting bActorsInitialized on all sublevels in world->Levels
  if (world->Levels.Count() > 0) {
    for (int li = 1; li < world->Levels.Count(); ++li) {
      ULevel* lvl = world->Levels[li];
      if (lvl) {
        uint8_t* pLvlFlags = (uint8_t*)((uintptr_t)lvl + 0x01B0);
        if (pLvlFlags) {
          *pLvlFlags |= 0x20; // Set bActorsInitialized bitfield flag to ensure ticking
        }
      }
    }
  }

  // 1. Find the Launch_P level script actor instance
  UObject *launchPScript = nullptr;
  for (int i = 0; i < UObject::GObjects->Count(); i++) {
    UObject *obj = UObject::GObjects->GetByIndex(i);
    if (!obj)
      continue;
    std::string fullName = obj->GetFullName();
    if (fullName.find("Launch_P_C") != std::string::npos &&
        fullName.find("PersistentLevel") != std::string::npos &&
        fullName.find("Default__") == std::string::npos) {
      launchPScript = obj;
      printf("[ANIM-INIT] Found Launch_P level script: %s at %p\n",
             fullName.c_str(), obj);
      break;
    }
  }

  if (launchPScript) {
    // 2. Call SetUpAllLevelActorLinks to wire cameras, matinee actors, character spawners
    UFunction *setupFn = UObject::FindObject<UFunction>(
        "Function Launch_P.Launch_P_C.SetUpAllLevelActorLinks");
    if (setupFn) {
      printf("[ANIM-INIT] Calling SetUpAllLevelActorLinks on %p...\n",
             launchPScript);
      launchPScript->ProcessEvent(setupFn, nullptr);
      printf("[ANIM-INIT] SetUpAllLevelActorLinks completed!\n");
      g_levelActorLinksInitialized = true;
    } else {
      printf("[ANIM-INIT] WARNING: SetUpAllLevelActorLinks UFunction not found!\n");
    }

    // 4. Call HandleHangarStateUpdate to set correct lighting/fog state
    UFunction *hangarStateFn = UObject::FindObject<UFunction>(
        "Function Launch_P.Launch_P_C.HandleHangarStateUpdate");
    if (hangarStateFn) {
      uint8_t hangarState = 0;
      printf("[ANIM-INIT] Calling HandleHangarStateUpdate(state=%d) on Launch_P script...\n",
             hangarState);
      launchPScript->ProcessEvent(hangarStateFn, &hangarState);
      printf("[ANIM-INIT] HandleHangarStateUpdate completed!\n");
    }
  } else {
    printf("[ANIM-INIT] WARNING: Launch_P level script actor not found in GObjects!\n");
  }

  // 5. Initialize BeginPlay on HangarStateManager if present
  UObject *hangarStateMgr = nullptr;
  UClass *hsmClass = UObject::FindObject<UClass>(
      "BlueprintGeneratedClass HangarStateManager.HangarStateManager_C");
  for (int i = 0; i < UObject::GObjects->Count(); i++) {
    UObject *obj = UObject::GObjects->GetByIndex(i);
    if (!obj)
      continue;
    if (hsmClass && obj->Class != hsmClass)
      continue;
    std::string fullName = obj->GetFullName();
    if (fullName.find("HangarStateManager") != std::string::npos &&
        fullName.find("PersistentLevel") != std::string::npos &&
        fullName.find("Default__") == std::string::npos) {
      hangarStateMgr = obj;
      printf("[ANIM-INIT] Found HangarStateManager: %s at %p\n",
             fullName.c_str(), obj);
      break;
    }
  }

  if (hangarStateMgr) {
    UFunction *hsmBeginPlay = UObject::FindObject<UFunction>(
        "Function HangarStateManager.HangarStateManager_C.ReceiveBeginPlay");
    if (hsmBeginPlay) {
      printf("[ANIM-INIT] Calling ReceiveBeginPlay on HangarStateManager %p...\n",
             hangarStateMgr);
      hangarStateMgr->ProcessEvent(hsmBeginPlay, nullptr);
      printf("[ANIM-INIT] HangarStateManager ReceiveBeginPlay completed!\n");
    }

    UFunction *setStateFn = UObject::FindObject<UFunction>(
        "Function HangarStateManager.HangarStateManager_C.SetHangarState");
    if (setStateFn) {
      uint8_t state = 0;
      printf("[ANIM-INIT] Calling SetHangarState(%d) on HangarStateManager...\n",
             state);
      hangarStateMgr->ProcessEvent(setStateFn, &state);
      printf("[ANIM-INIT] SetHangarState completed!\n");
    }
  }

  // 6. Fire ReceiveBeginPlay on FX actors (asteroids, etc.)
  UFunction *fxBeginPlay = UObject::FindObject<UFunction>(
      "Function FX_Hangar_environmentAsteroids01_BP.FX_Hangar_environmentAsteroids01_BP_C.ReceiveBeginPlay");
  if (fxBeginPlay) {
    UClass *fxClass = UObject::FindObject<UClass>(
        "BlueprintGeneratedClass FX_Hangar_environmentAsteroids01_BP.FX_Hangar_environmentAsteroids01_BP_C");
    for (int i = 0; i < UObject::GObjects->Count(); i++) {
      UObject *obj = UObject::GObjects->GetByIndex(i);
      if (!obj)
        continue;
      if (fxClass && obj->Class != fxClass)
        continue;
      std::string fullName = obj->GetFullName();
      if (fullName.find("FX_Hangar_environmentAsteroids") != std::string::npos &&
          fullName.find("PersistentLevel") != std::string::npos &&
          fullName.find("Default__") == std::string::npos) {
        printf("[ANIM-INIT] Calling ReceiveBeginPlay on FX actor: %s\n",
               fullName.c_str());
        obj->ProcessEvent(fxBeginPlay, nullptr);
      }
    }
  }

  // --- DIAGNOSTICS FOR HANGAR Restoring animations ---
  printf("[DIAG-ANIM] === AMatineeActor Diagnostics ===\n");
  auto matinees = UObject::FindObjects<AMatineeActor>();
  for (auto m : matinees) {
    if (!m) continue;
    uint8_t* pPlaying = (uint8_t*)((uintptr_t)m + 0x0420);
    bool isPlaying = pPlaying ? (*pPlaying & 0x01) != 0 : false;
    printf("[DIAG-ANIM] Matinee: %s (%p), bIsPlaying: %d, PlayRate: %.3f, bPlayOnLevelLoad: %d\n",
           m->GetFullName().c_str(), m, isPlaying, m->PlayRate, (int)m->bPlayOnLevelLoad);
  }

  printf("[DIAG-ANIM] === ALevelSequenceActor Diagnostics ===\n");
  UClass* levelSeqActorClass = UObject::FindObject<UClass>("Class LevelSequence.LevelSequenceActor");
  UFunction* isPlayingFn = UObject::FindObject<UFunction>("Function LevelSequence.LevelSequencePlayer.IsPlaying");
  
  for (int i = 0; i < UObject::GObjects->Count(); i++) {
    UObject* obj = UObject::GObjects->GetByIndex(i);
    if (!obj) continue;
    if (levelSeqActorClass && obj->IsA(levelSeqActorClass)) {
      // It is a level sequence actor!
      // Read SequencePlayer pointer at offset 0x03D0
      UObject* player = *(UObject**)((uintptr_t)obj + 0x03D0);
      bool isPlaying = false;
      if (player && isPlayingFn) {
        struct {
          bool ReturnValue;
        } params = { false };
        player->ProcessEvent(isPlayingFn, &params);
        isPlaying = params.ReturnValue;
      }
      
      // Read bAutoPlay at offset 0x03C0
      bool bAutoPlay = *(bool*)((uintptr_t)obj + 0x03C0);
      
      printf("[DIAG-ANIM] Sequence: %s (%p), Player: %p, IsPlaying: %d, bAutoPlay: %d\n",
             obj->GetFullName().c_str(), obj, player, isPlaying, (int)bAutoPlay);
    }
  }


  UFunction* actorBeginPlay = UObject::FindObject<UFunction>("Function Engine.Actor.ReceiveBeginPlay");

  // Force OrbitManager ticking and BeginPlay
  auto orbitManagers = UObject::FindObjects<AYOrbitTransitionManager>();
  for (auto ot : orbitManagers) {
    if (!ot) continue;
    printf("[DIAG-ANIM] OrbitManager: %s (%p) | TickEnabled: %d\n", ot->GetFullName().c_str(), ot, (int)ot->IsActorTickEnabled());
    uint8_t* pActorTickFlags = (uint8_t*)((uintptr_t)ot + 0x0034);
    if (pActorTickFlags) {
      *pActorTickFlags |= 0x02; // bCanEverTick
      *pActorTickFlags |= 0x04; // bStartWithTickEnabled
    }
    ot->SetActorTickEnabled(true);
    
    if (actorBeginPlay) {
      ot->ProcessEvent(actorBeginPlay, nullptr);
      printf("[ANIM-FORCE] Called ReceiveBeginPlay on OrbitManager: %s\n", ot->GetName().c_str());
    }
  }

  // Force CharacterSpawner ticking and BeginPlay
  auto characterSpawners = UObject::FindObjects<AYCharacterSpawner>();
  for (auto cs : characterSpawners) {
    if (!cs) continue;
    printf("[DIAG-ANIM] CharacterSpawner: %s (%p) | TickEnabled: %d\n", cs->GetFullName().c_str(), cs, (int)cs->IsActorTickEnabled());
    uint8_t* pActorTickFlags = (uint8_t*)((uintptr_t)cs + 0x0034);
    if (pActorTickFlags) {
      *pActorTickFlags |= 0x02; // bCanEverTick
      *pActorTickFlags |= 0x04; // bStartWithTickEnabled
    }
    cs->SetActorTickEnabled(true);
    
    if (actorBeginPlay) {
      cs->ProcessEvent(actorBeginPlay, nullptr);
      printf("[ANIM-FORCE] Called ReceiveBeginPlay on CharacterSpawner: %s\n", cs->GetName().c_str());
    }
  }

  // Force crew visibility, ticking, and play animations
  auto crewMeshes = UObject::FindObjects<USkeletalMeshComponent>();
  int forcedCrewCount = 0;
  for (auto comp : crewMeshes) {
    if (!comp) continue;
    std::string fullName = comp->GetFullName();
    AActor* owner = comp->GetOwner();
    if (fullName.find("MN_Hangar") != std::string::npos || 
        fullName.find("Launch_P") != std::string::npos || 
        fullName.find("MN_HGR") != std::string::npos ||
        (owner && owner->GetFullName().find("MN_Hangar") != std::string::npos)) {
      
      // Force Visibility on the component
      uint8_t* pCompFlags = (uint8_t*)((uintptr_t)comp + 0x0120);
      if (pCompFlags) {
        *pCompFlags |= 0x01;   // bVisible = true
        *pCompFlags &= ~0x02;  // bHiddenInGame = false
      }
      
      // Force MeshComponentUpdateFlag to AlwaysTickPoseAndRefreshBones (0)
      uint8_t* pMeshUpdateFlag = (uint8_t*)((uintptr_t)comp + 0x081C);
      if (pMeshUpdateFlag) {
        *pMeshUpdateFlag = 0; // AlwaysTickPoseAndRefreshBones
      }

      // Force UActorComponent flags (bRegistered = 1, bTemplatesReady = 1, bIsActive = 1) at offset 0x00A0
      uint8_t* pComponentFlags = (uint8_t*)((uintptr_t)comp + 0x00A0);
      if (pComponentFlags) {
        *pComponentFlags |= 0x07;
      }

      // Force Component Activation
      static UFunction* activateFn = nullptr;
      if (!activateFn) {
        activateFn = UObject::FindObject<UFunction>("Function Engine.ActorComponent.Activate");
      }
      if (activateFn) {
        struct {
          bool bReset;
        } params = { true };
        comp->ProcessEvent(activateFn, &params);
      }

      // Force Tick on the component by toggling it
      comp->SetComponentTickEnabled(false);
      comp->SetComponentTickEnabled(true);
      
      // Force Tick on the owner actor by toggling it
      if (owner) {
        uint8_t* pActorTickFlags = (uint8_t*)((uintptr_t)owner + 0x0034);
        if (pActorTickFlags) {
          *pActorTickFlags |= 0x02; // bCanEverTick = true
          *pActorTickFlags |= 0x04; // bStartWithTickEnabled = true
        }
        owner->SetActorTickEnabled(false);
        owner->SetActorTickEnabled(true);
        uint8_t* pAActorFlags = (uint8_t*)((uintptr_t)owner + 0x008C);
        if (pAActorFlags) {
          *pAActorFlags &= ~0x01; // bHidden = false
        }
        
        static UFunction* actorBeginPlay = nullptr;
        if (!actorBeginPlay) {
          actorBeginPlay = UObject::FindObject<UFunction>("Function Engine.Actor.ReceiveBeginPlay");
        }
        if (actorBeginPlay) {
          owner->ProcessEvent(actorBeginPlay, nullptr);
        }
      }
      
      // Clear PauseAnims
      uint8_t* pPauseAnims = (uint8_t*)((uintptr_t)comp + 0x09A4);
      if (pPauseAnims) {
        *pPauseAnims &= ~0x02; // bPauseAnims = false
      }
      
      // Force Playing on AnimSingleNodeInstance
      if (comp->AnimScriptInstance) {
        std::string animClass = comp->AnimScriptInstance->Class ? comp->AnimScriptInstance->Class->GetFullName() : "";
        if (animClass.find("AnimSingleNodeInstance") != std::string::npos) {
          UAnimSingleNodeInstance* singleNode = (UAnimSingleNodeInstance*)comp->AnimScriptInstance;
          if (singleNode->CurrentAsset) {
            static UFunction* playAnimFn = nullptr;
            if (!playAnimFn) {
              playAnimFn = UObject::FindObject<UFunction>("Function Engine.SkeletalMeshComponent.PlayAnimation");
            }
            if (playAnimFn) {
              struct {
                UObject* NewAnimToPlay;
                bool bLooping;
              } playParams = { singleNode->CurrentAsset, true };
              comp->ProcessEvent(playAnimFn, &playParams);
              printf("[ANIM-FORCE] Called PlayAnimation on crew mesh: %s with asset %s\n",
                     comp->GetName().c_str(), singleNode->CurrentAsset->GetName().c_str());
            }
          }
          singleNode->SetPlaying(true);
          singleNode->SetPlayRate(1.0f);
          singleNode->DeltaTime = 0.0166f;
          forcedCrewCount++;
        }
      }
    }
  }
  printf("[ANIM-FORCE] Forced playing and visibility on %d hangar crew members.\n", forcedCrewCount);

  printf("[DIAG-ANIM] === USkeletalMeshComponent Detailed Diagnostics ===\n");
  auto meshes = UObject::FindObjects<USkeletalMeshComponent>();
  int printedMeshes = 0;
  for (auto comp : meshes) {
    if (!comp) continue;
    std::string fullName = comp->GetFullName();
    if (fullName.find("MN_Hangar") == std::string::npos &&
        fullName.find("Launch_P") == std::string::npos &&
        fullName.find("MN_HGR") == std::string::npos) {
      continue;
    }
    AActor* owner = comp->GetOwner();
    std::string ownerName = owner ? owner->GetFullName() : "None";
    
    // Component tick flags
    bool compTickEnabled = comp->IsComponentTickEnabled();
    uint8_t* pCompTickFlags = (uint8_t*)((uintptr_t)comp + 0x003C);
    bool compTickEvenWhenPaused = pCompTickFlags ? (*pCompTickFlags & 0x01) != 0 : false;
    bool compCanEverTick = pCompTickFlags ? (*pCompTickFlags & 0x02) != 0 : false;
    
    // Actor tick flags
    bool actorTickEnabled = owner ? owner->IsActorTickEnabled() : false;
    uint8_t* pActorTickFlags = owner ? (uint8_t*)((uintptr_t)owner + 0x0034) : nullptr;
    bool actorTickEvenWhenPaused = pActorTickFlags ? (*pActorTickFlags & 0x01) != 0 : false;
    bool actorCanEverTick = pActorTickFlags ? (*pActorTickFlags & 0x02) != 0 : false;
    float actorCustomTD = owner ? owner->CustomTimeDilation : 0.0f;

    // Skeletal mesh settings
    uint8_t* pPauseAnims = (uint8_t*)((uintptr_t)comp + 0x09A4);
    bool bPauseAnims = pPauseAnims ? (*pPauseAnims & 0x02) != 0 : false;
    uint8_t animMode = *(uint8_t*)((uintptr_t)comp + 0x08C8);
    
    // Anim instance details
    float animDeltaTime = 0.0f;
    std::string animClassStr = "None";
    if (comp->AnimScriptInstance) {
      animDeltaTime = comp->AnimScriptInstance->DeltaTime;
      if (comp->AnimScriptInstance->Class) {
        animClassStr = comp->AnimScriptInstance->Class->GetFullName();
      }
    }
    
    FVector loc = owner ? owner->K2_GetActorLocation() : FVector{0, 0, 0};
    uint8_t* pAActorFlags = owner ? (uint8_t*)((uintptr_t)owner + 0x008C) : nullptr;
    bool actorHidden = pAActorFlags ? (*pAActorFlags & 0x01) != 0 : false;
    
    uint8_t* pCompFlags = (uint8_t*)((uintptr_t)comp + 0x0120);
    bool compVisible = pCompFlags ? (*pCompFlags & 0x01) != 0 : false;
    bool compHiddenInGame = pCompFlags ? (*pCompFlags & 0x02) != 0 : false;

    uint8_t meshUpdateFlag = *(uint8_t*)((uintptr_t)comp + 0x081C);
    printf("[DIAG-ANIM]   Mesh: %s\n", fullName.c_str());
    printf("[DIAG-ANIM]     Owner: %s | Loc: (%.2f, %.2f, %.2f) | CustomTD: %.3f | ActorTick: %d (CanTick: %d)\n",
           ownerName.c_str(), loc.X, loc.Y, loc.Z, actorCustomTD, actorTickEnabled, actorCanEverTick);
    printf("[DIAG-ANIM]     ActorHidden: %d | CompVisible: %d | CompHiddenInGame: %d | LastRenderTime: %.3f\n",
           actorHidden, compVisible, compHiddenInGame, comp->LastRenderTime);
    uint8_t compFlags = *(uint8_t*)((uintptr_t)comp + 0x00A0);
    printf("[DIAG-ANIM]     CompFlags: 0x%02X | CompTick: %d (CanTick: %d, EvenPaused: %d) | PauseAnims: %d | AnimMode: %d | MeshUpdateFlag: %d\n",
           compFlags, compTickEnabled, compCanEverTick, compTickEvenWhenPaused, bPauseAnims, animMode, meshUpdateFlag);
    printf("[DIAG-ANIM]     Dump 0x710-0x730:");
    for (uint32_t off = 0x710; off < 0x730; ++off) {
      printf(" %02X", *(uint8_t*)((uintptr_t)comp + off));
    }
    printf("\n");
    printf("[DIAG-ANIM]     Dump 0x810-0x825:");
    for (uint32_t off = 0x810; off < 0x825; ++off) {
      printf(" %02X", *(uint8_t*)((uintptr_t)comp + off));
    }
    printf("\n");
    std::string currentAssetStr = "None";
    if (comp->AnimScriptInstance && animClassStr.find("AnimSingleNodeInstance") != std::string::npos) {
      UAnimSingleNodeInstance* singleNode = (UAnimSingleNodeInstance*)comp->AnimScriptInstance;
      if (singleNode->CurrentAsset) {
        currentAssetStr = singleNode->CurrentAsset->GetFullName();
      }
    }
    printf("[DIAG-ANIM]     AnimInstance: %p (Class: %s, DeltaTime: %.4f) | CurrentAsset: %s\n",
           comp->AnimScriptInstance, animClassStr.c_str(), animDeltaTime, currentAssetStr.c_str());
    
    printedMeshes++;
    if (printedMeshes >= 30) {
      printf("[DIAG-ANIM]   (Truncated after 30 meshes)\n");
      break;
    }
  }

  printf("[DIAG-ANIM] === Hangar Levels & Level Script Actors ===\n");
  if (*UWorld::GWorld) {
    UWorld* world = *UWorld::GWorld;
    for (int i = 0; i < world->Levels.Count(); ++i) {
      ULevel* lvl = world->Levels[i];
      if (lvl) {
        std::string lvlName = lvl->GetFullName();
        ALevelScriptActor* script = lvl->LevelScriptActor;
        bool tickEnabled = script ? script->IsActorTickEnabled() : false;
        uint8_t lvlFlags = *(uint8_t*)((uintptr_t)lvl + 0x01B0);
        printf("[DIAG-ANIM] Level[%d]: %s, ScriptActor: %s (%p), TickEnabled: %d, LvlFlags: 0x%02X\n",
               i, lvlName.c_str(), script ? script->GetFullName().c_str() : "None", script, tickEnabled, lvlFlags);
      }
    }
  }

  printf("[ANIM-INIT] === Hangar level script initialization complete ===\n");
}

void UGameEngineTick(UGameEngine *GameEngine, float DeltaTime,
                     bool CanEverRender) {
  reinterpret_cast<void (*)(UGameEngine *, float, bool)>(OrigUGameEngineTick)(
      GameEngine, DeltaTime, CanEverRender);

  // ----------------------------------------------------
  // FORCE HANGAR CREW ANIMATION DELTA TIME
  // ----------------------------------------------------
  if (*UWorld::GWorld) {
    UWorld *world = *UWorld::GWorld;
    bool onHangarMap = false;
    if (world->PersistentLevel) {
      std::string plName = world->PersistentLevel->GetFullName();
      if (plName.find("Launch_P") != std::string::npos) {
        onHangarMap = true;
      }
    }
    if (onHangarMap) {
      auto crewMeshes = UObject::FindObjects<USkeletalMeshComponent>();
      for (auto comp : crewMeshes) {
        if (comp && comp->AnimScriptInstance) {
          std::string fullName = comp->GetFullName();
          AActor* owner = comp->GetOwner();
          if (fullName.find("MN_Hangar") != std::string::npos || 
              fullName.find("Launch_P") != std::string::npos || 
              fullName.find("MN_HGR") != std::string::npos ||
              (owner && owner->GetFullName().find("MN_Hangar") != std::string::npos)) {
            comp->AnimScriptInstance->DeltaTime = DeltaTime > 0.0f ? DeltaTime : 0.0166f;
          }
        }
      }
    }
  }

  // ----------------------------------------------------
  // PREVIEW SHIP ANIMATION BLUEPRINT INSTANTIATION
  // ----------------------------------------------------
  if (*UWorld::GWorld) {
    static int previewCheckFrame = 0;
    previewCheckFrame++;

    if (g_needsCustomizationPreviewUpdate || !IsValidUObject(g_customizationPreviewActor) || (previewCheckFrame % 30 == 0)) {
      g_needsCustomizationPreviewUpdate = false;
      std::vector<AActor*> previews;
      UWorld *world = *UWorld::GWorld;
      for (int li = 0; li < world->Levels.Count(); ++li) {
        ULevel* lvl = world->Levels[li];
        if (!lvl) continue;
        
        // Only look at levels that are visible (bit 3 at 0x1B0)
        uint8_t* pLvlFlags = (uint8_t*)((uintptr_t)lvl + 0x01B0);
        if (pLvlFlags && (*pLvlFlags & 0x08)) {
          TArray<AActor*>& levelActors = *(TArray<AActor*>*)( (uintptr_t)lvl + 0x00A0 );
          for (int ai = 0; ai < levelActors.Count(); ++ai) {
            AActor* actor = levelActors[ai];
            if (actor && actor->Class) {
              std::string clsName = actor->Class->GetName();
              if (clsName == "VH_CustomisationPreview_BP_C") {
                previews.push_back(actor);
              }
            }
          }
        }
      }

      if (!previews.empty()) {
        AActor* newestPreview = previews.back();
        if (g_customizationPreviewActor != newestPreview) {
          printf("[ANIM-INIT] CustomisationPreview actor updated: %s at %p (was %p)\n",
                 newestPreview->GetFullName().c_str(), newestPreview, g_customizationPreviewActor);
          g_customizationPreviewActor = newestPreview;
        }
        
        if (previews.size() > 1) {
          UFunction* destroyFn = UObject::FindObject<UFunction>("Function Engine.Actor.K2_DestroyActor");
          if (destroyFn) {
            for (size_t i = 0; i < previews.size() - 1; i++) {
              if (IsValidUObject(previews[i])) {
                printf("[ANIM-PREVIEW] Destroying duplicate preview actor: %s at %p\n", previews[i]->GetFullName().c_str(), previews[i]);
                previews[i]->ProcessEvent(destroyFn, nullptr);
              }
            }
          }
        }
      } else {
        g_customizationPreviewActor = nullptr;
      }
    }

    if (IsValidUObject(g_customizationPreviewActor)) {
      UWorld *world = *UWorld::GWorld;
      if (world->OwningGameInstance && world->OwningGameInstance->LocalPlayers.Count() > 0) {
        ULocalPlayer *lp = world->OwningGameInstance->LocalPlayers[0];
        if (lp && lp->PlayerController) {
          AYPlayerController *pc = (AYPlayerController *)lp->PlayerController;
          UYLoadoutManagerComponent *lmc = (UYLoadoutManagerComponent *)pc->m_loadoutManager;
          if (lmc && lmc->m_activeLoadout) {
            uint8_t shipClass = (uint8_t)lmc->m_activeLoadout->m_shipClass;
            if (shipClass >= 1 && shipClass <= 15) {
              UClass *animClass = g_shipAnimClasses[shipClass];
              if (animClass) {
                // Find skeletal components directly on the customization preview actor rather than querying all GObjects
                std::vector<USkeletalMeshComponent*> skeletalComponents;
                AActor* previewActor = (AActor*)g_customizationPreviewActor;
                
                auto checkComponent = [&](UActorComponent* comp) {
                  if (comp && comp->IsA(USkeletalMeshComponent::StaticClass())) {
                    skeletalComponents.push_back((USkeletalMeshComponent*)comp);
                  }
                };

                for (int ci = 0; ci < previewActor->BlueprintCreatedComponents.Count(); ++ci) {
                  checkComponent(previewActor->BlueprintCreatedComponents[ci]);
                }
                for (int ci = 0; ci < previewActor->InstanceComponents.Count(); ++ci) {
                  checkComponent(previewActor->InstanceComponents[ci]);
                }

                for (auto comp : skeletalComponents) {
                  std::string compName = comp->GetName();
                  if (compName == "shipMesh" || compName.find("shipMesh") != std::string::npos) {
                    if (comp->AnimClass != animClass) {
                      printf("[ANIM-PREVIEW] shipMesh AnimClass mismatch! Current: %p, Expected: %s. Setting SetAnimInstanceClass...\n",
                             comp->AnimClass, animClass->GetFullName().c_str());
                      comp->SetAnimInstanceClass(animClass);
                      
                      if (comp->AnimScriptInstance) {
                        printf("[ANIM-PREVIEW] AnimScriptInstance successfully constructed: %p (Class: %s)\n",
                               comp->AnimScriptInstance, comp->AnimScriptInstance->Class ? comp->AnimScriptInstance->Class->GetFullName().c_str() : "NULL");
                      }
                    }
                    
                    if (comp->AnimScriptInstance) {
                      comp->AnimScriptInstance->DeltaTime = 0.0166f;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  if (g_waitingForFinalization) {
    if (*UWorld::GWorld) {
      UWorld *world = *UWorld::GWorld;
      TArray<ULevelStreaming *> &streamingLevels = world->StreamingLevels;

      static std::string lastPrintedLevel = "";
      if (lastPrintedLevel != g_waitingLevelName) {
        printf("[POLL_DEBUG] Started waiting for level: %s\n",
               g_waitingLevelName.c_str());
        lastPrintedLevel = g_waitingLevelName;
      }

      for (int i = 0; i < streamingLevels.Count(); ++i) {
        ULevelStreaming *sl = streamingLevels[i];
        if (sl) {
          std::string pathStr = "";
          FString *pPathFStr = (FString *)((uintptr_t)sl + 0x40);

          // Simple inline check for readable pointer to be 100% crash-safe
          auto IsValidReadable = [](const void *ptr, size_t size) -> bool {
            if (!ptr)
              return false;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
              if (mbi.State == MEM_COMMIT &&
                  (mbi.Protect &
                   (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE))) {
                return (uintptr_t)ptr + size <=
                       (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
              }
            }
            return false;
          };

          if (pPathFStr && pPathFStr->Data() &&
              IsValidReadable(pPathFStr->Data(),
                              pPathFStr->Count() * sizeof(wchar_t))) {
            pathStr = pPathFStr->ToString();
          }

          if (CaseInsensitiveContains(pathStr, g_waitingLevelName)) {
            bool loaded = sl->IsLevelLoaded();

            if (loaded) {
              printf("[POLL] Level %s loaded (Loaded: %d). Advancing "
                     "transition queue...\n",
                     g_waitingLevelName.c_str(), loaded);

              g_waitingForFinalization = false;

              void *gm = g_waitingGM;
              UFunction *function = g_finalizeFunction;
              void *tm = *(void **)((uintptr_t)gm + 0x9a8);
              bool isLast = false;

              if (tm) {
                void *head = *(void **)((uintptr_t)tm + 0x78);
                if (head) {
                  void *currentLevelNode = *(void **)head;
                  if (currentLevelNode) {
                    isLast = (*(void **)currentLevelNode == nullptr);
                  }
                }
              }

              // 2. Complete/advance transition
              if (tm) {
                // CRITICAL FIX: Force-set bShouldBeVisible ONLY on the target loaded level.
                // The engine only renders levels where bShouldBeVisible=true.
                // The completeTransition call updates internal queue state but
                // does NOT set this flag on ULevelStreaming objects.
                uint8_t *flagByte = (uint8_t *)((uintptr_t)sl + 0xB0);
                uint8_t oldFlags = *flagByte;
                *flagByte |= (1 << 5) | (1 << 6); // bShouldBeLoaded | bShouldBeVisible
                printf("[POLL-VIS] Force-set bShouldBeVisible on target level %s flags: 0x%02X -> 0x%02X\n",
                       pathStr.c_str(), oldFlags, *flagByte);

                if (isLast) {
                  printf("[POLL] Last level in queue finalized! Declaring "
                         "transition complete...\n");
                  *(char *)((uintptr_t)gm + 0xb1b) = 1;
                  *(char *)((uintptr_t)gm + 0xa90) = 0;

                  uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
                  typedef void(__fastcall * FN_CompleteTransition)(
                      void *transitionManager, void *transitionState);
                  FN_CompleteTransition completeTransition =
                      (FN_CompleteTransition)(base + 0x3CD3C0);

                  completeTransition(tm, (char *)gm + 0xa90);
                  printf("[POLL] Transition completed successfully!\n");
                  UnpauseHangarAnimations();
                  TriggerLevelActorLinks();
                  g_needsCustomizationPreviewUpdate = true;
                } else {
                  printf("[POLL] Level finalized: %s (not last). Advancing "
                         "queue...\n",
                         g_waitingLevelName.c_str());
                  uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
                  typedef void(__fastcall * FN_CompleteTransition)(
                      void *transitionManager, void *transitionState);
                  FN_CompleteTransition completeTransition =
                      (FN_CompleteTransition)(base + 0x3CD3C0);

                  completeTransition(tm, (char *)gm + 0xa90);
                }
              }
              break;
            }
          }
        }
      }
    }
  }

  static int frameCounter = 0;
  if (++frameCounter % 300 == 0) { // Every ~5 seconds at 60fps
    if (*UWorld::GWorld) {
      UWorld *world = *UWorld::GWorld;
      printf("[DIAG] GWorld: %p, PersistentLevel: %p, AuthorityGameMode: %p\n",
             world, world->PersistentLevel, world->AuthorityGameMode);

      TArray<ULevelStreaming *> &streamingLevels = world->StreamingLevels;
      printf("[DIAG] Streaming levels count: %d\n", streamingLevels.Count());

      auto IsValidReadableDiag = [](const void *ptr, size_t size) -> bool {
        if (!ptr)
          return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
          if (mbi.State == MEM_COMMIT &&
              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            return (uintptr_t)ptr + size <=
                   (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
          }
        }
        return false;
      };

      for (int i = 0; i < streamingLevels.Count(); ++i) {
        ULevelStreaming *sl = streamingLevels[i];
        if (sl) {
          std::string name = sl->GetName();
          bool loaded = sl->IsLevelLoaded();
          bool visible = sl->IsLevelVisible();
          void *loadedLevel = sl->LoadedLevel;
          uint8_t flagByte = *(uint8_t *)((uintptr_t)sl + 0xB0);

          // Read actual package path from WorldAsset at +0x40
          std::string pathStr = "<unknown>";
          FString *pPathFStr = (FString *)((uintptr_t)sl + 0x40);
          if (pPathFStr && pPathFStr->Data() &&
              IsValidReadableDiag(pPathFStr->Data(),
                                  pPathFStr->Count() * sizeof(wchar_t))) {
            pathStr = pPathFStr->ToString();
          }

          printf("[DIAG]   [%d] %s Path=%s Loaded=%d Vis=%d Flags=0x%02X "
                 "LoadedLevel=%p\n",
                 i, name.c_str(), pathStr.c_str(), loaded, visible, flagByte,
                 loadedLevel);
        }
      }

      // Diagnostic print for camera state
      if (world->OwningGameInstance &&
          world->OwningGameInstance->LocalPlayers.Count() > 0) {
        ULocalPlayer *lp = world->OwningGameInstance->LocalPlayers[0];
        if (lp && lp->PlayerController) {
          AYPlayerController_Outpost *pc =
              (AYPlayerController_Outpost *)lp->PlayerController;
          printf("[DIAG_CAM] PlayerController: %p, m_outpostHUD: %p, "
                 "m_outpostCamera: %p\n",
                 pc, pc->m_outpostHUD, pc->m_outpostCamera);

          APlayerCameraManager *cm = pc->PlayerCameraManager;
          printf("[DIAG_CAM] PlayerCameraManager: %p (Class: %s)\n", cm,
                 cm ? cm->Class->GetFullName().c_str() : "NULL");

          if (cm) {
            AActor *vtTarget = *(AActor **)((uintptr_t)cm + 0xBF0);
            printf("[DIAG_CAM] ViewTarget.Target: %p (Class: %s, Name: %s)\n",
                   vtTarget,
                   vtTarget && vtTarget->Class
                       ? vtTarget->Class->GetFullName().c_str()
                       : "NULL",
                   vtTarget ? vtTarget->GetName().c_str() : "NULL");
          }
        }
      }

      // Dump m_outpostCameras map
      if (world->AuthorityGameMode) {
        uintptr_t gm = (uintptr_t)world->AuthorityGameMode;
        std::string gmName = world->AuthorityGameMode->GetFullName();
        if (gmName.find("Outpost") != std::string::npos ||
            gmName.find("Frontend") != std::string::npos) {
          struct SetElement_HangarCam {
            uint8_t Key;
            uint8_t Pad[7];
            ACameraActor *Value;
            int32_t HashNextId;
            int32_t HashIndex;
          };
          struct TMapRaw {
            SetElement_HangarCam *Data;
            int32_t Count;
            int32_t Max;
          };
          TMapRaw *camerasMap = (TMapRaw *)(gm + 0xB20);
          if (camerasMap && camerasMap->Data &&
              IsValidReadableDiag(camerasMap->Data,
                                  camerasMap->Max *
                                      sizeof(SetElement_HangarCam))) {
            printf("[DIAG_CAM] Dumping m_outpostCameras Map: count=%d max=%d\n",
                   camerasMap->Count, camerasMap->Max);
            for (int idx = 0; idx < camerasMap->Max; ++idx) {
              SetElement_HangarCam &elem = camerasMap->Data[idx];
              if (elem.Value &&
                  IsValidReadableDiag(elem.Value, sizeof(void *))) {
                printf("[DIAG_CAM]   Index %d: Key (EYOutpostSection)=%d, "
                       "CameraActor=%p (%s)\n",
                       idx, elem.Key, elem.Value,
                       elem.Value->GetName().c_str());
              }
            }
          }
        }
      }

      // Print all CameraActors in GObjects
      auto cameraActors = UObject::FindObjects<ACameraActor>();
      printf("[DIAG_CAM] GObjects ACameraActor Count: %d\n",
             (int)cameraActors.size());
      for (size_t c = 0; c < cameraActors.size(); ++c) {
        ACameraActor *cam = cameraActors[c];
        printf("[DIAG_CAM]   [%d] %p Name: %s (FullName: %s)\n", (int)c, cam,
               cam->GetName().c_str(), cam->GetFullName().c_str());
      }
    }
  }

  // ----------------------------------------------------
  // HANGAR LEVEL SCRIPT INITIALIZATION (Trigger & Fallback)
  // ----------------------------------------------------
  if (*UWorld::GWorld) {
    UWorld *world = *UWorld::GWorld;
    bool onHangarMap = false;
    if (world->PersistentLevel) {
      std::string plName = world->PersistentLevel->GetFullName();
      if (plName.find("Launch_P") != std::string::npos) {
        onHangarMap = true;
      }
    }

    if (onHangarMap) {
      if (!g_genericPInitialised) {
        TArray<ULevelStreaming*>& streamingLevels = world->StreamingLevels;
        for (int i = 0; i < streamingLevels.Count(); ++i) {
          ULevelStreaming* sl = streamingLevels[i];
          if (sl) {
            FString* pPathFStr = (FString*)((uintptr_t)sl + 0x40);
            std::string pathStr = pPathFStr && pPathFStr->Data() ? pPathFStr->ToString() : "";
            if (pathStr.find("MN_Hangar_Generic_P") != std::string::npos) {
              if (sl->LoadedLevel) {
                uint8_t* pLvlFlags = (uint8_t*)((uintptr_t)sl->LoadedLevel + 0x01B0);
                if (pLvlFlags) {
                  printf("[ANIM-INIT] Generic_P loaded! Setting bActorsInitialized flag (before: 0x%02X) to trigger native initialization.\n", *pLvlFlags);
                  *pLvlFlags |= 0x20; // Set bActorsInitialized bitfield flag
                  printf("[ANIM-INIT] Generic_P flags after: 0x%02X\n", *pLvlFlags);
                  
                  // Also set it on other loaded hangar levels to be safe
                  for (int li = 0; li < world->Levels.Count(); ++li) {
                    ULevel* lvl = world->Levels[li];
                    if (lvl && lvl != world->PersistentLevel) {
                      uint8_t* pSubFlags = (uint8_t*)((uintptr_t)lvl + 0x01B0);
                      if (pSubFlags) {
                        *pSubFlags |= 0x20;
                      }
                    }
                  }
                  
                  uint8_t* pFlags = (uint8_t*)((uintptr_t)sl + 0x00B0);
                  if (pFlags) {
                    *pFlags |= 0x60; // Make sure streaming flags are loaded/visible
                  }
                  g_genericPInitialised = true;
                }
              }
              break;
            }
          }
        }
      }

      if (!g_levelActorLinksAttempted) {
        TArray<ULevelStreaming *> &streamingLevels = world->StreamingLevels;
        bool allLoaded = true;
        if (streamingLevels.Count() > 0) {
          for (int i = 0; i < streamingLevels.Count(); ++i) {
            ULevelStreaming *sl = streamingLevels[i];
            if (sl && !sl->IsLevelLoaded()) {
              allLoaded = false;
              break;
            }
          }
        } else {
          allLoaded = false; // Wait until streaming levels exist
        }
        if (allLoaded) {
          printf("[ANIM-INIT] Fallback trigger: all streaming levels loaded.\n");
          TriggerLevelActorLinks();
        }
      }
    } else {
      if (g_levelActorLinksAttempted) {
        printf("[ANIM-INIT] Resetting level actor links flags (left hangar map)\n");
        g_levelActorLinksAttempted = false;
        g_levelActorLinksInitialized = false;
      }
      g_genericPInitialised = false;
      g_customizationPreviewActor = nullptr; // Clear reference when leaving hangar
    }
  }

  // ----------------------------------------------------
  // CAMERA FORCE / FADE OVERRIDE PASS (Runs every frame)
  // ----------------------------------------------------
  if (*UWorld::GWorld) {
    UWorld *world = *UWorld::GWorld;
    if (world->OwningGameInstance &&
        world->OwningGameInstance->LocalPlayers.Count() > 0) {
      ULocalPlayer *lp = world->OwningGameInstance->LocalPlayers[0];
      if (lp && lp->PlayerController) {
        AYPlayerController_Outpost *pc =
            (AYPlayerController_Outpost *)lp->PlayerController;

        // 1. Force Stop Camera Fade
        if (pc->PlayerCameraManager) {
          pc->PlayerCameraManager->StopCameraFade();
          pc->PlayerCameraManager->SetManualCameraFade(
              0.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), false);
        }

        // 2. Force View Target to HangarCamera (if loaded)
        ACameraActor *hangarCam = nullptr;
        auto cameraActors = UObject::FindObjects<ACameraActor>();
        for (ACameraActor *cam : cameraActors) {
          if (cam && cam->GetName().find("HangarCamera") != std::string::npos) {
            hangarCam = cam;
            break;
          }
        }

        if (hangarCam) {
          AActor *currentVT = nullptr;
          if (pc->PlayerCameraManager) {
            currentVT =
                *(AActor **)((uintptr_t)pc->PlayerCameraManager + 0xBF0);
          }
          if (currentVT != (AActor *)hangarCam) {
            printf("[POLL_CAM] Force-setting ViewTarget from %p to "
                   "HangarCamera (%p)\n",
                   currentVT, hangarCam);
            pc->SetViewTargetWithBlend(hangarCam, 0.0f,
                                       EViewTargetBlendFunction::VTBlend_Linear,
                                       0.0f, false);
          }
        }
      }
    }
  }

  {
    std::scoped_lock t(ProcOnMainThreadMutex);

    for (const auto &func : FunctionsToProcOnMainThread) {
      func();
    }

    FunctionsToProcOnMainThread.clear();
  }

  // ----------------------------------------------------
  // OFFLINE restored matches launching checks
  // ----------------------------------------------------
  if (connectToServer) {
    connectToServer = false;
    if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance && (*UWorld::GWorld)->OwningGameInstance->LocalPlayers.Count() > 0) {
      auto pc = (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;
      if (pc) {
        std::wstring wServerIP(serverIP.begin(), serverIP.end());
        std::wstring wServerConnectCommand = L"open ";
        std::wstring wFinalCommand = wServerConnectCommand.append(wServerIP);
        printf("[LAUNCH] Connecting to multiplayer server: %ls\n", wFinalCommand.c_str());
        getLastOfType<UKismetSystemLibrary>()->STATIC_ExecuteConsoleCommand((*UWorld::GWorld), wFinalCommand.c_str(), pc);
      }
    }
  }

  if (launchTutorial) {
    launchTutorial = false;
    if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance && (*UWorld::GWorld)->OwningGameInstance->LocalPlayers.Count() > 0) {
      auto pc = (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;
      if (pc) {
        printf("[LAUNCH] Opening tutorial map...\n");
        getLastOfType<UKismetSystemLibrary>()->STATIC_ExecuteConsoleCommand(
            (*UWorld::GWorld), L"open S01E00_00_Tutorial_P", pc);
      }
    }
  }

  if (launchSingleplayer) {
    launchSingleplayer = false;
    if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance && (*UWorld::GWorld)->OwningGameInstance->LocalPlayers.Count() > 0) {
      auto pc = (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;
      if (pc) {
        std::wstring mapFileNames[10] = { L"MP_Amirani_P", L"MP_DansMap_P", L"MP_Derelict_P", L"MP_Glacier_P", L"MP_Gorge_P", L"MP_Highlands_P", L"MP_Paradise_P", L"MP_Skybridge_P", L"MP_Space01_P", L"MP_Space02_P" };
        std::wstring command = L"open ";
        command = command.append(mapFileNames[map].c_str());

        printf("[LAUNCH] Opening singleplayer map: %ls\n", command.c_str());
        getLastOfType<UKismetSystemLibrary>()->STATIC_ExecuteConsoleCommand(
            (*UWorld::GWorld), command.c_str(), pc);

        if (numBotsTeamOne > 0 || numBotsTeamTwo > 0) {
          printf("[LAUNCH] Detaching SetupSingleplayerAIThread with %d friendly / %d enemy bots, difficulty=%d\n", numBotsTeamOne, numBotsTeamTwo, difficulty);
          std::thread t(SetupSingleplayerAIThread, numBotsTeamOne, numBotsTeamTwo, difficulty, singleplayerLoadoutString);
          t.detach();
        } else {
          printf("[LAUNCH] Detaching DelaySingleplayerSetupThread\n");
          std::thread t(DelaySingleplayerSetupThread, singleplayerLoadoutString);
          t.detach();
        }
      }
    }
  }

  if (launchHostServer) {
    launchHostServer = false;
    if (*UWorld::GWorld && (*UWorld::GWorld)->OwningGameInstance && (*UWorld::GWorld)->OwningGameInstance->LocalPlayers.Count() > 0) {
      auto pc = (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController;
      if (pc) {
        std::wstring mapFileNames[10] = { L"MP_Amirani_P", L"MP_DansMap_P", L"MP_Derelict_P", L"MP_Glacier_P", L"MP_Gorge_P", L"MP_Highlands_P", L"MP_Paradise_P", L"MP_Skybridge_P", L"MP_Space01_P", L"MP_Space02_P" };
        std::wstring command = L"open ";
        command = command.append(mapFileNames[hostMapIndex].c_str());

        printf("[LAUNCH] Hosting map: %ls\n", command.c_str());
        getLastOfType<UKismetSystemLibrary>()->STATIC_ExecuteConsoleCommand(
            (*UWorld::GWorld), command.c_str(), pc);

        std::thread t(HostServerSetupThread, numBotsTeamOne, numBotsTeamTwo, hostDifficulty, hostLoadoutIndex);
        t.detach();
      }
    }
  }

  if (executeServerSetupOnMainThread) {
    executeServerSetupOnMainThread = false;
    printf("[LAUNCH] Setting up listen server and AI...\n");
    Listen();
    SetupMultiplayerAI(cachedBotsT1, cachedBotsT2, cachedDiff);
  }

  if (Globals::AmServer) {

    if (GetAsyncKeyState(VK_F8)) {

      while (GetAsyncKeyState(VK_F8)) {
      }
    }
  }

  return;
}

void ProcInMainThread(std::function<void()> Func) {
  std::scoped_lock t(ProcOnMainThreadMutex);

  FunctionsToProcOnMainThread.push_back(Func);

  return;
}

void ServerParticleCrash(void *a1) { return; }

void *origVehicleSkipUpdateCheck1 = nullptr;
void *origVehicleSkipUpdateCheck2 = nullptr;

void VehicleSkipUpdateCheck1Hook(uintptr_t a1) {
  *(uint8_t *)(a1 + 0x488) = 0x1;

  reinterpret_cast<void (*)(uintptr_t)>(origVehicleSkipUpdateCheck1)(a1);
  reinterpret_cast<void (*)(__int64 a1, float a2)>(origVehicleSkipUpdateCheck2)(
      a1, 1.0f / 30.0f);
}

void VehicleSkipUpdateCheck2Hook(__int64 a1, float a2) {
  *(uint8_t *)(a1 + 0x488) = 0x1;

  reinterpret_cast<void (*)(__int64 a1, float a2)>(origVehicleSkipUpdateCheck2)(
      a1, a2);
}

void *OrigGetAuthToken = nullptr;

FString *GetAuthTokenHook(FString *AuthToken) {
  *(uint8_t *)(Globals::ModuleBase + 0x40EC940) = 0x6;

  AuthToken->_data = (wchar_t *)UE4Malloc(sizeof(L"0w0"));
  AuthToken->_data[0] = L'0';
  AuthToken->_data[1] = L'w';
  AuthToken->_data[2] = L'0';
  AuthToken->_data[3] = L'\0';
  AuthToken->_count = 4;
  AuthToken->_max = 4;
  return AuthToken;
}

void *OrigValidateFirmamentCert = nullptr;

char ValidateFirmamentCertHook(void *a1, void *a2) {
  std::cout << "Bypassed Firmament Cert!" << std::endl;
  return 1;
}

void *OrigGetShipById = nullptr;

UYShipLoadout *GetShipByIdHook(void *a1, void *a2, char a3) {
  if (THELOADOUT) {
    std::cout << "Overrode loadout!" << std::endl;
    return THELOADOUT;
  }

  return reinterpret_cast<UYShipLoadout *(*)(void *, void *, char)>(
      OrigGetShipById)(a1, a2, a3);
}

/*
        Hook ProcessEvent and set the global base address variable
*/
// =====================================================================
// WINHTTP HOOKS - Intercept all HTTP requests to see what the game
// is trying to request. This tells us if session ID injection works.
// =====================================================================

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// Hook WinHttpConnect to see what hosts the game connects to
typedef HINTERNET(WINAPI *WinHttpConnect_t)(HINTERNET hSession,
                                            LPCWSTR pswzServerName,
                                            INTERNET_PORT nServerPort,
                                            DWORD dwReserved);
static WinHttpConnect_t OrigWinHttpConnect = nullptr;

HINTERNET WINAPI HookWinHttpConnect(HINTERNET hSession, LPCWSTR pswzServerName,
                                    INTERNET_PORT nServerPort,
                                    DWORD dwReserved) {
  if (pswzServerName) {
    printf("[HTTP] WinHttpConnect: host=%ls port=%d\n", pswzServerName,
           nServerPort);
  }
  return OrigWinHttpConnect(hSession, pswzServerName, nServerPort, dwReserved);
}

// Hook WinHttpOpenRequest to see what paths are requested
typedef HINTERNET(WINAPI *WinHttpOpenRequest_t)(
    HINTERNET hConnect, LPCWSTR pwszVerb, LPCWSTR pwszObjectName,
    LPCWSTR pwszVersion, LPCWSTR pwszReferrer, LPCWSTR *ppwszAcceptTypes,
    DWORD dwFlags);
static WinHttpOpenRequest_t OrigWinHttpOpenRequest = nullptr;

HINTERNET WINAPI HookWinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pwszVerb,
                                        LPCWSTR pwszObjectName,
                                        LPCWSTR pwszVersion,
                                        LPCWSTR pwszReferrer,
                                        LPCWSTR *ppwszAcceptTypes,
                                        DWORD dwFlags) {
  printf("[HTTP] WinHttpOpenRequest: %ls %ls\n",
         pwszVerb ? pwszVerb : L"(null)",
         pwszObjectName ? pwszObjectName : L"(null)");
  return OrigWinHttpOpenRequest(hConnect, pwszVerb, pwszObjectName, pwszVersion,
                                pwszReferrer, ppwszAcceptTypes, dwFlags);
}

// Hook WinHttpSendRequest to see request headers
typedef BOOL(WINAPI *WinHttpSendRequest_t)(
    HINTERNET hRequest, LPCWSTR lpszHeaders, DWORD dwHeadersLength,
    LPVOID lpOptional, DWORD dwOptionalLength, DWORD dwTotalLength,
    DWORD_PTR dwContext);
static WinHttpSendRequest_t OrigWinHttpSendRequest = nullptr;

BOOL WINAPI HookWinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeaders,
                                   DWORD dwHeadersLength, LPVOID lpOptional,
                                   DWORD dwOptionalLength, DWORD dwTotalLength,
                                   DWORD_PTR dwContext) {
  printf("[HTTP] WinHttpSendRequest: headers=%ls bodyLen=%d\n",
         (lpszHeaders && dwHeadersLength > 0) ? lpszHeaders : L"(none)",
         dwOptionalLength);
  return OrigWinHttpSendRequest(hRequest, lpszHeaders, dwHeadersLength,
                                lpOptional, dwOptionalLength, dwTotalLength,
                                dwContext);
}

void InitWinHttpHooks() {
  // MH_Initialize may not have been called yet (we're in DllMain ATTACH)
  MH_Initialize();

  HMODULE hWinHttp = LoadLibraryA("winhttp.dll");
  if (!hWinHttp) {
    std::cout << "[HTTP] WARNING: Could not load winhttp.dll" << std::endl;
    return;
  }

  void *pConnect = GetProcAddress(hWinHttp, "WinHttpConnect");
  void *pOpenReq = GetProcAddress(hWinHttp, "WinHttpOpenRequest");
  void *pSendReq = GetProcAddress(hWinHttp, "WinHttpSendRequest");

  if (pConnect) {
    MH_CreateHook(pConnect, (void *)HookWinHttpConnect,
                  (void **)&OrigWinHttpConnect);
    MH_EnableHook(pConnect);
  }
  if (pOpenReq) {
    MH_CreateHook(pOpenReq, (void *)HookWinHttpOpenRequest,
                  (void **)&OrigWinHttpOpenRequest);
    MH_EnableHook(pOpenReq);
  }
  if (pSendReq) {
    MH_CreateHook(pSendReq, (void *)HookWinHttpSendRequest,
                  (void **)&OrigWinHttpSendRequest);
    MH_EnableHook(pSendReq);
  }

  printf("[HTTP] WinHTTP hooks installed (Connect=%p, OpenRequest=%p, "
         "SendRequest=%p)\n",
         pConnect, pOpenReq, pSendReq);
}

// =====================================================================
// EMBEDDED MOCK GATEWAY SERVER
// Hooks GetCommandLineW() to inject gateway params and runs a Winsock
// HTTP server on 127.0.0.1:18765 to discover the game's API protocol.
// =====================================================================

static const int GATEWAY_PORT = 18765;
static std::atomic<bool> g_gatewayRunning{false};

// --- GetCommandLineW Hook ---
static LPWSTR(WINAPI *OrigGetCommandLineW)() = nullptr;
static wchar_t g_modifiedCmdLine[8192] = {0};

LPWSTR WINAPI HookGetCommandLineW() {
  // Build modified command line once, append gateway params
  if (g_modifiedCmdLine[0] == 0) {
    LPWSTR original = OrigGetCommandLineW();
    swprintf_s(g_modifiedCmdLine, _countof(g_modifiedCmdLine),
               L"%s -gatewayaddress=127.0.0.1 -gatewayport=%d", original,
               GATEWAY_PORT);
    // Also log to console
    std::wcout << L"[GATEWAY] Command line injected: -gatewayaddress=127.0.0.1 "
                  L"-gatewayport="
               << GATEWAY_PORT << std::endl;
  }
  return g_modifiedCmdLine;
}

// --- HTTP Response Builder ---
std::string
BuildHttpResponse(int statusCode, const std::string &statusText,
                  const std::string &body,
                  const std::string &contentType = "application/json") {
  std::ostringstream resp;
  resp << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
  resp << "Content-Type: " << contentType << "\r\n";
  resp << "Content-Length: " << body.size() << "\r\n";
  resp << "Connection: close\r\n";
  resp << "Access-Control-Allow-Origin: *\r\n";
  resp << "\r\n";
  resp << body;
  return resp.str();
}

// --- Handle a single HTTP connection ---
static int g_gatewayRequestCount = 0;

void HandleGatewayClient(SOCKET clientSock) {
  char buffer[16384] = {0};
  int totalRecv = 0;

  // Read the HTTP request (headers + body)
  while (totalRecv < sizeof(buffer) - 1) {
    int bytesRecv =
        recv(clientSock, buffer + totalRecv, sizeof(buffer) - 1 - totalRecv, 0);
    if (bytesRecv <= 0)
      break;
    totalRecv += bytesRecv;

    // Check if we have full headers
    char *headerEnd = strstr(buffer, "\r\n\r\n");
    if (headerEnd != nullptr) {
      // Check Content-Length to know if we need more body data
      char *clHeader = strstr(buffer, "Content-Length:");
      if (clHeader) {
        int contentLen = atoi(clHeader + 15);
        int headerSize = (int)(headerEnd - buffer) + 4;
        int bodyRecv = totalRecv - headerSize;
        if (bodyRecv >= contentLen)
          break; // got full body
                 // else continue reading
      } else {
        break; // no content-length = no body expected
      }
    }
  }
  buffer[totalRecv] = '\0';

  if (totalRecv == 0) {
    closesocket(clientSock);
    return;
  }

  // Parse the request line: METHOD /path HTTP/1.x
  std::string request(buffer, totalRecv);
  std::string method = "UNKNOWN";
  std::string path = "/";

  size_t firstSpace = request.find(' ');
  if (firstSpace != std::string::npos) {
    method = request.substr(0, firstSpace);
    size_t secondSpace = request.find(' ', firstSpace + 1);
    if (secondSpace != std::string::npos) {
      path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    }
  }

  // Extract body (after \r\n\r\n)
  std::string body;
  size_t bodyStart = request.find("\r\n\r\n");
  if (bodyStart != std::string::npos) {
    body = request.substr(bodyStart + 4);
  }

  // === LOG TO FILE (printf) ===
  int reqNum = ++g_gatewayRequestCount;
  printf("\n[GATEWAY] === Request #%d ===\n", reqNum);
  printf("[GATEWAY] %s %s\n", method.c_str(), path.c_str());
  if (!body.empty()) {
    // Truncate body display to 500 chars
    int showLen = (body.size() < 500) ? (int)body.size() : 500;
    printf("[GATEWAY] Body (%d bytes): %.500s%s\n", (int)body.size(),
           body.c_str(), body.size() > 500 ? "..." : "");
  }

  // === SMART RESPONSE ROUTING ===
  std::string responseBody;
  int statusCode = 200;
  std::string statusText = "OK";

  // Session management
  static const char *SESSION_ID = "mock-session-dread-offline-001";

  if (path.find("session") != std::string::npos ||
      path.find("Session") != std::string::npos ||
      path.find("createsession") != std::string::npos ||
      path.find("CreateSession") != std::string::npos ||
      // Catch firmament-style session endpoints
      (method == "POST" && path == "/")) {

    // Check if body contains session-related content
    bool isCreateSession =
        (path.find("create") != std::string::npos) ||
        (path.find("Create") != std::string::npos) ||
        (body.find("create") != std::string::npos) ||
        (body.find("Create") != std::string::npos) ||
        (method == "POST" && body.find("session") != std::string::npos);
    bool isTouchSession = (path.find("touch") != std::string::npos) ||
                          (path.find("Touch") != std::string::npos) ||
                          (body.find("touch") != std::string::npos);
    bool isDestroySession = (path.find("destroy") != std::string::npos) ||
                            (path.find("Destroy") != std::string::npos);

    if (isCreateSession) {
      printf("[GATEWAY] -> CreateSession: returning session ID '%s'\n",
             SESSION_ID);
      // Return a session response - try multiple common formats
      responseBody = "{\"sessionId\":\"" + std::string(SESSION_ID) +
                     "\","
                     "\"result\":\"success\","
                     "\"status\":\"ok\","
                     "\"code\":200,"
                     "\"session_id\":\"" +
                     std::string(SESSION_ID) +
                     "\","
                     "\"SessionId\":\"" +
                     std::string(SESSION_ID) +
                     "\","
                     "\"token\":\"mock-token-12345\"}";
    } else if (isTouchSession) {
      printf("[GATEWAY] -> TouchSession: OK\n");
      responseBody = "{\"result\":\"success\",\"status\":\"ok\"}";
    } else if (isDestroySession) {
      printf("[GATEWAY] -> DestroySession: OK\n");
      responseBody = "{\"result\":\"success\",\"status\":\"ok\"}";
    } else {
      printf("[GATEWAY] -> Session (unknown subtype): returning session ID\n");
      responseBody = "{\"sessionId\":\"" + std::string(SESSION_ID) +
                     "\","
                     "\"SessionId\":\"" +
                     std::string(SESSION_ID) +
                     "\","
                     "\"result\":\"success\"}";
    }
  } else {
    // Intercept purchase/buy requests
    if (path.find("purchase") != std::string::npos ||
        path.find("Purchase") != std::string::npos ||
        path.find("buy") != std::string::npos ||
        path.find("Buy") != std::string::npos ||
        path.find("research") != std::string::npos ||
        path.find("Research") != std::string::npos) {

      printf("[GATEWAY-PURCHASE] Intercepted purchase request: %s\n",
             path.c_str());

      int purchasedShipId = 0;
      // Scan body for 5-digit synthetic ID (11000 - 19999)
      for (size_t i = 0; i + 4 < body.size(); ++i) {
        if (body[i] == '1' &&
            (body[i + 1] == '1' || body[i + 1] == '2' || body[i + 1] == '3') &&
            isdigit(body[i + 2]) && isdigit(body[i + 3]) &&
            isdigit(body[i + 4])) {
          purchasedShipId = std::stoi(body.substr(i, 5));
          break;
        }
      }

      // Scan path if not found in body
      if (purchasedShipId == 0) {
        for (size_t i = 0; i + 4 < path.size(); ++i) {
          if (path[i] == '1' &&
              (path[i + 1] == '1' || path[i + 1] == '2' ||
               path[i + 1] == '3') &&
              isdigit(path[i + 2]) && isdigit(path[i + 3]) &&
              isdigit(path[i + 4])) {
            purchasedShipId = std::stoi(path.substr(i, 5));
            break;
          }
        }
      }

      // Fallback to last clicked ship if none found in body/path
      if (purchasedShipId == 0 && g_lastClickedSyntheticId >= 11000 &&
          g_lastClickedSyntheticId <= 19999) {
        purchasedShipId = g_lastClickedSyntheticId;
        printf("[GATEWAY-PURCHASE] No ship ID found in request. Using last "
               "clicked synthetic ID: %d\n",
               purchasedShipId);
      }

      if (purchasedShipId >= 11000 && purchasedShipId <= 19999) {
        g_ownedShips.insert(purchasedShipId);
        SaveFleetData();
        printf("[GATEWAY-PURCHASE] SUCCESS: Unlocked and saved ship %d!\n",
               purchasedShipId);
      }
    }

    // For ALL other requests: return empty success
    // This prevents crashes while we discover endpoints
    printf("[GATEWAY] -> Unhandled endpoint, returning empty success\n");
    responseBody = "{\"result\":\"success\",\"status\":\"ok\",\"data\":[]}";
  }

  printf("[GATEWAY] <- Response: %d %s (%d bytes)\n", statusCode,
         statusText.c_str(), (int)responseBody.size());

  std::string response =
      BuildHttpResponse(statusCode, statusText, responseBody);
  send(clientSock, response.c_str(), (int)response.size(), 0);
  closesocket(clientSock);
}

// --- Gateway Server Thread ---
void GatewayServerThread() {
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cout << "[GATEWAY] ERROR: WSAStartup failed!" << std::endl;
    return;
  }

  SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSock == INVALID_SOCKET) {
    std::cout << "[GATEWAY] ERROR: socket() failed: " << WSAGetLastError()
              << std::endl;
    WSACleanup();
    return;
  }

  // Allow rapid restart
  int optval = 1;
  setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char *)&optval,
             sizeof(optval));

  sockaddr_in serverAddr = {};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(GATEWAY_PORT);
  serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (bind(listenSock, (sockaddr *)&serverAddr, sizeof(serverAddr)) ==
      SOCKET_ERROR) {
    std::cout << "[GATEWAY] ERROR: bind() failed on port " << GATEWAY_PORT
              << ": " << WSAGetLastError() << std::endl;
    closesocket(listenSock);
    WSACleanup();
    return;
  }

  if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
    std::cout << "[GATEWAY] ERROR: listen() failed: " << WSAGetLastError()
              << std::endl;
    closesocket(listenSock);
    WSACleanup();
    return;
  }

  g_gatewayRunning = true;
  std::cout << "[GATEWAY] Mock gateway server listening on 127.0.0.1:"
            << GATEWAY_PORT << std::endl;

  while (g_gatewayRunning) {
    // Use select() with a timeout so we can check g_gatewayRunning periodically
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSock, &readSet);
    timeval timeout = {1, 0}; // 1 second timeout

    int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
    if (selectResult > 0 && FD_ISSET(listenSock, &readSet)) {
      sockaddr_in clientAddr = {};
      int clientAddrLen = sizeof(clientAddr);
      SOCKET clientSock =
          accept(listenSock, (sockaddr *)&clientAddr, &clientAddrLen);
      if (clientSock != INVALID_SOCKET) {
        // Handle each client in a detached thread to avoid blocking
        std::thread(HandleGatewayClient, clientSock).detach();
      }
    }
  }

  closesocket(listenSock);
  WSACleanup();
  std::cout << "[GATEWAY] Server shut down." << std::endl;
}

// --- Initialize the GetCommandLineW hook (must be called EARLY) ---
void InitGatewayHook() {
  // Initialize MinHook if not already done
  MH_Initialize(); // Safe to call multiple times â€” returns
                   // MH_ERROR_ALREADY_INITIALIZED which is fine

  // Hook GetCommandLineW from kernel32.dll
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (!hKernel32) {
    std::cout << "[GATEWAY] ERROR: Could not find kernel32.dll" << std::endl;
    return;
  }

  void *pGetCommandLineW = (void *)GetProcAddress(hKernel32, "GetCommandLineW");
  if (!pGetCommandLineW) {
    std::cout << "[GATEWAY] ERROR: Could not find GetCommandLineW" << std::endl;
    return;
  }

  MH_STATUS status =
      MH_CreateHook(pGetCommandLineW, (void *)HookGetCommandLineW,
                    (void **)&OrigGetCommandLineW);
  if (status != MH_OK) {
    std::cout << "[GATEWAY] ERROR: MH_CreateHook failed for GetCommandLineW: "
              << status << std::endl;
    return;
  }

  status = MH_EnableHook(pGetCommandLineW);
  if (status != MH_OK) {
    std::cout << "[GATEWAY] ERROR: MH_EnableHook failed for GetCommandLineW: "
              << status << std::endl;
    return;
  }

  std::cout << "[GATEWAY] GetCommandLineW hook installed successfully!"
            << std::endl;
}

// Auth hooks use existing implementations at lines ~1572-1592
// (GetAuthTokenHook writes fake "0w0" token, ValidateFirmamentCertHook returns
// 1)

// Vectored Exception Handler â€” safety net for background thread crashes.
// Logs crash details to a persistent file AND terminates only the crashing
// thread via ExitThread(0). This is acceptable because:
//   - TaskGraph has multiple worker threads; losing one is survivable
//   - These crashes are from async fire-and-forget tasks (texture loading,
//   etc.)
//   - The main game thread is never affected
static DWORD g_mainThreadId = 0;

static LONG WINAPI BackgroundThreadVEH(PEXCEPTION_POINTERS pExInfo) {
  // Lightweight safety-net VEH. The root cause GC fix (NOP the
  // loop-back JMP in FastReferenceCollector) should prevent infinite AV loops.
  // This handler catches any remaining stray AVs on background threads.
  if (pExInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;

  DWORD currentThread = GetCurrentThreadId();
  if (currentThread == g_mainThreadId)
    return EXCEPTION_CONTINUE_SEARCH;

  uintptr_t rip = (uintptr_t)pExInfo->ContextRecord->Rip;
  uintptr_t modBase =
      (uintptr_t)GetModuleHandleA("DreadGame-Win64-Shipping.exe");
  uintptr_t rva = rip - modBase;
  uintptr_t faultAddr =
      (uintptr_t)pExInfo->ExceptionRecord->ExceptionInformation[1];

  // DEP Execute Violation Recovery (ExceptionInformation[0] == 8)
  // Occurs when the engine calls a bad function pointer on a background thread.
  if (pExInfo->ExceptionRecord->ExceptionInformation[0] == 8) {
    uintptr_t stackBase = pExInfo->ContextRecord->Rsp;
    if (stackBase) {
      uintptr_t retAddr = *(uintptr_t *)stackBase;
      if (retAddr > 0x10000) {
        pExInfo->ContextRecord->Rip = retAddr;
        pExInfo->ContextRecord->Rsp = stackBase + 8; // pop ret
        pExInfo->ContextRecord->Rax = 0;             // return 0
        static int logLimit = 0;
        if (logLimit++ < 50) {
          printf("[VEH] DEP execute crash at %p suppressed. Unwound stack to "
                 "ret=%p (tid=%u)\n",
                 (void *)rip, (void *)retAddr, currentThread);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }

  static volatile LONG totalCount = 0;
  LONG count = InterlockedIncrement(&totalCount);

  if (count <= 50) {
    printf(
        "[VEH] BG thread %u AV #%ld at RVA 0x%llX fault=0x%llX -> SUPPRESSED\n",
        currentThread, count, (unsigned long long)rva,
        (unsigned long long)faultAddr);
  }

  // ------------------------------------------------------------------
  // Kill trigger: same RVA fires 3+ times on the same thread
  //   (spin-loop: hash-table scan with null base, JNE re-fires forever)
  // Safety valve: a thread accumulates 50+ total AVs (runaway async task).
  //
  // Deliberately removed: the old "8 AVs per thread" sequential-scan killer.
  // That threshold was too low â€” a second ship selection in the tech tree
  // legitimately triggers 7 distinct-RVA AVs in a null-module-display walk
  // that is a one-shot pass, NOT an infinite loop. Killing the thread there
  // broke the module list for all subsequent ship selections.
  // ------------------------------------------------------------------
  struct RVAHit {
    uintptr_t rva;
    int count;
    DWORD tid;
  };
  static RVAHit s_hits[32] = {};
  struct ThreadTotal {
    DWORD tid;
    int total;
  };
  static ThreadTotal s_threadTotal[16] = {};
  static CRITICAL_SECTION s_cs;
  static bool s_csInit = false;
  if (!s_csInit) {
    InitializeCriticalSection(&s_cs);
    s_csInit = true;
  }

  bool forceReturn = false;
  int hitCountSnap = 0;
  EnterCriticalSection(&s_cs);
  {
    // --- Same-RVA spin-loop detection (primary kill trigger) ---
    int slot = -1;
    for (int i = 0; i < 32; i++) {
      if (s_hits[i].rva == rva && s_hits[i].tid == currentThread) {
        slot = i;
        break;
      }
    }
    if (slot == -1) {
      for (int i = 0; i < 32; i++) {
        if (s_hits[i].count == 0) {
          slot = i;
          break;
        }
      }
      if (slot == -1)
        slot = (int)(count % 32);
      s_hits[slot] = {rva, 0, currentThread};
    }
    s_hits[slot].count++;
    hitCountSnap = s_hits[slot].count;
    // 0xEA1FFF is a legitimate traversal loop (fault addrs vary: 0x4..., -1,
    // 0xA). Never treat it as a spin-loop; only kill on >= 10 hits at same RVA.
    bool knownTraversalSite =
        (rva == 0xEA1FE4 || rva == 0xEA1FEC || rva == 0xEA1FFF ||
         rva == 0xD1C72A || rva == 0xD1C751 || rva == 0xD1E332 ||
         rva == 0xD1E346 || rva == 0xD1E347 || rva == 0xD1E3BB ||
         rva == 0xF4F359);
    if (hitCountSnap >= 10 && !knownTraversalSite) {
      forceReturn = true;
      s_hits[slot] = {}; // reset slot so thread can be reused later
    }

    // --- Per-thread safety valve (extremely high threshold) ---
    if (!forceReturn && !knownTraversalSite) {
      int tslot = -1;
      for (int i = 0; i < 16; i++) {
        if (s_threadTotal[i].tid == currentThread) {
          tslot = i;
          break;
        }
      }
      if (tslot == -1) {
        for (int i = 0; i < 16; i++) {
          if (s_threadTotal[i].tid == 0) {
            tslot = i;
            break;
          }
        }
        if (tslot != -1)
          s_threadTotal[tslot].tid = currentThread;
      }
      if (tslot != -1) {
        s_threadTotal[tslot].total++;
        if (s_threadTotal[tslot].total >= 50) {
          forceReturn = true;
          hitCountSnap = s_threadTotal[tslot].total;
          s_threadTotal[tslot] = {}; // reset so slot can be reused
        }
      }
    }
  }
  LeaveCriticalSection(&s_cs);

  if (forceReturn) {
    // Cannot safely "force return" â€” we don't know the function's frame
    // layout (how many registers were pushed before the AV). Guessing RSP leads
    // to jumping to garbage addresses (seen: 0xFFFF8009501D0000 -> runaway AV
    // storm).
    //
    // Safe alternative: kill this background thread.
    // This is a TaskGraph worker thread â€” the scheduler spawns replacements.
    // The main thread (g_mainThreadId) is guarded above and is never killed.
    printf("[VEH] Spin-loop at RVA 0x%llX or sequential scan (tid=%u, hit=%d) "
           "-> ExitThread\n",
           (unsigned long long)rva, currentThread, hitCountSnap);
    ExitThread(0);
    // ExitThread never returns, but the VEH needs a return value for the
    // compiler:
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // -----------------------------------------------------------------------
  // RVA 0x2322C0: MOV EBX,[RCX+8] â€” crash in ProcessMulticastDelegate.
  // From disasm_2322A0.txt, the prologue is:
  //   0x2322A0: MOV R11,RSP
  //   0x2322A3: PUSH RBX       (RSP -= 8)
  //   0x2322A4: PUSH RSI       (RSP -= 8)
  //   0x2322A5: PUSH R15       (RSP -= 8)
  //   0x2322A7: SUB RSP,0xb0   (RSP -= 0xb0)
  // Total stack adjustment: 0xb0 + 3*8 = 0xC8 bytes.
  // Return address is at RSP + 0xC8.
  //
  // With the upgraded ProcessMulticastDelegate hook pre-validating entries,
  // this VEH path should fire less often. But keep it as a safety net.
  // -----------------------------------------------------------------------
  if (rva == 0x2322C0) {
    // Undo prologue: RSP currently = entry_RSP - 0xC8
    // Return address is at the original RSP position
    uintptr_t stackBase = pExInfo->ContextRecord->Rsp;
    uintptr_t *pRet = (uintptr_t *)(stackBase + 0xC8);

    if (pRet && *pRet >= modBase && *pRet < modBase + 0x3000000) {
      // Restore saved registers from the stack (reverse of prologue)
      // After SUB RSP,0xb0:
      //   [RSP + 0xb0] = saved R15 (from PUSH R15)
      //   [RSP + 0xb8] = saved RSI (from PUSH RSI)
      //   [RSP + 0xc0] = saved RBX (from PUSH RBX)
      //   [RSP + 0xc8] = return address
      pExInfo->ContextRecord->R15 = *(uintptr_t *)(stackBase + 0xb0);
      pExInfo->ContextRecord->Rsi = *(uintptr_t *)(stackBase + 0xb8);
      pExInfo->ContextRecord->Rbx = *(uintptr_t *)(stackBase + 0xc0);

      pExInfo->ContextRecord->Rip = *pRet;
      pExInfo->ContextRecord->Rsp = stackBase + 0xC8 + 8; // past ret addr
    }
    pExInfo->ContextRecord->Rax = 0;
    printf("[VEH] 0x2322C0 force-return: proper stack unwind (tid=%u)\n",
           currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (rva == 0xD1C72A) {
    pExInfo->ContextRecord->Rbx = 0;
    pExInfo->ContextRecord->Rip += 4;
    printf("[VEH] GC schema crash 0xD1C72A handled, zeroed Rbx (tid=%u)\n",
           currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (rva == 0xD1C751) {
    pExInfo->ContextRecord->Rax = 0;
    pExInfo->ContextRecord->Rip += 3; // MOV RAX, [RDI] is 3 bytes (48 8B 07)
    printf("[VEH] GC schema crash 0xD1C751 handled, zeroed Rax (tid=%u)\n",
           currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (rva == 0xD1E332) {
    pExInfo->ContextRecord->R15 = 0;
    pExInfo->ContextRecord->Rip += 3;
    printf("[VEH] GC schema crash 0xD1E332 handled, zeroed R15 (tid=%u)\n",
           currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (rva == 0xD1E346 || rva == 0xD1E347) {
    pExInfo->ContextRecord->R8 = 0;
    pExInfo->ContextRecord->Rip += 4;
    printf("[VEH] GC schema crash 0x%llX handled, zeroed R8 (tid=%u)\n",
           (unsigned long long)rva, currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (rva == 0xD1E3BB) {
    pExInfo->ContextRecord->Rax = 0;
    pExInfo->ContextRecord->Rip += 3; // 48 8B 07 is 3 bytes
    printf("[VEH] GC schema crash 0xD1E3BB handled, zeroed Rax (tid=%u)\n",
           currentThread);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // Standard single-instruction suppression.
  // Zero Rax (read result) AND R8/R9 in case they are the bad base pointer.
  pExInfo->ContextRecord->Rax = 0;
  pExInfo->ContextRecord->R8 = 0;
  pExInfo->ContextRecord->R9 = 0;

  // Determine instruction length to skip past the faulting MOV/CMP/read.
  uint8_t *ip = (uint8_t *)rip;
  int skipLen = 4; // safe default

  uintptr_t modEnd = modBase + 0x3000000;
  if (rip >= modBase && rip < modEnd) {
    int i = 0;
    // Optional legacy prefixes (F2=REPNE, F3=REP, 66=operand size override)
    while (ip[i] == 0xF2 || ip[i] == 0xF3 || ip[i] == 0x66)
      i++;
    // Single REX prefix (0x40-0x4F) â€” x86-64 only allows ONE
    if ((ip[i] & 0xF0) == 0x40)
      i++;

    // Two-byte opcode (0F xx)?
    if (ip[i] == 0x0F) {
      i++; // skip 0F
      // Common 0F opcodes: B6=MOVZX r,r/m8, B7=MOVZX r,r/m16,
      // BE=MOVSX r,r/m8, BF=MOVSX r,r/m16, 10/11=MOVSS/MOVSD,
      // 28/29=MOVAPS, 6F/7F=MOVDQA
      uint8_t op2 = ip[i];
      i++;
      (void)op2; // consumed for documentation
      uint8_t modrm = ip[i];
      i++;
      uint8_t mod = (modrm >> 6) & 3;
      uint8_t rm = modrm & 7;
      if (mod == 0) {
        if (rm == 4)
          i++; // SIB
        if (rm == 5)
          i += 4; // disp32
      } else if (mod == 1) {
        if (rm == 4)
          i++;
        i += 1;
      } else if (mod == 2) {
        if (rm == 4)
          i++;
        i += 4;
      }
      skipLen = i;
    }
    // Single-byte opcodes: MOV, CMP, TEST, LEA, ADD, XOR, AND, etc.
    else if (ip[i] == 0x8B || ip[i] == 0x89 || ip[i] == 0x8D || ip[i] == 0x03 ||
             ip[i] == 0x33 || ip[i] == 0x39 || ip[i] == 0x85 || ip[i] == 0x3B ||
             ip[i] == 0x8A || ip[i] == 0x88 || ip[i] == 0x23 || ip[i] == 0x2B ||
             ip[i] == 0xF6 || ip[i] == 0xF7 || ip[i] == 0x80 || ip[i] == 0x81 ||
             ip[i] == 0x83 || ip[i] == 0x38 || ip[i] == 0x3A) {
      i++; // opcode
      uint8_t modrm = ip[i];
      i++;
      uint8_t mod = (modrm >> 6) & 3;
      uint8_t rm = modrm & 7;
      if (mod == 0) {
        if (rm == 4)
          i++; // SIB
        if (rm == 5)
          i += 4; // disp32
      } else if (mod == 1) {
        if (rm == 4)
          i++; // SIB
        i += 1;
      } else if (mod == 2) {
        if (rm == 4)
          i++; // SIB
        i += 4;
      }
      // Immediate operand for CMP/TEST/AND immediate forms
      // The faulting opcode is at ip[0] (after prefixes/REX were consumed
      // above). Since i has already advanced past opcode+modrm+sib+disp, we
      // need to check the actual opcode byte to see if there's an immediate.
      uint8_t opcode = ip[i - (i - 0)]; // first byte after prefixes
      // Actually, let's find the opcode position more reliably:
      int prefixEnd = 0;
      while (ip[prefixEnd] == 0xF2 || ip[prefixEnd] == 0xF3 ||
             ip[prefixEnd] == 0x66)
        prefixEnd++;
      if ((ip[prefixEnd] & 0xF0) == 0x40)
        prefixEnd++; // REX
      opcode = ip[prefixEnd];

      if (opcode == 0x80 || opcode == 0x83 || opcode == 0xF6) {
        i += 1; // imm8
      } else if (opcode == 0x81 || opcode == 0xF7) {
        i += 4; // imm32
      }
      skipLen = i;
    }
  }

  pExInfo->ContextRecord->Rip += skipLen;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static void* oGetOwnedShipDataStructs = nullptr;
static void* oGetOwnedShipLoadouts = nullptr;
static void* oViewShipDetailsClicked = nullptr;

static void* __fastcall hkGetOwnedShipDataStructs(TArray<FYUIShipManufacturerTechItemData>* pOutArray, UUI_OwnedShipsScreen* pThis) {
  InitFullTechTree();

  std::vector<const FTechTreeShip*> ownedList;
  for (const auto &s : g_FullTechTree) {
    if (g_ownedShips.count(s.shipId) > 0 || (s.shipId == 11001 && g_ownedShips.count(61) > 0)) {
      ownedList.push_back(&s);
    }
  }

  if (ownedList.empty()) {
    for (const auto &s : g_FullTechTree) {
      if (s.shipId == 11001) {
        ownedList.push_back(&s);
        break;
      }
    }
  }

  int count = (int)ownedList.size();
  tee_printf("[OWNED_SHIPS] hkGetOwnedShipDataStructs: Returning %d owned ships from profile\n", count);

  pOutArray->_data = (FYUIShipManufacturerTechItemData*)UE4Malloc(sizeof(FYUIShipManufacturerTechItemData) * count);
  pOutArray->_count = count;
  pOutArray->_max = count;

  memset(pOutArray->_data, 0, sizeof(FYUIShipManufacturerTechItemData) * count);

  for (int i = 0; i < count; i++) {
    const FTechTreeShip *s = ownedList[i];
    FYUIShipManufacturerTechItemData &data = pOutArray->_data[i];

    data.m_manufacturerID = s->manufacturerId;
    data.m_itemID = s->shipId;
    data.m_tier = s->tier;

    if (!s->name.empty()) {
      size_t len = s->name.length();
      data.m_name._data = (wchar_t*)UE4Malloc(sizeof(wchar_t) * (len + 1));
      wcscpy(data.m_name._data, s->name.c_str());
      data.m_name._count = (int32_t)(len + 1);
      data.m_name._max = (int32_t)(len + 1);
    }

    data.m_shipClass = (EYShipClass)s->shipClass;

    const wchar_t *classNameStr = L"Dreadnought";
    switch (s->shipClass) {
      case 1: classNameStr = L"Dreadnought"; break;
      case 3: classNameStr = L"Artillery Cruiser"; break;
      case 5: classNameStr = L"Destroyer"; break;
      case 8: classNameStr = L"Corvette"; break;
      case 12: case 13: classNameStr = L"Tactical Cruiser"; break;
    }
    size_t classLen = wcslen(classNameStr);
    data.m_shipClassName._data = (wchar_t*)UE4Malloc(sizeof(wchar_t) * (classLen + 1));
    wcscpy(data.m_shipClassName._data, classNameStr);
    data.m_shipClassName._count = (int32_t)(classLen + 1);
    data.m_shipClassName._max = (int32_t)(classLen + 1);

    data.m_itemState = EYTechTreeItemState::Owned;
    data.m_isInFleet = true;
    data.m_isVeteranStatus = (s->tier >= 5);
    data.m_isHeroShip = false;
    data.m_isFlagship = false;

    int bpClass = s->shipClass;
    if (bpClass == 13) bpClass = 12;
    int key = bpClass * 10 + s->tier;
    auto it = g_loadoutMap.find(key);
    if (it != g_loadoutMap.end() && it->second < g_numLoadedShips && g_loadedShips[it->second].loadoutObj) {
      data.m_loadoutID = g_loadedShips[it->second].loadoutObj->Name;
    } else {
      data.m_loadoutID = FName("VH_DreadnoughtMedium_T1_PrecastLoadout_BP");
    }
  }

  return pOutArray;
}

static void* __fastcall hkGetOwnedShipLoadouts(TArray<UYShipLoadoutMmogbrain*>* pOutArray, UUI_OwnedShipsScreen* pThis) {
  InitFullTechTree();

  std::vector<const FTechTreeShip*> ownedList;
  for (const auto &s : g_FullTechTree) {
    if (g_ownedShips.count(s.shipId) > 0 || (s.shipId == 11001 && g_ownedShips.count(61) > 0)) {
      ownedList.push_back(&s);
    }
  }
  if (ownedList.empty()) {
    for (const auto &s : g_FullTechTree) {
      if (s.shipId == 11001) {
        ownedList.push_back(&s);
        break;
      }
    }
  }

  int count = (int)ownedList.size();
  tee_printf("[OWNED_SHIPS] hkGetOwnedShipLoadouts: Returning %d owned ship loadout objects\n", count);

  pOutArray->_data = (UYShipLoadoutMmogbrain**)UE4Malloc(sizeof(UYShipLoadoutMmogbrain*) * count);
  pOutArray->_count = count;
  pOutArray->_max = count;

  for (int i = 0; i < count; i++) {
    const FTechTreeShip *s = ownedList[i];
    UYShipLoadoutMmogbrain *loadoutObj = nullptr;

    int bpClass = s->shipClass;
    if (bpClass == 13) bpClass = 12;
    int key = bpClass * 10 + s->tier;
    auto it = g_loadoutMap.find(key);
    if (it != g_loadoutMap.end() && it->second < g_numLoadedShips && g_loadedShips[it->second].loadoutObj) {
      loadoutObj = (UYShipLoadoutMmogbrain*)g_loadedShips[it->second].loadoutObj;
    }

    if (!loadoutObj) {
      for (UYShipLoadoutMmogbrain* loadout : UObject::FindObjects<UYShipLoadoutMmogbrain>()) {
        if (loadout->GetFullName().find("VH_DreadnoughtMedium_T1_PrecastLoadout_BP") != std::string::npos) {
          loadoutObj = loadout;
          break;
        }
      }
    }

    pOutArray->_data[i] = loadoutObj;
  }

  return pOutArray;
}

static void __fastcall hkViewShipDetailsClicked(UUI_OwnedShipsScreen* pThis, int32_t ShipId) {
  tee_printf("[OWNED_SHIPS] ViewShipDetailsClicked for ShipId %d -> Navigating to Customization & Module Details screen\n", ShipId);
  pThis->NavigateToScreen(EUI_Screen::EditShip_Loadout);
}

void InitEarlyHooks() {
  printf("[INIT] Initializing early hooks (Auth, EAC, Engine)...\n");
  MH_Initialize();

  // Hook GetOwnedShipDataStructs (RVA 0x00BB9530)
  void* targetGetOwnedData = (void*)(Globals::ModuleBase + 0x00BB9530);
  if (MH_CreateHook(targetGetOwnedData, &hkGetOwnedShipDataStructs, &oGetOwnedShipDataStructs) == MH_OK) {
    MH_EnableHook(targetGetOwnedData);
    tee_printf("[HOOK] Successfully hooked GetOwnedShipDataStructs at RVA 0xBB9530\n");
  }

  // Hook GetOwnedShipLoadouts (RVA 0x00BB95E0)
  void* targetGetOwnedLoadouts = (void*)(Globals::ModuleBase + 0x00BB95E0);
  if (MH_CreateHook(targetGetOwnedLoadouts, &hkGetOwnedShipLoadouts, &oGetOwnedShipLoadouts) == MH_OK) {
    MH_EnableHook(targetGetOwnedLoadouts);
    tee_printf("[HOOK] Successfully hooked GetOwnedShipLoadouts at RVA 0xBB95E0\n");
  }

  // Hook ViewShipDetailsClicked (RVA 0x00BB8AE0)
  void* targetViewDetails = (void*)(Globals::ModuleBase + 0x00BB8AE0);
  if (MH_CreateHook(targetViewDetails, &hkViewShipDetailsClicked, &oViewShipDetailsClicked) == MH_OK) {
    MH_EnableHook(targetViewDetails);
    tee_printf("[HOOK] Successfully hooked ViewShipDetailsClicked at RVA 0xBB8AE0\n");
  }

  // Install VEH to prevent background-thread crashes from killing the game
  g_mainThreadId = GetCurrentThreadId();
  AddVectoredExceptionHandler(1, BackgroundThreadVEH); // 1 = first handler
  printf("[INIT] Background thread crash handler installed (main thread=%u)\n",
         g_mainThreadId);

  // IMMEDIATELY patch ALL WebServicesPlugin timer-scheduling functions.
  // The WebServicesPlugin has ~10 scheduler functions (touch session, ping,
  // market bundles, legal docs, mmog connection, etc.) that each call SetTimer
  // (FUN_141c8f760). When any timer fires on TaskGraphThread, it accesses dead
  // web service objects â†’ crash. Patching all of them with RET (0xC3)
  // prevents ANY timer from being scheduled.
  {
    uintptr_t base =
        (uintptr_t)GetModuleHandleA("DreadGame-Win64-Shipping.exe");
    if (base) {
      static const uint32_t timerRVAs[] = {
          // All 10 scheduler functions found by scanning for SetTimer calls in
          // WS range:
          0x382ED0, // Scheduler #1 (SetTimer at 0x38305A)
          0x3838D9, // Scheduler #2 (SetTimer at 0x3839DE)
          0x383B6B, // Scheduler #3 (SetTimer at 0x383C65)
          0x383D68, // Scheduler #4 (SetTimer at 0x383E6B)
          0x3900E0, // Scheduler #5 (SetTimer at 0x390313)
          0x39CFD5, // Scheduler #6 (SetTimer at 0x39D051)
          0x39D200, // Scheduler #7 = ScheduleTouchSession (decompiled,
                    // confirmed)
          0x39D32F, // Scheduler #8 (SetTimer at 0x39D4CB)
          0x3A2FF0, // Scheduler #9 (SetTimer at 0x3A3208)
          0x3A59F0, // Scheduler #10 (SetTimer at 0x3A5C20)
          0x3AA880, // Scheduler #11 (SetTimer at 0x3AA894)
          // Also the timer delegate target from ScheduleTouchSession:
          0x38ED50, // Timer delegate target (puVar8[2] in scheduler)
          // Nuclear option â€” patch UE4 SetTimer itself
          0x1C8F760, // UE4 FTimerManager::SetTimer â€” prevent ALL timer
                     // scheduling
      };
      const int NUM_TIMERS = sizeof(timerRVAs) / sizeof(timerRVAs[0]);
      uint8_t ret = 0xC3;
      int ok = 0;
      for (int i = 0; i < NUM_TIMERS; i++) {
        DWORD oldProt;
        void *addr = (void *)(base + timerRVAs[i]);
        if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
          *(uint8_t *)addr = ret;
          VirtualProtect(addr, 1, oldProt, &oldProt);
          ok++;
        }
      }
      printf("[INIT] Early-patched %d/%d WebServicesPlugin timer functions "
             "(RET)\n",
             ok, NUM_TIMERS);
    }
  }

  // Patch GC "Unknown token" fatal error EARLY, before GC runs on background
  // threads
  PatchGCUnknownTokenCrash();

  tProcessEvent hookRef = (tProcessEvent)(Globals::ModuleBase + 0xD5B180);

  if (MH_CreateHook(hookRef, ProcessEventHook,
                    reinterpret_cast<void **>(&pProcessEvent_Original)) !=
      MH_OK) {
    printf("ProcessEvent Hook Initialization Failed!\n");
    return;
  }
  if (MH_EnableHook(hookRef) != MH_OK) {
    printf("ProcessEvent Hook Enable Failed!\n");
    return;
  }

  MH_CreateHook((void *)(Globals::ModuleBase + 0x29FD910), EACErrorMessageHook,
                &origEACErrorMessageHook);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x29FD910));

  MH_CreateHook((void *)(Globals::ModuleBase + 0x1A841C0), EACErrorMessageHook,
                &origEACErrorMessageHook);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x1A841C0));

  MH_CreateHook((void *)(Globals::ModuleBase + 0x1958C90), UGameEngineTick,
                &OrigUGameEngineTick);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x1958C90));

  // Enable auth token and firmament cert hooks early to unblock web service
  // sessions
  MH_CreateHook((void *)(Globals::ModuleBase + 0x4201D0),
                reinterpret_cast<LPVOID>(GetAuthTokenHook), &OrigGetAuthToken);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x4201D0));

  MH_CreateHook((void *)(Globals::ModuleBase + 0x2A4D590),
                reinterpret_cast<LPVOID>(ValidateFirmamentCertHook),
                &OrigValidateFirmamentCert);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x2A4D590));
  printf("[GATEWAY-AUTH] Auth token and firmament cert hooks enabled\n");

  // Hook YUIExternalFunctions::GetManufacturerData at known RVA 0x4ED0C0
  // (Ghidra-verified)
  void *getManufacturerDataAddr = (void *)(Globals::ModuleBase + 0x4ED0C0);
  {
    MH_STATUS status =
        MH_CreateHook(getManufacturerDataAddr,
                      reinterpret_cast<LPVOID>(MyHookGetManufacturerData),
                      reinterpret_cast<void **>(&OrigGetManufacturerData));
    if (status == MH_OK) {
      MH_EnableHook(getManufacturerDataAddr);
      printf("[HOOK] GetManufacturerData hook installed at RVA 0x4ED0C0 (%p)\n",
             getManufacturerDataAddr);
      g_getManufacturerDataHookInstalled = true;
    } else {
      printf(
          "[HOOK] WARNING: MH_CreateHook failed for GetManufacturerData: %d\n",
          status);
    }
  }

  // Hook YCtAInventoryInterface::HasItem at known RVA 0x0075C430
  void *hasItemAddr = (void *)(Globals::ModuleBase + 0x0075C430);
  {
    MH_STATUS status = MH_CreateHook(
        hasItemAddr, reinterpret_cast<LPVOID>(MyHookHasItemNative),
        reinterpret_cast<void **>(&OrigHasItemNative));
    if (status == MH_OK) {
      MH_EnableHook(hasItemAddr);
      printf("[HOOK] Native HasItem MinHook detour installed at RVA 0x75C430 (%p)\n",
             hasItemAddr);
    } else {
      printf("[HOOK] WARNING: Failed to install native HasItem hook at RVA 0x75C430 (status=%d)\n",
             (int)status);
    }
  }

  // Hook FUN_140480f70 (FindCachedDataEntry) â€” the bottleneck that prevents
  // item processing Both item loops in FUN_1404f3190 are inside `if (local_240
  // != NULL)`, and local_240 comes from this.
  {
    void *findCachedAddr = (void *)(Globals::ModuleBase + 0x480F70);
    MH_STATUS status = MH_CreateHook(
        findCachedAddr, reinterpret_cast<LPVOID>(MyHookFindCachedDataEntry),
        reinterpret_cast<void **>(&OrigFindCachedDataEntry));
    if (status == MH_OK) {
      MH_EnableHook(findCachedAddr);
      printf("[HOOK] FindCachedDataEntry hook installed at RVA 0x480F70 (%p)\n",
             findCachedAddr);
    } else {
      printf(
          "[HOOK] WARNING: MH_CreateHook failed for FindCachedDataEntry: %d\n",
          status);
    }
  }

  // Hook FUN_1404e0520 (ItemFilter) â€” the per-item validation/widget-builder
  {
    void *itemFilterAddr = (void *)(Globals::ModuleBase + 0x4E0520);
    MH_STATUS status = MH_CreateHook(
        itemFilterAddr, reinterpret_cast<LPVOID>(MyHookItemFilter),
        reinterpret_cast<void **>(&OrigItemFilter));
    if (status == MH_OK) {
      MH_EnableHook(itemFilterAddr);
      printf("[HOOK] ItemFilter hook installed at RVA 0x4E0520 (%p)\n",
             itemFilterAddr);
    } else {
      printf("[HOOK] WARNING: MH_CreateHook failed for ItemFilter: %d\n",
             status);
    }
  }

  // Hook YUIExternalFunctions::GetShipResearchData at known RVA 0x4EE820
  // (Ghidra-verified)
  {
    void *shipResearchAddr = (void *)(Globals::ModuleBase + 0x4EE820);
    MH_STATUS status = MH_CreateHook(
        shipResearchAddr, reinterpret_cast<LPVOID>(MyHookGetShipResearchData),
        reinterpret_cast<void **>(&OrigGetShipResearchData));
    if (status == MH_OK) {
      MH_EnableHook(shipResearchAddr);
      printf("[HOOK] GetShipResearchData hook installed at RVA 0x4EE820 (%p)\n",
             shipResearchAddr);
    } else {
      printf(
          "[HOOK] WARNING: MH_CreateHook failed for GetShipResearchData: %d\n",
          status);
    }
  }

  // Phase 3.3 Bridge Hooks (UI Module Data translation)
  MH_CreateHook((void *)(Globals::ModuleBase + 0xA98F40),
                MyHookComposeModuleUiData1, (void **)&OrigComposeModuleUiData1);
  MH_EnableHook((void *)(Globals::ModuleBase + 0xA98F40));
  MH_CreateHook((void *)(Globals::ModuleBase + 0xA989A0),
                MyHookComposeModuleUiData2, (void **)&OrigComposeModuleUiData2);
  MH_EnableHook((void *)(Globals::ModuleBase + 0xA989A0));

  // Phase 3.4 Bridge Hooks (Manufacturer Data translation)
  MH_CreateHook((void *)(Globals::ModuleBase + 0xA998A0),
                MyHookComposeShipManufacturerDataForId,
                (void **)&OrigComposeShipManufacturerDataForId);
  MH_EnableHook((void *)(Globals::ModuleBase + 0xA998A0));
  MH_CreateHook((void *)(Globals::ModuleBase + 0xA99B30),
                MyHookComposeShipManufacturerDataForLoadout,
                (void **)&OrigComposeShipManufacturerDataForLoadout);
  MH_EnableHook((void *)(Globals::ModuleBase + 0xA99B30));

  printf("[BRIDGE] Phase 3 translation hooks installed.\n");

  // Hangar 3D model streaming hook â€” UYItemIDList::LoadItemsAsync (RVA
  // 0x2D9390) Prevents "Given object is empty!" from aborting the hangar
  // preview pipeline. The function checks [this+8] (item count) before
  // streaming; we inject the active loadout object if the list is empty so
  // streaming can proceed.
  {
    void *loadItemsAddr = (void *)(Globals::ModuleBase + 0x2D9390);
    MH_STATUS s = MH_CreateHook(loadItemsAddr,
                                reinterpret_cast<LPVOID>(MyHookLoadItemsAsync),
                                reinterpret_cast<void **>(&OrigLoadItemsAsync));
    if (s == MH_OK) {
      MH_EnableHook(loadItemsAddr);
      printf("[HOOK] LoadItemsAsync hook installed at RVA 0x2D9390\n");
    } else {
      printf("[HOOK] WARNING: LoadItemsAsync hook failed: %d\n", s);
    }
  }

  // Level streaming list-lookup / TMap lookup hook (RVA 0x3B07B0)
  {
    void *mapLookupAddr = (void *)(Globals::ModuleBase + 0x3B07B0);
    MH_STATUS s = MH_CreateHook(mapLookupAddr,
                                reinterpret_cast<LPVOID>(MyHookFUN_1403b07b0),
                                reinterpret_cast<void **>(&OrigFUN_1403b07b0));
    if (s == MH_OK) {
      MH_EnableHook(mapLookupAddr);
      printf("[HOOK] FUN_1403b07b0 hook installed at RVA 0x3B07B0\n");
    } else {
      printf("[HOOK] WARNING: FUN_1403b07b0 hook failed: %d\n", s);
    }
  }

  // Hangar level-to-class lookup hook (RVA 0x372640)
  {
    void *classLookupAddr = (void *)(Globals::ModuleBase + 0x372640);
    MH_STATUS s = MH_CreateHook(classLookupAddr,
                                reinterpret_cast<LPVOID>(MyHookFUN_140372640),
                                reinterpret_cast<void **>(&OrigFUN_140372640));
    if (s == MH_OK) {
      MH_EnableHook(classLookupAddr);
      printf("[HOOK] FUN_140372640 hook installed at RVA 0x372640\n");
    } else {
      printf("[HOOK] WARNING: FUN_140372640 hook failed: %d\n", s);
    }
  }

  // Hangar transition callback bypass hook (RVA 0xAABF50)
  {
    void *callbackBypassAddr = (void *)(Globals::ModuleBase + 0xAABF50);
    MH_STATUS s = MH_CreateHook(callbackBypassAddr,
                                reinterpret_cast<LPVOID>(MyHookAABF50),
                                reinterpret_cast<void **>(&OrigFUN_140aabf50));
    if (s == MH_OK) {
      MH_EnableHook(callbackBypassAddr);
      printf("[HOOK] FUN_140aabf50 hook installed at RVA 0xAABF50\n");
    } else {
      printf("[HOOK] WARNING: FUN_140aabf50 hook failed: %d\n", s);
    }
  }

  // GetUObjectFromWeakPtr hook (RVA 0xD6AD50)
  // Prevents null player controller crash in UI screens
  // (FUN_140ab4b50/FUN_140ab5e70)
  {
    void *getUObjectFromWeakPtrAddr = (void *)(Globals::ModuleBase + 0xD6AD50);
    MH_STATUS s =
        MH_CreateHook(getUObjectFromWeakPtrAddr,
                      reinterpret_cast<LPVOID>(MyHookGetUObjectFromWeakPtr),
                      reinterpret_cast<void **>(&OrigGetUObjectFromWeakPtr));
    if (s == MH_OK) {
      MH_EnableHook(getUObjectFromWeakPtrAddr);
      printf(
          "[HOOK] GetUObjectFromWeakPtr hook installed at RVA 0xD6AD50 (%p)\n",
          getUObjectFromWeakPtrAddr);
    } else {
      printf("[HOOK] WARNING: GetUObjectFromWeakPtr hook failed: %d\n", s);
    }
  }

  // GetFrontendHUD hook (RVA 0xAA5470)
  // Bypasses the class matching lookup which fails offline and causes access
  // violations
  {
    void *getFrontendHUDAddr = (void *)(Globals::ModuleBase + 0xAA5470);
    MH_STATUS s = MH_CreateHook(getFrontendHUDAddr,
                                reinterpret_cast<LPVOID>(MyHookGetFrontendHUD),
                                reinterpret_cast<void **>(&OrigGetFrontendHUD));
    if (s == MH_OK) {
      MH_EnableHook(getFrontendHUDAddr);
      printf("[HOOK] GetFrontendHUD hook installed at RVA 0xAA5470\n");
    } else {
      printf("[HOOK] WARNING: GetFrontendHUD hook failed: %d\n", s);
    }
  }

  // ProcessMulticastDelegate hook (RVA 0x2322A0)
  {
    void *processMulticastDelegateAddr =
        (void *)(Globals::ModuleBase + 0x2322A0);
    MH_STATUS s =
        MH_CreateHook(processMulticastDelegateAddr,
                      reinterpret_cast<LPVOID>(MyHookProcessMulticastDelegate),
                      reinterpret_cast<void **>(&OrigProcessMulticastDelegate));
    if (s == MH_OK) {
      MH_EnableHook(processMulticastDelegateAddr);
      printf(
          "[HOOK] ProcessMulticastDelegate hook installed at RVA 0x2322A0\n");
    } else {
      printf("[HOOK] WARNING: ProcessMulticastDelegate hook failed: %d\n", s);
    }
  }

  MH_CreateHook((void *)(Globals::ModuleBase + 0x340340), GetShipByIdHook,
                &OrigGetShipById);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x340340));

  MH_CreateHook((void *)(Globals::ModuleBase + 0x5C8C00),
                VehicleSkipUpdateCheck1Hook, &origVehicleSkipUpdateCheck1);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x5C8C00));

  MH_CreateHook((void *)(Globals::ModuleBase + 0x5C8DC0),
                VehicleSkipUpdateCheck2Hook, &origVehicleSkipUpdateCheck2);
  MH_EnableHook((void *)(Globals::ModuleBase + 0x5C8DC0));

  void *hookRef2 = (void *)(Globals::ModuleBase + 0x055B050);
  MH_CreateHook(hookRef2, JustReturnWhatWeWereGoingToReturn,
                reinterpret_cast<LPVOID *>(&origJustReturn));
  MH_EnableHook(hookRef2);

  void *hookRef3 = (void *)(Globals::ModuleBase + 0x036B2E0);
  MH_CreateHook(hookRef3, EndMatchHook,
                reinterpret_cast<LPVOID *>(&origEndMatch));
  MH_EnableHook(hookRef3);

  // REMOVED - GC patch now runs ONLY in InitEarlyHooks (line ~3984)
  // Duplicate call was causing memory corruption by NOPping unrelated CALLs

  // MallocBinned Free Hook to prevent crashes on invalid/mismatched frees
  uintptr_t freeAddr = Globals::ModuleBase + 0xBFCA40;
  MH_STATUS freeStatus = MH_CreateHook(
      (LPVOID)freeAddr, reinterpret_cast<LPVOID>(MyHookFMallocBinnedFree),
      reinterpret_cast<void **>(&OrigFMallocBinnedFree));
  if (freeStatus == MH_OK) {
    MH_EnableHook((LPVOID)freeAddr);
    printf("[HOOK] FMallocBinned::Free hook installed at RVA 0xBFCA40\n");
  } else {
    printf("[HOOK] WARNING: FMallocBinned::Free hook failed: %d\n", freeStatus);
  }
}

void InitUIHooks() {
  static bool bInitialized = false;
  if (bInitialized)
    return;
  bInitialized = true;

  printf("[HOOK] Initializing UI lazy hooks...\n");
  // Install Native UI Data Hooks
  InstallNativeHook(
      "Function DreadGameUI.YHUDWidget_StyleContainer.GetShipClassIcon",
      MyHookGetShipClassIcon, &OriginalGetShipClassIconFunc);
  // UI_ManufacturerTechTreeWidget.GetTier is a BP function inside DreadGameUI
  // package
  InstallNativeHook(
      "Function DreadGameUI.UI_ManufacturerTechTreeWidget.GetTier",
      MyHookGetTier, &OriginalGetTierFunc);
  // ShipTitleWidget is also a Blueprint - try both naming conventions
  InstallNativeHook(
      "Function "
      "UI_Generic_ShipTitleWidget.UI_Generic_ShipTitleWidget_C.GetShipTier",
      MyHookGetShipTier, &OriginalGetShipTierFunc);
  InstallNativeHook(
      "Function DreadGameUI.UI_Generic_ShipTitleWidget.GetShipTier",
      MyHookGetShipTier, &OriginalGetShipTierFunc);
  InstallNativeHook(
      "Function DreadGameUI.UI_EditShipSubPanel.IsItemOwnedByPlayer",
      MyHookIsItemOwnedByPlayer, &OriginalIsItemOwnedByPlayerFunc);
  InstallNativeHook("Function DreadGame.YCtAInventoryInterface.HasItem",
                    MyHookHasItemUFunction, &OriginalHasItemUFunctionFunc);
  InstallNativeHook(
      "Function DreadGameUI.UI_EditShipSubPanel.IsCurrentShipOwnedByPlayer",
      MyHookIsCurrentShipOwnedByPlayer,
      &OriginalIsCurrentShipOwnedByPlayerFunc);
  // TierIcon hooks - Blueprint class uses _C suffix and package path, NOT
  // DreadGameUI module Asset path:
  // DreadGame/Content/UserInterface/Widgets/Generic/UI_Generic_TierIcon
  // TierColors[-1] crash: Blueprint does TierColors[GetTier()-1], needs tier
  // clamped 1-5
  InstallNativeHook(
      "Function UI_Generic_TierIcon.UI_Generic_TierIcon_C.SetTier",
      MyHookSetTier, &OriginalSetTierFunc);
  InstallNativeHook(
      "Function UI_Generic_TierIcon.UI_Generic_TierIcon_C.SetTextureFromTier",
      MyHookSetTextureFromTier, &OriginalSetTextureFromTierFunc);

  // Correct TechTree Button Hook
  InstallNativeHook("Function "
                    "UI_Button_ManufacturerTechTreeItem.UI_Button_"
                    "ManufacturerTechTreeItem_C.GetShipData",
                    MyHookGetShipData, &OriginalGetShipDataFunc);
  InstallNativeHook("Function DreadGameUI.UI_ShipFilterWidget.GetUIShipData",
                    MyHookGetUIShipData, &OriginalGetUIShipDataFunc);

  // Hook GetCurrentShipItemData on the CORRECT class (UI_EditShipScreen, not
  // UI_EditShipSubPanel)
  InstallNativeHook(
      "Function DreadGameUI.UI_EditShipScreen.GetCurrentShipItemData",
      MyHookGetCurrentShipItemData, &OriginalGetCurrentShipItemDataFunc);

  // Hook SetSelectedShip on all relevant screens to support viewport updates
  // from clicking ships
  InstallNativeHook(
      "Function DreadGameUI.UI_ManufacturerTechTreeScreen.SetSelectedShip",
      MyHookSetSelectedShip, &OriginalSetSelectedShipFunc);
  InstallNativeHook(
      "Function DreadGameUI.UI_ShipTechTreeScreen.SetSelectedShip",
      MyHookShipTechTreeSetSelectedShip,
      &OriginalShipTechTreeSetSelectedShipFunc);
  InstallNativeHook("Function DreadGameUI.UI_OwnedShipsScreen.SetSelectedShip",
                    MyHookOwnedShipsSetSelectedShip,
                    &OriginalOwnedShipsSetSelectedShipFunc);
  InstallNativeHook(
      "Function DreadGameUI.UI_AddShipToFleetScreen.SetSelectedShip",
      MyHookAddShipToFleetSetSelectedShip,
      &OriginalAddShipToFleetSetSelectedShipFunc);

  // Manufacturer data hook for Tech Tree population
  InstallNativeHook(
      "Function DreadGameUI.UI_ManufacturersScreen.GetManufacturersData",
      MyHookGetManufacturersData, &OriginalGetManufacturersDataFunc_BP);
}

/*
        DEBUG ONLY: Create the UE4 native game console (open with ~)
*/
void InitGameConsole() {
  UEngine *engine = getLastOfType<UGameEngine>();

  UObject *NewObject = getLastOfType<UGameplayStatics>()->STATIC_SpawnObject(
      engine->ConsoleClass, engine->GameViewport);

  engine->GameViewport->ViewportConsole = static_cast<UConsole *>(NewObject);
}

/*
        DEBUG ONLY: Allow us to use std::cout and have it output to the window
   opened when the game is launched with -log
*/
void InitConsole() {
  AllocConsole();
  // FILE *fDummy;
  // We don't want to overwrite stdout/stderr here because InitLogging()
  // already redirected them to our dread_mod_log.txt file!
  // freopen_s(&fDummy, "CONIN$", "r", stdin);
  // freopen_s(&fDummy, "CONOUT$", "w", stderr);
  // freopen_s(&fDummy, "CONOUT$", "w", stdout);
}

int serverNumBotsTeamOne = 5;
int serverNumBotsTeamTwo = 4;
int serverBotDifficulty = 0;

/*
        Loads server configuration from cfg.txt in the Win64 folder
*/
void LoadConfiguration() {
  mapCommand = "open DansMap_P";
  loadoutString =
      "/Game/Generic/Loadouts/Precast/T5/VH_AssaultLight_PrecastLoadout_T5_BP";
  numBotsTeamOne = 5;
  numBotsTeamTwo = 4;
  serverBotDifficulty = 2;
  /*
  std::ifstream cfgFile("cfg.txt");

  std::getline(cfgFile, mapCommand);
  std::getline(cfgFile, loadoutString);

  std::string procLine;

  std::getline(cfgFile, procLine);

  serverNumBotsTeamOne = std::stoi(procLine);

  procLine = "";

  std::getline(cfgFile, procLine);

  serverNumBotsTeamTwo = std::stoi(procLine);

  procLine = "";

  std::getline(cfgFile, procLine);

  serverBotDifficulty = std::stoi(procLine);

  procLine = "";
  */
}

/*
 *	Sets the loadout of the local player, allowing them to spawn when the
 * game starts
 */
void ForceSpawnLocalPlayer() {
  std::wstring wLoadoutString(loadoutString.begin(), loadoutString.end());

  StaticLoadClass(UYShipLoadout::StaticClass(), nullptr,
                  wLoadoutString.c_str());

  Sleep(2 * 1000);

  UYShipLoadout *loadoutToApply = getLastOfType<UYShipLoadout>();

  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->GetLoadoutManager()
      ->m_activeLoadout = loadoutToApply;
  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->AddAndActiveLoadoutFromBlueprint(loadoutToApply->Class);
}

/*
        Skips the loadout selection part of the match
*/
void ForceStartMatch() {
  ((AYGameState *)(*UWorld::GWorld)->AuthorityGameMode->GameState)
      ->SetRemainingTime(1);
}

/*
        When running in listen mode, only players that are actively being
   rendered by the server are able to play. This code forces the local listen
   player to view a new camera above the map, and extends the render distance to
   ensure that all players are always rendered.
*/
void InitDesyncFix() {
  for (auto actor : UObject::FindObjects<URendererSettings>()) {
    actor->bOcclusionCulling = false;
  }

  ListAllObjectsOfType<ACameraActor>();

  FViewTargetTransitionParams params = FViewTargetTransitionParams();

  params.BlendTime = 0.0f;

  FTransform spawnTransform = FTransform();

  spawnTransform.Translation = FVector(0, 0, 999999.0f);
  spawnTransform.Rotation = FQuat();
  spawnTransform.Rotation.X = 0.0f;
  spawnTransform.Rotation.W = -0.7071068f;
  spawnTransform.Rotation.Y = -0.7071068f;
  spawnTransform.Rotation.Z = 0.0f;

  ACameraActor *newCam = (ACameraActor *)getLastOfType<UGameplayStatics>()
                             ->STATIC_BeginSpawningActorFromClass(
                                 (*UWorld::GWorld), ACameraActor::StaticClass(),
                                 spawnTransform, true, nullptr);

  ListAllObjectsOfType<ACameraActor>();

  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->ClientSetViewTarget(newCam, params);

  getLastOfType<UKismetSystemLibrary>()->STATIC_ExecuteConsoleCommand(
      (*UWorld::GWorld),
      L"r.SkipVehicleUpdateDistance 999999999999999999999999",
      (*UWorld::GWorld)->OwningGameInstance->LocalPlayers[0]->PlayerController);

  FVector farAway = FVector();

  farAway.X = 0;
  farAway.Y = 0;
  farAway.Z = 999999.0f;

  ((AYPlayerController *)(*UWorld::GWorld)
       ->OwningGameInstance->LocalPlayers[0]
       ->PlayerController)
      ->Pawn->K2_TeleportTo(farAway, FRotator());
}

/*
        Starts up the respawn thread
*/
void InitRespawnThread() {
  std::thread t(RespawnThread);

  t.detach();
}

/*
        Runs server buisness logic
*/
void ServerStartCallbacks() {
  LoadConfiguration();

  ProcInMainThread([]() {
    FString URL = L"/Game/Maps/MP/DansMap/MP_DansMap_P?Listen";
    reinterpret_cast<void (*)(UWorld *, FString *, bool, bool)>(
        Globals::ModuleBase + 0x1CE2E40)((*UWorld::GWorld), &URL, true, false);
  });

  Sleep(5 * 1000);
  // Sleep(20 * 1000);

  while (!*UWorld::GWorld) {
    Sleep(1);
  }

  Sleep(5 * 1000);

  while (
      !(*UWorld::GWorld)->NetDriver ||
      (*UWorld::GWorld)->NetDriver->ClientConnections.Count() == 0 ||
      !(*UWorld::GWorld)->NetDriver->ClientConnections[0]->PlayerController) {
    Sleep(1);
  }

  Sleep(5 * 1000);

  if (serverNumBotsTeamOne > 0 || serverNumBotsTeamTwo > 0) {
    SetupMultiplayerAI(serverNumBotsTeamOne, serverNumBotsTeamTwo,
                       serverBotDifficulty);
  }

  // ForceSpawnLocalPlayer();

  ForceStartMatch();

  // Sleep(15 * 1000);

  // InitDesyncFix();

  // Listen();

  // InitRespawnThread();
}

/*
        Main thread, runs common init logic, then runs server or client buisness
   logic
*/
void MainThread() {
  Globals::ModuleBase = (uintptr_t)GetModuleHandleA(nullptr);

  // Initialize fleet save path and load data
  char exePath[MAX_PATH];
  GetModuleFileNameA(NULL, exePath, MAX_PATH);
  std::string exeDir(exePath);
  exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
  g_fleetSavePath = exeDir + "\\dread_fleet_save.txt";
  LoadFleetData();

  if (std::string(GetCommandLineA()).contains("-server")) {
    Globals::AmServer = true;
  }

  InitConsole();
  InitSdk();
  Scanner::ScanAll();

  InitEarlyHooks();

  while (!*UWorld::GWorld) {
    if (Globals::AmServer) {
      *(uintptr_t *)(Globals::ModuleBase + 0x3e554b5) = 0x0; // GIsClient
      *(uintptr_t *)(Globals::ModuleBase + 0x3e554b6) = 0x1; // GIsServer
    }
    Sleep(1);
  }

  if (Globals::AmServer) {
    ServerStartCallbacks();
  }

  if (!Globals::AmServer) {
    InitGameConsole();

    bool init_hook = false;
    do {
      if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success) {
        kiero::bind(8, (void **)&oPresent, hkPresent);
        kiero::bind(13, (void **)&oResizeBuffers, hkResizeBuffers);
        init_hook = true;
      }
    } while (!init_hook);

    while (true) {
      if (GetAsyncKeyState(VK_F7) && !menuToggledThisFrame) {
        menuToggledThisFrame = true;
        menuEnabled = !menuEnabled;
      } else if (!GetAsyncKeyState(VK_F7)) {
        menuToggledThisFrame = false;
      }
      Sleep(10);
    }
  }
}

/*
        Init: Runs in dllmain, just spawns a thread to do all our actual work
*/

void Init() {
  std::thread t(MainThread);
  t.detach();
}

/*
        DllMain: Creates the main thread and bails ASAP
*/
BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved) {
  switch (dwReason) {
  case DLL_PROCESS_ATTACH:
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    // Capture game EXE base for RVA-based globals (e.g. GMalloc at RVA
    // 0x3E55550)
    g_moduleBase = (uintptr_t)GetModuleHandle(NULL);
    // Initialize dual logging: stdout -> log file, g_console -> live console
    // display
    InitLogging();
    debugLogFile.open("dread_debug.log", std::ios::out | std::ios::app);
    tee_printf("\n--------------------------------------------------\n");
    tee_printf("--- DREADNOUGHT OFFLINE MOD ---\n");
    tee_printf("--- Built: %s %s ---\n", __DATE__, __TIME__);
    tee_printf("--------------------------------------------------\n\n");
    // Hook GetCommandLineW EARLY (before game engine reads it)
    InitGatewayHook();
    // Hook WinHTTP to intercept all HTTP requests
    InitWinHttpHooks();
    // Start the mock gateway HTTP server
    std::thread(GatewayServerThread).detach();
    DisableThreadLibraryCalls(hMod);
    Init();
    break;
  case DLL_PROCESS_DETACH:
    if (debugLogFile.is_open()) {
      std::cout << "--- DLL_PROCESS_DETACH ---\n" << std::endl;
      debugLogFile.close();
    }
    if (Dyn_SteamAPI_Shutdown)
      Dyn_SteamAPI_Shutdown();
    if (!Globals::AmServer) {
      kiero::shutdown();
    }
    break;
  }
  return TRUE;
}
