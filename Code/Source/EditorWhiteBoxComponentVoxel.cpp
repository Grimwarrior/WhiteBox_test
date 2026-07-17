/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * EditorWhiteBoxComponent - CUBE STAMP (voxel) translation unit: the snap-guide stamp/carve
 * gestures (union/subtract into the freeform mesh), the inert cell records and Clear Cube
 * Stamp, layer-space mapping for committed geometry, and mesh repair.
 */

#include "EditorWhiteBoxComponent.h"
#include "EditorWhiteBoxComponentModeBus.h"

#include "Util/WhiteBoxMeshUtil.h"
#include "Util/WhiteBoxVoxelUtil.h"

#include <AzCore/Math/Quaternion.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <cmath>

namespace WhiteBox
{
    void EditorWhiteBoxComponent::NormalizeVoxelData()
    {
        // Older scenes stored just the cell coordinates plus a single baked cell size
        // (m_voxel.m_legacySize). Give every such cell its own size entry so the rest of the
        // pipeline can treat all data as per-cell sized. Also guards against any stale
        // mismatch between the two parallel arrays.
        if (m_voxel.m_sizes.size() != m_voxel.m_cells.size())
        {
            const float legacy = m_voxel.m_legacySize < 0.05f ? 0.05f : m_voxel.m_legacySize;
            m_voxel.m_sizes.assign(m_voxel.m_cells.size(), legacy);
        }
        m_voxel.m_legacyMerged.clear(); // legacy per-cube "merged" flags - the feature is removed
    }

    bool EditorWhiteBoxComponent::CarveCubeGrids(const WhiteBoxMesh& cutter, const AZ::Transform& cutterTransform)
    {
        // Subtract the draw-shape cutter from the cube grid so a carve cuts through stamped
        // cubes too. The freeform mesh is handled separately by the draw mode. The caller
        // serializes and rebuilds afterwards.
        // @note The carve edits the grid MESH, not the voxel occupancy, so a later stamp of the
        // same cube size (which regenerates the surface from occupancy) restores carved cubes.
        WhiteBoxMesh* grid = m_gridMesh.get();
        return grid != nullptr && !Api::MeshFaceHandles(*grid).empty() &&
            Api::ApplyMeshBoolean(*grid, cutter, cutterTransform, Api::BooleanOperation::Subtraction);
    }

    void EditorWhiteBoxComponent::MapMeshToActiveLayerSpace(WhiteBoxMesh& mesh, const size_t firstVertexIndex)
    {
        if (m_activeLayerIndex < 0 || m_activeLayerIndex >= GetLayerCount())
        {
            return;
        }
        const WhiteBoxLayer& layer = m_layers[m_activeLayerIndex];
        const bool identity = layer.m_position.IsZero() && layer.m_rotation.IsZero() &&
            layer.m_scale.IsClose(AZ::Vector3::CreateOne(), 1e-6f);
        if (identity)
        {
            return;
        }

        // Inverse of ApplyTransformToMesh (scale -> rotate -> translate): untranslate,
        // unrotate, unscale. Callers recalculate normals/UVs afterwards.
        const AZ::Quaternion inverseRotation =
            AZ::Quaternion::CreateFromEulerAnglesDegrees(layer.m_rotation).GetInverseFull();
        const AZ::Vector3 safeScale(
            AZStd::max(layer.m_scale.GetX(), 0.001f), AZStd::max(layer.m_scale.GetY(), 0.001f),
            AZStd::max(layer.m_scale.GetZ(), 0.001f));
        for (const Api::VertexHandle vertexHandle : Api::MeshVertexHandles(mesh))
        {
            if (static_cast<size_t>(vertexHandle.Index()) < firstVertexIndex)
            {
                continue;
            }
            AZ::Vector3 p = Api::VertexPosition(mesh, vertexHandle);
            p -= layer.m_position;
            p = inverseRotation.TransformVector(p);
            p /= safeScale;
            Api::SetVertexPosition(mesh, vertexHandle, p);
        }
    }

    void EditorWhiteBoxComponent::SetVoxelCell(const AZ::Vector3& cellMin, const bool filled)
    {
        SetVoxelCells(AZStd::vector<AZ::Vector3>{cellMin}, filled);
    }

