/*
Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include "math.h"
#include <shared/bird.h>
#include <algorithm>

namespace omm
{
    static constexpr int    kTexCoordInvalid = 0x7FFFFFFF;
    static constexpr int    kTexCoordBorder = 0x7FFFFFFE;
    static constexpr int2   kTexCoordInvalid2{ kTexCoordInvalid, kTexCoordInvalid };
    static constexpr int2   kTexCoordBorder2{ kTexCoordBorder, kTexCoordBorder };

    enum TexelOffset {
        I0x0,
        I1x0,
        I0x1,
        I1x1,
        MAX_NUM,
    };

    template<TextureAddressMode eAddressMode>
    static inline int2 GetTexCoord(const int2& texCoord, const int2& texSize) {
        switch (eAddressMode)
        {
        case TextureAddressMode::Wrap: {
            return int2(uint2(texCoord) % uint2(texSize));
        }
        case TextureAddressMode::Mirror: {
            const int2 texCoordAbs = (int2)glm::abs((float2)texCoord + 0.5f);
            const uint2 isFlipped = (uint2((texCoordAbs) / texSize) % uint2(2u));
            const int2 wrapped = int2(uint2((texCoordAbs)) % uint2(texSize));
            return { isFlipped.x ? texSize.x - wrapped.x - 1 : wrapped.x ,
                     isFlipped.y ? texSize.y - wrapped.y - 1 : wrapped.y };
        }
        case TextureAddressMode::Clamp: {
            return { std::clamp(texCoord.x, 0, texSize.x - 1), std::clamp(texCoord.y, 0, texSize.y - 1) };
        }
        case TextureAddressMode::Border: {
            int2 res = texCoord;
            if (texCoord.x >= texSize.x || texCoord.x < 0)
                res.x = kTexCoordBorder;
            if (texCoord.y >= texSize.y || texCoord.y < 0)
                res.y = kTexCoordBorder;
            return res;
        }
        case TextureAddressMode::MirrorOnce: {
            const int2 texCoordAbs = (int2)glm::abs(float2(texCoord) + 0.5f);
            return { std::clamp(texCoordAbs.x, 0, texSize.x - 1), std::clamp(texCoordAbs.y, 0, texSize.y - 1) };
        }
        default: {
            return kTexCoordInvalid2;
        }
        }
    }

    static inline int2 GetTexCoord(TextureAddressMode addressingMode, const int2& texCoord, const int2& texSize) {
        switch (addressingMode)
        {
        case TextureAddressMode::Wrap: {
            return GetTexCoord<TextureAddressMode::Wrap>(texCoord, texSize);
        }
        case TextureAddressMode::Mirror: {
            return GetTexCoord<TextureAddressMode::Mirror>(texCoord, texSize);
        }
        case TextureAddressMode::Clamp: {
            return GetTexCoord<TextureAddressMode::Clamp>(texCoord, texSize);
        }
        case TextureAddressMode::Border: {
            return GetTexCoord<TextureAddressMode::Border>(texCoord, texSize);
        }
        case TextureAddressMode::MirrorOnce: {
            return GetTexCoord<TextureAddressMode::MirrorOnce>(texCoord, texSize);
        }
        default: {
            return kTexCoordInvalid2;
        }
        }
    }

    static inline void GatherTexCoord4(TextureAddressMode addressingMode, const int2& texCoord, const int2& texSize, int2 coords[TexelOffset::MAX_NUM]) {
        const int2 offset   = GetTexCoord(addressingMode, texCoord, texSize);
        const int2 offset11 = GetTexCoord(addressingMode, texCoord + int2{ 1, 1 }, texSize);
        coords[TexelOffset::I0x0] = { offset.x,     offset.y };
        coords[TexelOffset::I1x0] = { offset11.x,   offset.y };
        coords[TexelOffset::I0x1] = { offset.x,     offset11.y };
        coords[TexelOffset::I1x1] = { offset11.x,   offset11.y };
    }

    template<TextureAddressMode eAddressMode>
    static inline void GatherTexCoord4(const int2& texCoord, const int2& texSize, int2 coords[TexelOffset::MAX_NUM]) {
        const int2 offset = GetTexCoord<eAddressMode>(texCoord, texSize);
        const int2 offset11 = GetTexCoord<eAddressMode>(texCoord + int2{ 1, 1 }, texSize);
        coords[TexelOffset::I0x0] = { offset.x,     offset.y };
        coords[TexelOffset::I1x0] = { offset11.x,   offset.y };
        coords[TexelOffset::I0x1] = { offset.x,     offset11.y };
        coords[TexelOffset::I1x1] = { offset11.x,   offset11.y };
    }
   
    static inline uint32_t GetTexCoordFormatSize(TexCoordFormat format) {
        switch (format) {
        case TexCoordFormat::UV16_UNORM:
            return sizeof(uint16_t) * 2;
        case TexCoordFormat::UV16_FLOAT:
            return sizeof(uint16_t) * 2;
        case TexCoordFormat::UV32_FLOAT:
            return sizeof(float2);
        default:
            return 0;
        }
    }

} // namespace omm