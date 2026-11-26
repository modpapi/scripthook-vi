// GAME-SPECIFIC: GTA VI. Entities as the Xbox build (ts 1794606036) shows them
// statically (TASKS T1.7/T1.8; RESEARCH_PLAN Q4, eleventh and twelfth passes).
// There are no GTA V-style int handles for scripts:
//   * Scripts hold a 16-byte reference (two arg slots). 0x1404372D0 resolves
//     it to an object; typed resolvers then check object->IsA(hash) (vtable
//     slot 1). The hashes are atStringHash of RAGE class names:
//       CPed           0x94821659  0x14526E340 / 0x145C30BF0  (1,354 handlers)
//       CVehicle       0xF820AAA9  0x140D35050 / 0x145C30C50  (670)
//       CEntity        0xB1389614  0x140350170 / 0x14450CCF0  (362)
//       CDynamicEntity 0x291DD2F1  0x1407BA630                (281)
//       CObject        0x9CEC50B6  0x14133BBD0 / 0x145C30C20  (94)
//       fwEntity       0x9423CDDB  0x1404374E0 / 0x140C51680  (26)
//       unnamed        0x830B8272, 0x9F73BE3F, 0x33BA0025
//     0x1402A6520(obj, out, 2) makes a reference from an object.
//   * The object: +0x30 type byte (2, 3, 4 pick the component family),
//     +0x38 transform handle, +0x40/+0x50/+0x60 matrix rows right/forward/up
//     (xyz + pad), +0x70 packed world position, +0x9C u32 entity id.
//   * Components live in an ECS: entity id -> world record -> u16 index per
//     component slot -> {base, stride} storage. Each engine lookup
//     (0x144DA10F0, 0x144D9F7C0, ...) is that formula for one fixed slot.
//   * Objects also sit in pools with GTA V-like handles (index | generation),
//     and per-world pools are indexed by the ECS id (qword_14ABBE7C0[32]).
// Pure functions over plain reads; unit-tested without the game.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>

