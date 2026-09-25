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
#include "Viewport/WhiteBoxShapeBuilders.h"
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

    //! Overwrite @p target with the contents of @p snapshot (taken earlier with Api::CloneMesh).
    //!
    //! Used to revert a cancelled drag: the viewport tools clone the mesh at mouse-down and
    //! restore it if the user presses Escape. Restores topology as well as positions, so it also
    //! undoes an extrude/append that happened during the drag.
    //!
    //! @return False if @p snapshot could not be serialised or the write failed, in which case
    //! @p target is left untouched.
    bool RestoreMeshFromSnapshot(WhiteBoxMesh& target, const WhiteBoxMesh& snapshot);

    //! Build a copy of @p src with every face's winding REVERSED (normals point the other way).
    //! Used for a layer's "Invert Normals": flipping the actual mesh (rather than only the
    //! render faces) makes rendering, physics cooking and selection all agree - a room built
    //! from an inverted box collides from the INSIDE, because PhysX triangle meshes are
    //! single-sided and follow the winding.
    Api::WhiteBoxMeshPtr FlippedMeshWinding(const WhiteBoxMesh& src);

    //! @p src plus its reflection across a plane normal to local axis @p axis (0 X, 1 Y, 2 Z): the plane through the
    //! origin, or the mesh's low side on that axis when the mesh straddles the origin (a centred shape would otherwise
    //! land on itself). Polygons lying on the plane are dropped from both halves, so the two close into one shell.
    //! Materials, paint, UVs and blend weights carry over; the halves meet with duplicate vertices, not welded.
    Api::WhiteBoxMeshPtr MirrorMesh(const WhiteBoxMesh& src, int axis);

    //! @p count copies of @p src, each @p offset further along than the last (the first stays in place).
    Api::WhiteBoxMeshPtr ArrayMesh(const WhiteBoxMesh& src, int count, const AZ::Vector3& offset);

    //! Build a single render face from a white box face handle, optionally reversing the
    //! winding (and flipping the normal) so the face renders inside-out.
    //! @param withBlend read the corners' vertex-blend weights; callers building a whole mesh check MeshHasVertexBlend once.
    WhiteBoxFace BuildWhiteBoxFace(
        const WhiteBoxMesh& whiteBox, const Api::FaceHandle& faceHandle, bool flipWinding, bool withBlend = true);

    //! Build intermediate render data (faces + material) passed to WhiteBoxRenderMeshInterface
    //! to generate the concrete render mesh.
    WhiteBoxRenderData CreateWhiteBoxRenderData(
        const WhiteBoxMesh& whiteBox, const WhiteBoxMaterial& material, bool flipWinding = false);

    //! Generate a parametric shape solid in layer-local space: footprint centred on the origin
    //! in XY, extruded from z=0 up to @p height. The extents are FULL sizes along each axis.
    //! For DrawShapeType::Room, @p width / @p depth / @p height are the INTERIOR dimensions and the
    //! room-only parameters (@p wallThickness, @p cavityGap, @p floor, @p ceiling) drive the double
    //! walls and caps. Door uses width/height for its panel or opening, depth for extrusion,
    //! wallThickness for its frame, and archHeight for the rise above the jambs.
    //! CircularStairs uses width for tread width, height for total rise, and ignores depth.
    Api::WhiteBoxMeshPtr BuildParametricShapeMesh(
        DrawShapeType shape, float width, float depth, float height, int sides, int steps,
        float wallThickness = 0.15f, float cavityGap = 0.1f, bool floor = true, bool ceiling = false,
        bool doorFrame = true, float archHeight = 0.0f, float innerRadius = 0.5f, float sweepAngle = 270.0f,
        bool stepsByHeight = false, float stepHeight = 0.25f, float holeRatio = 0.5f,
        int tubeSides = DefaultTubeSides);

    //! True when the default shape is set to a custom mesh asset.
    bool DisplayingAsset(DefaultShapeType defaultShapeType);

    //! Ask the property grid to refresh its attributes and values.
    void RefreshProperties();
} // namespace WhiteBox
