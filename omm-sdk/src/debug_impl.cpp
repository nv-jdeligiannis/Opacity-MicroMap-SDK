/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "debug_impl.h"
#include "bake_cpu_impl.h"
#include "texture_impl.h"

#include <shared/bird.h>
#include <shared/math.h>
#include <shared/triangle.h>
#include <shared/cpu_raster.h>
#include <shared/texture.h>
#include <shared/parse.h>

#include <stb_image_write.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <optional>

namespace omm
{
    template<class T>
    struct Image {

        Image(StdAllocator<uint8_t>& memoryAllocator, int2 size) : _memoryAllocator(memoryAllocator), _size(size), _data(memoryAllocator) {
            _data.resize(_size.x * _size.y);
        }

        Image(StdAllocator<uint8_t>& memoryAllocator, int2 size, T initalValue) : _memoryAllocator(memoryAllocator), _size(size), _data(memoryAllocator) {
            _data.resize(_size.x * _size.y, initalValue);
        }

        Image(const Image& image) : _memoryAllocator(image._memoryAllocator), _size(image._size), _data(_memoryAllocator)
        {
            _data.insert(_data.begin(), image._data.begin(), image._data.end());
        }

        bool IsValid(int2 idx) const {
            return idx.x >= 0 && idx.y >= 0 && idx.x < _size.x && idx.y < _size.y;
        }

        void Store(int2 idx, T val) {
            OMM_ASSERT(idx.x < _size.x);
            OMM_ASSERT(idx.y < _size.y);
            _data[idx.x + idx.y * _size.x] = val;
        }

        T Load(int2 idx) const {
            OMM_ASSERT(idx.x < _size.x);
            OMM_ASSERT(idx.y < _size.y);
            return _data[idx.x + idx.y * _size.x];
        }

        T Sample(omm::TextureAddressMode mode, const float2& p) const {
            const int2 pi = (int2)(glm::floor(p * float2(_size)));
            const int2 idx = omm::GetTexCoord(mode, pi, _size);
            return Load(idx);
        }

        template<class T2 = T>
        T BilinearSample(omm::TextureAddressMode mode, const float2& p) const {

            float2 pixelOffset = float2(p * (float2)_size - 0.5f);
            int2 coords[omm::TexelOffset::MAX_NUM];
            omm::GatherTexCoord4(mode, int2(glm::floor(pixelOffset)), _size, coords);

            const float2 weight = glm::fract(pixelOffset);
            T2 a = (T2)Load(coords[omm::TexelOffset::I0x0]);
            T2 b = (T2)Load(coords[omm::TexelOffset::I0x1]);
            T2 c = (T2)Load(coords[omm::TexelOffset::I1x0]);
            T2 d = (T2)Load(coords[omm::TexelOffset::I1x1]);

            T2 ac = glm::lerp<T2>(a, c, weight.x);
            T2 bd = glm::lerp<T2>(b, d, weight.x);
            T2 bilinearValue = glm::lerp(ac, bd, weight.y);
            return bilinearValue;
        }

        T Load(omm::TextureAddressMode mode, const int2& idx) const {
            const int2 idxAddessed = omm::GetTexCoord(mode, idx, _size);
            return Load(idxAddessed);
        }

        void for_each(const std::function<void(int2, T&)>& cb) {
            for (int j = 0; j < _size.y; ++j) {
                for (int i = 0; i < _size.x; ++i) {
                    int2 dst = { i, j };
                    cb(dst, _Load(dst));
                }
            }
        }

        int2 GetSize() const { return _size; }
        int GetWidth() const { return _size.x; }
        int GetHeight() const { return _size.y; }
        const char* GetData() const { return (const char*)_data.data(); }
        size_t GetDataSize() const { return _size.x * _size.y * sizeof(T); }

    private:
        T& _Load(int2 idx) {
            OMM_ASSERT(idx.x < _size.x);
            OMM_ASSERT(idx.y < _size.y);
            return _data[idx.x + idx.y * _size.x];
        }
        StdAllocator<uint8_t>& _memoryAllocator;
        int2 _size;
        vector<T> _data;
    };