    void EditorWhiteBoxComponent::SetVoxelCells(const AZStd::vector<AZ::Vector3>& cellMins, const bool filled)
    {
        if (cellMins.empty())
        {
            return;
        }

        NormalizeVoxelData();

        WhiteBoxMesh* freeform = GetWhiteBoxMesh();
        if (freeform == nullptr)
        {
            return;
        }

        const AZ::s32 sizeKey = VoxelDetail::QuantizeSize(m_drawShapeData.m_unitCubeSize);
        const float cellSize = VoxelDetail::SizeFromQuantized(sizeKey);

        // The grid is only a SNAP GUIDE: mesh this one gesture's cells as a single greedy-merged
        // solid (a 2x2x2 drag is ONE box - merged faces, corner vertices only - not 8 cubes) ...
        VoxelDetail::CellSet gestureCells;
        for (const AZ::Vector3& cellMin : cellMins)
        {
            gestureCells.insert(VoxelDetail::PackCell(
                static_cast<int>(std::floor(cellMin.GetX())), static_cast<int>(std::floor(cellMin.GetY())),
                static_cast<int>(std::floor(cellMin.GetZ()))));
        }
        Api::WhiteBoxMeshPtr solid = Api::CreateWhiteBoxMesh();
        VoxelDetail::GenerateSurface(*solid, gestureCells, cellSize);
        if (Api::MeshFaceHandles(*solid).empty())
        {
            return;
        }
        // The gesture is expressed in entity-local (viewport) space; the layer may display
        // transformed, so map the solid into layer storage space or the stamp lands shifted.
        MapMeshToActiveLayerSpace(*solid, 0);
        Api::CalculateNormals(*solid);
        Api::CalculatePlanarUVs(*solid);

        // ... then commit it to the freeform mesh with ONE boolean for the whole gesture. From
        // here on the stamped geometry IS ordinary mesh: editable with every tool, never
        // regenerated, and free on the rebuild/live-boolean hot path (no grid mesh exists).
        AzToolsFramework::ScopedUndoBatch undoBatch(filled ? "Stamp Cubes" : "Carve Cubes");
        const AZ::Transform identity = AZ::Transform::CreateIdentity();

        // Cell records are INERT bookkeeping (nothing on the hot path reads them): they exist so
        // "Clear Cube Stamp" can subtract the stamped volume later.
        VoxelDetail::SizeGroups records = VoxelDetail::GroupBySize(m_voxel.m_cells, m_voxel.m_sizes);
        VoxelDetail::CellSet& sizeRecords = records[sizeKey];

        if (filled)
        {
            // Union the gesture solid into the freeform; when they don't intersect (or the
            // freeform is empty / non-manifold) fall back to appending it as its own island.
            if (Api::MeshFaceHandles(*freeform).empty() ||
                !Api::ApplyMeshBoolean(*freeform, *solid, identity, Api::BooleanOperation::Union))
            {
                AppendMesh(*freeform, *solid);
            }
            for (const AZ::u64 key : gestureCells)
            {
                sizeRecords.insert(key);
            }
        }
        else
        {
            // Carve: subtract the gesture volume. This cuts stamped cubes AND any freeform
            // geometry inside the cells - by design (the cells mark a volume, not "cubes").
            if (!Api::MeshFaceHandles(*freeform).empty() &&
                !Api::ApplyMeshBoolean(*freeform, *solid, identity, Api::BooleanOperation::Subtraction))
            {
                // Manifold reports an EMPTY result (the cut removes everything) as failure and
                // leaves the mesh untouched - e.g. carving the cell of the only cube. Detect
                // that case and empty the mesh for real.
                AZStd::vector<AZ::u64> gestureList(gestureCells.begin(), gestureCells.end());
                const AZStd::vector<float> gestureSizes(gestureList.size(), cellSize);
                if (MeshFullyInsideCells(*freeform, gestureList, gestureSizes))
                {
                    ClearMeshInPlace(*freeform);
                }
            }
            for (const AZ::u64 key : gestureCells)
            {
                sizeRecords.erase(key);
            }
        }

        Api::CalculateNormals(*freeform);
        Api::CalculatePlanarUVs(*freeform);
        VoxelDetail::FlattenGroups(records, m_voxel.m_cells, m_voxel.m_sizes);
        m_voxel.m_legacyMerged.clear(); // legacy flags, feature removed

        SerializeWhiteBox();
        RebuildWhiteBox();
        // The freeform mesh changed under the active component mode: refresh the intersection
        // data only (a FULL mode refresh would destroy the DrawShapeMode instance that is
        // mid-mouse-interaction right now).
        EditorWhiteBoxComponentModeRequestBus::Event(
            AZ::EntityComponentIdPair(GetEntityId(), GetId()),
            &EditorWhiteBoxComponentModeRequests::MarkWhiteBoxIntersectionDataDirty);
        undoBatch.MarkEntityDirty(GetEntityId());
    }

