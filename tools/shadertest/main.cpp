// shadertest: translate every dumped NVN control section (NV_gpu_program5 assembly, or the
// Maxwell code in a .code file of the same name)
// to GLSL and compile it to SPIR-V, the same way the runtime does.
//
//   build/<cfg>/shadertest.exe orig/shaders_ctrl [--out DIR] [--show N]
// The dumps come from: python -P tools/shader_census.py --dump-ctrl orig/shaders_ctrl
// --out writes <hash>.glsl (and .spv) for every program; failures always get a .glsl.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "gpu/maxwell.h"
#include "gpu/nvasm.h"
#include "gpu/spirv.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: shadertest <dir of .ctrl> [--out DIR] [--show N]\n");
        return 2;
    }
    std::string out;
    int show = 5;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--show" && i + 1 < argc) show = atoi(argv[++i]);
    }
    if (!out.empty()) fs::create_directories(out);
    int total = 0, translate_fail = 0, compile_fail = 0, shown = 0;
    std::map<std::string, int> reasons;
    for (auto& e : fs::directory_iterator(argv[1])) {
        if (e.path().extension() != ".ctrl") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
        total++;
        std::string name = e.path().stem().string();
        // a .code file next to the control section: a program without assembly (maxwell.h)
        fs::path code_path = e.path();
        code_path.replace_extension(".code");
        nvasm::Result r;
        if (fs::exists(code_path)) {
            std::ifstream cf(code_path, std::ios::binary);
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(cf)), {});
            r = maxwell::translate(b.data(), b.size(), code.data(), code.size());
        } else {
            r = nvasm::translate(b.data(), b.size());
        }
        if (!r.ok) {
            translate_fail++;
            reasons[r.error.substr(0, r.error.find("  [in:"))]++;
            if (shown++ < show) printf("TRANSLATE %s: %s\n", name.c_str(), r.error.c_str());
            continue;
        }
        std::vector<uint32_t> spv;
        std::string log;
        bool ok = gpu::compile_glsl(r.refl.stage, r.glsl, spv, log);
        if (!ok) {
            compile_fail++;
            reasons["glslang: " + log.substr(0, log.find('\n'))]++;
            if (shown++ < show) printf("COMPILE %s:\n%s\n", name.c_str(), log.c_str());
        }
        if (!out.empty() || !ok) {
            std::string dir = out.empty() ? "build/shadertest_fail" : out;
            fs::create_directories(dir);
            std::ofstream(dir + "/" + name + ".glsl") << "/*\n" << r.asm_text << "\n*/\n" << r.glsl;
            if (ok) std::ofstream(dir + "/" + name + ".spv", std::ios::binary).write((const char*)spv.data(), (std::streamsize)(spv.size() * 4));
        }
    }
    printf("%d programs: %d translated and compiled, %d translation failures, %d compile failures\n", total,
           total - translate_fail - compile_fail, translate_fail, compile_fail);
    for (auto& [k, v] : reasons) printf("  %5d  %s\n", v, k.c_str());
    return translate_fail || compile_fail ? 1 : 0;
}
