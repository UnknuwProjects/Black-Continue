// HLSL to SPIR-V for the Vulkan renderer, through glslang's HLSL front end, with a cache on disk
#include "vk_shader.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "common/filelog.h"
#include "core/log.h"

namespace vkshader {

namespace {

std::filesystem::path g_dir;
std::mutex g_lock;
// bumped when the translation below changes, so older cached modules are not used
constexpr uint32_t kCacheVersion = 3;
// mali: let glslang's optimizer inline the helpers that take a texture or sampler, so none goes through a function variable,
// and (full precision) take the relaxed precision off every value, which Mali runs as 16 bit floats
bool g_inline = false;
bool g_precise = false;

struct Param {
    std::string type, name;
};

bool Opaque(const std::string& type) { return type.find("Texture") != std::string::npos || type.find("Sampler") != std::string::npos; }

std::vector<Param> Params(const std::string& list) {
    std::vector<Param> out;
    static const std::regex one(R"(\s*(.*?)\s*(\w+)\s*(,|$))");
    for (auto it = std::sregex_iterator(list.begin(), list.end(), one); it != std::sregex_iterator(); ++it)
        if ((*it)[2].length()) out.push_back({(*it)[1], (*it)[2]});
    return out;
}

// the index past the brace that closes the one at open
size_t CloseOf(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        if (s[i] == '}' && --depth == 0) return i + 1;
    }
    return std::string::npos;
}

// glslang without the optimizer hands a texture or sampler parameter over through a function variable, which
// Vulkan's SPIR-V forbids (a store to an opaque type, VUID 06924). Adreno drivers run it all the same; Mali's
// sample garbage through it, which the glow passes do with the SDK's spreadtap and shadow maps with its shadow:
// the glow came out saturated and flickering (issues 4 and 20). Each helper taking them is replaced by a copy per
// call site, named for the globals passed and reading them directly, declared after them, before main
std::string Specialize(const std::string& hlsl) {
    static const std::regex def(R"(\n(\w[\w<>]*)[ \t]+(\w+)\(([^()]*)\)\s*\{)");
    std::string s = hlsl, made;
    size_t from = 0;
    for (std::smatch m; std::regex_search(s.cbegin() + long(from), s.cend(), m, def);) {
        size_t start = from + size_t(m.position(0)), open = start + size_t(m.length(0)) - 1, end = CloseOf(s, open);
        std::vector<Param> params = Params(m[3]);
        size_t opaque = 0;
        while (opaque < params.size() && Opaque(params[opaque].type)) opaque++;
        if (!opaque || end == std::string::npos) {
            from = open;
            continue;
        }
        std::string type = m[1], name = m[2], body = s.substr(open, end - open), rest;
        for (size_t i = opaque; i < params.size(); i++) rest += (i > opaque ? ", " : "") + params[i].type + " " + params[i].name;
        s.erase(start, end - start);
        // the calls: name(global, global, ...) with one identifier per opaque parameter
        std::string pattern = "\\b" + name + "\\(";
        for (size_t i = 0; i < opaque; i++) pattern += R"(\s*(\w+)\s*,)";
        const std::regex call(pattern);
        std::string out;
        size_t at = 0;
        for (std::smatch c; std::regex_search(s.cbegin() + long(at), s.cend(), c, call);) {
            std::string spec = name, copy = body;
            for (size_t i = 0; i < opaque; i++) {
                spec += "_" + c[i + 1].str();
                copy = std::regex_replace(copy, std::regex("\\b" + params[i].name + "\\b"), c[i + 1].str());
            }
            std::string decl = "\n" + type + " " + spec + "(" + rest + ") " + copy + "\n";
            if (made.find(decl) == std::string::npos) made += decl;
            out += s.substr(at, size_t(c.position(0))) + spec + "(";
            at += size_t(c.position(0)) + size_t(c.length(0));
        }
        s = out + s.substr(at);
    }
    if (made.empty()) return hlsl;
    // the pixel shader's entry point follows the texture and sampler declarations
    size_t main = s.rfind("\nfloat4 main(");
    if (main == std::string::npos) return hlsl;
    return s.insert(main, made);
}

// fxc's spellings glslang does not read: 1.#INF for infinity
std::string Adapt(const std::string& hlsl) {
    std::string s = Specialize(hlsl);
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) s.replace(at, 6, "asfloat(0x7F800000u)");
    // the vertex inputs at the location of their register, ATTRn: the pipeline's attributes are numbered so
    static const std::regex attr(R"((\n[ \t]*)(\w+ v\d+ : ATTR(\d+);))");
    return std::regex_replace(s, attr, "$1[[vk::location($3)]] $2");
}

std::filesystem::path CachePath(uint64_t hash) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx_%u%s%s.spv", (unsigned long long)hash, kCacheVersion, g_inline ? "i" : "",
                  g_precise ? "f" : "");
    return g_dir / name;
}

bool Load(uint64_t hash, std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return false;
    std::ifstream f(CachePath(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || n % 4) return false;
    spirv.resize(size_t(n) / 4);
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(spirv.data()), n));
}

