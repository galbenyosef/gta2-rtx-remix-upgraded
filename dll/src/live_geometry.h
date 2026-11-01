// World-space geometry recovered from the game's own draw calls.
//
// GTA2 hands the renderer screen-space vertices, but it does not throw the
// world-space ones away: every vertex it projects also has its **absolute**
// world position written four slots further along the same array, by
// FUN_0046bbf0 for map faces and FUN_004b9990 for sprites. FUN_0046bbf0 adds
// the camera position back in before storing, so these are absolute map
// coordinates, not camera-relative ones.
//
// That makes it possible to place sprites and the odd block shapes as real 3D
// geometry without ever inverting a projection: the numbers here are the input
// to the game's transform, not the output.
//
// Used for sprites - cars, pedestrians, powerups - which are rebuilt each frame
// so they can move. Map geometry is deliberately *not* sourced this way: the
// game only draws what is currently visible, so anything built from its stream
// would pop into existence as the camera reached it.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <d3d9.h>
#include <map>
#include <vector>

#include "../../src/camera.h"
#include "../../src/math3d.h"
#include "ground.h"

namespace gta2dx9 {

// How far a sprite is lifted off the floor when there is no floor to measure -
// off the edge of the map, over a hole, or with conforming switched off. See
// the long note in live_geometry.cpp for why any lift is needed at all and how
// this number was arrived at. It doubles as the ceiling on the extra clearance
// a footprint straddling a kerb is given, so one knob still bounds how far a
// sprite can ever be pushed off the ground. Sprites are rebuilt from the game's
// stream every frame, so a change takes effect on the next one.
constexpr float kDefaultSpriteLift = 0.075f;

void SetSpriteLift(float blocks);

// Stand each sprite on the ground plane under it rather than at a fixed height
// above its own level: the lid is sampled under the four corners, a plane is
// fitted to it, and that plane becomes the sprite's transform. On when the map
// can be read; off restores the fixed lift exactly.
void SetSpriteConform(bool on);
bool SpriteConform();

// Small flat sprites that are not cars or pedestrians - the mission arrows, pick-ups,
// signs - are drawn from textures a few dozen texels across, so under a path tracer
// they show as chunky blocks with stair-stepped edges. With this on they are drawn
// from a 4x smoothed copy of the artwork (edge-aware upscale, then bilinear), which
// keeps the shape and the hard cutout but rounds off the staircase. Cars and
// pedestrians are untouched: their stepping is part of how they look, and cars have
// HD art of their own.
constexpr bool kDefaultSpriteSmooth = true;
void SetSpriteSmooth(bool on);

// Switches for finding what a stray artefact is, from the F4 menu. Each one takes a single part of
// the picture out (or puts one back) while the game runs; none is saved.
enum GhostSwitch {
    kGhostHidePeds = 0,    // draw no pedestrians at all
    kGhostHideDoors,       // draw no car door or dent pieces
    kGhostNoBoardUi,       // a boarding pedestrian is drawn in the world, not the screen layer
    kGhostNoLift,          // pedestrians are not lifted over cars
    kGhostNoRoofSprites,   // small sprites on a car's roof (the gang logos) are not lifted clear of the dome
    kGhostSwitchCount
};
void SetGhostSwitch(int which, bool on);

// How high above the ground a pedestrian is held while he climbs into or out of a car, as a
// fraction of the car's roof height. The car's body hides whatever of him is higher than its
// own surface there, so low he goes under the car's edge and is gone; around the height of the
// door sill he is seen stepping in over the door and goes under the roof only once inside.
void SetBoardingHeight(float fraction);
float BoardingHeight();
bool GhostSwitchOn(int which);
bool SpriteSmooth();
// The smoothed 4x copy of a small flat texture (see above), for the overlay's world-anchored
// HUD arrows and markers; the texture itself when smoothing is off or it is not small artwork.
IDirect3DTexture9* SmoothedSmallTexture(IDirect3DDevice9* device, IDirect3DTexture9* source);

// How far a sprite is held off the ground once it is conformed to it. A quarter
// of a ground texel: enough to break the coplanarity that has no defined answer
// in a path tracer, and nothing more. The fixed lift had to be twenty times this
// only because a horizontal quad on a ramp needed the headroom.
//
// This is a depth-buffer number, not a visual one. How high the sprite *looks*
// is kDefaultSpriteHeight below, which is a completely different question.
constexpr float kConformClearance = 1.0f / 256.0f;

// How high the object a sprite stands in for sits above the road.
//
// Separate from the clearance above: clearance answers "which surface does the
// ray hit", this answers "is there a car here, or a picture of one painted on
// the road".
//
// This was 0.3 - the mid-height of a car body at GTA2's 2 m block - on the
// reasoning that a flat quad lying on the ground casts its shadow exactly where
// it already is and so shows nothing. That reasoning is right about shadows and
// wrong about the result: at a near-top-down camera the visible effect of ride
// height is not the shadow, it is the parallax, and 60 cm of it reads as the
// sprite hovering rather than as the sprite having depth. Judged by eye at both,
// a tenth of that sits on the road.
//
// So the height is small, and the *clipping* is handled where it belongs - by
// the conform, and by clearance that is computed per sprite rather than budgeted
// for in advance.
constexpr float kDefaultSpriteHeight = 0.03f;

void SetSpriteHeight(float blocks);
float SpriteHeight();

// The most extra clearance a sprite can be given on top of its ride height, for
// ground the fitted plane could not describe. Geometry bounds the real figure
// well below this - the worst case is a footprint across a whole block step -
// so anything reaching the cap is a bad sample rather than a steep street, and
// the cap is what stops it launching a car into the air.
constexpr float kMaxExtraClearance = 1.0f;

// How much of a sideways tilt a sprite keeps, 0..1.
//
// Fitting a plane to the ground and using the whole of it is right for a rigid
// body and wrong for a car. A car crossing a ramp at an angle stands on a plane
// tilted along both of its own axes, so the body rolls as well as pitches and a
// corner lifts - which is not something a car on wheels does. Pitching along the
// direction of travel is the part that looks right, and that part is kept whole.
//
// The sprite's long axis is taken as its direction of travel, which for a car
// quad is the side measuring about two blocks against the other's one. Rotation
// about that axis is the roll, and this scales it. 0 keeps sprites level side to
// side, 1 is the full fitted plane again.
constexpr float kDefaultSpriteRoll = 0.25f;

void SetSpriteRoll(float amount);
float SpriteRoll();

// The steepest ground a sprite is rotated onto. GTA2's 26 degree ramps come in
// under it; its 45 degree blocks, 0.14% of the surfaces a sprite can stand on,
// are clamped here because standing top-down artwork on its edge looks far worse
// than the clipping it would fix. What the clamp leaves behind is picked up as
// clearance.
constexpr float kMaxTiltDegrees = 30.0f;

// How much to soften a sprite's cutout edge, 0..1. GTA2's alpha is 1-bit, so
// this is invented rather than recovered - see FeatherAlpha in alpha_bleed.h.
// Sprites only: the world mesh keeps its hard edges, which is what a fence or a
// wall wants.
void SetSpriteFeather(float strength);
float SpriteFeather();

// How far apart to hold sprites that are stacked on the same spot.
//
// A GTA2 car is not one sprite: the body is drawn, then its lights, then any
// logo, each a separate quad at exactly the same height. The game got away with
// that because it never depth tested - it just painted them in order - but as
// real geometry they are coplanar and the depth test picks a winner per pixel,
// which is the flicker and the dropouts. Each sprite landing on a spot another
// has already taken this frame is lifted one step higher than the last, in the
// order the game drew them, so the painter's order becomes a real stacking
// order. 0 disables it.
void SetSpriteStackStep(float blocks);
float SpriteStackStep();
float SpriteLift();

// Car lamps drawn by us rather than by GTA2.
//
// With lighting on the game draws a car's lit lamps as a second quad the size of
// the whole car, with a texture built for whichever lamps are on - a new texture
// for every car and every combination of lamps, and a quad that could drift off
// the body. Instead that quad is left out and each lit lamp gets a small quad of
// its own, laid on the body exactly over the lamp, textured with that lamp's own
// pixels cut out of the car's artwork. So it looks as the game's does, and each
// lamp of each car model is one small texture that never changes, which an
// emissive can be put on once.
constexpr bool kDefaultOwnCarLamps = true;
// How brightly a lit car lamp glows, as the emissive intensity of the Remix material
// each lamp is drawn with (0 draws it as a plain quad, as before). The lamp's own
// colours at full brightness, so a headlight is a point of light rather than a tan
// smudge on the paint.
constexpr float kDefaultCarLampGlow = 6.0f;
// How big the glowing quad of a lamp is, as a fraction of the lamp's own pixels (it shrinks
// toward the lamp's centre; the lamp's whole picture is still shown, only smaller).
constexpr float kDefaultCarLampSize = 0.60f;
void SetCarLampSize(float fraction);
void SetCarLampGlow(float intensity);
// Blood on the ground (pools, drops, trails) drawn with a material of its own: wet, dark and
// glossy. roughness 0..1 (0 mirror), albedo is a multiplier on the game's red.
void SetBloodLook(bool gloss, float roughness, float albedo);
float CarLampGlow();
void SetOwnCarLamps(bool on);
bool OwnCarLamps();

// Tyres at the four corners of every car, the front pair turned with the steering. The
// sprites are drawn from above and have no wheels to show, so each car gets four small
// dark tyres that sit under its dome and peek out at the rim - and swing outward past the
// body as the front wheels turn. GTA2 does not keep a steering angle the renderer can read,
// so it is worked out from how the car moves: the angle that would give the turn the car
// just made over the distance it just went (a bicycle model). `maxAngle` caps it, in
// degrees. Off draws no tyres.
constexpr bool kDefaultCarWheels = false;
constexpr float kDefaultCarWheelMaxAngle = 32.0f;
// How far out the tyres sit, as a fraction of the body's width from its middle line (0.5 is
// the very edge), and how far in from the nose and tail the axles are, as a fraction of
// the body's length.
constexpr float kDefaultCarWheelSide = 0.41f;
constexpr float kDefaultCarWheelAxleInset = 0.24f;
// The front tyres sit this much further out than the rear ones (same units as the side),
// so they show past the bodywork when they turn.
constexpr float kDefaultCarWheelFrontExtra = 0.09f;
void SetCarWheels(bool on, float maxAngleDegrees, float side = kDefaultCarWheelSide,
                  float axleInset = kDefaultCarWheelAxleInset,
                  float frontExtra = kDefaultCarWheelFrontExtra);
bool CarWheels();

// Cars as bodies rather than flat pictures.
//
// GTA2's cars are one flat sprite each, seen from straight above. Under a path
// tracer a flat quad has nothing to reflect with - a horizontal mirror seen from
// directly overhead reflects only the sky - so however glossy the material, a
// car looks painted on the road. With this on, each car's sprite is lifted to the
// height of its roof and pulled in a little at the edges, and a sloping skirt in
// the car's own paint colour runs from the sprite's edge down to the road. The
// skirt faces sideways and upwards at an angle, which is what gives a glossy
// surface something to reflect: lamps, walls, other cars.
//
// The height and inset are fractions of a block: a car is about one block long.
constexpr bool kDefaultCarBodies = true;
constexpr float kDefaultCarBodyHeight = 0.10f;
constexpr float kDefaultCarBodyInset = 0.12f;
void SetCarBodies(bool on);
bool CarBodies();
void SetCarBodyHeight(float blocks);
float CarBodyHeight();
// How square the dome's shoulders are: the exponent its height rises with from the car's outline,
// 0.7 the original rounded hump, lower is boxier (steeper sides, flatter top) so the shadow of the
// car lies against it like a solid object's instead of a dome's.
void SetCarDomeShoulder(float exponent);
float CarDomeShoulder();
void SetCarBodyInset(float fraction);
float CarBodyInset();
// The car's own Remix material: submitted through the Remix API, so it can be
// glossy metal while the rest of the world keeps its rough default. Roughness
// and metallic are 0..1. Without the API, the dome is drawn as plain geometry.
constexpr bool kDefaultCarMaterial = true;
constexpr float kDefaultCarRoughness = 0.25f;
constexpr float kDefaultCarMetallic = 0.40f;
void SetCarMaterial(bool on, float roughness, float metallic);
// Use the hand-made car artwork and maps from gta2dx9_hd\ where there are some.
void SetCarHd(bool hd, bool normalMaps);
// How much of the maps' surface tilt the cars keep, 0..1. 1 is the map as built.
void SetCarNormalStrength(float strength);
// How much of a mirror the cars' glass is made: 0 leaves it as the maps are, 1 is
// the default, above 1 brightens the glass's own colour further.
void SetCarGlassGloss(float gloss);
// How glossy the cars' paint is made on top of what the maps say: 0 leaves the maps
// as they are, 1 (the default) makes paint smoother and a little more metal, so that
// lamps and other cars show in it as crisp reflections rather than a dull smear.
void SetCarPaintGloss(float gloss);
void SetCarPaintGlossFor(int sprite, float gloss);   // 0..1; negative: the global setting
// How far the glass is bulged outwards, 0..1.5: 0 leaves it the flat panel the maps
// make, which reflects the same bit of sky everywhere; 0.6 is the default.
void SetCarGlassCurve(float curve);

// Skid marks and the smoke off the tyres. The game's skid strips are redrawn from a
// soft, translucent copy; every new strip near a car lets off a puff of smoke.
struct SkidSmokeSettings {
    bool  soft = true;            // redraw rubber skid strips soft (off: as GTA2 draws them)
    float skidOpacity = 0.45f;    // peak opacity of the rubber
    float skidSoftness = 1.0f;    // how far the edges and the stipple are blurred
    // The game forgets a skid strip long before it should. With this on the renderer
    // remembers every strip it has drawn, keeps drawing it at full strength for
    // skidHold seconds, and then fades it out gently over skidFade seconds.
    bool  persist = true;
    float skidHold = 60.0f;       // seconds at full strength
    float skidFade = 60.0f;       // seconds to fade away after that
    bool  smoke = false;          // off unless the menu or gta2dx9.ini turns it on
    float smokeAmount = 1.0f;     // puffs per new strip
    bool  smokeGlow = true;       // self-lit white smoke (additive); off: lit like any surface, greyer
    float smokeOpacity = 0.22f;   // a puff's peak opacity
    float smokeSize = 1.0f;
    float smokeLife = 1.5f;       // seconds
    float smokeRise = 1.0f;
    float smokeDrift = 1.0f;
};
SkidSmokeSettings& SkidSmoke();
// Reads the [skids] section of the renderer's ini, and remembers the path for Save.
void SkidSmokeLoad(const char* iniPath);
// Writes it back to the ini it was read from. False if there is none.
bool SkidSmokeSave();

// Burning cars. The game's flame sprite sits at the car's own level, which is under the
// dome that now stands up from the car, so the car is drawn over its own fire. Flames
// over a car are lifted clear of its roof, drawn larger, backed by stacked orange layers
// that flicker, and let off dark smoke. Explosions (sprites bigger than maxSize) are only
// lifted, not enlarged.
struct FireSettings {
    bool  onTop = true;           // lift effect sprites over a car above its roof
    float sizeScale = 1.6f;       // flame size multiplier
    float maxSize = 0.8f;         // tiles; bigger sprites are explosions and are left their size
    int   layers = 3;             // extra flame layers stacked above the sprite
    float layerRise = 0.05f;      // tiles each layer sits higher
    float layerGrow = 0.25f;      // each layer is this much larger than the last
    float layerBright = 0.70f;    // brightness left after each layer
    float tint[3] = {1.0f, 0.55f, 0.18f};   // colour of the extra layers
    float flicker = 0.20f;        // how much the size wanders
    float flickerHz = 13.0f;
    bool  smoke = true;
    float smokeRate = 7.0f;       // puffs per second, per burning car
    float smokeOpacity = 0.35f;
    float smokeSize = 1.0f;
    float smokeLife = 2.5f;       // seconds
    float smokeRise = 1.0f;
    float smokeDrift = 1.0f;
    float smokeGrey = 0.16f;      // 0 black .. 1 white, at birth
    // Flames of our own, drawn as soft additive particles rising off a burning car. The game's
    // flame sprite is one small repeating picture; this is a mass of tongues, hot yellow at
    // the root and red at the tip, that never repeats.
    bool  flames = true;
    float flameRate = 90.0f;      // tongues per second, per burning car
    float flameSize = 1.0f;
    float flameRise = 1.0f;
    float flameLife = 0.7f;       // seconds
    float flameBright = 1.0f;
    float emberRate = 12.0f;      // sparks per second, per burning car
    float spriteBright = 0.5f;    // how much of the game's own flame sprite is kept under them
    bool scorch = true;           // a burnt mark on the ground where something blew up
    float scorchLife = 60.0f;     // seconds from the blast until it has gone
    float scorchOpacity = 0.85f;  // how dark it starts
    float scorchSize = 1.0f;      // against the size of the blast
};
FireSettings& FireEffects();
// Reads and writes the [fire] section of the renderer's ini.
void FireEffectsLoad(const char* iniPath);
bool FireEffectsSave();
// How strongly dents show on a car with hand-made artwork: 0 draws none, 1 is the
// full change the game's damage art makes, the default is a little under that.
void SetCarDamage(float strength);

// One lamp delta of one car sprite, set when a district's style is loaded: its
// pixels cropped to the rectangle they cover (alpha 0 where the delta paints
// nothing), where that rectangle is on the sprite, and the sprite's size.
struct LampArt {
    int x0 = 0, y0 = 0, width = 0, height = 0;
    int spriteWidth = 0, spriteHeight = 0;
    std::vector<uint32_t> argb;
    int opaque = 0;   // how many of the pixels paint anything; set by SetCarDeltaArt
    bool Valid() const { return width > 0 && height > 0 && spriteWidth > 0 && spriteHeight > 0; }
};
void SetCarLampArt(int delta, std::vector<LampArt> bySprite);
// The same for the deltas that are not lamps - dents and doors - which a car with
// hand-made artwork needs drawn over its body, since the game paints them into its
// own low-resolution sprite and the redraw never sees them.
void SetCarDeltaArt(int delta, std::vector<LampArt> bySprite);

// The texture a lamp is drawn with, exactly as it is uploaded: the crop, mirrored
// for a lamp on the far side, with every transparent texel given the mean colour
// of the opaque ones so filtering cannot pull a dark fringe in. Its Remix hash is
// what tools/lamp_emissives.py reproduces to put an emissive on it.
std::vector<uint32_t> LampTexels(const LampArt& art, bool mirrored);

// How many sprites were drawn additively last frame - the fire and explosion
// artwork. A free function because the menu has no handle on the LiveGeometry
// instance. See EffectSpriteSettings in texture_store.h.
int EffectBatchesDrawn();

// How many distinct object-space quads the session has had to keep, and for how
// many sprite textures. This is the health check on the whole scheme below: a
// count that settles is geometry Remix can recognise frame to frame, and one
// that keeps climbing means something is dithering and the motion vectors will
// be no better than they were.
void SpriteShapeCounts(int* shapes, int* textures);

// --- Why each sprite is its own draw call ---------------------------------
//
// Because RTX Remix has to recognise a sprite as the same sprite it saw last
// frame, and it can only do that from what the draw call looks like.
//
// This pass used to hand Remix, once per texture, a bag of world-space vertices
// for every sprite sharing that texture, drawn with an identity world matrix.
// Reading dxvk-remix, that is close to the worst thing to give it:
//
//   * DrawCallCache buckets geometry by its *topological* hash, and
//     hashGeometryDescriptor (rtx_hashing.cpp) mixes in the **vertex count**. A
//     batch grows and shrinks as cars and pedestrians come into view, so its
//     bucket key changed constantly and every such frame allocated a fresh
//     BlasEntry with no history at all.
//
//   * Previous-frame vertex positions only exist when the *same* BlasEntry is
//     matched again (SceneManager::processGeometryInfo keeps them in
//     historyBuffer on kUpdateBVH). Inside a batch, vertex i belongs to whichever
//     sprite the game happened to submit i'th that frame. Two cars swapping draw
//     order silently repointed every vertex at a different object, so the
//     previous positions Remix did have described the wrong sprite - a motion
//     vector from one car to another. That is the flickering copies.
//
//   * With an identity world matrix, InstanceManager::updateInstance sees
//     hasTransformChanged == false; with no usable history hasPreviousPositions
//     is false too, and surface.isStatic is then set, which tells the renderer to
//     skip motion vector calculation for a surface that is visibly moving.
//
// So the geometry has to be the thing that stays still and the transform the
// thing that moves - which is what every other game gives Remix. Each sprite is
// now one draw call of a canonical quad in its own object space, quantised so it
// is bit-identical frame to frame, with the placement in D3DTS_WORLD. Remix then
// matches it by topological hash plus proximity (DrawCallTracker's spatial map)
// and takes the motion straight from objectToWorld against prevObjectToWorld.
//
// The cost is one draw call per sprite instead of per texture - a few hundred
// two-triangle draws, which is nothing for either D3D9 or Remix, and it lets
// every sprite of a kind share one BLAS instead of rebuilding a merged one.

class LiveGeometry {
public:
    // Public only so the object-space shape cache in live_geometry.cpp can name
    // it; nothing outside this pass has any use for it.
    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };

