#include "voice_over.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace gta2dx9 {
namespace {

// The game's text lookup: a binary search of the loaded .gxt by key, called with
// a pointer to the key's 8 characters. Its first three instructions are 9 bytes
// and touch nothing but registers, so the jump we put there can be undone by
// running them ourselves.
constexpr uintptr_t kTextLookup = 0x004C2120;
const uint8_t kExpected[9] = {0x8B, 0x41, 0x04,        // mov eax, [ecx+4]
                              0x8B, 0x09,              // mov ecx, [ecx]
                              0x8B, 0x54, 0x24, 0x04}; // mov edx, [esp+4]
uintptr_t s_resume = kTextLookup + 9;

struct Clip {
    char key[9];
    std::vector<uint8_t> wav;  // a whole WAV file, kept whole for PlaySound
    DWORD lastSeen;
};

std::vector<Clip> g_clips;
bool g_enabled = true;
// A text that stays on screen is looked up again and again. A new showing is a
// lookup after a quiet gap.
constexpr DWORD kQuietGapMs = 1500;

std::string GameDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

// Scales 16-bit PCM in place. Returns false for anything that is not that.
bool ApplyVolume(std::vector<uint8_t>& wav, int percent) {
    if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t at = 12;
    bool pcm16 = false;
    while (at + 8 <= wav.size()) {
        uint32_t size = 0;
        memcpy(&size, wav.data() + at + 4, 4);
        if (memcmp(wav.data() + at, "fmt ", 4) == 0 && at + 8 + 16 <= wav.size()) {
            uint16_t tag = 0, bits = 0;
            memcpy(&tag, wav.data() + at + 8, 2);
            memcpy(&bits, wav.data() + at + 8 + 14, 2);
            pcm16 = (tag == 1 && bits == 16);
        } else if (memcmp(wav.data() + at, "data", 4) == 0) {
            if (!pcm16) return percent == 100;
            if (percent == 100) return true;
            const size_t begin = at + 8;
            const size_t end = (begin + size > wav.size()) ? wav.size() : begin + size;
            for (size_t i = begin; i + 1 < end; i += 2) {
                int16_t s;
                memcpy(&s, wav.data() + i, 2);
                int v = s * percent / 100;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                s = static_cast<int16_t>(v);
                memcpy(wav.data() + i, &s, 2);
            }
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

void LoadClips(int volumePercent) {
    const std::string dir = GameDirectory() + "gta2dx9_voice\\";
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA((dir + "*.wav").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        Log("voice: no gta2dx9_voice folder, nothing to play");
        return;
    }
    do {
        std::string name = found.cFileName;
        name.resize(name.size() - 4);  // ".wav"
        if (name.empty() || name.size() > 8) continue;
        FILE* f = fopen((dir + found.cFileName).c_str(), "rb");
        if (!f) continue;
        Clip clip = {};
        memcpy(clip.key, name.c_str(), name.size());
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 44 && size < 64 * 1024 * 1024) {
            clip.wav.resize(static_cast<size_t>(size));
            if (fread(clip.wav.data(), 1, clip.wav.size(), f) != clip.wav.size()) clip.wav.clear();
        }
        fclose(f);
        if (clip.wav.empty() || !ApplyVolume(clip.wav, volumePercent)) {
            Log("voice: %s is not a 16-bit PCM WAV, skipped", found.cFileName);
            continue;
        }
        clip.lastSeen = 0;
        g_clips.push_back(std::move(clip));
    } while (FindNextFileA(find, &found));
    FindClose(find);
    Log("voice: %u recordings loaded", static_cast<unsigned>(g_clips.size()));
}

// Called from the thunk below with the key the game is looking up.
void __cdecl VoiceKeySeen(const char* key) {
    if (!g_enabled || g_clips.empty() || !key) return;
    char k[9] = {};
#ifdef _MSC_VER
    __try {
        memcpy(k, key, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#else
    memcpy(k, key, 8);
#endif
    if (k[0] < '0' || k[0] > '9') return;  // the lines are all numbered
    for (Clip& c : g_clips) {
        if (strcmp(c.key, k) != 0) continue;
        const DWORD now = GetTickCount();
        const bool fresh = (now - c.lastSeen) > kQuietGapMs;
        c.lastSeen = now;
        if (fresh) {
            PlaySoundA(reinterpret_cast<LPCSTR>(c.wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            Log("voice: playing %s", c.key);
        }
        return;
    }
}

static __declspec(naked) void LookupThunk() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 40]   // the key: the lookup's argument, under our 36 bytes and its return address
        push eax
        call VoiceKeySeen
        add esp, 4
        popad
        popfd
        mov eax, dword ptr [ecx + 4]
        mov ecx, dword ptr [ecx]
        mov edx, dword ptr [esp + 4]
        jmp dword ptr [s_resume]
    }
}

// ---- Engine volume ---------------------------------------------------------------------
//
// The game plays its sounds through Miles, one slot per sound, and sets each slot's volume,
// pitch and sample with a handful of small methods on one global object (0x66C950, thiscall,
// the slot number first). Those methods are called from a few places each, always as
// "call <method>", so each of those calls is pointed at a stub of ours that looks at the slot
// and passes on to the method - nothing is displaced, nothing re-implemented.
//
// Which slot is an engine? The sample numbers are not labelled, but an engine is the one sound
// whose pitch the game rewrites every frame as the car's revs change, so a slot whose playback
// rate keeps changing is taken for one. The first couple of frames of a new engine play at the
// unscaled volume while that becomes clear. What was taken for an engine is logged, once per
// sample number, so the guess can be checked and tightened.
constexpr int kSoundSlots = 64;
float g_enginePercent = 70.0f;               // the menu's slider; 100 is the game's own level
volatile int32_t s_engineScale = 90;         // the same, in 1/128ths, for the stubs
uint8_t s_slotEngine[kSoundSlots] = {};      // 1: this slot's sound is an engine
uint8_t s_slotChanges[kSoundSlots] = {};     // how many times its pitch has changed
int32_t s_slotRate[kSoundSlots] = {};        // the pitch it was last given
int32_t s_slotSound[kSoundSlots] = {};       // the sample it is playing
bool g_engineLogged[512] = {};
bool g_engineHooked = false;
int g_engineSlotsSeen = 0;

uintptr_t s_origVol3D = 0x004B62B0;    // set volume (3D sample)
uintptr_t s_origVol2D = 0x004B6110;    // set volume (2D sample)
uintptr_t s_origRate3D = 0x004B6320;   // set pitch (3D sample)
uintptr_t s_origRate2D = 0x004B6150;   // set pitch (2D sample)
uintptr_t s_origStart3D = 0x004B6220;  // set up a 3D sample: (slot, sample number, pitch)
uintptr_t s_origStart2D = 0x004B60C0;  // point a 2D sample at its data: (slot, sample number)

void __cdecl EngineNoteSound(int slot, int sound) {
    if (slot < 0 || slot >= kSoundSlots) return;
    s_slotEngine[slot] = 0;
    s_slotChanges[slot] = 0;
    s_slotRate[slot] = 0;
    s_slotSound[slot] = sound;
}

void __cdecl EngineNoteRate(int slot, int rate) {
    if (slot < 0 || slot >= kSoundSlots) return;
    float percent = g_enginePercent;
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 150.0f) percent = 150.0f;
    s_engineScale = static_cast<int32_t>(percent * 1.28f + 0.5f);
    const int32_t last = s_slotRate[slot];
    s_slotRate[slot] = rate;
    if (s_slotEngine[slot] || last == 0 || rate == last) return;
    if (++s_slotChanges[slot] < 2) return;
    s_slotEngine[slot] = 1;
    ++g_engineSlotsSeen;
    const int sound = s_slotSound[slot];
    if (sound >= 0 && sound < 512 && !g_engineLogged[sound]) {
        g_engineLogged[sound] = true;
        Log("engine volume: sample %d on slot %d changes its pitch with the revs - taken for an engine", sound, slot);
    }
}

static __declspec(naked) void SoundStart3DThunk() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 44]   // the sample number
        push eax
        mov eax, dword ptr [esp + 44]   // the slot
        push eax
        call EngineNoteSound
        add esp, 8
        popad
        popfd
        jmp dword ptr [s_origStart3D]
    }
}

