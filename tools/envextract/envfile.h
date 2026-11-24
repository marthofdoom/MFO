// envfile.h - header-only reader for MFOENV v2 files written by tools/envextract (README.md is the contract).
// No dependencies beyond the C++17 standard library.  Little endian host assumed (x86/ARM64).
// load() validates EVERYTHING before any accessor can be used: a file that loads can be traversed without bounds checks
// by the caller (every index in the tables is in range, strings are terminated, sections are aligned and inside the file).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mfoenv {
#pragma pack(push, 1)
struct Mesh {   // 72
    uint32_t id, cellId, worldId; int16_t gridX, gridY;          // grid 0x7FFF = interior
    uint32_t vertBase, vertCount, triBase, triCount, linkBase, linkCount, doorBase, doorCount;
    float min[3], max[3];
};
struct Vert { float x, y, z; };
struct Tri { uint16_t v[3]; int16_t nb[3]; uint16_t flags, cover; };   // v, nb are LOCAL to the mesh; nb -1 none or external (see Link)
struct Link { uint32_t meshIdx; uint16_t tri; uint8_t edge, kind; uint32_t targetMeshId; int32_t targetMeshIdx; uint16_t targetTri, flags; float dz; };
struct Door { uint32_t meshIdx; uint16_t tri, pad; uint32_t crc, doorRefId; int32_t refIdx; };
struct Comp { uint16_t walk, weak; };   // per triangle component ids (0 = largest)
struct Ref {    // 128
    uint32_t id, base, cellId; uint8_t kind, baseSig; uint16_t flags;
    float pos[3], rot[3], scale, boundsMin[3], boundsMax[3];
    uint32_t key; uint8_t lockLevel, lockFlags; uint16_t count;
    uint32_t teleportRef; float tpPos[3], tpRot[3]; uint32_t tpFlags, tpCellId, enableParent, baseNameOff, modelOff, reserved;
};
#pragma pack(pop)
static_assert(sizeof(Mesh) == 72 && sizeof(Vert) == 12 && sizeof(Tri) == 16 && sizeof(Link) == 24 && sizeof(Door) == 20 &&
              sizeof(Comp) == 4 && sizeof(Ref) == 128, "layout");

enum TriFlag : uint16_t { kEdge0Ext = 1, kEdge1Ext = 2, kEdge2Ext = 4, kDeleted = 8, kNoLarge = 0x10, kOverlap = 0x20, kPreferred = 0x40, kWater = 0x200, kDoor = 0x400 };
enum LinkFlag : uint16_t { kStaleTarget = 1, kNoOwnerEdge = 2, kDzUnknown = 4 };
enum Kind : uint8_t { kDoorRef, kContainer, kActivator, kFurniture, kItem, kActor, kStatic, kFlora, kTree };
enum RefFlag : uint16_t { kPersistent = 1, kLeveledActor = 2, kHasEnableParent = 4, kLocked = 8, kTeleport = 16, kEnableInverted = 32, kStartsDisabled = 64 };

