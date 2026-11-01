#include "live_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <map>
#include <string>
#include <vector>

#include "../../src/gta2_map.h"
#include "../../src/renderer.h"
#include "game_access.h"
#include "gibs.h"
#include "log.h"
#include "overlay.h"
#include "remix_api.h"
#include "texture_store.h"

namespace gta2dx9 {
namespace {

constexpr DWORD kWorldFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
constexpr int kGameVertexFloats = 8;
constexpr int kUvU = 6;
constexpr int kUvV = 7;

// How far a sprite is lifted off the floor it stands on.
//
// GTA2 itself lifts them by nothing at all. Its object pass puts all four
// corners of the quad at exactly the level of the block underneath - read back
// out of the game's own vertex shadow, a pedestrian at cell (158,138) comes out
// as (158.2656 138.7500 2.0000) (158.2656 138.2500 2.0000) (158.7344 138.2500
// 2.0000) (158.7344 138.7500 2.0000), all four at 2.0000, while the floor there
// is the lid at 2.0. Perfectly coplanar.
//
// It gets away with that because its renderers never depth test: 3dfx.dll calls
// grDepthBufferFunction(GR_CMP_ALWAYS) and imports no grDepthMask or
// grDepthBufferMode at all, and d3ddll.dll never sets ZENABLE, ZWRITEENABLE or
// ZFUNC. Both are pure painter's algorithm - the map is drawn level by level and
// the object pass paints over the floor it just laid down. That is also why the
// screen-space overlay pass never had this problem: it replays submission order
// with no depth test, which is the original behaviour exactly.
//
// Turning a sprite into real 3D geometry is what breaks it. Coplanar surfaces
// have no defined order in 3D: the depth test drops them, and under Remix the
// ray hit is ambiguous however the raster state is set, so ZFUNC_LESSEQUAL would
// fix the raster view and still leave the path tracer fighting itself.
//
// So the lift is ours and has to be justified rather than guessed.
//
// A quarter of a ground texel - 1/256 of a block, the first value used here -
// is enough to break the coplanarity and nothing more. That settles flat
// ground, and does nothing at all for a slope: GTA2 gives all four corners of
// the quad the object's single level, so on a ramp a horizontal quad cuts
// straight through the lid and the uphill half of the sprite disappears into
// it. A sprite of length L on a gradient g sinks by up to g*L/2.
//
// Measured over the shipped districts, of the surfaces a sprite can actually
// stand on (ground type road or pavement, 16419 of them):
//
//     flat                                  72.06%
//     7 degrees   (8 steps, g = 0.125)      21.88%
//     26 degrees  (2 steps, g = 0.5)         5.92%
//     45 degrees  (1 step,  g = 1.0)         0.14%
//
// A pedestrian quad is 0.47 blocks across (measured off the game's own shadow
// slots), a car about 2. So 1/8 of a block clears 99.5% of the sloped surfaces
// a pedestrian can reach and 78% of those a car can, which is every 7 degree
// ramp - the common case by a wide margin. Doubling it to 1/4 buys nothing more
// for cars, and it takes a full 1/2 block to cover them on 26 degree ramps,
// which floats every sprite in the city half a block to fix 6% of surfaces.
//
// No single number does both jobs, which is why this is now the fallback rather
// than the mechanism. It is used where there is no floor to measure - off the
// map, over a hole, or with conforming switched off - and as the ceiling on the
// extra clearance a footprint straddling a kerb is given.
float g_spriteLift = kDefaultSpriteLift;

// --- Conforming -----------------------------------------------------------
//
// The real answer, and what runs by default: sample the lid under the sprite's
// four corners, fit a plane to it, and make that plane the sprite's transform.
// The quad is then parallel to what it is standing on, so the whole slope term
// above disappears and the clearance collapses to kConformClearance - a quarter
// of a ground texel, enough to break coplanarity and nothing more.
//
// The part that matters for Remix: **the object-space quad does not change.**
// Baking per-corner heights into the vertices is the obvious way to tilt a
// sprite and it is the wrong one - local[] would become ground-dependent, so it
// would change continuously as a car climbed a ramp, CanonicalShape's eight-shape
// cache would fill and then dither, and the topological hash would churn exactly
// the way the note above Place explains it must not. So the shape is still
// measured from the flat corners the game gave, bit for bit, and every bit of
// the ground adaptation lives in objectToWorld, which stays a pure rotation and
// translation. That is the one form of this that Remix is happy with, and it
// pays twice over: the normal handed to the path tracer becomes the ground
// normal, so a car on a ramp is lit as one, and a replacement mesh placed by
// this matrix inherits the bank for free.
//
// Two rules keep it out of trouble without having to recognise anything:
//
//   * A sprite is never moved *down*. Where the game's own level is above the
//     floor the map reports, the game's wins - which leaves a jumping car, a
//     helicopter and a bullet exactly where they were put, with no notion of
//     what any of them are.
//   * The tilt fades out over the same gap, so something leaving the ground
//     rotates level again smoothly instead of snapping when it crosses a
//     threshold.
bool g_spriteConform = true;

// How high the object sits above the road, as opposed to how far the quad is
// held off it for the depth test. See kDefaultSpriteHeight: this is the one that
// decides whether a car casts a shadow, and it was the thing the old fixed lift
// was accidentally providing a little of while trying to do something else.
float g_spriteHeight = kDefaultSpriteHeight;

float Smoothstep(float edge0, float edge1, float x) {
    if (x <= edge0) return 0.0f;
    if (x >= edge1) return 1.0f;
    const float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
}

// Where the tilt starts to fade and where it is gone. The near edge is well
// above any step a sprite stands on and well below a real jump; the far edge is
// a block, at which point the game has plainly taken the object off the floor.
constexpr float kTiltFadeNear = 0.25f;
constexpr float kTiltFadeFar = 1.0f;

// The tilt cap as the sine of the angle, which is what the normal is clamped
// against: the horizontal part of a unit normal is exactly sin of the slope.
const float kMaxTiltTan = std::tan(kMaxTiltDegrees * 3.14159265358979f / 180.0f);

// How much a sprite keeps of a sideways tilt. See kDefaultSpriteRoll.
float g_spriteRoll = kDefaultSpriteRoll;

// How far a footprint may depart from the plane fitted through it before the
// tilt stops being believed. Below the first figure the ground really is one
// plane and the fit is worth following whole; past the second it is a step or a
// block edge, where a confident tilt is worse than none.
constexpr float kFitGood = 0.05f;
constexpr float kFitPoor = 0.30f;

// How much of the way to this frame's answer a sprite moves. The game is paced
// at 30 fps, so a quarter converges in about a third of a second: fast enough to
// follow a car up a ramp, slow enough that a footprint crossing a kerb eases
// rather than steps.
constexpr float kSmoothing = 0.25f;

// Wider than anything can travel between two frames - a car at GTA2's top speed
// covers about a third of this - and narrower than the gap to the next sprite.
constexpr float kTrackRadius = 0.75f;

// Anything outside this is stale data left in the shadow slots by an earlier
// draw rather than a position the game just computed.
constexpr float kWorldMargin = 8.0f;
constexpr float kMinLevel = -4.0f;
constexpr float kMaxLevel = 16.0f;

struct Vec {
    float x, y, z;
};

Vec Cross(const Vec& a, const Vec& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace

void SetSpriteLift(float blocks) {
    // Negative would push sprites into the floor, which is the bug this exists
    // to avoid; the ceiling is a whole block, past which a sprite is standing on
    // the storey above.
    g_spriteLift = blocks < 0.0f ? 0.0f : (blocks > 1.0f ? 1.0f : blocks);
}

float SpriteLift() { return g_spriteLift; }

void SetSpriteConform(bool on) { g_spriteConform = on; }
bool SpriteConform() { return g_spriteConform; }

void SetSpriteRoll(float amount) {
    g_spriteRoll = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

float SpriteRoll() { return g_spriteRoll; }

void SetSpriteHeight(float blocks) {
    // Negative would bury the sprite, which is the bug all of this exists to
    // avoid. The ceiling is a whole block: past that a sprite is standing at the
    // height of the storey above the one the game put it on.
    g_spriteHeight = blocks < 0.0f ? 0.0f : (blocks > 1.0f ? 1.0f : blocks);
}

float SpriteHeight() { return g_spriteHeight; }

float g_spriteFeather = 0.0f;

void SetSpriteFeather(float strength) {
    g_spriteFeather = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
}

float SpriteFeather() { return g_spriteFeather; }

// A hundredth of a block. Far below anything visible at GTA2's near-top-down
// camera, and comfortably above the depth buffer's resolving power at the ten to
// twenty units the camera actually sits at.
float g_spriteStackStep = 0.01f;

void SetSpriteStackStep(float blocks) {
    g_spriteStackStep = blocks < 0.0f ? 0.0f : (blocks > 0.25f ? 0.25f : blocks);
}

float SpriteStackStep() { return g_spriteStackStep; }

int g_effectBatches = 0;
int EffectBatchesDrawn() { return g_effectBatches; }

bool g_ownCarLamps = kDefaultOwnCarLamps;
void ReleaseLampTextures();
// Per lamp delta, one entry per car sprite.
std::map<int, std::vector<LampArt>> g_lampArt;
void SetOwnCarLamps(bool on) { g_ownCarLamps = on; }
bool OwnCarLamps() { return g_ownCarLamps; }

namespace {
bool g_carBodies = kDefaultCarBodies;
float g_carBodyHeight = kDefaultCarBodyHeight;
float g_carBodyInset = kDefaultCarBodyInset;
}  // namespace

void SetCarBodies(bool on) { g_carBodies = on; }
bool CarBodies() { return g_carBodies; }
void SetCarBodyHeight(float blocks) { g_carBodyHeight = (std::max)(0.0f, (std::min)(blocks, 0.6f)); }
float CarBodyHeight() { return g_carBodyHeight; }
void SetCarBodyInset(float fraction) { g_carBodyInset = (std::max)(0.0f, (std::min)(fraction, 0.4f)); }
float CarBodyInset() { return g_carBodyInset; }

namespace {
bool g_carMaterial = kDefaultCarMaterial;
float g_carRoughness = kDefaultCarRoughness;
float g_carMetallic = kDefaultCarMetallic;
}  // namespace

void SetCarMaterial(bool on, float roughness, float metallic) {
    g_carMaterial = on;
    g_carRoughness = (std::max)(0.0f, (std::min)(roughness, 1.0f));
    g_carMetallic = (std::max)(0.0f, (std::min)(metallic, 1.0f));
}

std::vector<uint32_t> LampTexels(const LampArt& art, bool mirrored) {
    std::vector<uint32_t> texels(art.argb.size(), 0);
    uint32_t sum[3] = {}, count = 0;
    for (uint32_t c : art.argb) {
        if (!(c >> 24)) continue;
        sum[0] += (c >> 16) & 0xFF;
        sum[1] += (c >> 8) & 0xFF;
        sum[2] += c & 0xFF;
        ++count;
    }
    const uint32_t fill =
        count ? ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count) : 0;
    for (int y = 0; y < art.height; ++y) {
        for (int x = 0; x < art.width; ++x) {
            const int from = mirrored ? art.width - 1 - x : x;
            const uint32_t c = art.argb[static_cast<size_t>(y) * art.width + from];
            texels[static_cast<size_t>(y) * art.width + x] = (c >> 24) ? c : fill;
        }
    }
    return texels;
}

void SetCarLampArt(int delta, std::vector<LampArt> bySprite) {
    // Textures point at the art they were made from.
    ReleaseLampTextures();
    g_lampArt[delta] = std::move(bySprite);
}

// Textures for the lamps, made on first use, keyed by the art and which side.
std::map<std::pair<const LampArt*, bool>, IDirect3DTexture9*> g_lampTextures;
IDirect3DDevice9* g_lampDevice = nullptr;

void ReleaseLampTextures() {
    for (auto& entry : g_lampTextures) {
        if (entry.second) entry.second->Release();
    }
    g_lampTextures.clear();
}

IDirect3DTexture9* LampTexture(IDirect3DDevice9* device, const LampArt* art, bool mirrored) {
    if (!art || !art->Valid()) return nullptr;
    if (g_lampDevice != device) {
        ReleaseLampTextures();
        g_lampDevice = device;
    }
    auto found = g_lampTextures.find({art, mirrored});
    if (found != g_lampTextures.end()) return found->second;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(art->width, art->height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &texture, nullptr))) {
        texture = nullptr;
    } else {
        const std::vector<uint32_t> texels = LampTexels(*art, mirrored);
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < art->height; ++y) {
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
                       &texels[static_cast<size_t>(y) * art->width],
                       static_cast<size_t>(art->width) * 4);
            }
            texture->UnlockRect(0);
        }
    }
    g_lampTextures[{art, mirrored}] = texture;
    return texture;
}

