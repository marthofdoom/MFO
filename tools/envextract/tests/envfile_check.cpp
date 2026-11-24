// compile + smoke test of envfile.h against a real file:  g++ -std=c++17 envfile_check.cpp -o t && ./t FILE.mfoenv
#include "../envfile.h"
#include <cstdio>
int main(int argc, char** argv) {
    mfoenv::Env e; std::string err;
    if (argc < 2 || !e.load(argv[1], &err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
    size_t tris = 0, ext = 0; int bad = 0;
    for (uint32_t i = 0; i < e.meshCount(); ++i) {
        const auto& m = e.mesh(i); const auto* t = e.tris(m); tris += m.triCount;
        for (uint32_t k = 0; k < m.triCount; ++k) for (int j = 0; j < 3; ++j) {
            if (t[k].v[j] >= m.vertCount) ++bad;
            if (t[k].flags & (1u << j)) ++ext; else if (t[k].nb[j] >= (int)m.triCount) ++bad;
        }
    }
    uint32_t doors = 0, actors = 0;
    for (uint32_t i = 0; i < e.refCount(); ++i) { doors += e.ref(i).kind == mfoenv::kDoorRef; actors += e.ref(i).kind == mfoenv::kActor; }
    std::printf("meshes %u tris %zu external edges %zu refs %u doors %u actors %u bad %d plugin0 %s meta %zu bytes\n", e.meshCount(), tris, ext, e.refCount(), doors, actors, bad, e.pluginName(0x00000000), e.metaJson().size());
    return bad ? 2 : 0;
}
