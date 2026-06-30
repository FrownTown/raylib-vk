/**********************************************************************************************
*
*   rvk_pipeline.cpp - PSO hashing/cache, shaderc compilation, descriptor layouts, uniforms
*
*   Implements: RvkRenderState equality + hashing (PSO cache key), the graphics-pipeline
*   cache, runtime GLSL->SPIR-V via shaderc with GL->Vulkan GLSL auto-preprocessing
*   (confirmed plan decision), SPIR-V reflection for uniform/attribute locations, and the
*   uniform/push-constant plumbing.
*
*   STATUS: P2 scaffold. Render-state hashing is implemented; Vulkan pipeline assembly and
*   shaderc wiring are marked TODO(P2)/TODO(P5).
*
**********************************************************************************************/
#include "rvk_internal.hpp"

#include <cstring>

#if defined(RVK_HAVE_SHADERC)
#include <shaderc/shaderc.hpp>
#endif

//----------------------------------------------------------------------------------
// Render-state equality + hash (the PSO cache key)
//----------------------------------------------------------------------------------
bool RvkRenderState::operator==(const RvkRenderState &o) const
{
    return shaderProgram == o.shaderProgram && topology == o.topology &&
           blendEnabled == o.blendEnabled && blendMode == o.blendMode &&
           customSrcRGB == o.customSrcRGB && customDstRGB == o.customDstRGB &&
           customSrcA == o.customSrcA && customDstA == o.customDstA &&
           customEqRGB == o.customEqRGB && customEqA == o.customEqA &&
           depthTest == o.depthTest && depthMask == o.depthMask &&
           cullEnabled == o.cullEnabled && cullFace == o.cullFace &&
           colorMask[0] == o.colorMask[0] && colorMask[1] == o.colorMask[1] &&
           colorMask[2] == o.colorMask[2] && colorMask[3] == o.colorMask[3] &&
           wireMode == o.wireMode && colorFormat == o.colorFormat && depthFormat == o.depthFormat;
}

uint64_t RvkRenderState::hash() const
{
    // FNV-1a over the salient fields
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(shaderProgram);
    mix((uint64_t)topology);
    mix((blendEnabled ? 1u : 0u) | ((uint32_t)blendMode << 1));
    mix(((uint64_t)(uint32_t)customSrcRGB << 32) | (uint32_t)customDstRGB);
    mix(((uint64_t)(uint32_t)customSrcA << 32) | (uint32_t)customDstA);
    mix(((uint64_t)(uint32_t)customEqRGB << 32) | (uint32_t)customEqA);
    mix((depthTest?1:0) | (depthMask?2:0) | (cullEnabled?4:0) | (wireMode?8:0) | ((uint32_t)cullFace<<4));
    mix((colorMask[0]?1:0)|(colorMask[1]?2:0)|(colorMask[2]?4:0)|(colorMask[3]?8:0));
    mix(((uint64_t)colorFormat << 32) | (uint32_t)depthFormat);
    return h;
}