static __declspec(naked) void SoundStart2DThunk() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 44]
        push eax
        mov eax, dword ptr [esp + 44]
        push eax
        call EngineNoteSound
        add esp, 8
        popad
        popfd
        jmp dword ptr [s_origStart2D]
    }
}

static __declspec(naked) void Rate3DThunk() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 44]   // the pitch
        push eax
        mov eax, dword ptr [esp + 44]   // the slot
        push eax
        call EngineNoteRate
        add esp, 8
        popad
        popfd
        jmp dword ptr [s_origRate3D]
    }
}

static __declspec(naked) void Rate2DThunk() {
    __asm {
        pushfd
        pushad
        mov eax, dword ptr [esp + 44]
        push eax
        mov eax, dword ptr [esp + 44]
        push eax
        call EngineNoteRate
        add esp, 8
        popad
        popfd
        jmp dword ptr [s_origRate2D]
    }
}

// The volume is the second argument, 0-127. eax and edx are free at a call; ecx (this) is not touched.
static __declspec(naked) void Volume3DThunk() {
    __asm {
        mov eax, dword ptr [esp + 4]
        cmp eax, 64
        jae pass
        cmp byte ptr [s_slotEngine + eax], 0
        je pass
        mov edx, dword ptr [esp + 8]
        imul edx, dword ptr [s_engineScale]
        sar edx, 7
        cmp edx, 127
        jle fits
        mov edx, 127
    fits:
        mov dword ptr [esp + 8], edx
    pass:
        jmp dword ptr [s_origVol3D]
    }
}