class Env {
public:
    bool load(const char* path, std::string* err = nullptr) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return fail(err, "cannot open");
        if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return fail(err, "seek failed"); }
        long n = std::ftell(f);
        if (n < 0 || n > (1L << 30) || std::fseek(f, 0, SEEK_SET) != 0) { std::fclose(f); return fail(err, "bad file size"); }
        std::vector<uint8_t> b((size_t)n);
        size_t got = n ? std::fread(b.data(), 1, (size_t)n, f) : 0;
        std::fclose(f);
        if (got != (size_t)n) return fail(err, "short read");
        return loadBuffer(std::move(b), err);
    }

    bool loadBuffer(std::vector<uint8_t> b, std::string* err = nullptr) {
        *this = Env();
        buf_ = std::move(b); const uint64_t n = buf_.size();
        if (n < 16) return fail(err, "file too short");
        if (std::memcmp(buf_.data(), "MFOENV\0\0", 8) != 0) return fail(err, "bad magic");
        uint32_t ver, ns; std::memcpy(&ver, &buf_[8], 4); std::memcpy(&ns, &buf_[12], 4);
        if (ver != 2) return fail(err, "unsupported version");
        if (ns == 0 || ns > 64) return fail(err, "bad section count");
        const uint64_t tableEnd = 16 + 16ull * ns;
        if (tableEnd > n) return fail(err, "section table outside file");
        struct Sec { uint32_t off = 0, cnt = 0, rec = 0; bool have = false; } S[10];
        static const char* tags[10] = {"META", "STRS", "PLUG", "MESH", "VERT", "TRIS", "LINK", "DOOR", "REFS", "TCMP"};
        static const uint32_t recs[10] = {1, 1, 4, sizeof(Mesh), sizeof(Vert), sizeof(Tri), sizeof(Link), sizeof(Door), sizeof(Ref), sizeof(Comp)};
        for (uint32_t i = 0; i < ns; ++i) {
            const uint8_t* p = &buf_[16 + 16 * (size_t)i]; uint32_t off, cnt, rec;
            std::memcpy(&off, p + 4, 4); std::memcpy(&cnt, p + 8, 4); std::memcpy(&rec, p + 12, 4);
            for (int t = 0; t < 10; ++t) {
                if (std::memcmp(p, tags[t], 4) != 0) continue;   // unknown tags are ignored on purpose (append-only evolution)
                if (S[t].have) return fail(err, "duplicate section");
                if (rec != recs[t]) return fail(err, "section record size mismatch");
                if (off % 4 != 0) return fail(err, "section not 4-byte aligned");
                if (off < tableEnd || (uint64_t)off + (uint64_t)cnt * rec > n) return fail(err, "section out of range");
                S[t].off = off; S[t].cnt = cnt; S[t].rec = rec; S[t].have = true;
            }
        }
        for (int t = 0; t < 10; ++t) if (!S[t].have) return fail(err, "required section missing");
        const uint8_t* base = buf_.data();
        meta_.assign((const char*)base + S[0].off, S[0].cnt);
        strs_ = base + S[1].off; strsN_ = S[1].cnt;
        if (strsN_ == 0 || strs_[0] != 0 || strs_[strsN_ - 1] != 0) return fail(err, "string pool not terminated");
        plug_ = (const uint32_t*)(base + S[2].off); plugN_ = S[2].cnt;
        mesh_ = (const Mesh*)(base + S[3].off); meshN_ = S[3].cnt;
        vert_ = (const Vert*)(base + S[4].off); vertN_ = S[4].cnt;
        tri_ = (const Tri*)(base + S[5].off); triN_ = S[5].cnt;
        link_ = (const Link*)(base + S[6].off); linkN_ = S[6].cnt;
        door_ = (const Door*)(base + S[7].off); doorN_ = S[7].cnt;
        ref_ = (const Ref*)(base + S[8].off); refN_ = S[8].cnt;
        comp_ = (const Comp*)(base + S[9].off);
        if (S[9].cnt != triN_) return fail(err, "component table size mismatch");
        for (uint32_t i = 0; i < plugN_; ++i) if (plug_[i] >= strsN_) return fail(err, "plugin name offset");
        auto idOk = [&](uint32_t id) { return id == 0 || (id >> 24) < plugN_; };
        uint64_t vEnd = 0, tEnd = 0, lEnd = 0, dEnd = 0;   // meshes must tile their tables in order
        for (uint32_t i = 0; i < meshN_; ++i) {
            const Mesh& m = mesh_[i];
            if (m.vertBase != vEnd || m.triBase != tEnd || m.linkBase != lEnd || m.doorBase != dEnd) return fail(err, "mesh ranges not contiguous");
            vEnd += m.vertCount; tEnd += m.triCount; lEnd += m.linkCount; dEnd += m.doorCount;
            if (vEnd > vertN_ || tEnd > triN_ || lEnd > linkN_ || dEnd > doorN_) return fail(err, "mesh range outside table");
            if (!idOk(m.id) || !idOk(m.cellId) || !idOk(m.worldId)) return fail(err, "mesh id plugin index");
            for (uint32_t k = 0; k < m.triCount; ++k) {
                const Tri& t = tri_[m.triBase + k];
                for (int j = 0; j < 3; ++j) {
                    if (t.v[j] >= m.vertCount) return fail(err, "triangle vertex index");
                    if (t.nb[j] < -1 || t.nb[j] >= (int)m.triCount) return fail(err, "triangle neighbour index");
                }
            }
            for (uint32_t k = 0; k < m.linkCount; ++k) if (link_[m.linkBase + k].meshIdx != i) return fail(err, "link owner mismatch");
            for (uint32_t k = 0; k < m.doorCount; ++k) if (door_[m.doorBase + k].meshIdx != i) return fail(err, "door owner mismatch");
        }
        if (vEnd != vertN_ || tEnd != triN_ || lEnd != linkN_ || dEnd != doorN_) return fail(err, "mesh ranges do not cover tables");
        for (uint32_t i = 0; i < linkN_; ++i) {
            const Link& l = link_[i]; const Mesh& m = mesh_[l.meshIdx];
            if (!(l.flags & kNoOwnerEdge) && (l.tri >= m.triCount || l.edge > 2)) return fail(err, "link triangle or edge");
            if (l.targetMeshIdx < -1 || l.targetMeshIdx >= (int64_t)meshN_) return fail(err, "link target mesh");
            if (l.targetMeshIdx >= 0 && !(l.flags & kStaleTarget) && l.targetTri >= mesh_[l.targetMeshIdx].triCount) return fail(err, "link target triangle");
            if (!idOk(l.targetMeshId)) return fail(err, "link id plugin index");
        }
        for (uint32_t i = 0; i < doorN_; ++i) {
            const Door& d = door_[i];
            if (d.tri >= mesh_[d.meshIdx].triCount) return fail(err, "door triangle");
            if (d.refIdx < -1 || d.refIdx >= (int64_t)refN_) return fail(err, "door ref index");
            if (!idOk(d.doorRefId)) return fail(err, "door id plugin index");
        }
        for (uint32_t i = 0; i < refN_; ++i) {
            const Ref& r = ref_[i];
            if (r.kind > kTree) return fail(err, "ref kind");
            if (!idOk(r.id) || !idOk(r.base) || !idOk(r.cellId) || !idOk(r.key) || !idOk(r.teleportRef) || !idOk(r.tpCellId) || !idOk(r.enableParent)) return fail(err, "ref id plugin index");
            if (r.baseNameOff >= strsN_ || r.modelOff >= strsN_) return fail(err, "ref string offset");
        }
        return true;
    }

    const std::string& metaJson() const { return meta_; }
    const char* str(uint32_t off) const { return (off < strsN_) ? (const char*)strs_ + off : ""; }
    const char* pluginName(uint32_t id) const { uint32_t i = id >> 24; return i < plugN_ ? str(plug_[i]) : ""; }
    static uint32_t local(uint32_t id) { return id & 0xFFFFFF; }
    uint32_t meshCount() const { return meshN_; }       const Mesh& mesh(uint32_t i) const { return mesh_[i]; }
    const Vert* verts(const Mesh& m) const { return vert_ + m.vertBase; }
    const Tri* tris(const Mesh& m) const { return tri_ + m.triBase; }
    const Comp* comps(const Mesh& m) const { return comp_ + m.triBase; }
    const Link* links(const Mesh& m) const { return link_ + m.linkBase; }
    const Door* doors(const Mesh& m) const { return door_ + m.doorBase; }
    uint32_t refCount() const { return refN_; }         const Ref& ref(uint32_t i) const { return ref_[i]; }
    uint32_t linkCount() const { return linkN_; }       const Link& link(uint32_t i) const { return link_[i]; }
    uint32_t doorCount() const { return doorN_; }       const Door& door(uint32_t i) const { return door_[i]; }
private:
    static bool fail(std::string* err, const char* m) { if (err) *err = m; return false; }
    std::vector<uint8_t> buf_; std::string meta_;
    const uint8_t* strs_ = nullptr; uint32_t strsN_ = 0; const uint32_t* plug_ = nullptr; uint32_t plugN_ = 0;
    const Mesh* mesh_ = nullptr; uint32_t meshN_ = 0; const Vert* vert_ = nullptr; uint32_t vertN_ = 0; const Tri* tri_ = nullptr; uint32_t triN_ = 0;
    const Link* link_ = nullptr; uint32_t linkN_ = 0; const Door* door_ = nullptr; uint32_t doorN_ = 0; const Ref* ref_ = nullptr; uint32_t refN_ = 0;
    const Comp* comp_ = nullptr;
};
}  // namespace mfoenv