// The last complete frame's conform counts. The menu has no handle on the
// LiveGeometry instance - the same reason EffectBatchesDrawn is a free function.
LiveGeometry::Conform g_lastConform;
const LiveGeometry::Conform& SpriteConformCounts() { return g_lastConform; }


// ---- Cars as Remix materials ------------------------------------------------
//
// A D3D9 draw can only carry the global legacy material, so a car cannot be made
// glossy without making the road and the grass glossy too. The Remix API can
// give one mesh its own material, but it takes the albedo as a file: each car
// sprite's artwork is written once to a small DDS beside the game, and a material
// made from it. Everything here fails soft - any refusal leaves the dome to be
// drawn as ordinary geometry, as it was before.
namespace {
struct CarMaterial {
    remixapi_MaterialHandle handle = nullptr;
    bool failed = false;
    uint32_t born = 0;   // the frame it was made in; see kCarMaterialWarmup
};
uint32_t g_carFrame = 0;
// The last artwork of each car (by game object) whose material was ready. A car's
// artwork changes for a few frames whenever a door opens, and each change is a
// new material to load; through those frames the car keeps the last one it had
// loaded rather than falling back to the flat sprite and back.
struct WarmArt {
    IDirect3DTexture9* art = nullptr;
    int sprite = -1;
    uint32_t seen = 0;
};
std::map<uintptr_t, WarmArt> g_warmArt;
// Remix loads a new material's textures in the background, and until they are in
// it draws something else. A car's art changes with its doors, its damage and
// its paint, so a new material is not trusted for this many frames, and the
// plain sprite stands in for the car meanwhile rather than the car flickering.
constexpr uint32_t kCarMaterialWarmup = 8;
std::map<std::pair<IDirect3DTexture9*, int>, CarMaterial> g_carMaterials;
std::map<uint64_t, remixapi_MeshHandle> g_carMeshes;
int g_carMaterialLogs = 0;

uint64_t Fnv64(const void* data, size_t bytes, uint64_t hash = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring CacheDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    return dir + L"\\gta2dx9_cache";
}

// Uncompressed BGRA8 DDS with a full mip chain, so the car is not aliased when it
// is drawn smaller than the texture.
bool WriteDds(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra) {
    std::vector<std::vector<uint8_t>> levels;
    std::vector<std::pair<UINT, UINT>> sizes;
    levels.push_back(bgra);
    sizes.push_back({width, height});
    while (sizes.back().first > 1 || sizes.back().second > 1) {
        const UINT pw = sizes.back().first, ph = sizes.back().second;
        const UINT nw = (std::max)(1u, pw / 2), nh = (std::max)(1u, ph / 2);
        const std::vector<uint8_t>& src = levels.back();
        std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
        for (UINT y = 0; y < nh; ++y) {
            for (UINT x = 0; x < nw; ++x) {
                const UINT x0 = (std::min)(x * 2, pw - 1), x1 = (std::min)(x * 2 + 1, pw - 1);
                const UINT y0 = (std::min)(y * 2, ph - 1), y1 = (std::min)(y * 2 + 1, ph - 1);
                for (int c = 0; c < 4; ++c) {
                    const unsigned sum = src[(static_cast<size_t>(y0) * pw + x0) * 4 + c] +
                                         src[(static_cast<size_t>(y0) * pw + x1) * 4 + c] +
                                         src[(static_cast<size_t>(y1) * pw + x0) * 4 + c] +
                                         src[(static_cast<size_t>(y1) * pw + x1) * 4 + c];
                    dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        sizes.push_back({nw, nh});
    }
    uint32_t header[32] = {};
    header[0] = 0x20534444;   // "DDS "
    header[1] = 124;
    header[2] = 0x100F | 0x20000;   // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT | MIPMAPCOUNT
    header[3] = height;
    header[4] = width;
    header[5] = width * 4;
    header[7] = static_cast<uint32_t>(levels.size());
    header[19] = 32;          // pixel format size
    header[20] = 0x41;        // RGB | ALPHAPIXELS
    header[22] = 32;
    header[23] = 0x00FF0000;
    header[24] = 0x0000FF00;
    header[25] = 0x000000FF;
    header[26] = 0xFF000000;
    header[27] = 0x1000 | 0x8 | 0x400000;   // TEXTURE | COMPLEX | MIPMAP
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, header, sizeof(header), &written, nullptr) && written == sizeof(header);
    for (const auto& level : levels) {
        ok = ok && WriteFile(file, level.data(), static_cast<DWORD>(level.size()), &written,
                             nullptr) && written == level.size();
    }
    CloseHandle(file);
    return ok;
}

// The first mip of a BGRA8 DDS written by WriteDds or by the tool that made the
// artwork. Anything else is refused.
bool ReadDds(const std::wstring& path, int* width, int* height, std::vector<uint8_t>* bgra) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint32_t header[32] = {};
    DWORD got = 0;
    bool ok = ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header);
    ok = ok && header[0] == 0x20534444 && header[1] == 124 && header[19] == 32 &&
         header[20] == 0x41 && header[22] == 32 && header[3] > 0 && header[4] > 0 &&
         header[3] <= 8192 && header[4] <= 8192;
    if (ok) {
        *width = static_cast<int>(header[4]);
        *height = static_cast<int>(header[3]);
        bgra->resize(static_cast<size_t>(*width) * *height * 4);
        ok = ReadFile(file, bgra->data(), static_cast<DWORD>(bgra->size()), &got, nullptr) &&
             got == bgra->size();
    }
    CloseHandle(file);
    return ok;
}

bool g_carHd = true;
bool g_carNormalMaps = true;

// One car's hand-made artwork, found in gta2dx9_hd\: the sprite redrawn at some
// whole multiple of its own size, and the maps that go with it. All are laid out
// exactly as the sprite is, so the sprite's own texture coordinates address them.
struct HdCar {
    bool tried = false;
    bool ok = false;
    int width = 0, height = 0;
    std::vector<uint8_t> albedo;   // BGRA8, at the redrawn size
    std::vector<uint8_t> roughPx, metalPx;   // BGRA8 too, when they are the albedo's size
    // The sprite as the game draws it in its own default paint, at the sprite's own
    // size, when car_NN_base.dds is there: what a repaint is measured against.
    std::vector<uint8_t> basePx;
    int baseWidth = 0, baseHeight = 0;
    std::wstring normal, rough, metal;
    std::wstring normalUsed;     // the normal map handed to Remix: this one, scaled
    bool normalPrepared = false;
};
float g_carNormalStrength = 0.5f;
std::map<int, HdCar> g_hdCars;

std::wstring HdDirectory() {
    std::wstring dir = CacheDirectory();
    const size_t slash = dir.find_last_of(L'\\');
    return dir.substr(0, slash) + L"\\gta2dx9_hd";
}

HdCar& HdFor(int sprite) {
    HdCar& hd = g_hdCars[sprite];
    if (hd.tried) return hd;
    hd.tried = true;
    wchar_t name[64];
    const std::wstring dir = HdDirectory();
    swprintf(name, 64, L"\\car_%02d_albedo.dds", sprite);
    if (!ReadDds(dir + name, &hd.width, &hd.height, &hd.albedo)) return hd;
    swprintf(name, 64, L"\\car_%02d_normal.dds", sprite);
    hd.normal = dir + name;
    swprintf(name, 64, L"\\car_%02d_rough.dds", sprite);
    hd.rough = dir + name;
    swprintf(name, 64, L"\\car_%02d_metal.dds", sprite);
    hd.metal = dir + name;
    hd.ok = GetFileAttributesW(hd.normal.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(hd.rough.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(hd.metal.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hd.ok) {
        swprintf(name, 64, L"\\car_%02d_base.dds", sprite);
        if (!ReadDds(dir + name, &hd.baseWidth, &hd.baseHeight, &hd.basePx)) hd.basePx.clear();
        // Read for the glass: see RecolourHd. A map that does not fit is simply
        // not used for that.
        int w = 0, h = 0;
        if (ReadDds(hd.rough, &w, &h, &hd.roughPx) && (w != hd.width || h != hd.height)) {
            hd.roughPx.clear();
        }
        if (ReadDds(hd.metal, &w, &h, &hd.metalPx) && (w != hd.width || h != hd.height)) {
            hd.metalPx.clear();
        }
    }
    return hd;
}

// The redrawn car in the colours this sprite is actually drawn in. The redraw is
// in the car's default paint; the game recolours the sprite per car, so the
// redraw's fine detail - the ratio of each of its pixels to the average of the
// sprite pixel it sits in - is laid over the sprite's own colours, smoothly
// enlarged. Every paint job gets the new detail without being redrawn.
// The car's normal map at the strength the setting asks for. The maps on disk are
// built to show every panel line and curve; a lot of that is more than a painted
// redraw can carry and shows up as odd glints, so the tilt of every normal is
// scaled back here and the result kept in the cache beside the artwork.
const std::wstring& PrepareNormal(HdCar& hd) {
    if (hd.normalPrepared) return hd.normalUsed;
    hd.normalPrepared = true;
    hd.normalUsed = hd.normal;
    const float strength = g_carNormalStrength;
    if (strength >= 0.995f) return hd.normalUsed;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!ReadDds(hd.normal, &w, &h, &px)) return hd.normalUsed;
    uint64_t hash = Fnv64(px.data(), px.size());
    const int percent = static_cast<int>(strength * 100.0f + 0.5f);
    hash = Fnv64(&percent, sizeof(percent), hash);
    const std::wstring dir = CacheDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\normal_%016llx.dds", static_cast<unsigned long long>(hash));
    const std::wstring path = dir + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            const float nx = (px[i + 2] / 255.0f * 2.0f - 1.0f) * strength;
            const float ny = (px[i + 1] / 255.0f * 2.0f - 1.0f) * strength;
            const float nz = (std::max)(px[i + 0] / 255.0f * 2.0f - 1.0f, 0.05f);
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            px[i + 2] = static_cast<uint8_t>((nx / len * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[i + 1] = static_cast<uint8_t>((ny / len * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[i + 0] = static_cast<uint8_t>((nz / len * 0.5f + 0.5f) * 255.0f + 0.5f);
        }
        if (!WriteDds(path, w, h, px)) return hd.normalUsed;
    }
    hd.normalUsed = path;
    return hd.normalUsed;
}

bool RecolourHd(const HdCar& hd, const std::vector<uint8_t>& game, int w, int h,
                std::vector<uint8_t>* out) {
    if (w <= 0 || h <= 0 || hd.width % w != 0 || hd.height % h != 0) return false;
    const int S = hd.width / w;
    if (S < 1 || hd.height / h != S) return false;
    const int W = hd.width, H = hd.height;
    // The redraw averaged over the sprite's own pixels, weighted by its alpha.
    std::vector<float> low(static_cast<size_t>(w) * h * 4, 0.0f);   // premultiplied b, g, r, a
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = &hd.albedo[(static_cast<size_t>(y) * W + x) * 4];
            float* l = &low[(static_cast<size_t>(y / S) * w + x / S) * 4];
            const float a = p[3] / 255.0f;
            l[0] += p[0] * a;
            l[1] += p[1] * a;
            l[2] += p[2] * a;
            l[3] += a;
        }
    }
    // The game's sprite, premultiplied.
    std::vector<float> gp(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const float a = game[i * 4 + 3] >= 128 ? 1.0f : 0.0f;
        gp[i * 4 + 0] = game[i * 4 + 0] * a;
        gp[i * 4 + 1] = game[i * 4 + 1] * a;
        gp[i * 4 + 2] = game[i * 4 + 2] * a;
        gp[i * 4 + 3] = a;
    }
    // The default-paint sprite, premultiplied the same way, if the car has one.
    const bool haveBase = hd.baseWidth == w && hd.baseHeight == h &&
                          hd.basePx.size() == static_cast<size_t>(w) * h * 4;
    std::vector<float> bp;
    if (haveBase) {
        bp.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const float a = hd.basePx[i * 4 + 3] >= 128 ? 1.0f : 0.0f;
            bp[i * 4 + 0] = hd.basePx[i * 4 + 0] * a;
            bp[i * 4 + 1] = hd.basePx[i * 4 + 1] * a;
            bp[i * 4 + 2] = hd.basePx[i * 4 + 2] * a;
            bp[i * 4 + 3] = a;
        }
    }
    // Per-sprite-pixel repaint gain, where both the game's sprite and its default
    // paint are solid, and grown outward a few pixels for the redraw's silhouette
    // edge: the redraw reaches a little past the sprite's own alpha there, and
    // keeping its default colour in that band left a pale rim around a repainted car.
    std::vector<float> gainField;
    std::vector<uint8_t> gainValid;
    if (haveBase) {
        const size_t n = static_cast<size_t>(w) * h;
        gainField.assign(n * 3, 1.0f);
        gainValid.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (gp[i * 4 + 3] > 0.5f && bp[i * 4 + 3] > 0.5f) {
                gainValid[i] = 1;
                for (int c = 0; c < 3; ++c) {
                    const float g = (gp[i * 4 + c] + 10.0f) / (bp[i * 4 + c] + 10.0f);
                    gainField[i * 3 + c] = (std::max)(0.2f, (std::min)(5.0f, g));
                }
            }
        }
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<uint8_t> nextValid = gainValid;
            std::vector<float> nextGain = gainField;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (gainValid[i]) continue;
                    float acc[3] = {0, 0, 0};
                    int count = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            const size_t j = static_cast<size_t>(yy) * w + xx;
                            if (!gainValid[j]) continue;
                            for (int c = 0; c < 3; ++c) acc[c] += gainField[j * 3 + c];
                            ++count;
                        }
                    }
                    if (count) {
                        nextValid[i] = 1;
                        for (int c = 0; c < 3; ++c) nextGain[i * 3 + c] = acc[c] / count;
                    }
                }
            }
            gainValid.swap(nextValid);
            gainField.swap(nextGain);
        }
    }
    // Bilinear sample of the grown gain field at hi-res pixel (x, y); false if the
    // pixel is further out than the growth reached.
    auto sampleGain = [&](int x, int y, float* gain) {
        const float fx = (x + 0.5f) / S - 0.5f, fy = (y + 0.5f) / S - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        float acc[3] = {0, 0, 0}, wsum = 0.0f;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int xi = (std::max)(0, (std::min)(w - 1, x0 + i));
                const int yj = (std::max)(0, (std::min)(h - 1, y0 + j));
                const size_t k = static_cast<size_t>(yj) * w + xi;
                if (!gainValid[k]) continue;
                const float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                for (int c = 0; c < 3; ++c) acc[c] += gainField[k * 3 + c] * wgt;
                wsum += wgt;
            }
        }
        if (wsum < 0.25f) return false;
        for (int c = 0; c < 3; ++c) gain[c] = acc[c] / wsum;
        return true;
    };
    // Bilinear sample of a premultiplied sprite-resolution image at hi-res pixel (x, y).
    auto sample = [&](const std::vector<float>& img, int x, int y, float* rgb, float* wsum) {
        const float fx = (x + 0.5f) / S - 0.5f, fy = (y + 0.5f) / S - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int xi = (std::max)(0, (std::min)(w - 1, x0 + i));
                const int yj = (std::max)(0, (std::min)(h - 1, y0 + j));
                const float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                const float* s = &img[(static_cast<size_t>(yj) * w + xi) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += s[c] * wgt;
            }
        }
        *wsum = acc[3];
        for (int c = 0; c < 3; ++c) rgb[c] = acc[3] > 1e-4f ? acc[c] / acc[3] : 0.0f;
    };
    out->assign(static_cast<size_t>(W) * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = &hd.albedo[(static_cast<size_t>(y) * W + x) * 4];
            uint8_t* o = &(*out)[(static_cast<size_t>(y) * W + x) * 4];
            o[3] = p[3];
            float hs[3], gs[3], hw = 0.0f, gw = 0.0f;
            sample(low, x, y, hs, &hw);
            sample(gp, x, y, gs, &gw);
            if (haveBase) {
                // The redraw is already in the car's default paint, so where the
                // game's sprite is the default paint too nothing changes at all,
                // and a repaint is the sprite's colour relative to its default,
                // applied to the redraw: its own detail, sharp, in the new colour.
                float bs[3], bw = 0.0f;
                sample(bp, x, y, bs, &bw);
                if (gw < 0.25f || bw < 0.25f) {
                    float gain[3];
                    if (sampleGain(x, y, gain)) {
                        for (int c = 0; c < 3; ++c)
                            o[c] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, p[c] * gain[c])));
                    } else {
                        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
                    }
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    const float gain = (std::max)(0.2f, (std::min)(5.0f, (gs[c] + 10.0f) / (bs[c] + 10.0f)));
                    const float v = p[c] * gain;
                    o[c] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, v)));
                }
                continue;
            }
            if (hw < 1e-3f || gw < 0.25f) {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
                continue;
            }
            // Glass is not painted: the game draws a car's windows in the car's
            // own colour, and recolouring the redraw from the sprite would turn
            // them into paint. Where the redraw's own maps say glass - smooth and
            // not metal - the redraw's dark glass is kept as it is.
            float glass = 0.0f;
            if (!hd.roughPx.empty() && !hd.metalPx.empty()) {
                const size_t at = (static_cast<size_t>(y) * W + x) * 4;
                const float rough = hd.roughPx[at] / 255.0f;
                const float metal = hd.metalPx[at] / 255.0f;
                const float a = (std::max)(0.0f, (std::min)(1.0f, (0.17f - rough) / 0.08f));
                const float b = (std::max)(0.0f, (std::min)(1.0f, (0.22f - metal) / 0.10f));
                glass = a * b;
            }
            // hs: the redraw at sprite resolution, but "low" holds sums per block, so
            // it is already an average once divided by the alpha total, as sample does.
            const float lumH = 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
            const float lumS = 0.114f * hs[0] + 0.587f * hs[1] + 0.299f * hs[2];
            float ratio = (lumH + 6.0f) / (lumS + 6.0f);
            ratio = (std::max)(0.4f, (std::min)(2.5f, ratio));
            for (int c = 0; c < 3; ++c) {
                const float painted = gs[c] * ratio;
                const float v = painted + (p[c] - painted) * glass;
                o[c] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, v)));
            }
        }
    }
    return true;
}