namespace shvi::gta6::ecs {

// ------------------------------------------------------------ entity id
inline constexpr std::uint32_t kIndexMask = 0x1FFFF;     // bits 0-16
inline constexpr std::uint32_t kWorldShift = 17;         // bits 17-21
inline constexpr std::uint32_t kWorldMask = 0x1F;
inline constexpr std::uint32_t kMainWorld = 3;           // what the fixed-slot lookups require (0x60000)
inline constexpr std::uint32_t kGenerationShift = 22;    // bits 22-31, checked by the per-world pools
inline constexpr std::uint32_t kNoIndex = 0x1FFFF;       // the engine's "no entity"
constexpr std::uint32_t Index(std::uint32_t id) { return id & kIndexMask; }
constexpr std::uint32_t World(std::uint32_t id) { return (id >> kWorldShift) & kWorldMask; }
constexpr std::uint32_t Generation(std::uint32_t id) { return id >> kGenerationShift; }

// ------------------------------------------------------------ class hashes
// atStringHash(name), the argument of the IsA virtual (vtable slot 1).
namespace cls {
inline constexpr std::uint32_t kPed = 0x94821659;            // "CPed"
inline constexpr std::uint32_t kVehicle = 0xF820AAA9;        // "CVehicle"
inline constexpr std::uint32_t kEntity = 0xB1389614;         // "CEntity"
inline constexpr std::uint32_t kDynamicEntity = 0x291DD2F1;  // "CDynamicEntity"
inline constexpr std::uint32_t kObject = 0x9CEC50B6;         // "CObject"
inline constexpr std::uint32_t kFwEntity = 0x9423CDDB;       // "fwEntity"
}  // namespace cls

namespace detail {
inline std::uint32_t Rd32(std::uintptr_t a) { std::uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
inline std::uint64_t Rd64(std::uintptr_t a) { std::uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
}  // namespace detail

// ------------------------------------------------------------ pools
// Layout read by every pool user, e.g. 0x140D36890 (object -> handle) and
// 0x144516560 / 0x143C30F40 (handle -> object).
namespace pool {
inline constexpr std::size_t kBase = 0x0;       // u8* items
inline constexpr std::size_t kStride = 0x8;     // u32 item size
inline constexpr std::size_t kCapacity = 0xC;   // u32 item count, the bound of the engine's walks (0x140518D30)
inline constexpr std::size_t kCount = 0x10;     // u32 live items (decremented on free, 0x140D2EA30)
inline constexpr std::size_t kFreeList = 0x20;  // head of the free list
inline constexpr std::size_t kInfo = 0x28;      // u32* per item: bits 17-26 generation, low bits flags
inline constexpr std::size_t kRequired = 0x30;  // u32 flags an item must have to be live
inline constexpr std::size_t kSize = 0x38;
inline constexpr std::uint32_t kHandleHigh = 0xFFFE0000;  // what the engine copies from info into a handle
inline constexpr std::uint32_t kGeneration = 0x7FE0000;   // info bits 17-26
}  // namespace pool

// Object pointer -> pool handle: index | (info[index] & 0xFFFE0000). 0x140D36890.
inline std::uint32_t PoolHandleOf(std::uintptr_t p, std::uintptr_t obj) {
    using namespace detail;
    std::uint64_t base = Rd64(p + pool::kBase);
    std::uint32_t stride = Rd32(p + pool::kStride);
    if (!base || !stride || obj < base) return kNoIndex;
    std::uint32_t index = static_cast<std::uint32_t>((obj - base) / stride) & kIndexMask;
    std::uint32_t info = Rd32(static_cast<std::uintptr_t>(Rd64(p + pool::kInfo)) + 4ull * index);
    return index | (info & pool::kHandleHigh);
}

// Pool handle -> object, or 0 when stale or dead. 0x144516560.
inline std::uintptr_t PoolAt(std::uintptr_t p, std::uint32_t handle) {
    using namespace detail;
    std::uint32_t index = handle & kIndexMask;
    if (index == kNoIndex) return 0;
    std::uint32_t info = Rd32(static_cast<std::uintptr_t>(Rd64(p + pool::kInfo)) + 4ull * index);
    if ((Rd32(p + pool::kRequired) & ~info) != 0) return 0;
    if ((info & pool::kGeneration) != (handle & pool::kHandleHigh)) return 0;
    std::uint64_t base = Rd64(p + pool::kBase);
    if (!base) return 0;
    return static_cast<std::uintptr_t>(base + std::uint64_t{Rd32(p + pool::kStride)} * index);
}

inline std::uint32_t PoolCapacity(std::uintptr_t p) { return detail::Rd32(p + pool::kCapacity); }
inline std::uint32_t PoolCount(std::uintptr_t p) { return detail::Rd32(p + pool::kCount); }

// Live items of a pool, the engine's own walk: every index below the capacity
// whose info has the required flags. Writes up to `max` object pointers and
// returns how many are live in total.
inline std::size_t LiveItems(std::uintptr_t p, std::uintptr_t* out, std::size_t max) {
    using namespace detail;
    std::uint64_t base = Rd64(p + pool::kBase);
    if (!base) return 0;
    std::uint32_t stride = Rd32(p + pool::kStride), cap = PoolCapacity(p), need = Rd32(p + pool::kRequired);
    auto info = static_cast<std::uintptr_t>(Rd64(p + pool::kInfo));
    std::size_t n = 0;
    for (std::uint32_t i = 0; i < cap && i < kNoIndex; ++i) {
        if ((need & ~Rd32(info + 4ull * i)) != 0) continue;
        if (out && n < max) out[n] = static_cast<std::uintptr_t>(base + std::uint64_t{stride} * i);
        ++n;
    }
    return n;
}

// ECS id -> item of the per-world pool table (32 pool pointers), or 0. 0x143C30F40.
inline std::uintptr_t WorldPoolItem(std::uintptr_t table, std::uint32_t id) {
    using namespace detail;
    std::uint32_t index = Index(id);
    if (index == kNoIndex) return 0;
    std::uintptr_t p = static_cast<std::uintptr_t>(Rd64(table + 8ull * World(id)));
    if (!p) return 0;
    std::uint32_t info = Rd32(static_cast<std::uintptr_t>(Rd64(p + pool::kInfo)) + 4ull * index);
    if (((info >> 17) & 0x3FF) != Generation(id)) return 0;
    std::uint32_t need = Rd32(p + pool::kRequired);
    if ((need & info) != need) return 0;
    std::uint64_t base = Rd64(p + pool::kBase);
    if (!base) return 0;
    return static_cast<std::uintptr_t>(base + std::uint64_t{Rd32(p + pool::kStride)} * index);
}

// ------------------------------------------------------------ world record
// Generic lookup 0x145049640 indexes 32 records of 0x238 bytes; the fixed-slot
// lookups use one record directly (ymmword_14ABBEEB8 on this build).
namespace world {
inline constexpr std::size_t kRecordSize = 0x238;
inline constexpr std::size_t kSlotsPerEntity = 0x14;  // u32: row width of the index table (0 = world unused)
inline constexpr std::size_t kIndexTable = 0x20;      // u16* [entity][slot]; 0xFFFF = no component
inline constexpr std::size_t kStorage = 0x30;         // {u8* base; u64 stride}* per slot
inline constexpr std::size_t kStorageEntry = 16;
inline constexpr std::uint16_t kNone = 0xFFFF;
}  // namespace world

// Component `slot` of entity `id` in the world record at `record`, or 0.
// Reads only; the caller holds SEH. Same formula as every engine lookup.
inline std::uintptr_t Component(std::uintptr_t record, std::uint32_t id, std::uint32_t slot) {
    auto rd32 = [](std::uintptr_t a) { std::uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; };
    auto rd64 = [](std::uintptr_t a) { std::uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; };
    std::uint32_t width = rd32(record + world::kSlotsPerEntity);
    if (width == 0 || slot >= width) return 0;
    std::uintptr_t table = static_cast<std::uintptr_t>(rd64(record + world::kIndexTable));
    std::uint16_t idx;
    std::memcpy(&idx, reinterpret_cast<const void*>(table + 2ull * (std::uint64_t{Index(id)} * width + slot)), 2);
    if (idx == world::kNone) return 0;
    std::uintptr_t storage = static_cast<std::uintptr_t>(rd64(record + world::kStorage)) + world::kStorageEntry * slot;
    return static_cast<std::uintptr_t>(rd64(storage) + rd64(storage + 8) * idx);
}

// Slots the fixed lookups use (main world). Names are still open (T1.6/T1.8);
// these are the engine functions, most used first, with their call counts.
namespace slot {
inline constexpr std::uint32_t k44 = 44;  // 0x144D9F7C0, 4,269 callers
inline constexpr std::uint32_t k70 = 70;  // 0x144DA10F0, 3,134
inline constexpr std::uint32_t k71 = 71;  // 0x144DD7540, 2,721
inline constexpr std::uint32_t k37 = 37;  // 0x144FA1860, 1,861
inline constexpr std::uint32_t k64 = 64;  // 0x144EF6280, 1,366
inline constexpr std::uint32_t k43 = 43;  // 0x144F3AEF0, 835
inline constexpr std::uint32_t k26 = 26;  // 0x144FDF400, 522
inline constexpr std::uint32_t k34 = 34;  // 0x144E726B0, 132
inline constexpr std::uint32_t k14 = 14;  // 0x1407BA8D0, 59
}  // namespace slot

// ------------------------------------------------------------ script object
namespace object {
inline constexpr std::size_t kType = 0x30;       // u8: 2, 3, 4 select the component family (0x144EF4040)
inline constexpr std::size_t kTransform = 0x38;  // u32 handle {u16 index, u16 generation} into qword_14ABBF348
inline constexpr std::size_t kMatrix = 0x40;     // 3 rows of 16 bytes: right, forward, up (xyz + pad)
inline constexpr std::size_t kPosition = 0x70;   // PackedPosition (16 bytes)
inline constexpr std::size_t kEntityId = 0x9C;   // u32 ECS id
}  // namespace object

inline constexpr std::size_t kReferenceSize = 16;  // a script entity reference: two 8-byte arg slots

// ------------------------------------------------------------ world position
// 0x144DC3790 (7,784 callers): x, y, z floats local to a 32 m sector, then
// int16 sector x/y. World = (x + 32*sx, y + 32*sy, z). The scale (32, 32, 0, 0)
// is xmmword_146159C90.
struct PackedPosition {
    float x, y, z;
    std::int16_t sectorX, sectorY;
};
static_assert(sizeof(PackedPosition) == 16);
inline constexpr float kSectorSize = 32.0f;

struct Vec3 {
    float x, y, z;
};
constexpr Vec3 Decode(const PackedPosition& p) {
    return {p.x + kSectorSize * p.sectorX, p.y + kSectorSize * p.sectorY, p.z};
}
// The inverse, keeping |local| <= 16 m (the engine's own packer is not located yet).
inline PackedPosition Encode(Vec3 w) {
    auto sector = [](float v) {
        float s = v / kSectorSize;
        long r = static_cast<long>(s >= 0 ? s + 0.5f : s - 0.5f);
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        return static_cast<std::int16_t>(r);
    };
    PackedPosition p{};
    p.sectorX = sector(w.x);
    p.sectorY = sector(w.y);
    p.x = w.x - kSectorSize * p.sectorX;
    p.y = w.y - kSectorSize * p.sectorY;
    p.z = w.z;
    return p;
}

// ------------------------------------------------------------ orientation
// Rows as the object stores them at +0x40/+0x50/+0x60.
struct Matrix {
    Vec3 right, forward, up;
};
inline Matrix ReadMatrix(std::uintptr_t obj) {
    Matrix m;
    std::memcpy(&m.right, reinterpret_cast<const void*>(obj + object::kMatrix + 0x00), 12);
    std::memcpy(&m.forward, reinterpret_cast<const void*>(obj + object::kMatrix + 0x10), 12);
    std::memcpy(&m.up, reinterpret_cast<const void*>(obj + object::kMatrix + 0x20), 12);
    return m;
}
// pos + o.x*right + o.y*forward + o.z*up: 0x141B4F130 (GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS).
constexpr Vec3 LocalToWorld(const Matrix& m, Vec3 pos, Vec3 o) {
    return {pos.x + o.x * m.right.x + o.y * m.forward.x + o.z * m.up.x,
            pos.y + o.x * m.right.y + o.y * m.forward.y + o.z * m.up.y,
            pos.z + o.x * m.right.z + o.y * m.forward.z + o.z * m.up.z};
}
// Dot of (w - pos) with each row: 0x145B8F760 (GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS).
constexpr Vec3 WorldToLocal(const Matrix& m, Vec3 pos, Vec3 w) {
    Vec3 d{w.x - pos.x, w.y - pos.y, w.z - pos.z};
    return {d.x * m.right.x + d.y * m.right.y + d.z * m.right.z,
            d.x * m.forward.x + d.y * m.forward.y + d.z * m.forward.z,
            d.x * m.up.x + d.y * m.up.y + d.z * m.up.z};
}
// up.z (+0x68): 0x142084980 (GET_ENTITY_UPRIGHT_VALUE).
constexpr float UprightValue(const Matrix& m) { return m.up.z; }

}  // namespace shvi::gta6::ecs
