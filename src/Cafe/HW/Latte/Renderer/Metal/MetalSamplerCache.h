#pragma once

#include <Metal/Metal.hpp>
#include <array>
#include <map>

#include "HW/Latte/Core/LatteConst.h"
#include "HW/Latte/ISA/LatteReg.h"

class MetalSamplerCache
{
public:
    MetalSamplerCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalSamplerCache();

    MTL::SamplerState* GetSamplerState(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords);

private:
    class MetalRenderer* m_mtlr;

    // Keyed by the exact sampler words + the Metal border colour. It used to be a 64-bit rotate-and-add hash
    // whose fields overlapped (WORD0 bits 0-14, i.e. CLAMP_X/Y/Z and the XY filters, landed on the same bits as
    // WORD1 bits 17-31, i.e. MAX_LOD's top bits and LOD_BIAS), with no equality check: e.g. CLAMP_LAST_TEXEL with
    // MAX_LOD 10.0 got the cached REPEAT sampler of MAX_LOD 14.0 (Monita's face tiling sideways in Nintendo Land).
    using SamplerKey = std::array<uint32, 4>;
    std::map<SamplerKey, MTL::SamplerState*> m_samplerCache;

    SamplerKey CalculateSamplerKey(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords);
};
