/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <Rendering/WhiteBoxRenderData.h>

namespace WhiteBox
{
    //! Default gap between lightmap charts, in UV units (about 2.5 texels on a 512 lightmap).
    inline constexpr float DefaultLightmapMargin = 0.005f;

    //! Give every face a second, non-overlapping UV set for baked lighting (the UV1 stream, glTF TEXCOORD_1). Connected
    //! coplanar triangles form a chart flattened onto its own plane, so a chart never overlaps itself; the charts are packed
    //! into the unit square keeping their world sizes in proportion, @p margin apart. Works on the finished triangles, so
    //! it covers every layer, boolean and bake alike.
    void GenerateLightmapUvs(WhiteBoxFaces& faces, float margin = DefaultLightmapMargin);
} // namespace WhiteBox