    void BeginFrame();

    // The floor sprites are stood on. Held, not copied; WorldView owns both this
    // and the sampler, so the pointer stays good for the process. Null, or a
    // sampler with no map in it yet, falls back to the fixed lift.
    void SetGround(const GroundSampler* ground) { ground_ = ground; }

    // A sprite quad or triangle. `vertices` is the game's screen-space array;
    // the world positions are read from its shadow slots.
    //
    // `lamps` marks the second quad the game draws for a car, its lit lamps on
    // their own (see kObjectDrawLampsReturn). That one is not placed on its own
    // at all: it takes the car body's placement from the draw just before it.
    void AddSprite(unsigned flags, const void* texture, const float* vertices, int corners,
                   bool lamps = false);

    void Draw(IDirect3DDevice9* device, const gta2::Camera& camera, int width, int height);
    void ReleaseResources();

    int SpriteQuads() const { return spriteQuads_; }
    // Sprites drawn additively last frame - the fire and explosion artwork. See
    // EffectSpriteSettings in texture_store.h.
    int EffectBatches() const { return effectQuads_; }

private:
    // One sprite: a quad in its own object space, and where that space sits in
    // the world. The vertices are the part Remix hashes, so they are quantised
    // and carry no trace of where the sprite is; the matrix is the part Remix
    // differentiates for motion, so it carries all of it.
    struct Sprite {
        const void* texture;
        Vertex local[4];
        int corners;
        gta2::Mat4 objectToWorld;
        // One of our own lamp textures instead of a game texture; see OwnCarLamps.
        const LampArt* lamp = nullptr;
        bool lampMirrored = false;
        // A decal laid with the lamp code but not lit: a car's roof number.
        bool lampPlain = false;
        // Never effect artwork, whatever its outline: a power-up token.
        bool plain = false;
        // A car's flat body, left out when its dome is drawn from hand-made
        // artwork, which would otherwise have the low-resolution sprite under it.
        bool hidden = false;
        // A pedestrian on his feet: lifted clear of a car he is over. See Draw.
        bool pedestrian = false;
        // ... that is, or just was, getting into or out of a car: left where it is.
        bool recentlyInCar = false;
        // ... and has a vehicle of his own right now: in the middle of getting in or out.
        bool inVehicleNow = false;
        // ... in the climb into or out of a car, by his animation frame. See Draw.
        bool climbing = false;
        // A pedestrian lying on the road, dead or knocked down. Hidden under a car; see Draw.
        bool corpse = false;
        // Drawn through the screen-space layer, over everything, while he boards a car.
        bool boardUi = false;
        // Not drawn: a pedestrian over his car as he gets in or out. See Draw.
        bool ghost = false;
        int shell = -1;   // which dome stands in for it, in shells_
        // Small flat artwork drawn from a smoothed, enlarged copy. See SetSpriteSmooth.
        bool smooth = false;
        // The game object this quad belongs to (0: unknown); only used to follow one thing from frame
        // to frame in the flicker diagnostic.
        uintptr_t object = 0;
    };
public:
    using SpriteType = Sprite;   // for the skid memory in live_geometry.cpp
private:

    // The lamps of the car drawn last, as quads of their own on its body.
    void AddCarLamps(const Sprite& body);

    // One car's dome: the car's own sprite, drawn a second time as a grid that
    // swells from nothing at its edges to a rounded top, so that the body has
    // surfaces facing every way to reflect with. There are no walls: the dome
    // meets the ground at the sprite's own outline. Same object space and
    // placement as the body, and textured with the same artwork.
    static constexpr int kDomeSegments = 8;
    static constexpr int kDomeVertices = (kDomeSegments + 1) * (kDomeSegments + 1);
    static constexpr int kDoorSegments = 4;
    static constexpr int kDoorVertices = (kDoorSegments + 1) * (kDoorSegments + 1);
    struct CarShell {
        const void* texture;
        int sprite = -1;   // the sprite number within the car base
        uintptr_t object = 0;   // the game object it belongs to
        gta2::Mat4 objectToWorld;
        Vertex local[kDomeVertices];
        // The car's open doors and dents, each a grid laid over the dome where the
        // delta's pixels are. See BuildCarDoors.
        struct Door {
            int delta = 0;
            const LampArt* art = nullptr;
            Vertex local[kDoorVertices];
        };
        std::vector<Door> doors;
        // Four tyres: one shared mesh, centred on its own origin, and where each goes.
        // The front pair's matrices carry the steering, so the mesh itself never
        // changes and Remix sees the same shape every frame.
        static constexpr int kTyreVertices = 20;
        Vertex tyreMesh[kTyreVertices];
        std::vector<gta2::Mat4> tyres;
    };
    std::vector<CarShell> shells_;
    // Fills shell->tyres. `peak` is the dome's height, signed by which way up is.
    void BuildCarTyres(const Sprite& body, CarShell* shell, const void* object, float peak);
    // Fills shell->doors from the car's delta mask. Needs the body's corners.
    void BuildCarDoors(const Sprite& body, CarShell* shell, const void* object);