void Store(uint64_t hash, const std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return;
    std::filesystem::path p = CachePath(hash), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
}

}  // namespace

void Configure(bool optimize, bool full_precision) {
    g_inline = optimize;
    g_precise = full_precision;
}

void Init(const std::filesystem::path& cache_dir) {
    std::lock_guard lock(g_lock);
    glslang::InitializeProcess();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    g_dir = ec ? std::filesystem::path() : cache_dir;
}

namespace {

constexpr uint32_t kOpNop = 0x00010000;  // opcode 0, one word

// the RelaxedPrecision decorations in a module (OpDecorate 71 and OpMemberDecorate 72, decoration 0), made into
// OpNops when apply is set; how many there were
int RelaxedPrecision(std::vector<uint32_t>& s, bool apply) {
    int n = 0;
    for (size_t i = 5; i < s.size();) {
        uint32_t length = s[i] >> 16, op = s[i] & 0xFFFF;
        if (!length || i + length > s.size()) break;
        if ((op == 71 && length == 3 && s[i + 2] == 0) || (op == 72 && length == 4 && s[i + 3] == 0)) {
            n++;
            if (apply)
                for (uint32_t k = 0; k < length; k++) s[i + k] = kOpNop;
        }
        i += length;
    }
    return n;
}

int FunctionCalls(const std::vector<uint32_t>& s) {
    int n = 0;
    for (size_t i = 5; i < s.size();) {
        uint32_t length = s[i] >> 16, op = s[i] & 0xFFFF;
        if (!length || i + length > s.size()) break;
        if (op == 57) n++;  // OpFunctionCall
        i += length;
    }
    return n;
}

// the text through glslang to SPIR-V; the caller holds the lock
bool Build(const std::string& text, const char* entry, Stage stage, bool optimize, std::vector<uint32_t>& spirv) {
    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    const char* src = text.c_str();
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingConstants);
    sh.setShiftBinding(glslang::EResTexture, kBindingTextures);
    sh.setShiftBinding(glslang::EResSampler, kBindingSamplers);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vulkan: %s %s does not compile: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, sh.getInfoLog());
        filelog::Write("vulkan", "%s %s does not compile: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry,
                       sh.getInfoLog());
        filelog::Write("vulkan", "%s", text.c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(messages) || !program.mapIO()) {
        LOGE("vulkan: %s does not link: %s", entry, program.getInfoLog());
        filelog::Write("vulkan", "%s does not link: %s", entry, program.getInfoLog());
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = !optimize;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    return !spirv.empty();
}

// does the optimizer do anything in this build of glslang: a helper taking a texture and a sampler is inlined by it,
// the call is gone from the module. Told once in the log
void TestOptimizer() {
    static bool done = false;
    if (done) return;
    done = true;
    static const char* kTest = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 Helper(Texture2D tex, SamplerState ss, float2 uv) { return tex.Sample(ss, uv); }
float4 main(float2 uv : TEXCOORD0) : SV_Target { return Helper(t, s, uv); }
)";
    std::vector<uint32_t> plain, optimized;
    bool a = Build(kTest, "main", Stage::Pixel, false, plain), b = Build(kTest, "main", Stage::Pixel, true, optimized);
    if (!a || !b) {
        filelog::Write("vulkan", "optimizer test: the test shader did not build (%d, %d)", a, b);
        return;
    }
    filelog::Write("vulkan", "optimizer test: calls without it %d, with it %d, words %zu to %zu: %s", FunctionCalls(plain),
                   FunctionCalls(optimized), plain.size(), optimized.size(),
                   FunctionCalls(optimized) < FunctionCalls(plain) ? "the optimizer works" : "NO EFFECT in this build");
}

}  // namespace

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv) {
    if (Load(hash, spirv)) return true;
    std::lock_guard lock(g_lock);
    if (g_inline) TestOptimizer();
    std::string text = Adapt(hlsl);
    // diagnosis: a helper still taking a texture or sampler after Specialize is what Mali gets wrong
    {
        static const std::regex left(R"(\n\w[\w<>]*[ \t]+(?!main)\w+\([^()]*(Texture|Sampler)[^()]*\)\s*\{)");
        static int logged = 0;
        if (std::regex_search(text, left)) {
            LOGE("vulkan: opaque param left in %s", entry);
            if (logged < 6) {
                logged++;
                filelog::Write("vulkan", "opaque param left in %s (the helper still takes a texture or a sampler):", entry);
                filelog::Write("vulkan", "%s", text.c_str());
            }
        }
    }
    if (!Build(text, entry, stage, g_inline, spirv)) return false;
    int relaxed = RelaxedPrecision(spirv, g_precise);
    static int told = 0;
    if (told < 14 && (relaxed || told < 4)) {
        told++;
        filelog::Write("vulkan", "shader %s: %d relaxed precision decorations%s", entry, relaxed, g_precise ? ", taken off" : "");
    }
    Store(hash, spirv);
    return true;
}

}  // namespace vkshader