static __declspec(naked) void Volume2DThunk() {
    __asm {
        mov eax, dword ptr [esp + 4]
        cmp eax, 64
        jae pass
        cmp byte ptr [s_slotEngine + eax], 0
        je pass
        mov edx, dword ptr [esp + 8]
        imul edx, dword ptr [s_engineScale]
        sar edx, 7
        cmp edx, 127
        jle fits
        mov edx, 127
    fits:
        mov dword ptr [esp + 8], edx
    pass:
        jmp dword ptr [s_origVol2D]
    }
}

// ---- Radio reception -------------------------------------------------------------------
//
// The game keeps up to five radio sources in the map. Close to one it plays the station's stereo
// file (5.wav); further out, but still in range, it swaps to the mono copy (5A.wav) and carries
// on from the same place - which sounds like losing the signal. The choice is made in one spot:
// after picking a source it stores "1 = stereo, 0 = mono" in a field the file-opening code reads.
// That store (mov [esi+54FCh], edx at 4B24E3, six bytes) is replaced by a call to a stub that
// stores 1 instead when the reception effect is off. Which source is playing and how loud it is
// are left to the game.
bool g_radioReception = false;                 // the menu's switch; off = always the stereo file
bool g_radioHooked = false;
constexpr uintptr_t kRadioStore = 0x004B24E3;
const uint8_t kRadioStoreBytes[6] = {0x89, 0x96, 0xFC, 0x54, 0x00, 0x00};

static __declspec(naked) void RadioVariantThunk() {
    __asm {
        cmp byte ptr [g_radioReception], 0
        jne keep
        mov edx, 1
    keep:
        mov dword ptr [esi + 0x54FC], edx
        ret
    }
}