remixapi_MaterialHandle CarMaterialFor(IDirect3DTexture9* art, int sprite) {
    if (!art || !RemixApiHasInstances()) return nullptr;
    CarMaterial& slot = g_carMaterials[{art, sprite}];
    if (slot.handle || slot.failed) return slot.handle;
    slot.failed = true;   // until proven otherwise, so a refusal is tried once

    D3DSURFACE_DESC desc;
    if (FAILED(art->GetLevelDesc(0, &desc))) return nullptr;
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) return nullptr;
    D3DLOCKED_RECT locked;
    if (FAILED(art->LockRect(0, &locked, nullptr, D3DLOCK_READONLY))) return nullptr;
    std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(&pixels[static_cast<size_t>(y) * desc.Width * 4],
               static_cast<const uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch,
               static_cast<size_t>(desc.Width) * 4);
    }
    art->UnlockRect(0);
    if (desc.Format == D3DFMT_X8R8G8B8) {
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }

    // The hand-made version of this car, in this paint, when there is one.
    UINT outWidth = desc.Width, outHeight = desc.Height;
    HdCar* hd = nullptr;
    if (g_carHd && sprite >= 0) {
        HdCar& found = HdFor(sprite);
        std::vector<uint8_t> recoloured;
        if (found.ok && RecolourHd(found, pixels, static_cast<int>(desc.Width),
                                   static_cast<int>(desc.Height), &recoloured)) {
            hd = &found;
            outWidth = static_cast<UINT>(found.width);
            outHeight = static_cast<UINT>(found.height);
            pixels.swap(recoloured);
        } else if (g_carMaterialLogs++ < 8) {
            Log("car material: no hand-made artwork for car sprite %d here (%s)", sprite,
                found.ok ? "size does not fit the sprite" : "files not found");
        }
    }

    // Which cars have turned up, and whether they got the hand-made artwork:
    // the sprite number is not the name the game gives a car, so this is the
    // way to tell whether a particular one has been on the road.
    static int s_seenLogs = 0;
    if (s_seenLogs++ < 200) {
        Log("car sprite %d seen: %s", sprite, hd ? "HD artwork" : "original sprite");
    }

    const uint64_t hash = Fnv64(pixels.data(), pixels.size(), Fnv64(&outWidth, sizeof(outWidth)));
    const std::wstring dir = CacheDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\car_%016llx.dds", static_cast<unsigned long long>(hash));
    const std::wstring path = dir + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!WriteDds(path, outWidth, outHeight, pixels)) {
            if (g_carMaterialLogs++ < 8) Log("car material: could not write the artwork file");
            return nullptr;
        }
    }

    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1.0f, 1.0f, 1.0f};
    opaque.opacityConstant = 1.0f;
    // The cut-out comes from the instance, which says what the sprite's alpha
    // test is; see the draw.
    opaque.useDrawCallAlphaState = 1;
    if (hd) {
        // The maps say where it is glass, paint and chrome, so the constants step
        // aside.
        opaque.roughnessConstant = 1.0f;
        opaque.metallicConstant = 1.0f;
        opaque.roughnessTexture = hd->rough.c_str();
        opaque.metallicTexture = hd->metal.c_str();
    } else {
        opaque.roughnessConstant = g_carRoughness;
        opaque.metallicConstant = g_carMetallic;
    }

    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash | 1;
    info.albedoTexture = path.c_str();
    if (hd && g_carNormalMaps) info.normalTexture = PrepareNormal(*hd).c_str();
    info.emissiveColorConstant = {0.0f, 0.0f, 0.0f};
    info.filterMode = 1;   // linear
    info.wrapModeU = 2;    // clamp to edge
    info.wrapModeV = 2;

    remixapi_MaterialHandle handle = nullptr;
    const remixapi_ErrorCode result = RemixApi()->CreateMaterial(&info, &handle);
    if (g_carMaterialLogs++ < 8) {
        Log("car material: CreateMaterial -> %d (%s)", static_cast<int>(result),
            result == REMIXAPI_ERROR_CODE_SUCCESS ? "ok" : "refused");
    }
    if (result != REMIXAPI_ERROR_CODE_SUCCESS || !handle) return nullptr;
    slot.handle = handle;
    slot.failed = false;
    slot.born = g_carFrame;
    return handle;
}

