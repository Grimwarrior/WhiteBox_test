/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/vector.h>

namespace WhiteBox
{
    //! Reduce a welded triangle list to about keepFraction of its triangles with quadric edge collapses, never flipping
    //! a face by more than maxNormalDeviationDegrees. Triangles OpenMesh cannot take (non-manifold) pass through unchanged.
    //! @return false and leaves the input alone when nothing could be simplified.
    bool SimplifyTriangles(
        AZStd::vector<AZ::Vector3>& vertices, AZStd::vector<AZ::u32>& indices, float keepFraction,
        float maxNormalDeviationDegrees = 60.0f);
} // namespace WhiteBox