// The per-frame downgrade (4B2B6B): in the stereo state, when the signal for the stereo file falls under
// 0x5A, the game does three things in a row:
//     mov [esi+54FCh], 0          ; switch to the mono file          (4B2B6B, ten bytes)
//     mov [esi+54F6h], al         ; the volume it just worked out    (4B2B75)
//     ... [esi+5506h] = 16..25    ; a mute timer for the file swap   (4B2B7B..4B2B8D)
// The timer silences the radio while it runs. It is meant to run once, because the next frame the
// state is already mono. Keeping the stereo file but still running the rest made the timer restart
// on every frame: the radio went quiet and stayed quiet. So with the effect off, this stub stores the
// volume and leaves the function (4B2B93: pop esi / ret), skipping the switch and the timer both;
// with the effect on, it does what the game did and carries on at 4B2B75.
constexpr uintptr_t kRadioDowngradeSkip = 0x004B2B93;     // pop esi / ret
constexpr uintptr_t kRadioDowngradeResume = 0x004B2B75;   // the game's own code after the store
constexpr uintptr_t kRadioDowngrade = 0x004B2B6B;
const uint8_t kRadioDowngradeBytes[10] = {0xC7, 0x86, 0xFC, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Reached by a jump, not a call: no return address on the stack. al holds the volume.
static __declspec(naked) void RadioDowngradeThunk() {
    __asm {
        cmp byte ptr [g_radioReception], 0
        jne effect
        mov byte ptr [esi + 0x54F6], al
        push 0x004B2B93
        ret
    effect:
        mov dword ptr [esi + 0x54FC], 0
        push 0x004B2B75
        ret
    }
}

}  // namespace

bool& RadioReceptionEffect() { return g_radioReception; }
bool RadioReceptionAvailable() { return g_radioHooked; }

void RadioInstall() {
    uint8_t* site = reinterpret_cast<uint8_t*>(kRadioStore);
    if (memcmp(site, kRadioStoreBytes, sizeof(kRadioStoreBytes)) != 0) {
        Log("radio: gta2.exe is not the build this knows; no radio quality control");
        return;
    }
    // The second place the game picks the variant is its per-frame update of the playing source
    // (4B2AD0): in stereo, when the signal for the stereo file falls under 0x5A it stores 0 (the mono
    // copy), and in mono it goes back to 1 above 0x6E. That is the switching heard while driving, and
    // the store at 4B24E3 alone does not stop it. The downgrade (mov [esi+54FCh], 0 at 4B2B6B, ten
    // bytes) becomes a jump to a stub that, unless the effect is on, keeps the stereo file, stores the
    // volume and skips the mute timer that follows (see RadioDowngradeThunk). The volume itself is the
    // game's.
    uint8_t* downgrade = reinterpret_cast<uint8_t*>(kRadioDowngrade);
    const bool haveDowngrade = memcmp(downgrade, kRadioDowngradeBytes, sizeof(kRadioDowngradeBytes)) == 0;
    DWORD previous = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &previous)) {
        Log("radio: could not patch the radio store; radio quality is the game's own");
        return;
    }
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RadioVariantThunk) - (kRadioStore + 5));
    site[0] = 0xE8;
    memcpy(site + 1, &to, sizeof(to));
    site[5] = 0x90;
    VirtualProtect(site, 6, previous, &previous);
    if (haveDowngrade) {
        DWORD previous2 = 0;
        if (VirtualProtect(downgrade, 10, PAGE_EXECUTE_READWRITE, &previous2)) {
            const int32_t to2 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RadioDowngradeThunk) - (kRadioDowngrade + 5));
            downgrade[0] = 0xE9;
            memcpy(downgrade + 1, &to2, sizeof(to2));
            memset(downgrade + 5, 0x90, 5);
            VirtualProtect(downgrade, 10, previous2, &previous2);
            Log("radio: the per-frame downgrade is patched too (stereo kept, no mute timer)");
        } else {
            Log("radio: could not patch the per-frame downgrade; the stereo file can still drop to mono");
        }
    } else {
        Log("radio: the per-frame downgrade is not where this knows it; the stereo file can still drop to mono");
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_radioHooked = true;
    Log("radio: stereo kept at any distance unless the reception effect is switched on");
}