bool CarMaterialWarm(IDirect3DTexture9* art, int sprite) {
    const auto found = g_carMaterials.find({art, sprite});
    return found != g_carMaterials.end() && found->second.handle &&
           g_carFrame - found->second.born >= kCarMaterialWarmup;
}

remixapi_MeshHandle CarMeshFor(const void* vertices, int vertexCount, size_t stride,
                               const std::vector<uint16_t>& indices,
                               remixapi_MaterialHandle material) {
    // The shape depends on the car's size and nothing else, so it is built once.
    uint64_t key = Fnv64(vertices, vertexCount * stride);
    key = Fnv64(&material, sizeof(material), key) | 1;
    auto found = g_carMeshes.find(key);
    if (found != g_carMeshes.end()) return found->second;
    if (g_carMeshes.size() > 1024) return nullptr;
    g_carMeshes[key] = nullptr;   // a refusal is not retried every frame

    std::vector<remixapi_HardcodedVertex> verts(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
        const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(vertices) + i * stride);
        remixapi_HardcodedVertex out = {};
        out.position[0] = v[0];
        out.position[1] = v[1];
        out.position[2] = v[2];
        out.normal[0] = v[3];
        out.normal[1] = v[4];
        out.normal[2] = v[5];
        out.texcoord[0] = v[6];
        out.texcoord[1] = v[7];
        out.color = 0xFFFFFFFFu;
        verts[i] = out;
    }
    std::vector<uint32_t> index32(indices.begin(), indices.end());

    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = verts.data();
    surface.vertices_count = verts.size();
    surface.indices_values = index32.data();
    surface.indices_count = index32.size();
    surface.skinning_hasvalue = 0;
    surface.material = material;
    remixapi_MeshInfo mesh = {};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = key;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;

    remixapi_MeshHandle handle = nullptr;
    const remixapi_ErrorCode result = RemixApi()->CreateMesh(&mesh, &handle);
    if (g_carMaterialLogs++ < 8) {
        Log("car material: CreateMesh -> %d (%s)", static_cast<int>(result),
            result == REMIXAPI_ERROR_CODE_SUCCESS ? "ok" : "refused");
    }
    if (result != REMIXAPI_ERROR_CODE_SUCCESS || !handle) return nullptr;
    g_carMeshes[key] = handle;
    return handle;
}

void ReleaseCarMaterials() {
    // Handles belong to Remix and die with it; only our own bookkeeping goes.
    g_carMaterials.clear();
    g_carMeshes.clear();
    g_warmArt.clear();
}
}  // namespace

void SetCarNormalStrength(float strength) {
    g_carNormalStrength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
}

void SetCarHd(bool hd, bool normalMaps) {
    g_carHd = hd;
    g_carNormalMaps = normalMaps;
}

void LiveGeometry::BeginFrame() {
    sprites_.clear();
    shells_.clear();
    lastObject_ = nullptr;
    stacks_.clear();
    spriteQuads_ = 0;
    effectQuads_ = 0;
    g_lastConform = conform_;
    conform_ = Conform{};
    // This frame's placements become next frame's history.
    tracks_.swap(tracksNext_);
    tracksNext_.clear();
}

const LiveGeometry::Track* LiveGeometry::FindTrack(const void* texture, float x, float z) const {
    const Track* best = nullptr;
    float bestDistance = kTrackRadius * kTrackRadius;
    for (const Track& track : tracks_) {
        // The texture has to match: two sprites can share a spot - a car and its
        // own headlight quad - and easing one toward the other's answer is the
        // pop this is meant to remove, not a smaller one.
        if (track.texture != texture) continue;
        const float dx = track.x - x, dz = track.z - z;
        const float distance = dx * dx + dz * dz;
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &track;
        }
    }
    return best;
}

// Which layer of a stack this sprite belongs to.
//
// Two quads count as stacked when their centres are within about a third of a
// tile, which is well inside a car and well outside the gap to the next one. The
// search is linear over the sprites already taken this frame, which is tens of
// entries - a car's worth of parts, a few pedestrians - so it costs nothing
// worth indexing away.
//
// Only a sprite that is not much smaller counts as something to stack on. The
// stack is for a car's lights, logo and roof light, which are drawn after the
// body and are its own size or smaller, so they still climb onto it. It is not
// for the burst of exhaust smoke a car puffs out under itself as it pulls away:
// six small particles drawn *before* the body counted as a pile six deep and
// lifted the car by six steps, three times its ride height, which in Remix
// tears its shadow off - and then it eased back down as the smoke died. That
// was the hop when a car starts from standstill, and whatever small thing sat
// under a parked car held it up the same way.
int LiveGeometry::StackLayerFor(float cx, float cz, float area) {
    const float kSameSpot = 0.33f * 0.33f;
    // A quarter of the area: a car's roof light still counts against a car,
    // a smoke puff or a pedestrian does not.
    const float kComparable = 0.25f;
    int layer = 0;
    for (const Stack& s : stacks_) {
        const float dx = s.x - cx, dz = s.z - cz;
        if (dx * dx + dz * dz > kSameSpot || s.layer < layer) continue;
        if (s.area < area * kComparable) continue;
        layer = s.layer + 1;
    }
    // A car with a light and a logo is three deep; anything claiming more than
    // this is sprites that happen to share a spot rather than a real stack, and
    // letting it climb would float them.
    const int kMaxLayers = 6;
    if (layer > kMaxLayers) layer = kMaxLayers;
    stacks_.push_back({cx, cz, area, layer});
    return layer;
}

// The game's world frame is x east, y south, level up. Ours is x east, y up,
// z north, so the row is mirrored exactly the way the static mesh builder
// mirrors it - sharing the sign would put sprites on the wrong side of town.
bool LiveGeometry::ReadCorners(const float* vertices, int corners, const void* texture,
                              uintptr_t shadowBase, Vertex* out) const {
    const TextureRecord* record = static_cast<const TextureRecord*>(texture);
    const float invU = record && record->width ? 1.0f / record->width : 1.0f / 64.0f;
    const float invV = record && record->height ? 1.0f / record->height : 1.0f / 64.0f;

    for (int i = 0; i < corners; ++i) {
        const float* world = reinterpret_cast<const float*>(
            shadowBase + static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
        if (!std::isfinite(world[0]) || !std::isfinite(world[1]) || !std::isfinite(world[2])) {
            ++drops_.notFinite;
            return false;
        }
        if (world[0] < -kWorldMargin || world[0] > gta2::kMapWidth + kWorldMargin ||
            world[1] < -kWorldMargin || world[1] > gta2::kMapHeight + kWorldMargin ||
            world[2] < kMinLevel || world[2] > kMaxLevel) {
            ++drops_.outOfWorld;
            return false;
        }
        out[i].x = world[0];
        out[i].y = world[2];
        out[i].z = static_cast<float>(gta2::kMapHeight) - world[1];
        out[i].u = vertices[i * kGameVertexFloats + kUvU] * invU;
        out[i].v = vertices[i * kGameVertexFloats + kUvV] * invV;
    }

    // A real normal rather than a fixed "up": GTA2 sprites lie flat but the odd
    // block faces are vertical, and Remix uses the normal.
    const Vec e1{out[1].x - out[0].x, out[1].y - out[0].y, out[1].z - out[0].z};
    const Vec e2{out[2].x - out[0].x, out[2].y - out[0].y, out[2].z - out[0].z};
    Vec n = Cross(e1, e2);
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < 1e-9f) {
        ++drops_.degenerate;  // no area, nothing to draw
        return false;
    }
    n = {n.x / length, n.y / length, n.z / length};
    for (int i = 0; i < corners; ++i) {
        out[i].nx = n.x;
        out[i].ny = n.y;
        out[i].nz = n.z;
    }
    return true;
}

namespace {

// The quad a sprite has always had, rather than the one this frame worked out.
//
// Quantising the measured quad is not quite enough. The corners arrive from the
// game's fixed point through a normalise, so the last mantissa bits move about
// even for a parked car, and a value that happens to sit on a rounding boundary
// dithers between two quantised results - which is precisely the vertex-hash
// churn this whole exercise exists to remove.
//
// So the first quad seen for a sprite is quantised and kept, and every later one
// that is the same shape to within a coarse tolerance is answered with the
// stored copy, bit for bit. A sprite genuinely drawn at a second size gets a
// second entry; the cap is there so that something continuously scaled cannot
// grow this without bound.
struct Shape {
    LiveGeometry::Vertex local[4];
};
std::map<const void*, std::vector<Shape>> g_shapes;
constexpr size_t kMaxShapesPerTexture = 8;

void CanonicalShape(const void* texture, const LiveGeometry::Vertex* measured, int corners,
                    LiveGeometry::Vertex* out) {
    // A 256th of a tile, about 8 mm at GTA2's scale: far above the float noise
    // and far below any real difference in sprite size.
    const float kTolerance = 1.0f / 256.0f;

    std::vector<Shape>& known = g_shapes[texture];
    for (const Shape& shape : known) {
        bool same = true;
        for (int i = 0; i < corners && same; ++i) {
            same = std::fabs(shape.local[i].x - measured[i].x) < kTolerance &&
                   std::fabs(shape.local[i].y - measured[i].y) < kTolerance &&
                   std::fabs(shape.local[i].z - measured[i].z) < kTolerance &&
                   std::fabs(shape.local[i].u - measured[i].u) < kTolerance &&
                   std::fabs(shape.local[i].v - measured[i].v) < kTolerance;
        }
        if (same) {
            memcpy(out, shape.local, sizeof(Shape::local[0]) * corners);
            return;
        }
    }

    Shape shape{};
    auto quantise = [](float value) { return std::floor(value * 4096.0f + 0.5f) / 4096.0f; };
    for (int i = 0; i < corners; ++i) {
        shape.local[i] = measured[i];
        shape.local[i].x = quantise(measured[i].x);
        shape.local[i].y = quantise(measured[i].y);
        shape.local[i].z = quantise(measured[i].z);
    }
    memcpy(out, shape.local, sizeof(Shape::local[0]) * corners);
    if (known.size() < kMaxShapesPerTexture) known.push_back(shape);
}

}  // namespace