    using ImageRGB = Image<uchar3>;
    using ImageRGBA = Image<uchar4>;
    using ImageAlpha = Image<uint8_t>;
    using ImageAlphaFp = Image<float>;

    static inline bool SaveImageToFile(const std::filesystem::path& folder, const std::string& fileName, const std::optional<ImageRGBA>& image) {
        if (!folder.empty())
            std::filesystem::create_directory(folder);

        const uint CHANNEL_NUM = 4;

        std::string file = folder.string() + "/" + fileName;

        int res = stbi_write_png(file.c_str(), image->GetWidth(), image->GetHeight(), CHANNEL_NUM, (unsigned char*)image->GetData(), 0 /*stride in bytes*/);
        return res == 1;
    }

    Result SaveAsImagesImpl(StdAllocator<uint8_t>& memoryAllocator, const Cpu::BakeInputDesc& desc, const Cpu::BakeResultDesc* resDesc, const Debug::SaveImagesDesc& dumpDesc)
    {
        if (desc.texture == 0)
            return Result::INVALID_ARGUMENT;

        if (dumpDesc.detailedCutout && dumpDesc.oneFile)
            return Result::INVALID_ARGUMENT;

        TextureImpl* texImpl = (TextureImpl*)desc.texture;

        vector<omm::OpacityState> states(memoryAllocator);
        set<int32_t> dumpedOMMs(memoryAllocator);

        std::optional<ImageRGBA> target;

        vector<ImageAlphaFp> alphaFps(memoryAllocator);
        for (uint32_t mipIt = 0; mipIt < texImpl->GetMipCount(); ++mipIt)
        {
            alphaFps.emplace_back(memoryAllocator, texImpl->GetSize(mipIt), float(0.f));

            alphaFps.rbegin()->for_each([&desc, texImpl, mipIt](int2 pixel, float& val) {
                val = 1.f - (float)texImpl->Load(pixel, mipIt);

                val = (float)(int8_t(127.f * val + 0.5f)) / 127.f;
                });
        }

        // Iterate over macro triangles.
        const uint32_t primitiveCount = desc.indexCount / 3;
        for (uint32_t primIt = 0; primIt < primitiveCount; ++primIt) {

            const int32_t vmIdx = parse::GetOmmIndexForTriangleIndex(*resDesc, primIt);
            const bool isSpecialIndex = vmIdx < 0;
            const bool isAlreadyDrawn = dumpedOMMs.find(vmIdx) != dumpedOMMs.end();
            const bool highlightReuse = isAlreadyDrawn && !isSpecialIndex;

            dumpedOMMs.insert(vmIdx);

            int32_t subdivisionLevel = parse::GetTriangleStates(primIt, *resDesc, nullptr);

            const uint32_t maxVmCount = omm::bird::GetNumMicroTriangles(subdivisionLevel);
            states.resize(maxVmCount);

            parse::GetTriangleStates(primIt, *resDesc, states.data());

            const uint32_t texCoordStrideInBytes = desc.texCoordStrideInBytes == 0 ? GetTexCoordFormatSize(desc.texCoordFormat) : desc.texCoordStrideInBytes;

            // Construct the UV-mactro triangle from the model source data
            uint32_t triangleIndices[3];
            GetUInt32Indices(desc.indexFormat, desc.indexBuffer, 3ull * primIt, triangleIndices);

            omm::Triangle macroTriangle = FetchUVTriangle(desc.texCoords, texCoordStrideInBytes, desc.texCoordFormat, triangleIndices);

            const bool ClippedViewport = dumpDesc.detailedCutout;

            int2 scale;
            int2 srcSize;
            int2 offset;
            int2 size;
            if (ClippedViewport)
            {
                const int2 kMaxDim = (int2)8192;
                scale = glm::max(kMaxDim / (int2)alphaFps[0].GetSize(), 1);
                srcSize = alphaFps[0].GetSize() * scale;

                offset = int2(glm::floor(float2(srcSize) * macroTriangle.aabb_s));
                size = int2(glm::floor(float2(srcSize) * (macroTriangle.aabb_e - macroTriangle.aabb_s))) + int2{ 1,1 };
            }
            else {
                scale = (int2)5;
                srcSize = alphaFps[0].GetSize() * scale;
                offset = (int2)0;// int2(float2(srcSize)* macroTriangle.aabb_s);
                size = srcSize;// int2(float2(srcSize)* (macroTriangle.aabb_e - macroTriangle.aabb_s)) + int2{ 1,1 };
            }

            static float3 kStateColorDefaultLUT[4] = {
                float3{0,     0,      1.f}, // Transparent
                float3{0,     1.f,      0}, // Opaque
                float3{1.f,   0,      1.f}, // UnknownTransparent
                float3{1.f,   1.f,    0.f}, // UnknownOpaque
            };

            static float3 kStateColorMonoLUT[4] = {
                float3{0,     0,      1.f}, // Transparent
                float3{0,     1.f,      0}, // Opaque
                float3{1.f,   1,      0.f}, // UnknownTransparent
                float3{1.f,   1.f,    0.f}, // UnknownOpaque
            };

            float3* stateColorLUT = dumpDesc.monochromeUnknowns ? kStateColorMonoLUT : kStateColorDefaultLUT;

            {
                enum class Mode {
                    FillBackground,
                    FillOMMStates,
                    DrawContourLine,
                };

                struct RasterParams {
                    omm::OpacityState* states = nullptr;
                    omm::SamplerDesc runtimeSamplerDesc;
                    int32_t subdivisionLevel = 0;
                    std::optional<ImageRGBA>* target = nullptr;
                    const ImageAlphaFp* srcAlphaFp = nullptr;
                    float alphaCutoff = 0.f;
                    float2 invSrcSize;
                    int2 srcSize;
                    int2 offset;
                    int2 scale;
                    Mode mode;
                    bool highlightReuse;
                    bool macroTriangleIsBackfacing = false;
                    uint32_t mip = 0;
                    uint32_t mipCount = 0;
                };

                auto Kernel = [&stateColorLUT](int2 pixel, float3* bc, void* context) {

                    RasterParams* p = (RasterParams*)context;


                    int2 dst = (pixel - p->offset);
                    if (dst.x < 0 || dst.y < 0)
                        return;
                    if (dst.x >= p->target->value().GetSize().x || dst.y >= p->target->value().GetSize().y)
                        return;
                    // Apply offset to to from our smaller cutout to the large alpha texture.

                    if (p->mode == Mode::FillBackground)
                    {
                        const float2 uv = (float2(pixel)) * p->invSrcSize;

                        float4 rgb;
                        if (p->runtimeSamplerDesc.filter == TextureFilterMode::Linear)
                        {
                            float alphaFinal = 0.f;
                            for (uint32_t mipIt = 0; mipIt < p->mipCount; ++mipIt)
                            {
                                float alpha = 1.f - p->srcAlphaFp[mipIt].BilinearSample(p->runtimeSamplerDesc.addressingMode, uv);

                                if (alpha < (1.f - p->alphaCutoff))
                                    alphaFinal++;
                            }

                            alphaFinal /= (float)p->mipCount;

                            rgb = 255.f * float4(1.f - alphaFinal);
                        }
                        else
                        {
                            rgb = 255.f * float4(1.f - p->srcAlphaFp[p->mip].Sample(p->runtimeSamplerDesc.addressingMode, uv));
                        }
                        const uchar4 finalRGBA = uchar4(rgb.r, rgb.g, rgb.b, 255);
                        p->target->value().Store(dst, finalRGBA);
                    }
                    else if (p->mode == Mode::FillOMMStates)
                    {
                        bool isUpright = false;

                        float2 bc2 = p->macroTriangleIsBackfacing ? float2(bc->x, bc->y) : float2(bc->z, bc->x);
                        float2 bc2Clamp = glm::saturate(bc2);

                        uint32_t vmIdx = omm::bird::bary2index(bc2Clamp, p->subdivisionLevel, isUpright);
                        vmIdx = std::clamp<uint32_t>(vmIdx, 0u, omm::bird::GetNumMicroTriangles(p->subdivisionLevel) - 1);
                        float3 vmColor = stateColorLUT[(uint32_t)p->states[vmIdx]];
                        if (isUpright)
                            vmColor = vmColor * 0.9f;

                        const float3 tint = p->highlightReuse ? float3(0.5f, 0.5f, 0.5f) : float3(1.f);

                        const float3 prevVal = float3(p->target->value().Load(dst)) / float3(255.f);
                        //const float3 blend = glm::lerp(vmColor, prevVal, 0.75f);
                        const float3 blend = glm::lerp(vmColor, prevVal, 0.5f);
                        const float3 finalRGB = tint * blend;
                        const uchar4 finalRGBA = uchar4(finalRGB.r * 255.f, finalRGB.g * 255.f,  finalRGB.b * 255.f, 255);
                        p->target->value().Store(dst, finalRGBA);
                    }
                    else if (p->mode == Mode::DrawContourLine)
                    {
                        uint32_t opaque = 0;
                        uint32_t trans = 0;

                        float delta = 0.f;
                        if (p->runtimeSamplerDesc.filter == TextureFilterMode::Linear)
                        {
                            const float alpha0x0 = p->srcAlphaFp[p->mip].BilinearSample<float>(p->runtimeSamplerDesc.addressingMode, float2(pixel - int2(0, 0)) * p->invSrcSize);
                            const float alpha1x0 = p->srcAlphaFp[p->mip].BilinearSample<float>(p->runtimeSamplerDesc.addressingMode, float2(pixel - int2(1, 0)) * p->invSrcSize);
                            const float alpha0x1 = p->srcAlphaFp[p->mip].BilinearSample<float>(p->runtimeSamplerDesc.addressingMode, float2(pixel - int2(0, 1)) * p->invSrcSize);
                            const float alpha1x1 = p->srcAlphaFp[p->mip].BilinearSample<float>(p->runtimeSamplerDesc.addressingMode, float2(pixel - int2(1, 1)) * p->invSrcSize);
                            alpha0x0 > (1.f - p->alphaCutoff) ? opaque++ : trans++;
                            alpha1x0 > (1.f - p->alphaCutoff) ? opaque++ : trans++;
                            alpha0x1 > (1.f - p->alphaCutoff) ? opaque++ : trans++;
                            alpha1x1 > (1.f - p->alphaCutoff) ? opaque++ : trans++;

                            delta = 0.25f * (alpha0x0 + alpha1x0 + alpha0x1 + alpha1x1) - p->alphaCutoff;

                            const float epsilon = 1e-6f;
                            // This is a bit inaccurate... The contour line is drawn exactly between two pixels (in nearest mode...)
                            // here the pixel up/left of the contour line will be marked.
                            const bool isContour = (trans != 0 && opaque != 0) || (std::abs(delta) < epsilon);
                            if (isContour) {
                                const float3 finalRGB = isContour ? float3(1.f, 0, 0) : float3(1.f, 0, 0);
                                const uchar4 finalRGBA = uchar4(finalRGB.r * 255.f, finalRGB.g * 255.f, finalRGB.b * 255.f, 255);
                                p->target->value().Store(dst, finalRGBA);
                            }
                        }
                        else // Nearest
                        {
                            OMM_ASSERT((uint32_t)TextureFilterMode::MAX_NUM == 2);

                            const float alpha0x0 = p->srcAlphaFp->Sample(p->runtimeSamplerDesc.addressingMode, (float2(pixel)) * p->invSrcSize);
                            
                            if (alpha0x0 > p->alphaCutoff) {
                                const float3 prevVal = float3(p->target->value().Load(dst)) / float3(255.f);
                                const float3 fillColor = float3(1.f, 0, 0);
                                const float3 finalRGB = 0.5f * (prevVal + fillColor);

                                const uchar4 finalRGBA = uchar4(finalRGB.r * 255.f, finalRGB.g * 255.f, finalRGB.b * 255.f, 255);

                                p->target->value().Store(dst, finalRGBA);
                            }
                        }
                    }
                    else {
                        OMM_ASSERT(false);
                    }
                };

                RasterParams params;
                params.states = states.data();
                params.subdivisionLevel = subdivisionLevel;
                params.runtimeSamplerDesc = desc.runtimeSamplerDesc;
                params.target = &target;
                params.srcAlphaFp = alphaFps.data();
                params.alphaCutoff = (float)desc.alphaCutoff;
                params.invSrcSize = 1.f / float2(srcSize);
                params.srcSize = srcSize;
                params.offset = offset;
                params.scale = scale;
                params.mode = Mode::FillBackground;
                params.highlightReuse = highlightReuse;
                params.macroTriangleIsBackfacing = macroTriangle._winding == omm::WindingOrder::CW;
                params.mipCount = (uint32_t)alphaFps.size();

                // Clone the source texture and render each individual VM on top of it. 
                if (!target.has_value())
                { 
                    target.emplace(ImageRGBA(memoryAllocator, size, uchar4(0)));

                    // Fill background with the source alpha texture 
                    params.mode = Mode::FillBackground;

                    float2 p00;
                    float2 p10;
                    float2 p01;
                    float2 p11;
                    if (ClippedViewport)
                    {
                        // This snippet renders two triangles forming a quad that covers the cutout area.
                        p00 = macroTriangle.aabb_s;
                        p10 = float2(macroTriangle.aabb_e.x, macroTriangle.aabb_s.y);
                        p01 = float2(macroTriangle.aabb_s.x, macroTriangle.aabb_e.y);
                        p11 = macroTriangle.aabb_e;
                    }
                    else {
                        p00 = float2(0, 0);
                        p10 = float2(1, 0);
                        p01 = float2(0, 1);
                        p11 = float2(1, 1);
                    }

                    omm::Triangle t0(p00, p11, p01);
                    omm::RasterizeConservativeParallel(t0, srcSize, Kernel, &params);
                    omm::Triangle t1(p00, p10, p11);
                    omm::RasterizeConservativeParallel(t1, srcSize, Kernel, &params);
                }

                {  // Fill in each VM-substate color with a blend color
                    params.mode = Mode::FillOMMStates;
                    omm::RasterizeConservativeParallel(macroTriangle, srcSize, Kernel, &params);
                }

                if (!dumpDesc.oneFile || primitiveCount == primIt + 1)
                { 
                    // Fill in the contour line(s).
                    for (uint32_t mipIt = 0; mipIt < texImpl->GetMipCount(); mipIt++)
                    {
                        params.mode = Mode::DrawContourLine;
                        params.mip = mipIt;

                        float2 p00;
                        float2 p10;
                        float2 p01;
                        float2 p11;
                        if (ClippedViewport)
                        {
                            p00 = macroTriangle.aabb_s;
                            p10 = float2(macroTriangle.aabb_e.x, macroTriangle.aabb_s.y);
                            p01 = float2(macroTriangle.aabb_s.x, macroTriangle.aabb_e.y);
                            p11 = macroTriangle.aabb_e;
                        }
                        else {
                            p00 = float2(0, 0);
                            p10 = float2(1, 0);
                            p01 = float2(0, 1);
                            p11 = float2(1, 1);
                        }

                        omm::Triangle t0(p00, p11, p01);
                        omm::RasterizeConservativeParallel(t0, srcSize, Kernel, &params);
                        omm::Triangle t1(p00, p10, p11);
                        omm::RasterizeConservativeParallel(t1, srcSize, Kernel, &params);
                    }
                }

                if (!dumpDesc.oneFile)
                {
                    bool res = SaveImageToFile(dumpDesc.path, std::to_string(/*meshIt*/ 0) + "_" + std::to_string(primIt) + "_" + std::string(dumpDesc.filePostfix) + ".png", target);
                    if (!res)
                        return Result::FAILURE;
                    target.reset();
                }
            }
        }

        if (dumpDesc.oneFile)
        {
            bool res = SaveImageToFile(dumpDesc.path, std::to_string(/*meshIt*/ 0) + "_" + std::string(dumpDesc.filePostfix) + ".png", target);
            if (!res)
                return Result::FAILURE;
        }

        return Result::SUCCESS;
    }

