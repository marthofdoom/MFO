// envfile.h - header-only reader for MFOENV v1 files written by tools/envextract (README.md is the contract).
// No dependencies beyond the C++17 standard library.  Little endian host assumed (x86/ARM64).
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
struct Link { uint32_t meshIdx; uint16_t tri; uint8_t edge, kind; uint32_t targetMeshId; int32_t targetMeshIdx; uint16_t targetTri, pad; };
struct Door { uint32_t meshIdx; uint16_t tri, pad; uint32_t crc, doorRefId; int32_t refIdx; };
struct Ref {    // 128
    uint32_t id, base, cellId; uint8_t kind, baseSig; uint16_t flags;
    float pos[3], rot[3], scale, boundsMin[3], boundsMax[3];
    uint32_t key; uint8_t lockLevel, lockFlags; uint16_t count;
    uint32_t teleportRef; float tpPos[3], tpRot[3]; uint32_t tpFlags, tpCellId, enableParent, baseNameOff, modelOff, reserved;
};
#pragma pack(pop)
static_assert(sizeof(Mesh) == 72 && sizeof(Vert) == 12 && sizeof(Tri) == 16 && sizeof(Link) == 20 && sizeof(Door) == 20 && sizeof(Ref) == 128, "layout");

enum TriFlag : uint16_t { kEdge0Ext = 1, kEdge1Ext = 2, kEdge2Ext = 4, kDeleted = 8, kNoLarge = 0x10, kOverlap = 0x20, kPreferred = 0x40, kWater = 0x200, kDoor = 0x400 };
enum Kind : uint8_t { kDoorRef, kContainer, kActivator, kFurniture, kItem, kActor, kStatic, kFlora, kTree };
enum RefFlag : uint16_t { kPersistent = 1, kLeveledActor = 2, kHasEnableParent = 4, kLocked = 8, kTeleport = 16, kEnableInverted = 32 };

class Env {
public:
    bool load(const char* path, std::string* err = nullptr) {
        FILE* f = std::fopen(path, "rb");
        if (!f) { if (err) *err = "cannot open"; return false; }
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        buf_.resize((size_t)n);
        if (n < 16 || std::fread(buf_.data(), 1, (size_t)n, f) != (size_t)n) { std::fclose(f); if (err) *err = "short read"; return false; }
        std::fclose(f);
        if (std::memcmp(buf_.data(), "MFOENV\0\0", 8) != 0) { if (err) *err = "bad magic"; return false; }
        uint32_t ver, ns; std::memcpy(&ver, &buf_[8], 4); std::memcpy(&ns, &buf_[12], 4);
        if (ver != 1) { if (err) *err = "unsupported version"; return false; }
        for (uint32_t i = 0; i < ns; ++i) {
            const uint8_t* p = &buf_[16 + 16 * (size_t)i]; char tag[5] = {0}; uint32_t off, cnt, rec;
            std::memcpy(tag, p, 4); std::memcpy(&off, p + 4, 4); std::memcpy(&cnt, p + 8, 4); std::memcpy(&rec, p + 12, 4);
            if ((size_t)off + (size_t)cnt * rec > buf_.size()) { if (err) *err = "section out of range"; return false; }
            const uint8_t* d = &buf_[off];
            if (!std::strcmp(tag, "META")) meta_.assign((const char*)d, cnt);          // UTF-8 JSON
            else if (!std::strcmp(tag, "STRS")) strs_ = d, strsN_ = cnt;
            else if (!std::strcmp(tag, "PLUG")) plug_ = (const uint32_t*)d, plugN_ = cnt;
            else if (!std::strcmp(tag, "MESH")) mesh_ = (const Mesh*)d, meshN_ = cnt;
            else if (!std::strcmp(tag, "VERT")) vert_ = (const Vert*)d, vertN_ = cnt;
            else if (!std::strcmp(tag, "TRIS")) tri_ = (const Tri*)d, triN_ = cnt;
            else if (!std::strcmp(tag, "LINK")) link_ = (const Link*)d, linkN_ = cnt;
            else if (!std::strcmp(tag, "DOOR")) door_ = (const Door*)d, doorN_ = cnt;
            else if (!std::strcmp(tag, "REFS")) ref_ = (const Ref*)d, refN_ = cnt;
        }   // unknown tags are ignored on purpose
        return true;
    }
    const std::string& metaJson() const { return meta_; }
    const char* str(uint32_t off) const { return (off < strsN_) ? (const char*)strs_ + off : ""; }
    const char* pluginName(uint32_t id) const { uint32_t i = id >> 24; return i < plugN_ ? str(plug_[i]) : ""; }
    static uint32_t local(uint32_t id) { return id & 0xFFFFFF; }
    uint32_t meshCount() const { return meshN_; }       const Mesh& mesh(uint32_t i) const { return mesh_[i]; }
    const Vert* verts(const Mesh& m) const { return vert_ + m.vertBase; }
    const Tri* tris(const Mesh& m) const { return tri_ + m.triBase; }
    const Link* links(const Mesh& m) const { return link_ + m.linkBase; }
    const Door* doors(const Mesh& m) const { return door_ + m.doorBase; }
    uint32_t refCount() const { return refN_; }         const Ref& ref(uint32_t i) const { return ref_[i]; }
private:
    std::vector<uint8_t> buf_; std::string meta_;
    const uint8_t* strs_ = nullptr; uint32_t strsN_ = 0; const uint32_t* plug_ = nullptr; uint32_t plugN_ = 0;
    const Mesh* mesh_ = nullptr; uint32_t meshN_ = 0; const Vert* vert_ = nullptr; uint32_t vertN_ = 0; const Tri* tri_ = nullptr; uint32_t triN_ = 0;
    const Link* link_ = nullptr; uint32_t linkN_ = 0; const Door* door_ = nullptr; uint32_t doorN_ = 0; const Ref* ref_ = nullptr; uint32_t refN_ = 0;
};
}  // namespace mfoenv