void SpriteShapeCounts(int* shapes, int* textures) {
    int total = 0;
    for (const auto& entry : g_shapes) total += static_cast<int>(entry.second.size());
    if (shapes) *shapes = total;
    if (textures) *textures = static_cast<int>(g_shapes.size());
}

// Take the quad apart into a shape and a placement.
//
// The shape is the quad measured in its own frame: origin at its centre, one
// axis along its first edge, one along its normal. A GTA2 sprite is rigid, so
// for a given sprite that measurement is the same numbers wherever it stands and
// however it is turned - which is exactly the property Remix needs to recognise
// it again next frame. The placement is that frame's position in the world, and
// it is what Remix differentiates to get the motion vector.
//
// The measured shape goes through CanonicalShape above, which is what makes it
// bit-identical rather than merely close.
bool LiveGeometry::Place(const Vertex* world, int corners, const Vec3& centre,
                         const Vec3* groundUp, Sprite* out) const {
    // The corners' own centroid, which is what the shape is measured about. It is
    // deliberately not `centre`: the shape has to come out the same numbers
    // wherever the sprite stands, so the placement can never be allowed to leak
    // into it.
    const Vec measured{
        (world[0].x + world[1].x + world[2].x + world[3].x) * 0.25f,
        (world[0].y + world[1].y + world[2].y + world[3].y) * 0.25f,
        (world[0].z + world[1].z + world[2].z + world[3].z) * 0.25f,
    };

    // The quad's own frame: right along its first edge, up along its normal,
    // forward completing the set. Orthonormal by construction, so the matrix is a
    // rotation and a translation and nothing else - normals survive it unchanged
    // and Remix's inverse of it is exact.
    const Vec edge{world[1].x - world[0].x, world[1].y - world[0].y, world[1].z - world[0].z};
    const Vec up{world[0].nx, world[0].ny, world[0].nz};   // ReadCorners already normalised this

    const float edgeLength = std::sqrt(edge.x * edge.x + edge.y * edge.y + edge.z * edge.z);
    if (edgeLength < 1e-6f) {
        ++drops_.degenerate;
        return false;
    }
    const Vec right{edge.x / edgeLength, edge.y / edgeLength, edge.z / edgeLength};
    // Cross(right, up) rather than Cross(up, right): the three axes have to come
    // out right handed or the matrix is a reflection, which flips the winding and
    // has Remix guessing at which way the face points.
    const Vec forward = Cross(right, up);

    Vertex local[4];
    for (int i = 0; i < corners; ++i) {
        const Vec offset{world[i].x - measured.x, world[i].y - measured.y, world[i].z - measured.z};
        local[i].x = offset.x * right.x + offset.y * right.y + offset.z * right.z;
        local[i].y = offset.x * up.x + offset.y * up.y + offset.z * up.z;
        local[i].z = offset.x * forward.x + offset.y * forward.y + offset.z * forward.z;
        // The normal is the frame's own up, so in object space it is exactly that
        // for every sprite - one more thing that cannot drift.
        local[i].nx = 0.0f;
        local[i].ny = 1.0f;
        local[i].nz = 0.0f;
        local[i].u = world[i].u;
        local[i].v = world[i].v;
    }
    CanonicalShape(out->texture, local, corners, out->local);
    out->corners = corners;

    // Now the same frame rotated onto the ground. The shape above was measured
    // against the quad's own up; rotating both axes together is a rigid motion of
    // that frame, so local[] still describes the same sprite - which is precisely
    // why the tilt goes here and not into the vertices.
    Vec rightOut = right;
    Vec upOut = up;
    Vec forwardOut = forward;
    if (groundUp) {
        // The quad's normal can point either way depending on how the game wound
        // it, and the ground normal only ever points up. Matching the sign keeps
        // the frame's handedness, which local[] was built against; taking it
        // straight would mirror every sprite whose quad happened to face down.
        const float sign = up.y >= 0.0f ? 1.0f : -1.0f;
        const Vec ground{groundUp->x * sign, groundUp->y * sign, groundUp->z * sign};
        // The heading, re-squared against the new up. A GTA2 sprite's first edge
        // is horizontal, so this is a small rotation about it and nothing else.
        const float along = right.x * ground.x + right.y * ground.y + right.z * ground.z;
        const Vec projected{right.x - ground.x * along, right.y - ground.y * along,
                            right.z - ground.z * along};
        const float length = std::sqrt(projected.x * projected.x + projected.y * projected.y +
                                       projected.z * projected.z);
        // Only a quad standing on its edge could line up with the ground normal,
        // and there is no heading to recover from one. Those keep their own frame.
        if (length > 1e-4f) {
            rightOut = {projected.x / length, projected.y / length, projected.z / length};
            upOut = ground;
            forwardOut = Cross(rightOut, upOut);
        }
    }

    // D3D9 multiplies row vectors on the left, so the basis goes in the rows and
    // the translation in the last one.
    gta2::Mat4& m = out->objectToWorld;
    m = gta2::Mat4{};
    m.m[0][0] = rightOut.x;   m.m[0][1] = rightOut.y;   m.m[0][2] = rightOut.z;
    m.m[1][0] = upOut.x;      m.m[1][1] = upOut.y;      m.m[1][2] = upOut.z;
    m.m[2][0] = forwardOut.x; m.m[2][1] = forwardOut.y; m.m[2][2] = forwardOut.z;
    m.m[3][0] = centre.x;     m.m[3][1] = centre.y;     m.m[3][2] = centre.z;
    m.m[3][3] = 1.0f;
    return true;
}