float& EngineVolumePercent() { return g_enginePercent; }
bool EngineSoundAvailable() { return g_engineHooked; }
int EngineSoundSlotsSeen() { return g_engineSlotsSeen; }

void EngineSoundInstall() {
    struct Group {
        const uintptr_t* sites;
        int count;
        uintptr_t target;
        void* thunk;
    };
    static const uintptr_t vol3D[] = {0x00417644, 0x004177CC, 0x00417B20};
    static const uintptr_t vol2D[] = {0x0041769D, 0x004177DD, 0x00417C62};
    static const uintptr_t rate3D[] = {0x00417627, 0x0041774A, 0x00417B03};
    static const uintptr_t rate2D[] = {0x00417686, 0x0041775B, 0x00417C45};
    static const uintptr_t start3D[] = {0x00417AF4};
    static const uintptr_t start2D[] = {0x00417C36};
    const Group groups[] = {
        {vol3D, 3, s_origVol3D, reinterpret_cast<void*>(&Volume3DThunk)},
        {vol2D, 3, s_origVol2D, reinterpret_cast<void*>(&Volume2DThunk)},
        {rate3D, 3, s_origRate3D, reinterpret_cast<void*>(&Rate3DThunk)},
        {rate2D, 3, s_origRate2D, reinterpret_cast<void*>(&Rate2DThunk)},
        {start3D, 1, s_origStart3D, reinterpret_cast<void*>(&SoundStart3DThunk)},
        {start2D, 1, s_origStart2D, reinterpret_cast<void*>(&SoundStart2DThunk)},
    };
    // All the calls must be what they were found to be before any is touched.
    for (const Group& g : groups) {
        for (int i = 0; i < g.count; ++i) {
            const uint8_t* site = reinterpret_cast<const uint8_t*>(g.sites[i]);
            int32_t rel = 0;
            memcpy(&rel, site + 1, sizeof(rel));
            if (site[0] != 0xE8 || g.sites[i] + 5 + static_cast<uintptr_t>(rel) != g.target) {
                Log("engine volume: gta2.exe is not the build this knows; no engine volume control");
                return;
            }
        }
    }
    for (const Group& g : groups) {
        for (int i = 0; i < g.count; ++i) {
            uint8_t* site = reinterpret_cast<uint8_t*>(g.sites[i]);
            DWORD previous = 0;
            if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous)) {
                Log("engine volume: could not patch a call; engine volume is off");
                return;
            }
            const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(g.thunk) - (g.sites[i] + 5));
            memcpy(site + 1, &to, sizeof(to));
            VirtualProtect(site, 5, previous, &previous);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_engineHooked = true;
    Log("engine volume: hooked the game's sound calls");
}

void VoiceInstall() {
    char ini[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    std::string p = ini;
    const size_t slash = p.find_last_of("\\/");
    const std::string settings = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "gta2dx9_settings.ini";
    g_enabled = GetPrivateProfileIntA("voice", "enabled", 1, settings.c_str()) != 0;
    int volume = GetPrivateProfileIntA("voice", "volume", 100, settings.c_str());
    if (volume < 0) volume = 0;
    if (volume > 200) volume = 200;
    if (!g_enabled) {
        Log("voice: off in gta2dx9_settings.ini");
        return;
    }
    uint8_t* site = reinterpret_cast<uint8_t*>(kTextLookup);
    if (memcmp(site, kExpected, sizeof(kExpected)) != 0) {
        Log("voice: gta2.exe is not the build this knows; no voice-overs");
        return;
    }
    LoadClips(volume);
    if (g_clips.empty()) return;

    uint8_t patch[sizeof(kExpected)];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LookupThunk) - (kTextLookup + 5));
    memcpy(patch + 1, &to, sizeof(to));
    DWORD previous = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &previous)) {
        Log("voice: could not hook the text lookup");
        return;
    }
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("voice: hooked the text lookup");
}

}  // namespace gta2dx9