    // If `body` is a car, builds its skirt and lifts and shrinks the body to sit
    // on it. Everything it changes is a constant of the sprite and the settings,
    // so Remix still sees the same shape every frame.
    void RaiseCarBody(Sprite* body);

    // One entry per sprite already placed this frame, for spotting a stack.
    struct Stack {
        float x, z;
        float area;   // of the quad's footprint, in square blocks
        int layer;
    };
    std::vector<Stack> stacks_;

    // What a sprite was given last frame, so this frame can move toward its
    // answer instead of jumping to it.
    //
    // The ground under a footprint is not a continuous function of where that
    // footprint is: a corner crossing a kerb, or the crest where a ramp meets
    // flat, changes the fitted plane in one step. Applied directly that is a
    // sprite which twitches as it drives, so both the lift and the tilt are
    // eased toward the answer instead of being set to it.
    //
    // Sprites have no identity in this stream - they are rebuilt from scratch
    // every frame - so the match is the same trick the stack finder uses:
    // nearest entry from last frame with the same texture, within a radius wider
    // than anything can travel in one frame. No match means the sprite is new
    // and gets its answer immediately; easing one in from nothing would be the
    // visible pop this exists to remove.
    struct Track {
        const void* texture;
        float x, z;
        float offset;
        float nx, ny, nz;
    };
    std::vector<Track> tracks_;      // last frame
    std::vector<Track> tracksNext_;  // being built this frame

