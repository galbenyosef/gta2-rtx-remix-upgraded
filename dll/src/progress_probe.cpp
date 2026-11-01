// A probe for the game's progress: where the score lives and which values change when a job is
// completed. Nothing here changes the game; it only reads memory and writes to gta2dx9.log.
//
// GTA2 saves between levels only (player\plyslot0.dat), and a job completed inside a level lives
// in the script machine's memory, so to remember it the mod has to know which words of that
// memory say "this job is done" and where the money is. That can only be learned from the running
// game, so this records it:
//
//   F9   snapshot the game's data segment and the first part of the script manager, and log what
//        changed since the previous snapshot. Press it at: the start of a level (A), ten seconds
//        later without doing anything (B - this is the noise), and straight after finishing a job
//        (C). B->C minus A->B is what finishing a job changed.
//   F10  read a whole number from gta2dx9_probe.txt (the money on the screen) and search all of the
//        game's memory for it. Press again after the money has changed, with the new number in the
//        file, and the list is narrowed to what changed with it.
#include "progress_probe.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "log.h"

namespace gta2dx9 {
namespace {

// From gta2.exe (the build the rest of the mod is written against): the data segment, and the
// pointer to the script manager.
constexpr uintptr_t kDataBase = 0x0057C000;
constexpr size_t    kDataSize = 0x000FA718;
constexpr uintptr_t kScriptManagerPtr = 0x006644BC;
constexpr size_t    kManagerBytes = 0x8000;

struct Snapshot {
    std::vector<uint8_t> data;
    std::vector<uint8_t> manager;
    uintptr_t managerAt = 0;
};

std::vector<Snapshot> g_snaps;
std::set<uint32_t> g_noiseData, g_noiseManager;   // word indexes that changed with nothing going on
std::vector<uintptr_t> g_candidates;
bool g_scanned = false;
bool g_prev9 = false, g_prev10 = false;

bool ReadMem(uintptr_t address, void* out, size_t bytes) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, bytes, &got) &&
           got == bytes;
}

// Reads as much as is readable, page by page, zero-filling the rest so offsets stay put.
void ReadLoose(uintptr_t address, size_t bytes, std::vector<uint8_t>* out) {
    out->assign(bytes, 0);
    for (size_t at = 0; at < bytes; at += 0x1000) {
        const size_t chunk = (bytes - at) < 0x1000 ? (bytes - at) : 0x1000;
        ReadMem(address + at, out->data() + at, chunk);
    }
}

Snapshot Take() {
    Snapshot s;
    ReadLoose(kDataBase, kDataSize, &s.data);
    uintptr_t manager = 0;
    ReadMem(kScriptManagerPtr, &manager, sizeof(manager));
    s.managerAt = manager;
    if (manager > 0x10000) ReadLoose(manager, kManagerBytes, &s.manager);
    return s;
}

// Changed words between two buffers. Logs up to `cap` of them, skipping those in `skip`.
size_t Diff(const char* what, uintptr_t base, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
            const std::set<uint32_t>* skip, std::set<uint32_t>* collect, size_t cap) {
    size_t changed = 0, shown = 0, skipped = 0;
    const size_t words = (a.size() < b.size() ? a.size() : b.size()) / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t x, y;
        memcpy(&x, a.data() + i * 4, 4);
        memcpy(&y, b.data() + i * 4, 4);
        if (x == y) continue;
        ++changed;
        if (collect) collect->insert(static_cast<uint32_t>(i));
        if (skip && skip->count(static_cast<uint32_t>(i))) { ++skipped; continue; }
        if (shown < cap) {
            ++shown;
            Log("  %s %08X: %08X -> %08X  (%d -> %d)", what, static_cast<unsigned>(base + i * 4), x, y,
                static_cast<int>(x), static_cast<int>(y));
        }
    }
    Log("  %s: %zu word(s) changed, %zu of them also changed while idle, %zu shown", what, changed, skipped, shown);
    return changed;
}