//----------------------------------------------------------------------------------
// GL-token blend factor/equation -> Vulkan translation (used during pipeline assembly)
//----------------------------------------------------------------------------------
VkBlendFactor rvkBlendFactorToVk(int glFactor)
{
    switch (glFactor) {
        case RL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case RL_ONE: return VK_BLEND_FACTOR_ONE;
        case RL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case RL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case RL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case RL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case RL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case RL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case RL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case RL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case RL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp rvkBlendEquationToVk(int glEq)
{
    switch (glEq) {
        case RL_FUNC_ADD: return VK_BLEND_OP_ADD;
        case RL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case RL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case RL_MIN: return VK_BLEND_OP_MIN;
        case RL_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static VkPrimitiveTopology rvkTopologyToVk(int rlMode)
{
    switch (rlMode) {
        case RL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case RL_TRIANGLES:
        case RL_QUADS:  // QUADS are expanded to triangles via index buffer in the batcher
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

//----------------------------------------------------------------------------------
// GL-style GLSL -> Vulkan GLSL auto-preprocessor (plan decision, see §6)
//
// Normalizes #version to 450, injects layout(location=)/layout(binding=,set=) qualifiers,
// packs loose default uniforms into a UBO block, and remaps GL builtins (gl_FragColor,
// texture2D, etc.) so existing raylib/example/user GLSL keeps working under Vulkan.
//----------------------------------------------------------------------------------
static std::string rvkPreprocessGlslToVulkan(const std::string &src, VkShaderStageFlagBits stage,
                                             RvkProgram *program)
{
    (void)stage; (void)program;
    // TODO(P5): full transform. Outline:
    //  1. Replace/insert "#version 450".
    //  2. Auto-assign location= to in/out by declaration order; record attrib name->location.
    //  3. Collect bare 'uniform <type> name;' (non-sampler) into a single UBO block at
    //     (set=0, binding=0); record name->offset for rvkSetUniform.
    //  4. Assign (set=0, binding=N) to each sampler/image; record name->binding.
    //  5. Map gl_FragColor -> declared 'layout(location=0) out vec4'; texture2D->texture; etc.
    return src;
}

//----------------------------------------------------------------------------------
// Shader compilation (shaderc)
//----------------------------------------------------------------------------------
static bool rvkCompileGlsl(const std::string &source, VkShaderStageFlagBits stage,
                           std::vector<uint32_t> &outSpirv, RvkProgram *program)
{
    std::string vkGlsl = rvkPreprocessGlslToVulkan(source, stage, program);
#if defined(RVK_HAVE_SHADERC)
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc_shader_kind kind = shaderc_glsl_vertex_shader;
    if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) kind = shaderc_glsl_fragment_shader;
    else if (stage == VK_SHADER_STAGE_COMPUTE_BIT) kind = shaderc_glsl_compute_shader;

    auto result = compiler.CompileGlslToSpv(vkGlsl, kind, "rvk_shader", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        TraceLog(RL_LOG_WARNING, "RVK: shader compile error: %s", result.GetErrorMessage().c_str());
        return false;
    }
    outSpirv.assign(result.cbegin(), result.cend());
    return true;
#else
    (void)vkGlsl; (void)outSpirv;
    TraceLog(RL_LOG_WARNING, "RVK: shaderc unavailable; cannot compile GLSL at runtime");
    return false;
#endif
}

//----------------------------------------------------------------------------------
// Graphics pipeline cache
//----------------------------------------------------------------------------------
VkPipeline rvkGetGraphicsPipeline(const RvkRenderState &state, const rvkVertexBuffer *layout)
{
    (void)layout;
    uint64_t key = state.hash();
    auto it = g_rvk.psoCache.find(key);
    if (it != g_rvk.psoCache.end()) return it->second;

    VkPipeline pso = VK_NULL_HANDLE;
    // TODO(P2): assemble VkGraphicsPipelineCreateInfo using dynamic rendering
    //   (VkPipelineRenderingCreateInfo with state.colorFormat/depthFormat), the program's
    //   shader stages, the SoA vertex input (pos/uv/normal/color bindings), blend state from
    //   rvkBlendFactorToVk/rvkBlendEquationToVk, depth/cull/polygon-mode from `state`, and
    //   dynamic viewport/scissor/line-width. Then vkCreateGraphicsPipelines(..., pipelineCache).
    g_rvk.psoCache[key] = pso;
    return pso;
}

void rvkDestroyPipelineCache()
{
    for (auto &kv : g_rvk.psoCache) if (kv.second) vkDestroyPipeline(g_rvk.device, kv.second, nullptr);
    g_rvk.psoCache.clear();
    if (g_rvk.pipelineCache) vkDestroyPipelineCache(g_rvk.device, g_rvk.pipelineCache, nullptr);
    g_rvk.pipelineCache = VK_NULL_HANDLE;
}

void rvkInitDefaultShader()
{
    // TODO(P2): load shaders/default_sprite.{vert,frag}, compile, link a program, populate
    //   g_rvk.defaultShaderId + g_rvk.defaultShaderLocs.
}

//----------------------------------------------------------------------------------
// Public shader API
//----------------------------------------------------------------------------------
extern "C" {

unsigned int rvkLoadShader(const char *code, int type)
{
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    if (type == RL_FRAGMENT_SHADER) stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    else if (type == RL_COMPUTE_SHADER) stage = VK_SHADER_STAGE_COMPUTE_BIT;

    std::vector<uint32_t> spirv;
    if (!code || !rvkCompileGlsl(code, stage, spirv, nullptr)) return 0;

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(g_rvk.device, &ci, nullptr, &mod));

    g_rvk.shaders.push_back({});
    uint32_t id = (uint32_t)g_rvk.shaders.size() - 1;
    g_rvk.shaders[id].module = mod;
    g_rvk.shaders[id].stage = stage;
    g_rvk.shaders[id].spirv = std::move(spirv);
    g_rvk.shaders[id].inUse = true;
    return id;
}

unsigned int rvkLoadShaderProgram(const char *vsCode, const char *fsCode)
{
    return rvkLoadShaderProgramEx(rvkLoadShader(vsCode, RL_VERTEX_SHADER),
                                  rvkLoadShader(fsCode, RL_FRAGMENT_SHADER));
}

unsigned int rvkLoadShaderProgramEx(unsigned int vsId, unsigned int fsId)
{
    g_rvk.programs.push_back({});
    uint32_t id = (uint32_t)g_rvk.programs.size() - 1;
    g_rvk.programs[id].vsId = vsId;
    g_rvk.programs[id].fsId = fsId;
    g_rvk.programs[id].inUse = true;
    // TODO(P5): SPIR-V reflection to fill uniformLocs/attribLocs.
    return id;
}

unsigned int rvkLoadShaderProgramCompute(unsigned int csId)
{
    g_rvk.programs.push_back({});
    uint32_t id = (uint32_t)g_rvk.programs.size() - 1;
    g_rvk.programs[id].csId = csId;
    g_rvk.programs[id].isCompute = true;
    g_rvk.programs[id].inUse = true;
    return id;
}

void rvkUnloadShader(unsigned int id)
{
    if (id == 0 || id >= g_rvk.shaders.size() || !g_rvk.shaders[id].inUse) return;
    VkShaderModule mod = g_rvk.shaders[id].module;
    rvkDeferDestroy([mod]{ vkDestroyShaderModule(g_rvk.device, mod, nullptr); });
    g_rvk.shaders[id] = {};
}

void rvkUnloadShaderProgram(unsigned int id)
{
    if (id == 0 || id >= g_rvk.programs.size()) return;
    g_rvk.programs[id] = {};
    // TODO(P2): retire pipelines in psoCache that reference this program.
}

int rvkGetLocationUniform(unsigned int id, const char *uniformName)
{
    if (id >= g_rvk.programs.size() || !uniformName) return -1;
    auto &m = g_rvk.programs[id].uniformLocs;
    auto it = m.find(uniformName);
    return it == m.end() ? -1 : it->second;
}

int rvkGetLocationAttrib(unsigned int id, const char *attribName)
{
    if (id >= g_rvk.programs.size() || !attribName) return -1;
    auto &m = g_rvk.programs[id].attribLocs;
    auto it = m.find(attribName);
    return it == m.end() ? -1 : it->second;
}

void rvkSetUniform(int locIndex, const void *value, int uniformType, int count)
{ (void)locIndex; (void)value; (void)uniformType; (void)count; /* TODO(P5): write into per-draw UBO */ }
void rvkSetUniformMatrix(int locIndex, Matrix mat) { (void)locIndex; (void)mat; /* TODO(P5) */ }
void rvkSetUniformMatrices(int locIndex, const Matrix *mat, int count) { (void)locIndex; (void)mat; (void)count; /* TODO(P5) */ }
void rvkSetUniformSampler(int locIndex, unsigned int textureId) { (void)locIndex; (void)textureId; /* TODO(P5) */ }
void rvkSetShader(unsigned int id, int *locs) { (void)locs; if (id < g_rvk.programs.size()) g_rvk.state.shaderProgram = id; }

} // extern "C"
