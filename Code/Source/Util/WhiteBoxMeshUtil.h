/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "EditorWhiteBoxDefaultShapeTypes.h"
#include "Rendering/WhiteBoxMaterial.h"
#include "Rendering/WhiteBoxRenderData.h"

#include <AzCore/Math/Vector3.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace WhiteBox
{
    //! Mesh-level helpers shared by the EditorWhiteBoxComponent translation units.
    //! Extracted from EditorWhiteBoxComponent.cpp (previously file-static).

    //! Copy every polygon of @p src into @p dest (welding shared vertices within src by vertex
    //! handle), leaving dest's existing geometry untouched. The two meshes stay topologically
    //! separate islands - this is an APPEND, not a CSG merge.
    void AppendMesh(WhiteBoxMesh& dest, const WhiteBoxMesh& src);

    //! Combine a freeform mesh with the cube grid into one output mesh (a plain append - no
    //! CSG). Any argument may be null/empty. Returns null only if there is nothing to combine.
    Api::WhiteBoxMeshPtr CombineFreeformAndGrids(WhiteBoxMesh* freeform, WhiteBoxMesh* grid);

    //! Remove every face and vertex of @p mesh in place (works for both the component-local
    //! working mesh and an asset-backed mesh).
    void ClearMeshInPlace(WhiteBoxMesh& mesh);

    //! Build a copy of @p src with every face's winding REVERSED (normals point the other way).
    //! Used for a layer's "Invert Normals": flipping the actual mesh (rather than only the
    //! render faces) makes rendering, physics cooking and selection all agree - a room built
    //! from an inverted box collides from the INSIDE, because PhysX triangle meshes are
    //! single-sided and follow the winding.
    Api::WhiteBoxMeshPtr FlippedMeshWinding(const WhiteBoxMesh& src);

    //! Build a single render face from a white box face handle, optionally reversing the
    //! winding (and flipping the normal) so the face renders inside-out.
    WhiteBoxFace BuildWhiteBoxFace(const WhiteBoxMesh& whiteBox, const Api::FaceHandle& faceHandle, bool flipWinding);

    //! Build intermediate render data (faces + material) passed to WhiteBoxRenderMeshInterface
    //! to generate the concrete render mesh.
    WhiteBoxRenderData CreateWhiteBoxRenderData(
        const WhiteBoxMesh& whiteBox, const WhiteBoxMaterial& material, bool flipWinding = false);

    //! Generate a parametric shape solid in layer-local space: footprint centred on the origin
    //! in XY, extruded from z=0 up to @p height. The extents are FULL sizes along each axis.
    //! For DrawShapeType::Room, @p width / @p depth / @p height are the INTERIOR dimensions and the
    //! room-only parameters (@p wallThickness, @p cavityGap, @p floor, @p ceiling) drive the double
    //! walls and caps; those are ignored by every other shape.
    Api::WhiteBoxMeshPtr BuildParametricShapeMesh(
        DrawShapeType shape, float width, float depth, float height, int sides, int steps,
        float wallThickness = 0.15f, float cavityGap = 0.1f, bool floor = true, bool ceiling = false);

    //! True when the default shape is set to a custom mesh asset.
    bool DisplayingAsset(DefaultShapeType defaultShapeType);

    //! Ask the property grid to refresh its attributes and values.
    void RefreshProperties();
} // namespace WhiteBox