    // Nearest match from last frame, or null.
    const Track* FindTrack(const void* texture, float x, float z) const;

    int StackLayerFor(float cx, float cz, float area);

    bool ReadCorners(const float* vertices, int corners, const void* texture, uintptr_t shadowBase,
                     Vertex* out) const;
    // Splits world-space corners into a canonical object-space quad and the
    // transform that places it. False if the quad has no area to build a frame
    // from.
    //
    // `centre` is where the sprite ends up, which is not the centroid of the
    // corners: the corners are the shape, the centre is the placement. `groundUp`
    // rotates the sprite onto the floor - null, or an exactly vertical one,
    // leaves the frame the corners themselves describe.
    struct Vec3 {
        float x, y, z;
    };
    bool Place(const Vertex* world, int corners, const Vec3& centre, const Vec3* groundUp,
               Sprite* out) const;

    const GroundSampler* ground_ = nullptr;
    std::vector<Sprite> sprites_;
    // The last sprite placed and the game object it was drawn for, so a car's
    // lamp quad can be laid on its body.
    const void* lastObject_ = nullptr;
    size_t lastSprite_ = 0;
    int spriteQuads_ = 0;
    int effectQuads_ = 0;
    int spriteLogs_ = 0;

public:
    // Why sprites the game asked for did not reach the screen. Every one of
    // these is a frame where something the player can see is simply absent.
    struct Drops {
        int accepted = 0;  // quads we took from the game
        int drawn = 0;     // quads that actually reached the device
        int expandFlag = 0;
        int notTheSpriteArray = 0;
        int notFinite = 0;
        int outOfWorld = 0;
        int degenerate = 0;
        int noTexture = 0;
        bool Quiet() const {
            return !expandFlag && !notTheSpriteArray && !notFinite && !outOfWorld && !degenerate &&
                   !noTexture;
        }
    };
    const Drops& DropCounts() const { return drops_; }
    void ClearDropCounts() { drops_ = Drops{}; }