void OnSnapshotKey() {
    g_snaps.push_back(Take());
    const size_t n = g_snaps.size();
    const Snapshot& now = g_snaps.back();
    Log("PROBE snapshot #%zu taken: script manager at %08X, data %zu bytes, manager %zu bytes", n,
        static_cast<unsigned>(now.managerAt), now.data.size(), now.manager.size());
    if (n < 2) return;
    const Snapshot& before = g_snaps[n - 2];
    // The second snapshot is the idle one: whatever changed in it is noise, and is remembered as such.
    const bool idle = n == 2;
    Log("PROBE changes #%zu -> #%zu%s", n - 1, n, idle ? " (idle - this is the noise)" : "");
    if (before.managerAt == now.managerAt && !before.manager.empty()) {
        Diff("manager+", 0, before.manager, now.manager, idle ? nullptr : &g_noiseManager,
             idle ? &g_noiseManager : nullptr, idle ? 60 : 500);
    } else {
        Log("  script manager moved (%08X -> %08X); its words are not compared",
            static_cast<unsigned>(before.managerAt), static_cast<unsigned>(now.managerAt));
    }
    Diff("data", kDataBase, before.data, now.data, idle ? nullptr : &g_noiseData, idle ? &g_noiseData : nullptr,
         idle ? 60 : 300);
    Log("PROBE end of changes #%zu -> #%zu", n - 1, n);
}

bool ReadProbeValue(long long* value) {
    FILE* f = fopen("gta2dx9_probe.txt", "r");
    if (!f) return false;
    char line[64] = {};
    const bool ok = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    if (!ok) return false;
    char* end = nullptr;
    const long long v = _strtoi64(line, &end, 10);
    if (end == line) return false;
    *value = v;
    return true;
}

bool Writable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool InOurSnapshots(uintptr_t a) {
    for (const Snapshot& s : g_snaps) {
        const uintptr_t d = reinterpret_cast<uintptr_t>(s.data.data());
        const uintptr_t m = reinterpret_cast<uintptr_t>(s.manager.data());
        if (a >= d && a < d + s.data.size()) return true;
        if (a >= m && a < m + s.manager.size()) return true;
    }
    const uintptr_t c = reinterpret_cast<uintptr_t>(g_candidates.data());
    return a >= c && a < c + g_candidates.capacity() * sizeof(uintptr_t);
}

void OnScanKey() {
    long long value = 0;
    if (!ReadProbeValue(&value)) {
        Log("PROBE scan: put the number you see (the money) in gta2dx9_probe.txt, next to the game, and press F10");
        return;
    }
    const int32_t want = static_cast<int32_t>(value);
    if (!g_scanned) {
        g_candidates.clear();
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t at = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        std::vector<uint8_t> buf;
        while (at < end) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(at), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
            if (Writable(m) && m.RegionSize < 0x10000000) {
                buf.resize(m.RegionSize);
                if (ReadMem(base, buf.data(), m.RegionSize)) {
                    for (size_t i = 0; i + 4 <= m.RegionSize; i += 4) {
                        int32_t x;
                        memcpy(&x, buf.data() + i, 4);
                        if (x == want && !InOurSnapshots(base + i) && g_candidates.size() < 300000) {
                            g_candidates.push_back(base + i);
                        }
                    }
                }
            }
            at = base + m.RegionSize;
        }
        g_scanned = true;
        Log("PROBE scan #1 for %d: %zu candidate address(es)", want, g_candidates.size());
    } else {
        std::vector<uintptr_t> keep;
        for (uintptr_t a : g_candidates) {
            int32_t x = 0;
            if (ReadMem(a, &x, 4) && x == want) keep.push_back(a);
        }
        g_candidates.swap(keep);
        Log("PROBE scan (narrowed) for %d: %zu candidate address(es) left", want, g_candidates.size());
    }
    int shown = 0;
    for (uintptr_t a : g_candidates) {
        if (shown++ >= 60) break;
        MEMORY_BASIC_INFORMATION m;
        VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m));
        Log("  candidate %08X  (%s, region %08X +%X)", static_cast<unsigned>(a),
            m.Type == MEM_IMAGE ? "exe data" : "heap", static_cast<unsigned>(reinterpret_cast<uintptr_t>(m.AllocationBase)),
            static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(m.BaseAddress)));
    }
}

}  // namespace

void ProgressProbeTick() {
    DWORD pid = 0;
    if (HWND foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &pid);
    const bool ours = pid == GetCurrentProcessId();
    const bool f9 = ours && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = ours && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f9 && !g_prev9) OnSnapshotKey();
    if (f10 && !g_prev10) OnScanKey();
    g_prev9 = f9;
    g_prev10 = f10;
}

}  // namespace gta2dx9