void LiveGeometry::AddSprite(unsigned flags, const void* texture, const float* vertices,
                             int corners, bool lamps) {
    if (!texture || !vertices) return;
    // With this flag only vertex 0 is filled in, so the other three shadow slots
    // still hold whatever the previous draw left there.
    if (flags & quad_flags::kExpandFromTexture) {
        ++drops_.expandFlag;
        return;
    }

    // The shadow slots only describe the game's *own* vertex array. FUN_004be060
    // also draws an object's drop shadow from a nudged stack copy of those
    // vertices, and pairing that copy with the unmoved world positions stretched
    // the sprite into a second, wrong polygon - the extra "sprouting" panels on
    // trees. Only the real array can be trusted here.
    if (reinterpret_cast<uintptr_t>(vertices) != game::kSpriteVertexArray) {
        ++drops_.notTheSpriteArray;
        return;
    }

    // A pedestrian blown apart: the limbs are on the ground, so the body is not.
    // See gibs.cpp.
    if (GibsHideCurrentSprite()) return;

    Vertex out[4];
    if (!ReadCorners(vertices, corners, texture, game::kSpriteVertexArray, out)) return;

    // A car's lamps, laid on its body.
    //
    // The game draws them as a second quad with exactly the body's corners - the
    // same vertex array, only the texture coordinates set again (FUN_004B9BC0) -
    // and placed on its own it drifted from the body: a ground fit, a smoothing
    // history and a stack layer of its own, which on a slope or a kerb left the
    // lamps inside the car. So it takes the body's placement outright, shape and
    // transform, and sits a hair above it along the body's own normal, where it
    // cannot end up anywhere but on top.
    if (lamps) {
        const void* object = GameObjectBeingDrawn();
        if (g_ownCarLamps && object && object == lastObject_ && lastSprite_ < sprites_.size()) {
            // Ours instead of the game's. See OwnCarLamps.
            AddCarLamps(sprites_[lastSprite_]);
            return;
        }
        if (object && object == lastObject_ && lastSprite_ < sprites_.size() && corners == 4) {
            Sprite placed = sprites_[lastSprite_];
            placed.hidden = false;
            placed.texture = texture;
            // The quad's own up can face the ground, depending on how the game
            // wound it; "above" is whichever way its frame's up points in the
            // world.
            const float above = placed.objectToWorld.m[1][1] >= 0.0f ? 1.0f : -1.0f;
            for (int i = 0; i < 4; ++i) {
                placed.local[i].u = out[i].u;
                placed.local[i].v = out[i].v;
                placed.local[i].y += kConformClearance * above;
            }
            sprites_.push_back(placed);
            ++spriteQuads_;
            ++drops_.accepted;
            return;
        }
        // No body this frame to lay them on; placed like any other sprite.
    }

    // Limbs are drawn larger than their artwork. See gibs.cpp.
    const float scale = corners == 4 ? GibsCurrentScale() : 1.0f;
    if (scale != 1.0f) {
        const float mx = (out[0].x + out[1].x + out[2].x + out[3].x) * 0.25f;
        const float mz = (out[0].z + out[1].z + out[2].z + out[3].z) * 0.25f;
        for (int i = 0; i < 4; ++i) {
            out[i].x = mx + (out[i].x - mx) * scale;
            out[i].z = mz + (out[i].z - mz) * scale;
        }
    }

    // Only quads come through here - gbh_DrawQuad and gbh_DrawQuadClipped are the
    // only callers - and Place measures the frame from four corners.
    if (corners != 4) {
        ++drops_.degenerate;
        return;
    }

    // Where the game put the sprite. All four corners carry the single level of
    // the object, so this is that level and the footprint it covers.
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int i = 0; i < corners; ++i) {
        cx += out[i].x;
        cy += out[i].y;
        cz += out[i].z;
    }
    const float inv = 1.0f / static_cast<float>(corners);
    cx *= inv;
    cy *= inv;
    cz *= inv;

    // The stack layer is decided from where the sprite sits, before it is
    // lifted, so a car's lights and logo land on the body rather than on each
    // other's raised copies.
    // A quad's corners go round its edge in order, so two adjacent sides give
    // its footprint.
    const float sideAx = out[1].x - out[0].x, sideAz = out[1].z - out[0].z;
    const float sideBx = out[2].x - out[1].x, sideBz = out[2].z - out[1].z;
    const float area = std::sqrt(sideAx * sideAx + sideAz * sideAz) *
                       std::sqrt(sideBx * sideBx + sideBz * sideBz);
    // Skid marks, blood trails and blood pools lie on the road rather than
    // standing on it: no ride height, and none of the clearance a car is given.
    // They still follow the ground's tilt, keep the quarter-texel gap that
    // breaks coplanarity with the road, and stack against each other - but by
    // a sliver per layer, because a trail is dozens of overlapping strips and a
    // car's worth of stack step each piled them into the air.
    const bool decal = IsGroundDecal(texture);
    // A limb or a body lies on the road rather than standing on it like a
    // pedestrian, but it is not flat paint either: a third of a sprite's ride
    // height.
    const bool lying = scale != 1.0f || GameObjectIsCorpse();
    // A limb that has landed goes on the floor, whatever height the game left it
    // at. See GibsCurrentIsResting.
    const bool resting = scale != 1.0f && GibsCurrentIsResting();
    const float stackStep = decal ? kConformClearance * 0.25f : g_spriteStackStep;
    const float stacked = stackStep > 0.0f ? StackLayerFor(cx, cz, area) * stackStep : 0.0f;

    // Stand it on the floor the map says is under it. See the note on
    // g_spriteConform: the quad itself is not touched, only the frame it is
    // placed in.
    Vec3 groundUp{0.0f, 1.0f, 0.0f};
    bool tilted = false;
    float baseY = cy;
    float clearance = g_spriteLift;
    // The real floor under the corners and the middle, kept for the guard after
    // smoothing. A sample with no floor under it is left out.
    float guardX[5], guardZ[5], guardFloor[5];
    int guardSamples = 0;

    // A quad standing on its edge has no floor under it in any useful sense.
    // Nothing on this path should be one, but the guard is a line and the
    // alternative is a sprite laid flat on the road.
    const bool flatQuad = std::fabs(out[0].ny) > 0.5f;
    if (g_spriteConform && ground_ && ground_->Ready() && flatQuad) {
        // The corners plus the middle. A car parked across the ridge where a ramp
        // meets the flat has all four corners low and the crest under its belly,
        // which four samples cannot see at all.
        const float xs[5] = {out[0].x, out[1].x, out[2].x, out[3].x, cx};
        const float zs[5] = {out[0].z, out[1].z, out[2].z, out[3].z, cz};
        const gta2dx9::GroundPlane plane =
            ground_->Fit(xs, zs, corners + 1, cy + GroundSampler::kCeilingHeadroom, cx, cz);
        if (plane.valid) {
            for (int i = 0; i < corners + 1; ++i) {
                float floorHere = 0.0f;
                if (!ground_->SurfaceAt(xs[i], zs[i], cy + GroundSampler::kCeilingHeadroom,
                                        &floorHere)) {
                    continue;
                }
                guardX[guardSamples] = xs[i];
                guardZ[guardSamples] = zs[i];
                guardFloor[guardSamples] = floorHere;
                ++guardSamples;
            }
        }
        if (!plane.valid) {
            ++conform_.noGround;
        } else {
            const float gap = resting ? 0.0f : cy - plane.y;
            if (gap > conform_.maxGap) conform_.maxGap = gap;
            if (plane.residual > conform_.maxResidual) conform_.maxResidual = plane.residual;

            // Never downward: above its floor, the game's own height wins -
            // except for a limb at rest, whose gap is taken as none.
            baseY = gap > 0.0f ? cy : plane.y;

            // The gradient the ground actually has. Working in gradient rather
            // than in the normal keeps every adjustment below a plain scaling of
            // two numbers, and the normal falls out at the end.
            const float rawGx = -plane.nx / plane.ny;
            const float rawGz = -plane.nz / plane.ny;

            // Two reasons to use less of it than all. A footprint that is not one
            // plane has a fit that means little, so a large residual damps the
            // tilt instead of steering it - that is a corner over a kerb, which
            // is exactly the case that used to lift one corner of a car. And a
            // sprite off the ground levels out over the same gap the lift fades
            // across.
            const float trust = 1.0f - Smoothstep(kFitGood, kFitPoor, plane.residual);
            const float fade = 1.0f - Smoothstep(kTiltFadeNear, kTiltFadeFar, gap);
            float gx = rawGx * trust * fade;
            float gz = rawGz * trust * fade;

            // Pitch is kept, roll is damped. The long side of the quad is the
            // sprite's own direction of travel - two blocks against one for a
            // car - so a gradient along it tips the nose, which is what driving
            // up a ramp looks like, and a gradient across it lifts one side,
            // which a car on wheels does not do. See kDefaultSpriteRoll.
            float axisX = out[1].x - out[0].x, axisZ = out[1].z - out[0].z;
            const float sideA = std::sqrt(axisX * axisX + axisZ * axisZ);
            const float otherX = out[2].x - out[1].x, otherZ = out[2].z - out[1].z;
            const float sideB = std::sqrt(otherX * otherX + otherZ * otherZ);
            if (sideB > sideA) {
                axisX = otherX;
                axisZ = otherZ;
            }
            const float axisLength = (std::max)(sideA, sideB);
            if (axisLength > 1e-6f && g_spriteRoll < 1.0f) {
                axisX /= axisLength;
                axisZ /= axisLength;
                const float along = gx * axisX + gz * axisZ;   // the pitch part
                const float acrossX = gx - axisX * along;      // the roll part
                const float acrossZ = gz - axisZ * along;
                gx = axisX * along + acrossX * g_spriteRoll;
                gz = axisZ * along + acrossZ * g_spriteRoll;
            }

            const float slope = std::sqrt(gx * gx + gz * gz);
            if (slope > kMaxTiltTan) {
                const float scale = kMaxTiltTan / slope;
                gx *= scale;
                gz *= scale;
                ++conform_.capped;
            }
            if (slope > 1e-6f) {
                const float inv = 1.0f / std::sqrt(gx * gx + 1.0f + gz * gz);
                groundUp = {-gx * inv, inv, -gz * inv};
                tilted = true;
            }

            // Clearance is measured against the tilt the sprite actually ended up
            // with, not the one the ground has - so everything given away above,
            // to the roll damping, the trust and the cap, comes back here as
            // height instead of as a sprite cutting into the road.
            float needed = 0.0f;
            if (gap > kConformClearance) {
                // The game has it above the plane fitted under it, and that
                // plane is then not the floor. The case is a car on the upper
                // of two floors with part of its footprint over the lower one -
                // a kerb, a block edge: the fit runs a slope between the two,
                // and its residual measures the step, not anything under the
                // car. Adding that back as clearance floated the car by up to
                // half a block, and eased it up over a few frames as it started
                // to move onto an edge - the hop - or held it up while it stood
                // there and dropped it as it drove off.
                //
                // The samples themselves say what is actually under it: clear
                // whatever real floor rises above the quad, and nothing else.
                // The lower floor it overhangs contributes nothing, and the one
                // it stands on only what its own tilt dips into.
                const float ceiling = cy + GroundSampler::kCeilingHeadroom;
                for (int i = 0; i < corners + 1; ++i) {
                    float floorHere = 0.0f;
                    if (!ground_->SurfaceAt(xs[i], zs[i], ceiling, &floorHere)) continue;
                    const float quadHere = baseY + gx * (xs[i] - cx) + gz * (zs[i] - cz);
                    if (floorHere - quadHere > needed) needed = floorHere - quadHere;
                }
            } else {
                // On the fitted floor. The residual is how far the highest sample
                // pokes above the plane, so the quad has to clear that plus
                // whatever the plane itself rises above the quad at a corner -
                // which is where the gap between two flat things is widest.
                float worst = 0.0f;
                for (int i = 0; i < corners; ++i) {
                    const float dx = out[i].x - cx;
                    const float dz = out[i].z - cz;
                    const float floorHere = plane.y + rawGx * dx + rawGz * dz;
                    const float quadHere = baseY + gx * dx + gz * dz;
                    if (floorHere - quadHere > worst) worst = floorHere - quadHere;
                }
                needed = worst + plane.residual;
            }
            const float owed = (needed - g_spriteHeight * groundUp.y) / groundUp.y;
            const float extra = owed > 0.0f ? (std::min)(owed, kMaxExtraClearance) : 0.0f;
            clearance = kConformClearance + extra;
            if (needed > kConformClearance) ++conform_.straddled;
            if (fade < 1.0f) {
                ++conform_.airborne;
            } else {
                ++conform_.grounded;
            }
        }
    }

    // Ease toward this frame's answer rather than snapping to it. Only the lift
    // and the tilt are smoothed: baseY follows the ground directly, because a car
    // driving up a ramp *should* rise with it and lagging that would be the
    // strange movement rather than the fix for it. See the Track note.
    float offset = decal  ? kConformClearance + stacked
                 : lying  ? kConformClearance + g_spriteHeight * 0.33f + stacked
                          : g_spriteHeight + clearance + stacked;
    if (const Track* previous = FindTrack(texture, cx, cz)) {
        offset = previous->offset + (offset - previous->offset) * kSmoothing;
        Vec3 eased{previous->nx + (groundUp.x - previous->nx) * kSmoothing,
                   previous->ny + (groundUp.y - previous->ny) * kSmoothing,
                   previous->nz + (groundUp.z - previous->nz) * kSmoothing};
        const float length =
            std::sqrt(eased.x * eased.x + eased.y * eased.y + eased.z * eased.z);
        if (length > 1e-6f) {
            groundUp = {eased.x / length, eased.y / length, eased.z / length};
            tilted = std::fabs(groundUp.x) > 1e-5f || std::fabs(groundUp.z) > 1e-5f;
        }
    }

    // The floor guard, after the smoothing and against what is actually drawn.
    //
    // Clearance above is worked out for the tilt and height the sprite is
    // heading for, but both are then eased there over a few frames. Driving onto
    // a ramp that lag is the clip: the target tips the nose up and adds the
    // clearance the kink needs, the eased sprite is still nearly level and
    // nearly as low, and for a few frames the front corner is inside the slope.
    // So measure the real floor at the corners and the middle against the tilt
    // this frame really has, and if any of them would be under it, raise the
    // sprite to just clear it - now, not eased. Only ever up, and only as far as
    // the floor demands, so nothing already clear moves: easing still decides
    // everything else, and lowering stays smooth.
    if (guardSamples > 0 && groundUp.y > 1e-3f) {
        const float gx = -groundUp.x / groundUp.y;
        const float gz = -groundUp.z / groundUp.y;
        float least = offset;
        for (int i = 0; i < guardSamples; ++i) {
            const float under = baseY + gx * (guardX[i] - cx) + gz * (guardZ[i] - cz);
            const float need = (guardFloor[i] + kConformClearance - under) / groundUp.y;
            if (need > least) least = need;
        }
        offset = least;
    }
    tracksNext_.push_back({texture, cx, cz, offset, groundUp.x, groundUp.y, groundUp.z});

    // Along the ground normal rather than straight up: on a ramp those are not
    // the same direction, and clearance measured the wrong way is not clearance.
    // The ride height goes the same way, so a car on a ramp rides above the ramp
    // rather than leaning out of it.
    const Vec3 centre{cx + groundUp.x * offset, baseY + groundUp.y * offset,
                      cz + groundUp.z * offset};

    Sprite placed;
    placed.texture = texture;
    if (!Place(out, corners, centre, tilted ? &groundUp : nullptr, &placed)) return;

    // Which game object this is, written into the quad itself.
    //
    // Every sprite of one texture and size used to be the very same geometry, so
    // to Remix two pedestrians on the same animation frame, or two phone boxes,
    // were one mesh drawn twice. It tells those instances apart from frame to
    // frame by position alone (InstanceManager::findSimilarInstance: an exact
    // transform, else the nearest unclaimed instance of that mesh), and a
    // transform that moves by so much as a stack step loses the exact match. The
    // nearest one is then sometimes the other object, which hands this sprite
    // that object's history - its previous position and its motion - and the
    // denoiser and DLSS draw it where the other one was. That is the copy in the
    // wrong place.
    //
    // The game's sprite object is the identity: the same object every frame for
    // as long as the thing exists, and no two live things share one. A few low
    // bits of it become an offset along the quad's normal of at most 6e-5 tiles -
    // a quarter of a millimetre - which no one can see and which makes each
    // object's quad a mesh of its own, so there is nothing left to confuse.
    if (const void* object = GameObjectBeingDrawn()) {
        const uint32_t bits = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object) >> 2);
        const float nudge = static_cast<float>(bits & 0xFFFF) * (1.0f / 1073741824.0f);
        for (int i = 0; i < placed.corners; ++i) placed.local[i].y += nudge;
    }
    placed.plain = GameObjectIsPowerUp();
    if (g_carBodies && !decal && corners == 4) RaiseCarBody(&placed);
    lastObject_ = GameObjectBeingDrawn();
    lastSprite_ = sprites_.size();
    sprites_.push_back(placed);
    ++spriteQuads_;
    ++drops_.accepted;

    // The game's own corner positions, straight out of the shadow slots and
    // before our axis swap, so the question "does GTA2 lift a sprite off the
    // floor it stands on?" can be answered from its numbers rather than ours.
    if (spriteLogs_ < 8) {
        ++spriteLogs_;
        char line[320];
        int used = snprintf(line, sizeof(line), "sprite corners (game frame x,y,level):");
        for (int i = 0; i < corners && used > 0 && used < static_cast<int>(sizeof(line)) - 40; ++i) {
            const float* world = reinterpret_cast<const float*>(
                game::kSpriteVertexArray +
                static_cast<uintptr_t>(i + game::kWorldShadowSlots) * game::kVertexStride);
            used += snprintf(line + used, sizeof(line) - used, "  (%.4f %.4f %.4f)", world[0],
                             world[1], world[2]);
        }
        Log("%s", line);
    }
}