    // What the conform actually did, per frame. This is the health check on the
    // whole scheme: `grounded` should be nearly every sprite in a normal street,
    // and a large `maxGap` on a frame with nothing in the air means the game's
    // own level for a sprite disagrees with the lid the map says is under it -
    // which is the one thing that would make conforming pointless rather than
    // wrong, and it is measured here rather than assumed.
    struct Conform {
        int grounded = 0;    // stood and rotated onto a fitted plane
        int airborne = 0;    // above its floor, so left where the game put it
        int noGround = 0;    // nothing under the footprint; fixed lift used
        int capped = 0;      // ground steeper than kMaxTiltDegrees
        int straddled = 0;   // footprint over a step, given extra clearance
        float maxGap = 0.0f;       // furthest any sprite sat above its own floor
        float maxResidual = 0.0f;  // worst a footprint failed to be one plane
    };
    const Conform& ConformCounts() const { return conform_; }
    void ClearConformCounts() { conform_ = Conform{}; }

private:
    mutable Drops drops_;
    mutable Conform conform_;
};

// The last complete frame's conform counts, for the menu. A free function for
// the same reason EffectBatchesDrawn is one: nothing outside WorldView holds the
// LiveGeometry instance.
const LiveGeometry::Conform& SpriteConformCounts();

// Milliseconds the last LiveGeometry::Draw spent in each of five stages: setup, the sprite
// loop, remembered skid strips, issuing sprites and smoke, and the cars' domes. For the log.
const double* LiveDrawStageMs();

}  // namespace gta2dx9