    static Debug::Stats CollectStats(StdAllocator<uint8_t>& memoryAllocator, const omm::Cpu::BakeResultDesc& resDesc) {

        Debug::Stats stats;

        const uint32_t triangleCount = resDesc.ommIndexCount;

        for (uint32_t i = 0; i < triangleCount; ++i) {

            const int32_t vmIdx = omm::parse::GetOmmIndexForTriangleIndex(resDesc, i);

            if (vmIdx == (int32_t)omm::SpecialIndex::FullyTransparent) {
                stats.totalFullyTransparent++;
            }
            else if (vmIdx == (int32_t)omm::SpecialIndex::FullyOpaque) {
                stats.totalFullyOpaque++;
            }
            else if (vmIdx == (int32_t)omm::SpecialIndex::FullyUnknownTransparent) {
                stats.totalFullyUnknownTransparent++;
            }
            else if (vmIdx == (int32_t)omm::SpecialIndex::FullyUnknownOpaque) {
                stats.totalFullyUnknownOpaque++;
            }
            else {
                OMM_ASSERT(vmIdx < (int32_t)resDesc.ommDescArrayCount);
                // Calculate later
            }
        }

        struct DescStats
        {
            uint64_t totalOpaque = 0;
            uint64_t totalTransparent = 0;
            uint64_t totalUnknownOpaque = 0;
            uint64_t totalUnknownTransparent = 0;
        };

        vector<DescStats> descStats(memoryAllocator);
        descStats.resize(resDesc.ommDescArrayCount);
        for (uint32_t i = 0; i < resDesc.ommDescArrayCount; ++i)
        {
            OMM_ASSERT(i < resDesc.ommDescArrayCount);

            const omm::Cpu::OpacityMicromapDesc& vmDesc = resDesc.ommDescArray[i];
            const uint8_t* ommArrayData = (const uint8_t*)((const char*)resDesc.ommArrayData) + vmDesc.offset;
            const uint32_t numMicroTriangles = 1u << (vmDesc.subdivisionLevel << 1u);
            const uint32_t is2State = (omm::OMMFormat)vmDesc.format == omm::OMMFormat::OC1_2_State ? 1 : 0;
            for (uint32_t uTriIt = 0; uTriIt < numMicroTriangles; ++uTriIt)
            {
                int byteIndex = uTriIt >> (2 + is2State);
                uint8_t v = ((uint8_t*)ommArrayData)[byteIndex];
                omm::OpacityState state;
                if (is2State)   state = omm::OpacityState((v >> ((uTriIt << 0) & 7)) & 1); // 2-state
                else			state = omm::OpacityState((v >> ((uTriIt << 1) & 7)) & 3); // 4-state

                OMM_ASSERT(
                    state == omm::OpacityState::Opaque ||
                    state == omm::OpacityState::Transparent ||
                    state == omm::OpacityState::UnknownOpaque ||
                    state == omm::OpacityState::UnknownTransparent);

                if (state == omm::OpacityState::Opaque)
                    descStats[i].totalOpaque++;
                else if (state == omm::OpacityState::Transparent)
                    descStats[i].totalTransparent++;
                else if (state == omm::OpacityState::UnknownOpaque)
                    descStats[i].totalUnknownOpaque++;
                else if (state == omm::OpacityState::UnknownTransparent)
                    descStats[i].totalUnknownTransparent++;
            }
        }


        for (uint32_t i = 0; i < resDesc.ommIndexCount; ++i)
        {
            int32_t index = resDesc.ommIndexFormat == IndexFormat::I16_UINT ? ((int16_t*)resDesc.ommIndexBuffer)[i] : ((int32_t*)resDesc.ommIndexBuffer)[i];
            if (index < 0)
                continue;
            stats.totalOpaque += descStats[index].totalOpaque;
            stats.totalTransparent += descStats[index].totalTransparent;
            stats.totalUnknownOpaque += descStats[index].totalUnknownOpaque;
            stats.totalUnknownTransparent += descStats[index].totalUnknownTransparent;
        }

        return stats;
    }

    Result GetStatsImpl(StdAllocator<uint8_t>& memoryAllocator, const Cpu::BakeResultDesc* resDesc, Debug::Stats* out)
    {
        if (resDesc == nullptr)
            return Result::INVALID_ARGUMENT;
        if (out == nullptr)
            return Result::INVALID_ARGUMENT;

        *out = CollectStats(memoryAllocator, *resDesc);
        return Result::SUCCESS;
    }
}  // namespace omm