    AZ::Crc32 EditorWhiteBoxComponent::ClearVoxelCubes()
    {
        NormalizeVoxelData();

        if (m_voxel.m_cells.empty())
        {
            return AZ::Edit::PropertyRefreshLevels::None;
        }

        // Stamped cubes were CSG-unioned into the freeform mesh, so "clearing" them means
        // subtracting the recorded stamped volume back out. This cuts whatever now occupies
        // those cells (including freeform geometry that was drawn into them - by design).
        // Expensive (one big boolean) but rare and explicitly user-invoked.
        AzToolsFramework::ScopedUndoBatch undoBatch("Clear Cube Stamp");

        if (WhiteBoxMesh* freeform = GetWhiteBoxMesh(); freeform != nullptr && !Api::MeshFaceHandles(*freeform).empty())
        {
            // Build the cutter PER CELL, not greedy-merged: greedy merging can leave
            // T-junctions on a non-convex cell cluster, which the CSG library rejects as a
            // non-manifold operand ("mesh B is not a valid manifold"). Per-cell cubes are the
            // proven-valid boolean operand (this is what the old stamp path used).
            Api::WhiteBoxMeshPtr cutter = Api::CreateWhiteBoxMesh();
            const VoxelDetail::SizeGroups groups = VoxelDetail::GroupBySize(m_voxel.m_cells, m_voxel.m_sizes);
            for (const auto& group : groups)
            {
                VoxelDetail::GenerateSurfacePerCell(*cutter, group.second, VoxelDetail::SizeFromQuantized(group.first));
            }
            if (!Api::MeshFaceHandles(*cutter).empty())
            {
                // Records are entity-local; the layer mesh is stored in layer space.
                MapMeshToActiveLayerSpace(*cutter, 0);
                Api::CalculateNormals(*cutter);
                Api::CalculatePlanarUVs(*cutter);
                if (Api::ApplyMeshBoolean(
                        *freeform, *cutter, AZ::Transform::CreateIdentity(), Api::BooleanOperation::Subtraction))
                {
                    Api::CalculateNormals(*freeform);
                    Api::CalculatePlanarUVs(*freeform);
                }
                else if (MeshFullyInsideCells(*freeform, m_voxel.m_cells, m_voxel.m_sizes))
                {
                    // The subtract would remove EVERYTHING (e.g. the mesh is nothing but the
                    // stamped cubes) - Manifold reports the empty result as failure, so empty
                    // the mesh explicitly.
                    ClearMeshInPlace(*freeform);
                }
            }
        }

        m_voxel.m_cells.clear();
        m_voxel.m_sizes.clear();
        m_voxel.m_legacyMerged.clear();
        m_voxel.m_legacyGridMergedData.clear();
        SerializeWhiteBox();
        RebuildWhiteBox();
        RefreshComponentMode(); // full refresh is safe here (button press, not mid-interaction)
        undoBatch.MarkEntityDirty(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::None;
    }

    AZ::Crc32 EditorWhiteBoxComponent::FixNonManifoldMesh()
    {
        WhiteBoxMesh* mesh = GetWhiteBoxMesh();
        if (mesh == nullptr)
        {
            return AZ::Edit::PropertyRefreshLevels::None;
        }

        // Weld coincident vertices and regroup coplanar faces so the whole mesh becomes a
        // clean manifold. A single non-manifold region otherwise blocks every boolean.
        AzToolsFramework::ScopedUndoBatch undoBatch("Fix Non-Manifold Mesh");
        if (Api::RepairMesh(*mesh))
        {
            SerializeWhiteBox();
            RebuildWhiteBox();
            undoBatch.MarkEntityDirty(GetEntityId());
        }
        return AZ::Edit::PropertyRefreshLevels::None;
    }

    bool EditorWhiteBoxComponent::BuildColliderMesh(AZStd::vector<AZ::Vector3>& vertices, AZStd::vector<AZ::u32>& indices)
    {
        vertices.clear();
        indices.clear();

        // Stamped cubes are CSG-unioned into the freeform mesh at stamp time, so the collider's
        // normal per-face path already covers them exactly. The voxel records that remain are
        // inert bookkeeping for "Clear Cube Stamp" - they describe stamped VOLUME, not live
        // geometry (a cube may since have been carved or edited), so building collision from
        // them would produce ghost surfaces. Always defer to the per-face path.
        return false;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawUnitCubeSizeVisibility() const
    {
        // The Cube Size control only matters while the Unit Cube Stamp tool is active.
        return m_drawShapeData.m_unitCube ? AZ::Edit::PropertyVisibility::Show : AZ::Edit::PropertyVisibility::Hide;
    }
} // namespace WhiteBox