// A car's dome. See CarShell.
//
// Local space is the sprite's own: x along its first edge, z across, y along the
// normal. `above` is which way y points in the world, as for the lamps. The body
// quad itself is left exactly as it was, flat on the ground, underneath.
void LiveGeometry::RaiseCarBody(Sprite* body) {
    if (!body || body->corners != 4 || body->lamp) return;
    const uint8_t* object = static_cast<const uint8_t*>(GameObjectBeingDrawn());
    if (!object || !game::PlausiblePointer(object)) return;
    if (*reinterpret_cast<const int32_t*>(object + game::kSpriteObjectBase) != game::kSpriteBaseCar) {
        return;
    }
    if (g_carBodyHeight < 1e-4f) return;

    const float above = body->objectToWorld.m[1][1] >= 0.0f ? 1.0f : -1.0f;
    const float peak = g_carBodyHeight * above;
    // Clear of the flat body underneath, so the two never fight for the pixel.
    const float clear = 0.012f * above;

    constexpr int n = kDomeSegments;
    constexpr float kPi = 3.14159265358979f;
    struct P {
        float x, y, z;
    };
    // Position, texture coordinate of grid point (i, j): the bilinear blend of
    // the body's corners, which also carries however the game turned the sprite.
    auto blend = [&](float s, float t, float* u, float* v) {
        const float w0 = (1 - s) * (1 - t), w1 = s * (1 - t), w2 = s * t, w3 = (1 - s) * t;
        const Vertex* c = body->local;
        *u = c[0].u * w0 + c[1].u * w1 + c[2].u * w2 + c[3].u * w3;
        *v = c[0].v * w0 + c[1].v * w1 + c[2].v * w2 + c[3].v * w3;
        return P{c[0].x * w0 + c[1].x * w1 + c[2].x * w2 + c[3].x * w3,
                 c[0].y * w0 + c[1].y * w1 + c[2].y * w2 + c[3].y * w3,
                 c[0].z * w0 + c[1].z * w1 + c[2].z * w2 + c[3].z * w3};
    };
    // How high the dome stands at (s, t): flat-topped rather than pointed.
    auto height = [&](float s, float t) {
        const float k = std::sin(kPi * s) * std::sin(kPi * t);
        return clear + peak * std::pow((std::max)(k, 0.0f), 0.7f);
    };

    P grid[n + 1][n + 1];
    float us[n + 1][n + 1], vs[n + 1][n + 1];
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float s = static_cast<float>(i) / n, t = static_cast<float>(j) / n;
            P p = blend(s, t, &us[j][i], &vs[j][i]);
            p.y += height(s, t);
            grid[j][i] = p;
        }
    }

    // Which way is out of the sprite, from the body's own normal if it has one.
    P facing{body->local[0].nx, body->local[0].ny, body->local[0].nz};
    if (std::fabs(facing.x) + std::fabs(facing.y) + std::fabs(facing.z) < 1e-4f) {
        facing = {0.0f, above, 0.0f};
    }

    CarShell shell;
    shell.texture = body->texture;
    shell.sprite = static_cast<int>(*reinterpret_cast<const uint16_t*>(object + game::kSpriteObjectNumber));
    shell.object = reinterpret_cast<uintptr_t>(object);
    shell.objectToWorld = body->objectToWorld;
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            // Central differences along both axes, one-sided at the rim.
            const P& a = grid[j][(std::max)(i - 1, 0)];
            const P& b = grid[j][(std::min)(i + 1, n)];
            const P& c = grid[(std::max)(j - 1, 0)][i];
            const P& d = grid[(std::min)(j + 1, n)][i];
            const P ds{b.x - a.x, b.y - a.y, b.z - a.z};
            const P dt{d.x - c.x, d.y - c.y, d.z - c.z};
            P nrm{ds.y * dt.z - ds.z * dt.y, ds.z * dt.x - ds.x * dt.z,
                  ds.x * dt.y - ds.y * dt.x};
            const float len = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
            if (len < 1e-9f) return;   // a degenerate car: leave it flat
            nrm = {nrm.x / len, nrm.y / len, nrm.z / len};
            if (nrm.x * facing.x + nrm.y * facing.y + nrm.z * facing.z < 0.0f) {
                nrm = {-nrm.x, -nrm.y, -nrm.z};
            }
            Vertex& out = shell.local[j * (n + 1) + i];
            out.x = grid[j][i].x;
            out.y = grid[j][i].y;
            out.z = grid[j][i].z;
            out.nx = nrm.x;
            out.ny = nrm.y;
            out.nz = nrm.z;
            out.u = us[j][i];
            out.v = vs[j][i];
        }
    }
    shells_.push_back(shell);
    body->shell = static_cast<int>(shells_.size()) - 1;
    // The dome stands in for the body entirely when it has hand-made artwork:
    // the flat sprite beneath it would show at the edge and fight it for depth.
    if (g_carHd && g_carMaterial && RemixApiHasInstances() && HdFor(shell.sprite).ok) {
        body->hidden = true;
    }
}

// Each lit lamp of the car just drawn, as a quad of its own over the lamp's
// pixels on the body. The body's corners carry the sprite's texture coordinates,
// so a point of the sprite at (u, v) is the bilinear blend of the corners by
// where u and v fall between the corners' own - which also takes care of however
// the game turned or mirrored the quad.
void LiveGeometry::AddCarLamps(const Sprite& body) {
    const uint8_t* object = static_cast<const uint8_t*>(GameObjectBeingDrawn());
    if (!object || body.corners != 4) return;
    if (*reinterpret_cast<const int32_t*>(object + game::kSpriteObjectBase) != game::kSpriteBaseCar) {
        return;
    }
    const uint8_t* car = *reinterpret_cast<const uint8_t* const*>(object + game::kSpriteObjectOwner);
    if (!game::PlausiblePointer(car)) return;
    const uint32_t mask = *reinterpret_cast<const uint32_t*>(car + game::kVehicleDeltaMask);
    const int sprite = *reinterpret_cast<const uint16_t*>(object + game::kSpriteObjectNumber);

    float u0 = body.local[0].u, u1 = u0, v0 = body.local[0].v, v1 = v0;
    for (int i = 1; i < 4; ++i) {
        u0 = (std::min)(u0, body.local[i].u);
        u1 = (std::max)(u1, body.local[i].u);
        v0 = (std::min)(v0, body.local[i].v);
        v1 = (std::max)(v1, body.local[i].v);
    }
    if (u1 - u0 < 1e-6f || v1 - v0 < 1e-6f) return;
    // Which corner is which, by its texture coordinates: 0 at the low end.
    int corner[2][2] = {{0, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i) {
        const int su = body.local[i].u > (u0 + u1) * 0.5f ? 1 : 0;
        const int sv = body.local[i].v > (v0 + v1) * 0.5f ? 1 : 0;
        corner[su][sv] = i;
    }
    auto at = [&](float u, float v) {
        const float a = (u - u0) / (u1 - u0), b = (v - v0) / (v1 - v0);
        const Vertex& p00 = body.local[corner[0][0]];
        const Vertex& p10 = body.local[corner[1][0]];
        const Vertex& p01 = body.local[corner[0][1]];
        const Vertex& p11 = body.local[corner[1][1]];
        Vertex out = p00;
        out.x = (p00.x * (1 - a) + p10.x * a) * (1 - b) + (p01.x * (1 - a) + p11.x * a) * b;
        out.y = (p00.y * (1 - a) + p10.y * a) * (1 - b) + (p01.y * (1 - a) + p11.y * a) * b;
        out.z = (p00.z * (1 - a) + p10.z * a) * (1 - b) + (p01.z * (1 - a) + p11.z * a) * b;
        return out;
    };
    // Above the body, whichever way its frame faces.
    const float above = body.objectToWorld.m[1][1] >= 0.0f ? 1.0f : -1.0f;

    // Exactly the bits the game is about to draw as its lamp quad: the car's own
    // mask, cut down to the lamp mask FUN_004BE060 chose for this car.
    const uint32_t lit = mask & *reinterpret_cast<const uint32_t*>(game::kLampMaskPtr);
    for (int bit = 0; bit < 32; ++bit) {
        if (!(lit & (1u << bit))) continue;
        // FUN_0044AEA0's reading: from bit 22 up, delta bit - 17 mirrored.
        const bool mirrored = bit >= game::kMirroredDeltaFirstBit;
        const int delta = mirrored ? bit - game::kMirroredDeltaOffset : bit;
        auto arts = g_lampArt.find(delta);
        if (arts == g_lampArt.end() || sprite >= static_cast<int>(arts->second.size())) continue;
        const LampArt& r = arts->second[sprite];
        if (!r.Valid()) continue;
        {
            // The mirrored lamp is the same pixels flipped across the sprite.
            const int rx0 = r.x0, rx1 = r.x0 + r.width;
            const float px0 = static_cast<float>(mirrored ? r.spriteWidth - rx1 : rx0);
            const float px1 = static_cast<float>(mirrored ? r.spriteWidth - rx0 : rx1);
            const float tu0 = u0 + (u1 - u0) * px0 / r.spriteWidth;
            const float tu1 = u0 + (u1 - u0) * px1 / r.spriteWidth;
            const float tv0 = v0 + (v1 - v0) * static_cast<float>(r.y0) / r.spriteHeight;
            const float tv1 = v0 + (v1 - v0) * static_cast<float>(r.y0 + r.height) / r.spriteHeight;
            Sprite lamp = body;
            lamp.hidden = false;
            lamp.texture = nullptr;
            lamp.lamp = &r;
            lamp.lampMirrored = mirrored;
            const float us[4] = {tu0, tu1, tu1, tu0};
            const float vs[4] = {tv0, tv0, tv1, tv1};
            const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int i = 0; i < 4; ++i) {
                lamp.local[i] = at(us[i], vs[i]);
                lamp.local[i].y += kConformClearance * above;
                lamp.local[i].u = uvs[i][0];
                lamp.local[i].v = uvs[i][1];
            }
            sprites_.push_back(lamp);
            ++spriteQuads_;
        }
    }
}

