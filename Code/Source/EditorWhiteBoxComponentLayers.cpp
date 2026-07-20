/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * EditorWhiteBoxComponent - LAYER SYSTEM translation unit: layer storage/loading, the
 * combine pipeline (per-layer cache + CSG combine modes), layer operations (add / delete /
 * switch / meta edits / apply transform) and parametric shape layers.
 */

#include "EditorWhiteBoxComponent.h"

#include "Util/WhiteBoxMeshUtil.h"
#include "Util/WhiteBoxVoxelUtil.h"
#include "Viewport/WhiteBoxShapeBuilders.h"

#include <AzCore/Math/Quaternion.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/ComponentMode/ComponentModeDelegate.h>

namespace WhiteBox
{
    void EditorWhiteBoxComponent::WhiteBoxLayer::Reflect(AZ::ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<WhiteBoxLayer>()
                ->Version(2)
                ->Field("Name", &WhiteBoxLayer::m_name)
                ->Field("Id", &WhiteBoxLayer::m_id)
                ->Field("Visible", &WhiteBoxLayer::m_visible)
                ->Field("Collision", &WhiteBoxLayer::m_collision)
                ->Field("Parametric", &WhiteBoxLayer::m_parametric)
                ->Field("ParamShape", &WhiteBoxLayer::m_paramShape)
                ->Field("ParamWidth", &WhiteBoxLayer::m_paramWidth)
                ->Field("ParamDepth", &WhiteBoxLayer::m_paramDepth)
                ->Field("ParamHeight", &WhiteBoxLayer::m_paramHeight)
                ->Field("ParamSides", &WhiteBoxLayer::m_paramSides)
                ->Field("ParamSteps", &WhiteBoxLayer::m_paramSteps)
                ->Field("ParamWallThickness", &WhiteBoxLayer::m_paramWallThickness)
                ->Field("ParamCavityGap", &WhiteBoxLayer::m_paramCavityGap)
                ->Field("ParamFloor", &WhiteBoxLayer::m_paramFloor)
                ->Field("ParamCeiling", &WhiteBoxLayer::m_paramCeiling)
                ->Field("Tint", &WhiteBoxLayer::m_tint)
                ->Field("Combine", &WhiteBoxLayer::m_combineMode)
                ->Field("InvertNormals", &WhiteBoxLayer::m_invertNormals)
                ->Field("EdgesOnly", &WhiteBoxLayer::m_edgesOnly)
                ->Field("Position", &WhiteBoxLayer::m_position)
                ->Field("Rotation", &WhiteBoxLayer::m_rotation)
                ->Field("Scale", &WhiteBoxLayer::m_scale)
                ->Field("Freeform", &WhiteBoxLayer::m_freeformData)
                ->Field("Grid", &WhiteBoxLayer::m_gridData)
                ->Field("GridMerged", &WhiteBoxLayer::m_gridMergedData)
                ->Field("VoxelCells", &WhiteBoxLayer::m_voxelCells)
                ->Field("VoxelCellSizes", &WhiteBoxLayer::m_voxelCellSizes)
                ->Field("VoxelMerged", &WhiteBoxLayer::m_voxelMerged);

            if (AZ::EditContext* editContext = serializeContext->GetEditContext())
            {
                editContext->Class<WhiteBoxLayer>("White Box Layer", "One editable White Box layer.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->DataElement(AZ::Edit::UIHandlers::Default, &WhiteBoxLayer::m_name, "Name", "Layer name.")
                    ->DataElement(
                        AZ::Edit::UIHandlers::CheckBox, &WhiteBoxLayer::m_visible, "Visible",
                        "Show or hide this layer (hidden layers are excluded from the combined output).")
                    ->DataElement(
                        AZ::Edit::UIHandlers::CheckBox, &WhiteBoxLayer::m_collision, "Collision",
                        "Include or exclude this layer from the physics collision mesh.")
                    ->DataElement(
                        AZ::Edit::UIHandlers::Color, &WhiteBoxLayer::m_tint, "Tint",
                        "Render colour for this layer (used when 'Use Global Tint' is off).")
                    ->DataElement(
                        AZ::Edit::UIHandlers::ComboBox, &WhiteBoxLayer::m_combineMode, "Combine",
                        "How this layer combines with the layers below it: Separate keeps it as its own island; "
                        "Union fuses it; Subtract carves it out; Intersect keeps only the overlap.")
                    ->EnumAttribute(LayerCombineMode::Separate, "Separate")
                    ->EnumAttribute(LayerCombineMode::Union, "Union")
                    ->EnumAttribute(LayerCombineMode::Subtract, "Subtract")
                    ->EnumAttribute(LayerCombineMode::Intersect, "Intersect")
                    ->DataElement(
                        AZ::Edit::UIHandlers::CheckBox, &WhiteBoxLayer::m_invertNormals, "Invert Normals",
                        "Render this layer inside-out (flips its normals / winding). Non-destructive and reversible; "
                        "applies to existing and new geometry in the layer.")
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &WhiteBoxLayer::m_position, "Position",
                        "Translate this layer's geometry (applied non-destructively at combine time).")
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &WhiteBoxLayer::m_rotation, "Rotation",
                        "Rotate this layer's geometry, Euler degrees (XYZ).")
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &WhiteBoxLayer::m_scale, "Scale",
                        "Scale this layer's geometry (non-uniform).")
                    ->Attribute(AZ::Edit::Attributes::Min, 0.001f);
            }
        }
    }

    void EditorWhiteBoxComponent::StoreLayer(const int index)
    {
        if (index < 0 || index >= static_cast<int>(m_layers.size()))
        {
            return;
        }
        // The working streams are kept current by SerializeWhiteBox, so copy them (plus the
        // occupancy arrays) into the layer.
        WhiteBoxLayer& layer = m_layers[index];
        layer.m_freeformData = m_whiteBoxData;
        layer.m_gridData = m_voxel.m_legacyGridData;
        layer.m_gridMergedData.clear(); // legacy merged-grid stream, feature removed
        layer.m_voxelCells = m_voxel.m_cells;
        layer.m_voxelCellSizes = m_voxel.m_sizes;
        layer.m_voxelMerged.clear(); // legacy flags, feature removed
        m_layerRuntime.m_meshCache.erase(layer.m_id); // the stored data changed -> cached display mesh is stale
    }

    void EditorWhiteBoxComponent::LoadActiveLayer()
    {
        if (m_layers.empty())
        {
            return;
        }
        if (m_activeLayerIndex < 0)
        {
            m_activeLayerIndex = 0;
        }
        if (m_activeLayerIndex >= static_cast<int>(m_layers.size()))
        {
            m_activeLayerIndex = static_cast<int>(m_layers.size()) - 1;
        }
        const WhiteBoxLayer& layer = m_layers[m_activeLayerIndex];
        m_whiteBoxData = layer.m_freeformData;
        m_voxel.m_legacyGridData = layer.m_gridData;
        m_voxel.m_cells = layer.m_voxelCells;
        m_voxel.m_sizes = layer.m_voxelCellSizes;
        m_voxel.m_legacyMerged.clear(); // legacy flags, feature removed

        m_whiteBox = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*m_whiteBox, m_whiteBoxData); // new/empty layers stay empty (no default cube)
        m_gridMesh = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*m_gridMesh, m_voxel.m_legacyGridData);
        m_layerRuntime.m_loadedIndex = m_activeLayerIndex;
        m_layerRuntime.m_loadedId = m_layers[m_activeLayerIndex].m_id;
        m_layerRuntime.m_lastCount = static_cast<int>(m_layers.size());
    }

    void EditorWhiteBoxComponent::ApplyTransformToMesh(
        WhiteBoxMesh& mesh, const AZ::Vector3& position, const AZ::Vector3& eulerDegrees, const AZ::Vector3& scale)
    {
        const bool identity = position.IsZero() && eulerDegrees.IsZero() &&
            scale.IsClose(AZ::Vector3::CreateOne(), 1e-6f);
        if (identity)
        {
            return; // nothing to do - leave the mesh (and its UVs/normals) untouched
        }

        const AZ::Quaternion rotation = AZ::Quaternion::CreateFromEulerAnglesDegrees(eulerDegrees);
        for (const Api::VertexHandle vertexHandle : Api::MeshVertexHandles(mesh))
        {
            AZ::Vector3 p = Api::VertexPosition(mesh, vertexHandle);
            p *= scale;                       // component-wise (non-uniform) scale
            p = rotation.TransformVector(p);  // rotate
            p += position;                    // translate
            Api::SetVertexPosition(mesh, vertexHandle, p);
        }
        Api::CalculateNormals(mesh);
        Api::CalculatePlanarUVs(mesh);
    }

    Api::WhiteBoxMeshPtr EditorWhiteBoxComponent::BuildLayerMesh(const WhiteBoxLayer& layer)
    {
        // Deserialize a stored layer and combine its geometry into one display mesh.
        Api::WhiteBoxMeshPtr freeform = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*freeform, layer.m_freeformData);
        Api::WhiteBoxMeshPtr grid = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*grid, layer.m_gridData);
        if (!layer.m_gridMergedData.empty())
        {
            // Legacy "Merge With Mesh" cubes (feature removed): fold them into the grid.
            Api::WhiteBoxMeshPtr gridMerged = Api::CreateWhiteBoxMesh();
            Api::ReadMesh(*gridMerged, layer.m_gridMergedData);
            if (!Api::MeshFaceHandles(*gridMerged).empty())
            {
                AppendMesh(*grid, *gridMerged);
            }
        }
        Api::WhiteBoxMeshPtr combined = CombineFreeformAndGrids(freeform.get(), grid.get());
        if (combined)
        {
            ApplyTransformToMesh(*combined, layer.m_position, layer.m_rotation, layer.m_scale);
            if (layer.m_invertNormals)
            {
                // Flip the ACTUAL winding (not just the render faces) so rendering, physics
                // cooking and selection all agree - an inverted "room" collides from inside.
                combined = FlippedMeshWinding(*combined);
            }
        }
        return combined;
    }

    AZStd::vector<AZStd::pair<int, AZStd::string>> EditorWhiteBoxComponent::GetLayerNames()
    {
        AZStd::vector<AZStd::pair<int, AZStd::string>> names;
        if (m_layers.empty())
        {
            // Never return an empty value list: an empty combo would select index -1 and log
            // "Out of range combo box index -1". A single placeholder keeps value 0 valid until a
            // layer is created (drawing into the empty component auto-creates "Layer 1").
            names.emplace_back(0, AZStd::string("(no layers)"));
            return names;
        }
        names.reserve(m_layers.size());
        for (int i = 0; i < static_cast<int>(m_layers.size()); ++i)
        {
            const AZStd::string& name = m_layers[i].m_name;
            names.emplace_back(i, AZStd::string::format("%d: %s", i, name.empty() ? "Layer" : name.c_str()));
        }
        return names;
    }

    AZ::u32 EditorWhiteBoxComponent::OnActiveLayerChange()
    {
        // With no layers there is nothing to switch to; force a tree rebuild so a stale dropdown
        // (still listing deleted layers) is replaced by the "(no layers)" placeholder.
        if (m_layers.empty())
        {
            m_activeLayerIndex = 0;
            return AZ::Edit::PropertyRefreshLevels::EntireTree;
        }
        const int incoming = m_activeLayerIndex;
        if (m_activeLayerIndex < 0)
        {
            m_activeLayerIndex = 0;
        }
        if (m_activeLayerIndex >= static_cast<int>(m_layers.size()))
        {
            m_activeLayerIndex = static_cast<int>(m_layers.size()) - 1;
        }
        // The combo box can briefly hold entries for layers that were just deleted. If the picked
        // index was out of range we clamped it, so rebuild the tree to resync the dropdown.
        const bool wasStale = m_activeLayerIndex != incoming;
        const AZ::u32 refresh = wasStale ? AZ::Edit::PropertyRefreshLevels::EntireTree
                                         : AZ::Edit::PropertyRefreshLevels::ValuesOnly;
        if (m_activeLayerIndex == m_layerRuntime.m_loadedIndex)
        {
            return refresh;
        }
        AzToolsFramework::ScopedUndoBatch undoBatch("Switch White Box Layer");
        SerializeWhiteBox(); // commit the currently loaded layer into m_layers
        LoadActiveLayer();   // load the newly selected layer into the working members
        RebuildWhiteBox();
        RefreshComponentMode();
        undoBatch.MarkEntityDirty(GetEntityId());
        return refresh;
    }

    int EditorWhiteBoxComponent::AddParametricShapeLayer(const DrawShapeType shape)
    {
        AzToolsFramework::ScopedUndoBatch undoBatch("Add Parametric Shape");
        SerializeWhiteBox(); // commit the current layer

        WhiteBoxLayer layer;
        layer.m_name =
            AZStd::string::format("%s %d", Detail::DrawShapeName(shape), static_cast<int>(m_layers.size()) + 1);
        layer.m_id = AllocLayerId();
        layer.m_parametric = true;
        layer.m_paramShape = shape;
        // Shape-appropriate defaults (mirrors the Draw Shape side-count defaults).
        switch (shape)
        {
        case DrawShapeType::Cylinder:
        case DrawShapeType::Cone:
            layer.m_paramSides = 24;
            break;
        case DrawShapeType::Sphere:
            layer.m_paramSides = 16;
            break;
        case DrawShapeType::Room:
            // A 1x1x1 interior is uselessly small for a room; start with a walkable box.
            layer.m_paramSides = 4;
            layer.m_paramWidth = 4.0f;
            layer.m_paramDepth = 4.0f;
            layer.m_paramHeight = 3.0f;
            break;
        default:
            layer.m_paramSides = 4;
            break;
        }

        const Api::WhiteBoxMeshPtr mesh = BuildParametricShapeMesh(
            layer.m_paramShape, layer.m_paramWidth, layer.m_paramDepth, layer.m_paramHeight, layer.m_paramSides,
            layer.m_paramSteps, layer.m_paramWallThickness, layer.m_paramCavityGap, layer.m_paramFloor,
            layer.m_paramCeiling);
        Api::WriteMesh(*mesh, layer.m_freeformData);

        m_layers.push_back(AZStd::move(layer));
        m_activeLayerIndex = static_cast<int>(m_layers.size()) - 1;
        m_layerRuntime.m_lastCount = static_cast<int>(m_layers.size());
        LoadActiveLayer(); // make the new shape the edit target
        m_layerRuntime.m_lastSignature = LayerSignature();
        RebuildWhiteBox();
        RefreshComponentMode();
        undoBatch.MarkEntityDirty(GetEntityId());
        return m_activeLayerIndex;
    }

    EditorWhiteBoxComponent::ShapeParams EditorWhiteBoxComponent::GetLayerShapeParams(const int index) const
    {
        ShapeParams params;
        if (index >= 0 && index < GetLayerCount())
        {
            const WhiteBoxLayer& layer = m_layers[index];
            params.m_shape = layer.m_paramShape;
            params.m_width = layer.m_paramWidth;
            params.m_depth = layer.m_paramDepth;
            params.m_height = layer.m_paramHeight;
            params.m_sides = layer.m_paramSides;
            params.m_steps = layer.m_paramSteps;
            params.m_wallThickness = layer.m_paramWallThickness;
            params.m_cavityGap = layer.m_paramCavityGap;
            params.m_floor = layer.m_paramFloor;
            params.m_ceiling = layer.m_paramCeiling;
        }
        return params;
    }

    void EditorWhiteBoxComponent::SetLayerShapeParams(const int index, const ShapeParams& params)
    {
        if (!IsLayerParametric(index))
        {
            return;
        }
        WhiteBoxLayer& layer = m_layers[index];
        layer.m_paramShape = params.m_shape;
        layer.m_paramWidth = AZStd::max(params.m_width, 0.01f);
        layer.m_paramDepth = AZStd::max(params.m_depth, 0.01f);
        layer.m_paramHeight = AZStd::max(params.m_height, 0.01f);
        layer.m_paramSides = AZStd::clamp(params.m_sides, 3, 128);
        layer.m_paramSteps = AZStd::clamp(params.m_steps, 1, 128);
        layer.m_paramWallThickness = AZStd::max(params.m_wallThickness, 0.001f);
        layer.m_paramCavityGap = AZStd::max(params.m_cavityGap, 0.0f);
        layer.m_paramFloor = params.m_floor;
        layer.m_paramCeiling = params.m_ceiling;
        RegenerateParametricLayer(index);
    }

    void EditorWhiteBoxComponent::BakeParametricLayer(const int index)
    {
        if (IsLayerParametric(index))
        {
            // The generated mesh stays exactly as-is; the parameters just stop driving it,
            // which makes vertex-level edits safe (nothing will regenerate over them).
            m_layers[index].m_parametric = false;
        }
    }

    void EditorWhiteBoxComponent::RegenerateParametricLayer(const int index)
    {
        if (!IsLayerParametric(index))
        {
            return;
        }
        WhiteBoxLayer& layer = m_layers[index];
        const ShapeParams params = GetLayerShapeParams(index);
        Api::WhiteBoxMeshPtr mesh = BuildParametricShapeMesh(
            params.m_shape, params.m_width, params.m_depth, params.m_height, params.m_sides, params.m_steps,
            params.m_wallThickness, params.m_cavityGap, params.m_floor, params.m_ceiling);
        if (index == m_layerRuntime.m_loadedIndex)
        {
            // The parametric layer is the ACTIVE one: replace the working mesh so viewport
            // display/selection track the new shape, then flush it into the layer's stream.
            m_whiteBox = AZStd::move(mesh);
            SerializeWhiteBox();
        }
        else
        {
            Api::WriteMesh(*mesh, layer.m_freeformData);
        }
        m_layerRuntime.m_meshCache.erase(layer.m_id); // the cached display mesh is stale now
        RebuildWhiteBox();
        RefreshComponentMode();
    }

    AZ::Crc32 EditorWhiteBoxComponent::OnNewLayer()
    {
        AzToolsFramework::ScopedUndoBatch undoBatch("New White Box Layer");
        SerializeWhiteBox(); // commit the current layer
        WhiteBoxLayer layer;
        layer.m_name = AZStd::string::format("Layer %d", static_cast<int>(m_layers.size()) + 1);
        layer.m_id = AllocLayerId();
        m_layers.push_back(AZStd::move(layer));
        m_activeLayerIndex = static_cast<int>(m_layers.size()) - 1;
        m_layerRuntime.m_lastCount = static_cast<int>(m_layers.size());
        LoadActiveLayer(); // load the new empty layer
        m_layerRuntime.m_lastSignature = LayerSignature();
        RebuildWhiteBox();
        RefreshComponentMode();
        undoBatch.MarkEntityDirty(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::EntireTree;
    }

    AZ::Crc32 EditorWhiteBoxComponent::OnDeleteLayer()
    {
        if (m_layers.empty() || m_activeLayerIndex < 0 || m_activeLayerIndex >= static_cast<int>(m_layers.size()))
        {
            return AZ::Edit::PropertyRefreshLevels::None;
        }
        AzToolsFramework::ScopedUndoBatch undoBatch("Delete White Box Layer");
        m_layers.erase(m_layers.begin() + m_activeLayerIndex);
        m_layerRuntime.m_lastCount = static_cast<int>(m_layers.size());
        if (m_layers.empty())
        {
            // Deleting the last layer leaves an empty white box (nothing renders, stamps cleared).
            m_activeLayerIndex = 0;
            ClearWorkingLayer();
        }
        else
        {
            if (m_activeLayerIndex >= static_cast<int>(m_layers.size()))
            {
                m_activeLayerIndex = static_cast<int>(m_layers.size()) - 1;
            }
            m_layerRuntime.m_loadedIndex = -1; // force a reload of the working members
            LoadActiveLayer();
        }
        m_layerRuntime.m_lastSignature = LayerSignature();
        RebuildWhiteBox();
        RefreshComponentMode();
        undoBatch.MarkEntityDirty(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::EntireTree;
    }

    AZ::u32 EditorWhiteBoxComponent::OnLayersMetaChanged()
    {
        // If the id order/count changed, the list was added-to, removed-from or reordered (some
        // O3DE versions deliver these via this ChangeNotify) - do the full structural resync.
        if (LayerSignature() != m_layerRuntime.m_lastSignature)
        {
            SyncLayerStructure();
            return AZ::Edit::PropertyRefreshLevels::EntireTree;
        }

        // Otherwise this is an in-place edit (name / visibility / combine mode / transform). Keep
        // the working index pointing at the loaded layer and recombine to reflect the change.
        const int k = IndexOfLayerId(m_layerRuntime.m_loadedId);
        if (k >= 0)
        {
            m_layerRuntime.m_loadedIndex = k;
        }
        RebuildWhiteBox();
        return AZ::Edit::PropertyRefreshLevels::ValuesOnly;
    }

    void EditorWhiteBoxComponent::RefreshComponentMode()
    {
        // A layer swap replaces the working mesh, so any state cached by the active sub-mode
        // holds stale handles into the OLD mesh - e.g. Transform mode's vertex selection, which
        // crashes in Display() if it survives the swap. Issue a FULL component-mode refresh
        // (EditorWhiteBoxComponentMode::Refresh resets every sub-mode and rebuilds the
        // intersection data) instead of only marking the intersection data dirty. No-op when
        // the component is not in component mode. This matters now that the White Box pane can
        // add/delete/switch layers WHILE component mode is active.
        AzToolsFramework::ComponentModeFramework::ComponentModeSystemRequestBus::Broadcast(
            &AzToolsFramework::ComponentModeFramework::ComponentModeSystemRequests::Refresh,
            AZ::EntityComponentIdPair(GetEntityId(), GetId()));
    }

    void EditorWhiteBoxComponent::ClearWorkingLayer()
    {
        // Reset the working members to empty geometry. Used when there are zero layers so the
        // white box shows nothing and the stamp occupancy is cleared.
        m_whiteBox = Api::CreateWhiteBoxMesh();
        m_gridMesh = Api::CreateWhiteBoxMesh();
        m_voxel.m_cells.clear();
        m_voxel.m_sizes.clear();
        m_voxel.m_legacyMerged.clear();
        Api::WriteMesh(*m_whiteBox, m_whiteBoxData);
        Api::WriteMesh(*m_gridMesh, m_voxel.m_legacyGridData);
        m_voxel.m_legacyGridMergedData.clear();
        m_layerRuntime.m_loadedIndex = -1;
        m_layerRuntime.m_loadedId = 0;
    }

    AZ::Crc32 EditorWhiteBoxComponent::OnApplyLayerTransform()
    {
        if (m_layers.empty() || m_activeLayerIndex < 0 || m_activeLayerIndex >= static_cast<int>(m_layers.size()))
        {
            return AZ::Edit::PropertyRefreshLevels::None;
        }
        WhiteBoxLayer& layer = m_layers[m_activeLayerIndex];
        const bool identity = layer.m_position.IsZero() && layer.m_rotation.IsZero() &&
            layer.m_scale.IsClose(AZ::Vector3::CreateOne(), 1e-6f);
        if (identity)
        {
            return AZ::Edit::PropertyRefreshLevels::None; // nothing to bake
        }

        AzToolsFramework::ScopedUndoBatch undoBatch("Apply White Box Layer Transform");

        const bool isActive = m_activeLayerIndex == m_layerRuntime.m_loadedIndex;
        if (isActive)
        {
            SerializeWhiteBox(); // flush the working members into this layer's streams first
        }

        // Fold the grids into the freeform, bake the transform into every vertex, then clear the
        // grids + voxel occupancy (rotated/scaled cubes cannot map back to axis-aligned voxels, so
        // the stamps are frozen into the mesh) and reset the transform to identity.
        Api::WhiteBoxMeshPtr freeform = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*freeform, layer.m_freeformData);
        Api::WhiteBoxMeshPtr grid = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*grid, layer.m_gridData);
        Api::WhiteBoxMeshPtr gridMerged = Api::CreateWhiteBoxMesh();
        Api::ReadMesh(*gridMerged, layer.m_gridMergedData);
        if (!Api::MeshFaceHandles(*grid).empty())
        {
            AppendMesh(*freeform, *grid);
        }
        if (!Api::MeshFaceHandles(*gridMerged).empty())
        {
            AppendMesh(*freeform, *gridMerged);
        }
        ApplyTransformToMesh(*freeform, layer.m_position, layer.m_rotation, layer.m_scale);

        Api::WriteMesh(*freeform, layer.m_freeformData);
        layer.m_gridData.clear();
        layer.m_gridMergedData.clear();
        layer.m_voxelCells.clear();
        layer.m_voxelCellSizes.clear();
        layer.m_voxelMerged.clear();
        layer.m_position = AZ::Vector3::CreateZero();
        layer.m_rotation = AZ::Vector3::CreateZero();
        layer.m_scale = AZ::Vector3::CreateOne();

        if (isActive)
        {
            m_layerRuntime.m_loadedIndex = -1; // force the working members to reload from the baked streams
            LoadActiveLayer();
        }
        RebuildWhiteBox();
        RefreshComponentMode();
        undoBatch.MarkEntityDirty(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::EntireTree; // reset the Position/Rotation/Scale fields
    }

    AZ::u64 EditorWhiteBoxComponent::AllocLayerId()
    {
        if (m_nextLayerId == 0)
        {
            m_nextLayerId = 1;
        }
        return m_nextLayerId++;
    }

    int EditorWhiteBoxComponent::IndexOfLayerId(const AZ::u64 id) const
    {
        if (id == 0)
        {
            return -1;
        }
        for (int i = 0; i < static_cast<int>(m_layers.size()); ++i)
        {
            if (m_layers[i].m_id == id)
            {
                return i;
            }
        }
        return -1;
    }

    AZ::u64 EditorWhiteBoxComponent::LayerSignature() const
    {
        AZ::u64 h = 1469598103934665603ull ^ static_cast<AZ::u64>(m_layers.size());
        for (const WhiteBoxLayer& layer : m_layers)
        {
            h ^= layer.m_id;
            h *= 1099511628211ull;
        }
        return h;
    }

    void EditorWhiteBoxComponent::SyncLayerStructure()
    {
        // Give any natively-added ("+") layers a stable id first so ids are unique.
        for (WhiteBoxLayer& layer : m_layers)
        {
            if (layer.m_id == 0)
            {
                layer.m_id = AllocLayerId();
            }
        }

        const int count = static_cast<int>(m_layers.size());
        if (count == 0)
        {
            m_activeLayerIndex = 0;
            ClearWorkingLayer();
        }
        else
        {
            const int k = IndexOfLayerId(m_layerRuntime.m_loadedId);
            if (k >= 0)
            {
                // The working members still belong to a live layer: the list was reordered or
                // added to. Just reindex (so the render skips the correct slot) and keep the
                // selection on the same layer - no reload, so no in-progress edits are lost.
                m_layerRuntime.m_loadedIndex = k;
                m_activeLayerIndex = k;
            }
            else
            {
                // The loaded layer was removed: load whatever the active index now points at.
                if (m_activeLayerIndex < 0)
                {
                    m_activeLayerIndex = 0;
                }
                if (m_activeLayerIndex >= count)
                {
                    m_activeLayerIndex = count - 1;
                }
                m_layerRuntime.m_loadedIndex = -1;
                LoadActiveLayer();
            }
        }

        m_layerRuntime.m_lastCount = count;
        m_layerRuntime.m_lastSignature = LayerSignature();

        // Prune cached display meshes for layers that no longer exist (ids are never reused).
        for (auto it = m_layerRuntime.m_meshCache.begin(); it != m_layerRuntime.m_meshCache.end();)
        {
            it = (IndexOfLayerId(it->first) < 0) ? m_layerRuntime.m_meshCache.erase(it) : ++it;
        }

        RebuildWhiteBox();
        RefreshComponentMode();
        AzToolsFramework::ToolsApplicationEvents::Bus::Broadcast(
            &AzToolsFramework::ToolsApplicationEvents::InvalidatePropertyDisplay,
            AzToolsFramework::Refresh_EntireTree);
        // Tell layer UI (the White Box pane) the layer structure changed. This is the one choke
        // point every structural change funnels through - including the first layer auto-created
        // by drawing into an empty white box, which the OnTick signature poll routes here.
        EditorWhiteBoxComponentNotificationBus::Event(
            AZ::EntityComponentIdPair(GetEntityId(), GetId()),
            &EditorWhiteBoxComponentNotificationBus::Events::OnLayerStructureChanged);
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::EvaluatedMesh()
    {
        // When a stamp/grid layer exists it is combined with the freeform mesh (see
        // RebuildCombinedMesh) so render / collision / bounds / selection all see both.
        if (m_combinedMesh)
        {
            return m_combinedMesh.get();
        }
        // Non-destructive: render/collide/select against the evaluated result while
        // the editable base (GetWhiteBoxMesh) stays untouched.
        if (m_boolean.m_live && m_displayMesh)
        {
            return m_displayMesh.get();
        }
        return GetWhiteBoxMesh();
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::GetEvaluatedWhiteBoxMesh()
    {
        return EvaluatedMesh();
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::GridMesh()
    {
        if (!m_gridMesh)
        {
            m_gridMesh = Api::CreateWhiteBoxMesh();
        }
        return m_gridMesh.get();
    }

    Api::WhiteBoxMeshPtr EditorWhiteBoxComponent::CombinedWithGrid(WhiteBoxMesh* freeform)
    {
        const bool hasSeparate = m_gridMesh && !Api::MeshFaceHandles(*m_gridMesh).empty();
        if (freeform == nullptr || !hasSeparate)
        {
            return nullptr; // no grid layer -> caller uses the freeform mesh as-is
        }
        Api::WhiteBoxMeshPtr combined = Api::CloneMesh(*freeform);
        if (!combined)
        {
            return nullptr;
        }

        // Stamped cubes are appended alongside the freeform (their own islands) - plain append,
        // no CSG; the occupancy mesher already produced a clean watertight cube surface.
        AppendMesh(*combined, *m_gridMesh);

        Api::CalculateNormals(*combined);
        Api::CalculatePlanarUVs(*combined);
        return combined;
    }

    Api::WhiteBoxMeshPtr EditorWhiteBoxComponent::BuildCombined(WhiteBoxMesh* activeFreeform, bool physicsPass)
    {
        const int count = static_cast<int>(m_layers.size());
        const int activeIdx = m_layerRuntime.m_loadedIndex;

        AZStd::vector<int> visible;
        visible.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            if (m_layers[i].m_visible)
            {
                // Skip this layer if we are building the physics mesh and collision is off
                if (physicsPass && !m_layers[i].m_collision)
                {
                    continue;
                }
                visible.push_back(i);
            }
        }
        if (visible.empty())
        {
            return Api::CreateWhiteBoxMesh(); // nothing visible -> empty mesh (renders nothing)
        }

        const bool activeIdentity = activeIdx >= 0 && activeIdx < count &&
            m_layers[activeIdx].m_position.IsZero() && m_layers[activeIdx].m_rotation.IsZero() &&
            m_layers[activeIdx].m_scale.IsClose(AZ::Vector3::CreateOne(), 1e-6f);
        const bool activeNoGrid = !(m_gridMesh && !Api::MeshFaceHandles(*m_gridMesh).empty());
        const bool activeInverted = activeIdx >= 0 && activeIdx < count && m_layers[activeIdx].m_invertNormals;

        // Fast path: only the active layer is visible, identity transform, no grid, not inverted
        // -> return null so EvaluatedMesh falls back to the raw working freeform (no clone).
        // An inverted layer must NOT take this path: the raw working mesh has outward winding,
        // and the flip (render + physics + selection) only exists in the combined mesh.
        //
        // This must NOT run on the physics pass: there, `visible` can collapse to just the active
        // layer because OTHER visible layers were filtered out by their Collision flag. Returning
        // null then makes the caller fall back to the UNFILTERED evaluated mesh (which still
        // contains those non-collidable layers), so a per-layer Collision toggle would have no
        // effect. On the physics pass we always build the filtered mesh explicitly below.
        if (!physicsPass && visible.size() == 1 && visible[0] == activeIdx && activeIdentity && activeNoGrid &&
            !activeInverted)
        {
            return nullptr;
        }

        // Accumulate the visible layers in list order (index 0 = bottom). The first visible layer is
        // the base; every subsequent layer combines with the running result per its combine mode.
        // NON-ACTIVE layers come from m_layerRuntime.m_meshCache: their built display mesh only changes when
        // their data/meta change, so deserializing + transforming them on every rebuild (the old
        // behaviour) was pure waste - and the reason edits got slow once real geometry existed.
        Api::WhiteBoxMeshPtr acc;
        for (const int idx : visible)
        {
            Api::WhiteBoxMeshPtr ownedMesh; //!< Active layer only - always built fresh from the working mesh.
            WhiteBoxMesh* operand = nullptr; //!< The layer's display mesh (owned locally or by the cache).
            if (idx == activeIdx)
            {
                ownedMesh = CombinedWithGrid(activeFreeform);
                if (!ownedMesh && activeFreeform != nullptr)
                {
                    ownedMesh = Api::CloneMesh(*activeFreeform);
                }
                if (ownedMesh && !activeIdentity)
                {
                    ApplyTransformToMesh(
                        *ownedMesh, m_layers[activeIdx].m_position, m_layers[activeIdx].m_rotation,
                        m_layers[activeIdx].m_scale);
                }
                if (ownedMesh && activeInverted)
                {
                    ownedMesh = FlippedMeshWinding(*ownedMesh); // mesh-level flip (see BuildLayerMesh)
                }
                operand = ownedMesh.get();
            }
            else
            {
                const AZ::u64 layerId = m_layers[idx].m_id;
                auto it = m_layerRuntime.m_meshCache.find(layerId);
                if (it == m_layerRuntime.m_meshCache.end() || !it->second)
                {
                    it = m_layerRuntime.m_meshCache.insert_or_assign(layerId, BuildLayerMesh(m_layers[idx])).first;
                }
                operand = it->second.get(); // already transformed; used read-only below
            }
            if (operand == nullptr)
            {
                continue;
            }
            if (!acc)
            {
                // First visible layer is the base (its own mode is ignored). The accumulator gets
                // mutated, so a cached operand must be cloned; the active layer's mesh is moved.
                acc = ownedMesh ? AZStd::move(ownedMesh) : Api::CloneMesh(*operand);
                continue;
            }
            const AZ::Transform identity = AZ::Transform::CreateIdentity();
            // Inter-layer booleans run in both tint modes. Under per-layer tint the merged surface is
            // re-coloured per face by BuildColoredRenderData (each output face inherits the tint of
            // the source layer it lies on), so a boolean no longer loses per-layer colour.
            const LayerCombineMode mode = m_layers[idx].m_combineMode;
            switch (mode)
            {
            case LayerCombineMode::Union:
                if (Api::MeshFaceHandles(*acc).empty() ||
                    !Api::ApplyMeshBoolean(*acc, *operand, identity, Api::BooleanOperation::Union, m_csgSolver))
                {
                    AppendMesh(*acc, *operand);
                }
                break;
            case LayerCombineMode::Subtract:
                Api::ApplyMeshBoolean(*acc, *operand, identity, Api::BooleanOperation::Subtraction, m_csgSolver);
                break;
            case LayerCombineMode::Intersect:
                Api::ApplyMeshBoolean(*acc, *operand, identity, Api::BooleanOperation::Intersection, m_csgSolver);
                break;
            case LayerCombineMode::Separate:
            default:
                AppendMesh(*acc, *operand);
                break;
            }
        }
        if (!acc)
        {
            return Api::CreateWhiteBoxMesh();
        }
        Api::CalculateNormals(*acc);
        Api::CalculatePlanarUVs(*acc);
        return acc;
    }

    // void EditorWhiteBoxComponent::RebuildCombinedMesh()
    // {
    //     // When the entity boolean is live and applies to the WHOLE mesh (not just the active layer),
    //     // m_displayMesh already folds in every layer, grid and transform, so use it directly.
    //     if (m_boolean.m_live && m_displayMesh && !m_boolean.m_affectActiveOnly)
    //     {
    //         m_combinedMesh = Api::CloneMesh(*m_displayMesh);
    //         return;
    //     }

    //     // Otherwise the active-layer base is the live-boolean result (active-only mode) or the raw
    //     // editable mesh; BuildCombined folds in the grids, the other layers and all transforms.
    //     WhiteBoxMesh* freeform = (m_boolean.m_live && m_displayMesh) ? m_displayMesh.get() : GetWhiteBoxMesh();
    //     m_combinedMesh = BuildCombined(freeform);
    // }
    void EditorWhiteBoxComponent::RebuildCombinedMesh()
    {
        if (m_boolean.m_live && m_displayMesh && !m_boolean.m_affectActiveOnly)
        {
            m_combinedMesh = Api::CloneMesh(*m_displayMesh);
            // Physics uses the collision-FILTERED cut result (m_physicsDisplayMesh), not the visual
            // one, so layers with Collision off stay out of the collider even while the live boolean
            // is on. Fall back to the visual cut mesh only if the physics variant was not evaluated.
            m_physicsCombinedMesh =
                Api::CloneMesh(m_physicsDisplayMesh ? *m_physicsDisplayMesh : *m_displayMesh);
            BakeEntityScaleIntoPhysicsMesh();
            return;
        }

        WhiteBoxMesh* freeform = (m_boolean.m_live && m_displayMesh) ? m_displayMesh.get() : GetWhiteBoxMesh();
        m_combinedMesh = BuildCombined(freeform, false);       // Visual Mesh (entity non-uniform scale via render transform)
        m_physicsCombinedMesh = BuildCombined(freeform, true); // Physics Mesh
        BakeEntityScaleIntoPhysicsMesh();
    }

    AZ::Vector3 EditorWhiteBoxComponent::EntityNonUniformScale() const
    {
        AZ::Vector3 scale = AZ::Vector3::CreateOne();
        AZ::NonUniformScaleRequestBus::EventResult(scale, GetEntityId(), &AZ::NonUniformScaleRequests::GetScale);
        return scale;
    }

    void EditorWhiteBoxComponent::BakeEntityScaleIntoPhysicsMesh()
    {
        // The render mesh gets the entity's non-uniform scale from its render transform (Atom applies
        // it), but a cooked triangle-mesh collider does not reliably honour a non-uniform shape scale,
        // so bake it straight into the PHYSICS mesh geometry instead - exactly how per-layer scale is
        // handled. BuildCombined(physicsPass) always returns a fresh owned mesh, so mutating it here is
        // safe (it never aliases the editable working mesh). The collider then cooks scaled geometry and
        // its debug wireframe matches.
        const AZ::Vector3 scale = EntityNonUniformScale();
        if (m_physicsCombinedMesh && !scale.IsClose(AZ::Vector3::CreateOne()))
        {
            ApplyTransformToMesh(*m_physicsCombinedMesh, AZ::Vector3::CreateZero(), AZ::Vector3::CreateZero(), scale);
            Api::CalculateNormals(*m_physicsCombinedMesh);
        }
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::GetPhysicsMesh()
    {
        if (m_physicsCombinedMesh)
        {
            return m_physicsCombinedMesh.get();
        }
        return EvaluatedMesh(); // Fallback safely
    }
} // namespace WhiteBox
