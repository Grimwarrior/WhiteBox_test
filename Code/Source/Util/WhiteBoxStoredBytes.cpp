/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxStoredBytes.h"

#include <cstring>
#include <zlib.h>

namespace WhiteBox
{
    namespace
    {
        // WBZ1, little-endian u32 original length, then one checksummed zlib stream.
        constexpr AZ::u8 StoredMagic[] = { 'W', 'B', 'Z', '1' };
        constexpr size_t HeaderSize = sizeof(StoredMagic) + sizeof(AZ::u32);
        constexpr size_t MinCompressSize = 1024;
        // Bound the allocation made from a stored length. Larger original streams keep their raw format.
        constexpr size_t MaxCompressSize = 256 * 1024 * 1024;

        bool IsCompressed(const AZStd::vector<AZ::u8>& bytes)
        {
            return bytes.size() >= sizeof(StoredMagic) && std::memcmp(bytes.data(), StoredMagic, sizeof(StoredMagic)) == 0;
        }
    }

    void CompressStoredBytes(AZStd::vector<AZ::u8>& bytes)
    {
        if (bytes.size() < MinCompressSize || bytes.size() > MaxCompressSize || IsCompressed(bytes))
        {
            return;
        }

        const auto size = static_cast<AZ::u32>(bytes.size());
        AZStd::vector<AZ::u8> packed;
        packed.resize_no_construct(size - 1); // Z_BUF_ERROR if wrapping would not make this smaller.
        uLongf compressedSize = static_cast<uLongf>(packed.size() - HeaderSize);
        if (compress2(packed.data() + HeaderSize, &compressedSize, bytes.data(), size, Z_BEST_SPEED) != Z_OK)
        {
            return;
        }
        std::memcpy(packed.data(), StoredMagic, sizeof(StoredMagic));
        for (unsigned int i = 0; i < sizeof(size); ++i)
        {
            packed[sizeof(StoredMagic) + i] = static_cast<AZ::u8>(size >> (8 * i));
        }
        packed.resize(HeaderSize + compressedSize);
        packed.shrink_to_fit();
        bytes.swap(packed);
    }

    bool ExpandStoredBytes(const AZStd::vector<AZ::u8>& bytes, AZStd::vector<AZ::u8>& expansion)
    {
        expansion.clear();
        if (!IsCompressed(bytes))
        {
            return true;
        }
        if (bytes.size() <= HeaderSize || bytes.size() >= MaxCompressSize)
        {
            return false;
        }
        AZ::u32 size = 0;
        for (unsigned int i = 0; i < sizeof(size); ++i)
        {
            size |= static_cast<AZ::u32>(bytes[sizeof(StoredMagic) + i]) << (8 * i);
        }
        if (size <= bytes.size() || size > MaxCompressSize)
        {
            return false;
        }
        expansion.resize_no_construct(size);
        uLongf written = size;
        uLong consumed = static_cast<uLong>(bytes.size() - HeaderSize);
        if (uncompress2(expansion.data(), &written, bytes.data() + HeaderSize, &consumed) != Z_OK ||
            written != size || consumed != bytes.size() - HeaderSize)
        {
            expansion.clear();
            return false;
        }
        return true;
    }

    bool StoredBytesEqual(const AZStd::vector<AZ::u8>& lhs, const AZStd::vector<AZ::u8>& rhs)
    {
        if (lhs == rhs)
        {
            return true;
        }
        AZStd::vector<AZ::u8> leftExpansion, rightExpansion;
        return ExpandStoredBytes(lhs, leftExpansion) && ExpandStoredBytes(rhs, rightExpansion) &&
            (leftExpansion.empty() ? lhs : leftExpansion) == (rightExpansion.empty() ? rhs : rightExpansion);
    }
}
