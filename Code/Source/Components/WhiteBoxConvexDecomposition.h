/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>

namespace WhiteBox
{
    struct WhiteBoxColliderConfiguration;

    //! The points of one convex part; PhysX builds the hull around them.
    using HullPoints = AZStd::vector<AZ::Vector3>;

    //! Splits collision geometry into convex parts, one connected shell at a time: a convex shell is kept exactly, a flat
    //! one is given a thin backing, and only concave shells go through V-HACD. Concave results are cached by shell content,
    //! so an edit only re-decomposes the shells it touched.
    class ConvexDecomposer
    {
    public:
        //! @param vertices / indices a welded triangle list (shared corners share an index).
        AZStd::vector<HullPoints> Decompose(
            const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices,
            const WhiteBoxColliderConfiguration& configuration);

    private:
        AZStd::vector<HullPoints> DecomposeConcave(
            const AZStd::vector<AZ::Vector3>& points, const AZStd::vector<AZ::u32>& triangles, AZ::u32 maxHulls,
            const WhiteBoxColliderConfiguration& configuration);

        AZStd::unordered_map<size_t, AZStd::vector<HullPoints>> m_cache;
    };
} // namespace WhiteBox