void LiveGeometry::Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width,
                        int height) {
    if (!device) return;
    ++g_carFrame;
    if (sprites_.empty()) return;

    D3DMATRIX matrix;
    // No world matrix here: each sprite sets its own, which is the whole point.
    // See the note in live_geometry.h.
    const gta2::Mat4 view = camera.ViewMatrix();
    memcpy(&matrix, view.m, sizeof(matrix));
    device->SetTransform(D3DTS_VIEW, &matrix);
    const gta2::Mat4 projection =
        camera.ProjectionMatrix(static_cast<float>(width) / static_cast<float>(height));
    memcpy(&matrix, projection.m, sizeof(matrix));
    device->SetTransform(D3DTS_PROJECTION, &matrix);

    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    // Sprites are flat quads and the odd block faces come in either winding, so
    // culling is off rather than guessed at.
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    const bool blending = gta2::GetAlphaMode() == gta2::AlphaMode::Blend;
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, blending ? TRUE : FALSE);
    // An alpha test rather than blending: it keeps the geometry opaque to the
    // path tracer, which behaves far better than a blended surface under Remix.
    // Sprites were hardcoded to alpha test, which is why the mode switch appeared
    // to do nothing to them: it only ever reached the static world mesh. An
    // explosion is exactly the case that wants blending - it is a soft glow, not
    // a cutout like a fence or a tree.
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    // In blend mode the test drops to 1, which still discards the fully
    // transparent texels so they do not write depth over what is behind them.
    device->SetRenderState(D3DRS_ALPHAREF, blending ? 1u
                                                    : static_cast<DWORD>(gta2::GetAlphaRef()));
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    device->SetFVF(kWorldFvf);

    // Effect artwork - fire, explosions, muzzle flashes - is drawn in a second
    // pass, so the texture has to be resolved before the pass is chosen. See
    // texture_store.h for what makes a sprite an effect and why it matters.
    struct Drawable {
        const Sprite* sprite;
        IDirect3DTexture9* texture;
    };
    // Which domes are ready to stand in for their car's flat body: the artwork
    // resolved and its material made some frames ago. See kCarMaterialWarmup.
    std::vector<IDirect3DTexture9*> shellArt(shells_.size(), nullptr);    // the car's own
    std::vector<IDirect3DTexture9*> shellMatArt(shells_.size(), nullptr);  // what is drawn
    std::vector<char> shellWarm(shells_.size(), 0);
    if (g_carMaterial) {
        for (size_t i = 0; i < shells_.size(); ++i) {
            bool unusedEffect = false;
            shellArt[i] = DeviceTextureFor(device, shells_[i].texture, &unusedEffect, true);
            if (!shellArt[i]) continue;
            if (CarMaterialFor(shellArt[i], shells_[i].sprite) &&
                CarMaterialWarm(shellArt[i], shells_[i].sprite)) {
                shellWarm[i] = 1;
                shellMatArt[i] = shellArt[i];
                g_warmArt[shells_[i].object] = {shellArt[i], shells_[i].sprite, g_carFrame};
                continue;
            }
            // Not ready: the car's last loaded artwork stands in, if it has one
            // and it is still the same car.
            const auto last = g_warmArt.find(shells_[i].object);
            if (last != g_warmArt.end() && last->second.sprite == shells_[i].sprite &&
                CarMaterialWarm(last->second.art, last->second.sprite)) {
                shellWarm[i] = 1;
                shellMatArt[i] = last->second.art;
                last->second.seen = g_carFrame;
            }
        }
        // Forget cars not seen for a while, so a long drive does not keep every
        // car it ever met.
        if (g_warmArt.size() > 256) {
            for (auto it = g_warmArt.begin(); it != g_warmArt.end();) {
                it = (g_carFrame - it->second.seen > 600) ? g_warmArt.erase(it) : std::next(it);
            }
        }
    }
    std::vector<Drawable> plain, effects;
    plain.reserve(sprites_.size());
    for (const Sprite& sprite : sprites_) {
        if (sprite.hidden && !sprite.lamp && sprite.shell >= 0 &&
            sprite.shell < static_cast<int>(shellWarm.size()) && shellWarm[sprite.shell]) {
            continue;
        }
        if (sprite.lamp) {
            if (IDirect3DTexture9* lamp = LampTexture(device, sprite.lamp, sprite.lampMirrored)) {
                plain.push_back({&sprite, lamp});
            }
            continue;
        }
        bool effect = false;
        IDirect3DTexture9* texture = DeviceTextureFor(device, sprite.texture, &effect,
                                                      !sprite.plain);
        if (!texture) {
            // The sprite was accepted and then had no artwork to draw with:
            // this is a sprite that vanishes for exactly one frame.
            ++drops_.noTexture;
            continue;
        }
        // Recorded so the texture report can tell a fireball from a road tile
        // that happens to have a hole in it. See texture_store.h.
        NoteSpriteTexture(sprite.texture);
        (effect ? effects : plain).push_back({&sprite, texture});
    }

    // Two triangles from four corners, the same two every time. Indexed rather
    // than a six-vertex list so the index buffer is part of the topological hash
    // Remix buckets on, and so the quad really is four vertices rather than four
    // with two duplicated.
    static const uint16_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    // The device is a state machine and the texture rarely changes between
    // neighbouring sprites, so the redundant SetTexture calls are skipped. The
    // world matrix genuinely changes every time and cannot be.
    IDirect3DTexture9* bound = nullptr;
    auto issue = [&](const std::vector<Drawable>& list) {
        for (const Drawable& d : list) {
            D3DMATRIX world;
            memcpy(&world, d.sprite->objectToWorld.m, sizeof(world));
            device->SetTransform(D3DTS_WORLD, &world);
            if (d.texture != bound) {
                device->SetTexture(0, d.texture);
                bound = d.texture;
            }
            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, kQuadIndices,
                                           D3DFMT_INDEX16, d.sprite->local, sizeof(Vertex));
            ++drops_.drawn;
        }
    };

    issue(plain);

    // The domes of the cars just drawn: each the car's own artwork over a rounded
    // grid, so one car is one 81-vertex mesh. Same alpha test as the sprites.
    if (!shells_.empty()) {
        static std::vector<uint16_t> domeIndices;
        if (domeIndices.empty()) {
            constexpr int n = kDomeSegments;
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const uint16_t p00 = static_cast<uint16_t>(j * (n + 1) + i);
                    const uint16_t p10 = p00 + 1;
                    const uint16_t p01 = static_cast<uint16_t>(p00 + (n + 1));
                    const uint16_t p11 = p01 + 1;
                    // The body quad's own order, 0 1 2 / 0 2 3, so it faces the same way.
                    domeIndices.insert(domeIndices.end(), {p00, p10, p11, p00, p11, p01});
                }
            }
        }
        for (size_t shellIndex = 0; shellIndex < shells_.size(); ++shellIndex) {
            const CarShell& shell = shells_[shellIndex];
            IDirect3DTexture9* art = shellArt[shellIndex];
            if (!art) continue;
            D3DMATRIX world;
            memcpy(&world, shell.objectToWorld.m, sizeof(world));

            // With the Remix API, the dome is an instance with a material of its
            // own; the plain draw below is only the fallback.
            if (g_carMaterial) {
                IDirect3DTexture9* matArt = shellMatArt[shellIndex] ? shellMatArt[shellIndex] : art;
                const remixapi_MaterialHandle material = CarMaterialFor(matArt, shell.sprite);
                // Not yet loaded by Remix, and nothing earlier to stand in: the
                // flat body is drawn this frame instead.
                if (material && !shellWarm[shellIndex]) continue;
                const remixapi_MeshHandle mesh =
                    material ? CarMeshFor(shell.local, kDomeVertices, sizeof(Vertex), domeIndices,
                                          material)
                             : nullptr;
                if (mesh) {
                    remixapi_InstanceInfo instance = {};
                    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
                    // The sprite's own alpha test, so the transparent margin of the
                    // sprite is cut away rather than drawn black. The compare
                    // operation is a Vulkan one: 6 is greater-or-equal.
                    remixapi_InstanceInfoBlendEXT alpha = {};
                    alpha.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
                    alpha.alphaTestEnabled = 1;
                    alpha.alphaTestReferenceValue = static_cast<uint8_t>(
                        (std::max)(1, (std::min)(255, static_cast<int>(gta2::GetAlphaRef()))));
                    alpha.alphaTestCompareOp = 6;
                    alpha.alphaBlendEnabled = 0;
                    alpha.srcColorBlendFactor = 1;
                    alpha.dstColorBlendFactor = 0;
                    alpha.colorBlendOp = 0;
                    alpha.srcAlphaBlendFactor = 1;
                    alpha.dstAlphaBlendFactor = 0;
                    alpha.alphaBlendOp = 0;
                    alpha.writeMask = 0xFFFFFFFFu;
                    alpha.textureColorArg1Source = 1;
                    alpha.textureColorArg2Source = 0;
                    alpha.textureColorOperation = 3;
                    alpha.textureAlphaArg1Source = 1;
                    alpha.textureAlphaArg2Source = 0;
                    alpha.textureAlphaOperation = 1;
                    alpha.tFactor = 0xFFFFFFFFu;
                    alpha.isTextureFactorBlend = 0;
                    alpha.isVertexColorBakedLighting = 0;
                    instance.pNext = &alpha;
                    instance.mesh = mesh;
                    instance.doubleSided = 1;
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 4; ++c) instance.transform.matrix[r][c] = world.m[c][r];
                    }
                    if (RemixApi()->DrawInstance(&instance) == REMIXAPI_ERROR_CODE_SUCCESS) continue;
                }
            }
            device->SetTransform(D3DTS_WORLD, &world);
            if (art != bound) {
                device->SetTexture(0, art);
                bound = art;
            }
            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, kDomeVertices,
                                           static_cast<UINT>(domeIndices.size() / 3),
                                           domeIndices.data(), D3DFMT_INDEX16, shell.local,
                                           sizeof(Vertex));
        }
    }

    if (!effects.empty()) {
        // Additive, which is what this artwork was drawn for: it fades to black
        // on its way out, and black adds nothing. The rim disappears because it
        // stops being painted at all rather than because anything was cut out of
        // it, and the fireball ends up a glow over the scene the way it should.
        //
        // Remix treats additively blended draws as emissive particles, so this is
        // also the classification that gets a fireball to light the street rather
        // than sit on it as a flat decal.
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        // Still depth *tested*, so a fire behind a wall stays behind it, but not
        // depth written: a glow must not stop what is drawn after it.
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        issue(effects);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    effectQuads_ = static_cast<int>(effects.size());
    g_effectBatches = effectQuads_;

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    // Left as we found it, so the next pass that assumes a world matrix - and the
    // static mesh does - is not drawing at the last sprite's position.
    const gta2::Mat4 identity = gta2::Identity();
    memcpy(&matrix, identity.m, sizeof(matrix));
    device->SetTransform(D3DTS_WORLD, &matrix);
}

void LiveGeometry::ReleaseResources() {
    sprites_.clear();
    shells_.clear();
    ReleaseCarMaterials();
    ReleaseLampTextures();
    g_lampDevice = nullptr;
    spriteQuads_ = 0;
    effectQuads_ = 0;
    // The shapes are keyed by texture record, and a new level hands out new ones.
    g_shapes.clear();
}

}  // namespace gta2dx9
