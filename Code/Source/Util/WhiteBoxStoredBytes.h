/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/std/containers/vector.h>

namespace WhiteBox
{
    //! Store a serialized OpenMesh or render stream in its smaller representation.
    void CompressStoredBytes(AZStd::vector<AZ::u8>& bytes);

    //! For a legacy/raw stream, expansion stays empty and the caller uses the original bytes.
    //! A damaged compressed stream returns false with an empty expansion.
    bool ExpandStoredBytes(const AZStd::vector<AZ::u8>& bytes, AZStd::vector<AZ::u8>& expansion);

    //! Compare serialized content across raw and compressed representations.
    bool StoredBytesEqual(const AZStd::vector<AZ::u8>& lhs, const AZStd::vector<AZ::u8>& rhs);
}
