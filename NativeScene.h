#pragma once
#include "MobileBuild.h"
#include "cube/Game.h"
#include "gfx/ChunkBuffer.h"
#include "gfx/D3D11VertexBuffer.h"
#include <unordered_map>
#include <cmath>
namespace nativeScene {
// Offset of a chunk buffer's vertical chunk index (not described by CWSDK).
constexpr size_t CHUNK_BUFFER_Z = 0x5c;
// The executable is immutable during a process. Validate its identity once,
// rather than repeating PE-header inspection in scene/terrain queries.
inline unsigned char *executableBase() {
    static auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    return base;
}
inline bool supported() {
    static const bool matchesMobileBuild = [] {
        const auto *base = executableBase();
        if (!base)
            return false;
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->FileHeader.TimeDateStamp == mobileBuild::timestamp &&
               nt->OptionalHeader.SizeOfImage == mobileBuild::imageSize;
    }();
    return matchesMobileBuild;
}
// The game object, or nullptr on an unsupported build.
inline cube::Game *game() {
    // Read the game pointer each time; it may change when a world reloads.
    return supported()
               ? *reinterpret_cast<cube::Game **>(executableBase() + mobileBuild::gamePointerRva)
               : nullptr;
}
// True when `buffer` is the vertex buffer of the cloud or dark-cloud sprite.
inline bool cloud(cube::World *world, ID3D11Buffer *buffer) {
    if (!world || !world->spritemanager || !buffer)
        return false;
    const auto &sprites = world->spritemanager->sprites;
    // Verified native cloud and dark-cloud sprite slots and model dimensions.
    for (size_t index : {size_t(3512), size_t(3513)}) {
        if (index >= sprites.size())
            continue;
        auto s = sprites[index];
        if (s && s->something_with_offset_1 == 24 && s->something_with_offset_2 == 24 &&
            s->lowering_this_makes_plants_go_fast == (index == 3512 ? 16 : 9) &&
            s->gfx_D3D11VertexBuffer && s->gfx_D3D11VertexBuffer->buffer == buffer)
            return true;
    }
    return false;
}

// Where a terrain chunk buffer lives: its zone and vertical chunk index.
struct Chunk {
    IntVector2 zone;
    int z;
};
// GUIDs used with ID3D11Buffer::SetPrivateData to tag buffers between mods.
inline const GUID chunkTagGuid = {
    0x2b4f651e, 0x5a85, 0x4ee7, {0xb3, 0x5a, 0xe8, 0x39, 0x14, 0xc4, 0x43, 0x55}};
inline const GUID animatedGuid = {
    0x6d0d6559, 0xad22, 0x410f, {0xa7, 0xc3, 0xc3, 0x79, 0x10, 0x18, 0xbb, 0x6c}};
inline const GUID cloudDriftGuid = {
    0x2a7e9811, 0x9041, 0x4895, {0x81, 0x56, 0x2e, 0x49, 0x17, 0xc3, 0x66, 0x09}};
// Payload stored under chunkTagGuid on a replacement buffer.
struct ChunkTag {
    ID3D11Buffer *original;
    Chunk info;
};
using Chunks = std::unordered_map<ID3D11Buffer *, Chunk>;

// Nonblocking locks prevent the graphics thread from waiting on generation.
struct Locks {
    cube::World *world;
    bool held = false;
    explicit Locks(cube::World *w) : world(w) {
        if (!w || !TryEnterCriticalSection(&w->zones_critical_section))
            return;
        if (!TryEnterCriticalSection(&w->zones_mesh_critical_section)) {
            LeaveCriticalSection(&w->zones_critical_section);
            return;
        }
        held = true;
    }
    ~Locks() {
        if (held) {
            LeaveCriticalSection(&world->zones_mesh_critical_section);
            LeaveCriticalSection(&world->zones_critical_section);
        }
    }
};

// Collects the terrain chunk buffers of every zone within `radius` blocks of the
// local player. Returns false (leaving `out` untouched) when the world is busy.
inline bool snapshot(cube::World *w, Chunks &out, double radius) {
    Locks lock(w);
    if (!lock.held || !w->local_creature)
        return false;
    Chunks next;
    double px = w->local_creature->entity_data.position.x / 65536.,
           py = w->local_creature->entity_data.position.y / 65536.;
    // Zones are 64x64 blocks.
    int cx = (int)std::floor(px / 64), cy = (int)std::floor(py / 64),
        steps = (int)std::ceil(radius / 64) + 1;
    for (int ix = -steps; ix <= steps; ix++)
        for (int iy = -steps; iy <= steps; iy++) {
            auto found = w->zones.find(IntVector2(cx + ix, cy + iy));
            if (found == w->zones.end())
                continue;
            auto zone = found->second;
            if (!zone)
                continue;
            // Distance from the player to the zone's rectangle.
            double x = double(zone->position.x) * 64, y = double(zone->position.y) * 64;
            double dx = std::max(x - px, std::max(px - (x + 64), 0.)),
                   dy = std::max(y - py, std::max(py - (y + 64), 0.));
            if (dx * dx + dy * dy > radius * radius)
                continue;
            for (auto cb : zone->chunk.chunkbuffers) {
                if (!cb || !cb->vertex_buffer)
                    continue;
                auto v = (gfx::D3D11VertexBuffer *)cb->vertex_buffer;
                if (v->buffer)
                    next.emplace(v->buffer, Chunk{zone->position,
                                                  *(int *)((unsigned char *)cb + CHUNK_BUFFER_Z)});
            }
        }
    out.swap(next);
    return true;
}

// True if `buffer` is still the live buffer for chunk `info`; `result` receives its zone.
inline bool resident(cube::World *w, const Chunk &info, ID3D11Buffer *buffer, cube::Zone *&result) {
    auto found = w->zones.find(info.zone);
    if (found == w->zones.end() || !found->second)
        return false;
    result = found->second;
    for (auto cb : result->chunk.chunkbuffers)
        if (cb && cb->vertex_buffer &&
            ((gfx::D3D11VertexBuffer *)cb->vertex_buffer)->buffer == buffer &&
            *(int *)((unsigned char *)cb + CHUNK_BUFFER_Z) == info.z)
            return true;
    return false;
}

// Block type at zone-local (x, y, z); 0 (air) outside the zone or column.
inline int blockType(cube::Zone *zone, int x, int y, int z) {
    if (x < 0 || x >= 64 || y < 0 || y >= 64)
        return 0;
    const auto &field = zone->fields[x * 64 + y];
    int index = z - field.base_z;
    return index >= 0 && size_t(index) < field.blocks.size() ? int(field.blocks[index].type) : 0;
}
} // namespace nativeScene

