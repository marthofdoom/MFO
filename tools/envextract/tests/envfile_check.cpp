// g++ -std=c++17 -fsanitize=address,undefined envfile_check.cpp -o t && ./t FILE.mfoenv [iterations]
// Loads the file, then truncates it at every section boundary (+-1) and at every 61st byte, then corrupts random bytes
// (seeded); load() must reject or accept, and an accepted buffer is fully traversed (ASAN/UBSAN catch any bad index).
#include "../envfile.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static unsigned long traverse(const mfoenv::Env& e) {
    unsigned long h = 0;
    for (uint32_t i = 0; i < e.meshCount(); ++i) {
        const auto& m = e.mesh(i); const auto* t = e.tris(m); const auto* v = e.verts(m); const auto* c = e.comps(m);
        for (uint32_t k = 0; k < m.triCount; ++k) {
            for (int j = 0; j < 3; ++j) { h += (unsigned long)v[t[k].v[j]].x; if (t[k].nb[j] >= 0) h += t[t[k].nb[j]].flags; }
            h += c[k].walk;
        }
        for (uint32_t k = 0; k < m.linkCount; ++k) { const auto& l = e.links(m)[k]; if (l.targetMeshIdx >= 0 && !(l.flags & mfoenv::kStaleTarget)) h += e.tris(e.mesh(l.targetMeshIdx))[l.targetTri].flags; }
        for (uint32_t k = 0; k < m.doorCount; ++k) { const auto& d = e.doors(m)[k]; h += t[d.tri].flags; if (d.refIdx >= 0) h += e.ref(d.refIdx).kind; }
    }
    for (uint32_t i = 0; i < e.doorRefCount(); ++i) { const auto& d = e.doorRef(i); h += d.refIdx; if (d.tri >= 0) h += e.tris(e.mesh(d.meshIdx))[d.tri].flags; }
    for (uint32_t i = 0; i < e.seamCount(); ++i) h += e.links(e.mesh(e.link(e.seam(i).linkIdx).meshIdx)) != nullptr;
    for (uint32_t i = 0; i < e.landCount(); ++i) h += e.land(i).h[1088] > 0.0f;
    for (uint32_t i = 0; i < e.refCount(); ++i) { const auto& r = e.ref(i); h += std::strlen(e.str(r.baseNameOff)) + std::strlen(e.str(r.modelOff)) + std::strlen(e.pluginName(r.id)); }
    return h + e.metaJson().size();
}
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    mfoenv::Env e; std::string err;
    if (!e.load(argv[1], &err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
    traverse(e);
    FILE* f = std::fopen(argv[1], "rb"); std::vector<uint8_t> raw; int c; while ((c = std::fgetc(f)) != EOF) raw.push_back((uint8_t)c); std::fclose(f);
    std::vector<size_t> cuts; uint32_t ns; std::memcpy(&ns, &raw[12], 4);
    for (uint32_t i = 0; i < ns; ++i) { uint32_t off, cnt, rec; std::memcpy(&off, &raw[16 + 16 * i + 4], 4); std::memcpy(&cnt, &raw[16 + 16 * i + 8], 4); std::memcpy(&rec, &raw[16 + 16 * i + 12], 4);
        for (int d = -1; d <= 1; ++d) { cuts.push_back(off + d); cuts.push_back(off + (size_t)cnt * rec + d); } cuts.push_back(16 + 16 * i); }
    for (size_t k = 0; k < raw.size(); k += 61) cuts.push_back(k);
    int accepted = 0, rejected = 0;
    for (size_t k : cuts) {
        if (k >= raw.size()) continue;
        mfoenv::Env t; if (t.loadBuffer(std::vector<uint8_t>(raw.begin(), raw.begin() + k), &err)) { traverse(t); ++accepted; if (k < raw.size() - 16) { std::printf("truncation at %zu accepted\n", k); } } else ++rejected;
    }
    std::mt19937 rng(12345); long iters = argc > 2 ? std::atol(argv[2]) : 3000; int acc2 = 0;
    for (long it = 0; it < iters; ++it) {
        auto b = raw; int nb = 1 + (int)(rng() % 8);
        for (int j = 0; j < nb; ++j) b[rng() % (it % 3 == 0 ? std::min<size_t>(b.size(), 16 + 16 * ns) : b.size())] = (uint8_t)rng();
        mfoenv::Env t; if (t.loadBuffer(std::move(b), &err)) { traverse(t); ++acc2; } else ++rejected;
    }
    std::printf("ok: truncations accepted %d (only full-length), rejected %d, corrupted-but-valid %d of %ld\n", accepted, rejected, acc2, iters);
    return 0;
}
