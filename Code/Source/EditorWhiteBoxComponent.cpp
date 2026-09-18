/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * EditorWhiteBoxComponent - CORE translation unit: component lifecycle, reflection (serialize
 * + edit context), tick-driven coalescing, game-entity bake and small bus glue. The other
 * concerns live in sibling translation units:
 *   EditorWhiteBoxComponentLayers.cpp  - layer system, combine pipeline, parametric shapes
 *   EditorWhiteBoxComponentVoxel.cpp   - cube stamp gestures, cell records, mesh repair
 *   EditorWhiteBoxComponentBoolean.cpp - entity boolean (one-shot + live) and bake caches
 *   EditorWhiteBoxComponentRender.cpp  - render/physics meshes, bounds, selection, debug draw
 *   EditorWhiteBoxComponentAsset.cpp   - serialize/deserialize, Save As Asset, obj exports
 */

#include "Asset/EditorWhiteBoxMeshAsset.h"
#include "EditorWhiteBoxComponent.h"
#include "EditorWhiteBoxComponentMode.h"
#include "Rendering/WhiteBoxNullRenderMesh.h"
#include "Tools/WhiteBoxLayerUtil.h"
#include "WhiteBoxComponent.h"

#include "Util/WhiteBoxMeshUtil.h"

#include <AzCore/Asset/AssetSerializer.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzFramework/Visibility/BoundsBus.h>
#include <AzToolsFramework/API/EntityCompositionRequestBus.h>
#include <AzToolsFramework/API/EditorPythonRunnerRequestsBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Entity/EditorEntityHelpers.h>
#include <Components/EditorWhiteBoxColliderComponent.h>

namespace WhiteBox
{
    // callback for when the default shape field is changed
    AZ::Crc32 EditorWhiteBoxComponent::OnDefaultShapeChange()
    {
        const AZStd::string entityIdStr = AZStd::string::format("%llu", static_cast<AZ::u64>(GetEntityId()));
        const AZStd::string componentIdStr = AZStd::string::format("%llu", GetId());
        const AZStd::string shapeTypeStr = AZStd::string::format("%d", aznumeric_cast<int>(m_defaultShape));
        const AZStd::vector<AZStd::string_view> scriptArgs{entityIdStr, componentIdStr, shapeTypeStr};

        // if the shape type has just changed and it is no longer an asset type, check if a mesh asset
        // is in use and clear it if so (switch back to using the component serialized White Box mesh)
        if (!DisplayingAsset(m_defaultShape) && m_editorMeshAsset->InUse())
        {
            m_editorMeshAsset->Reset();
        }

        AzToolsFramework::EditorPythonRunnerRequestBus::Broadcast(
            &AzToolsFramework::EditorPythonRunnerRequestBus::Events::ExecuteByFilenameWithArgs,
            "@gemroot:WhiteBox@/Editor/Scripts/default_shapes.py", scriptArgs);

        EditorWhiteBoxComponentNotificationBus::Event(
            AZ::EntityComponentIdPair(GetEntityId(), GetId()),
            &EditorWhiteBoxComponentNotificationBus::Events::OnDefaultShapeTypeChanged, m_defaultShape);

        return AZ::Edit::PropertyRefreshLevels::EntireTree;
    }

    AZ::u32 EditorWhiteBoxComponent::DrawShapeData::OnShapeChange()
    {
        // Reset Draw Sides to a sensible default for the newly chosen shape:
        // angular shapes -> 4 (box / square base), round shapes -> 24 (smooth).
        switch (m_shape)
        {
        case DrawShapeType::Box:
        case DrawShapeType::Pyramid:
            m_sides = 4;
            break;
        case DrawShapeType::Cylinder:
        case DrawShapeType::Cone:
        case DrawShapeType::Pipe:
        case DrawShapeType::Torus:
            m_sides = 24;
            break;
        case DrawShapeType::Sphere:
            m_sides = 16; // longitude segments; latitude rings derived from this
            break;
        default:
            break;
        }

        // refresh so the Draw Sides / Draw Steps fields show the new value (and
        // toggle visibility of the Sphere-only / Staircase-only controls)
        return AZ::Edit::PropertyRefreshLevels::EntireTree;
    }

    // Tube Sides was split out of Draw Sides. Seed it from the old derived value so a torus drawn
    // before the split keeps the tessellation it had.
    int LegacyTubeSidesFromSides(const int sides)
    {
        return AZStd::clamp(sides / 2, 8, 32);
    }

    bool DrawShapeDataVersionConverter(
        AZ::SerializeContext& context, AZ::SerializeContext::DataElementNode& classElement)
    {
        if (classElement.GetVersion() <= 2)
        {
            int sides = 4;
            if (const int idx = classElement.FindElement(AZ_CRC_CE("Sides")); idx != -1)
            {
                classElement.GetSubElement(idx).GetData(sides);
            }
            classElement.AddElementWithData(context, "TubeSides", LegacyTubeSidesFromSides(sides));
        }
        return true;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawShapeData::TubeSidesVisibility() const
    {
        return m_shape == DrawShapeType::Torus ? AZ::Edit::PropertyVisibility::Show
                                               : AZ::Edit::PropertyVisibility::Hide;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawShapeData::SidesVisibility() const
    {
        // Sides applies to every solid shape: it sets the footprint resolution for
        // round shapes (Cylinder/Cone), the subdivision for the Sphere, and the
        // N-gon footprint for Box/Pyramid (3 = triangular prism, 4 = box, etc.).
        // Only the Staircase ignores it.
        return (m_shape == DrawShapeType::Staircase || m_shape == DrawShapeType::Plane || m_shape == DrawShapeType::Polygon)
            ? AZ::Edit::PropertyVisibility::Hide
                                                   : AZ::Edit::PropertyVisibility::Show;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawShapeData::StairVisibility() const
    {
        // The whole Stair group only shows for a Staircase. Within the group the
        // step-count / step-height split is handled by DrawStairData itself.
        return m_shape == DrawShapeType::Staircase ? AZ::Edit::PropertyVisibility::Show
                                                   : AZ::Edit::PropertyVisibility::Hide;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawStairData::StepsVisibility() const
    {
        // Step count only applies in step-count mode (the group is already hidden
        // unless the draw shape is a Staircase).
        return m_byHeight ? AZ::Edit::PropertyVisibility::Hide : AZ::Edit::PropertyVisibility::Show;
    }

    AZ::Crc32 EditorWhiteBoxComponent::DrawStairData::StepHeightVisibility() const
    {
        // Step height only applies in step-height mode.
        return m_byHeight ? AZ::Edit::PropertyVisibility::Show : AZ::Edit::PropertyVisibility::Hide;
    }

    bool EditorWhiteBoxVersionConverter(
        AZ::SerializeContext& context, AZ::SerializeContext::DataElementNode& classElement)
    {
        if (classElement.GetVersion() <= 1)
        {
            // find the old WhiteBoxMeshAsset stored directly on the component
            AZ::Data::Asset<Pipeline::WhiteBoxMeshAsset> meshAsset;
            const int meshAssetIndex = classElement.FindElement(AZ_CRC_CE("MeshAsset"));
            if (meshAssetIndex != -1)
            {
                classElement.GetSubElement(meshAssetIndex).GetData(meshAsset);
                classElement.RemoveElement(meshAssetIndex);
            }
            else
            {
                return false;
            }

            // add the new EditorWhiteBoxMeshAsset which will contain the previous WhiteBoxMeshAsset
            const int editorMeshAssetIndex =
                classElement.AddElement<EditorWhiteBoxMeshAsset>(context, "EditorMeshAsset");

            if (editorMeshAssetIndex != -1)
            {
                // insert the existing WhiteBoxMeshAsset into the new EditorWhiteBoxMeshAsset
                classElement.GetSubElement(editorMeshAssetIndex)
                    .AddElementWithData<AZ::Data::Asset<Pipeline::WhiteBoxMeshAsset>>(context, "MeshAsset", meshAsset);
            }
            else
            {
                return false;
            }
        }

        if (classElement.GetVersion() <= 2)
        {
            // v3 grouped the loose voxel / boolean / draw fields into structs. Move each old
            // flat field's data into the new nested elements (missing fields are skipped and
            // keep their defaults).

            // -- voxel / legacy grid fields -> VoxelData ("Voxel") --
            {
                EditorWhiteBoxComponent::VoxelData voxel;
                if (const int idx = classElement.FindElement(AZ_CRC_CE("VoxelCells")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_cells);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("VoxelCellSizes")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_sizes);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("VoxelCellSize")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_legacySize);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("VoxelMerged")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_legacyMerged);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("GridMeshData")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_legacyGridData);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("GridMergedData")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(voxel.m_legacyGridMergedData);
                    classElement.RemoveElement(idx);
                }
                classElement.AddElementWithData(context, "Voxel", voxel);
            }

            // -- boolean fields -> BooleanSettings ("Boolean"); the two mutually exclusive
            //    hide/delete bools collapse into the SourceAfterApply enum --
            {
                EditorWhiteBoxComponent::BooleanSettings boolean;
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanSource")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(boolean.m_sourceEntity);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanOp")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(boolean.m_operation);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanLive")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(boolean.m_live);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanAffectActive")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(boolean.m_affectActiveOnly);
                    classElement.RemoveElement(idx);
                }
                bool hideSource = false;
                bool deleteSource = false;
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanHideSource")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(hideSource);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("BooleanDeleteSource")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(deleteSource);
                    classElement.RemoveElement(idx);
                }
                boolean.m_sourceAfterApply =
                    deleteSource ? SourceAfterApply::Delete : (hideSource ? SourceAfterApply::Hide : SourceAfterApply::Keep);
                classElement.AddElementWithData(context, "Boolean", boolean);
            }

            // -- loose draw fields fold into the existing nested DrawShapeData element --
            {
                bool carve = false;
                bool mergeUnion = false;
                bool unitCube = false;
                float unitCubeSize = 1.0f;
                bool unitCubeShowGrid = true;
                if (const int idx = classElement.FindElement(AZ_CRC_CE("DrawCarve")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(carve);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("DrawMergeUnion")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(mergeUnion);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("DrawUnitCube")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(unitCube);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("DrawUnitCubeSize")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(unitCubeSize);
                    classElement.RemoveElement(idx);
                }
                if (const int idx = classElement.FindElement(AZ_CRC_CE("DrawUnitCubeShowGrid")); idx != -1)
                {
                    classElement.GetSubElement(idx).GetData(unitCubeShowGrid);
                    classElement.RemoveElement(idx);
                }
                if (const int dsIdx = classElement.FindElement(AZ_CRC_CE("DrawShapeData")); dsIdx != -1)
                {
                    AZ::SerializeContext::DataElementNode& drawShapeNode = classElement.GetSubElement(dsIdx);
                    drawShapeNode.AddElementWithData(context, "Carve", carve);
                    drawShapeNode.AddElementWithData(context, "MergeUnion", mergeUnion);
                    drawShapeNode.AddElementWithData(context, "UnitCube", unitCube);
                    drawShapeNode.AddElementWithData(context, "UnitCubeSize", unitCubeSize);
                    drawShapeNode.AddElementWithData(context, "UnitCubeShowGrid", unitCubeShowGrid);
                }
                // the "Merge With Mesh" feature was removed entirely
                if (const int idx = classElement.FindElement(AZ_CRC_CE("MergeGridWithMesh")); idx != -1)
                {
                    classElement.RemoveElement(idx);
                }
            }
        }

        if (classElement.GetVersion() <= 5)
        {
            // v6 moved the render data and the game-mode bakes out of reflection and into byte-stream
            // blobs. All of it is derived from the (still serialized) mesh streams, so the old elements
            // are simply dropped and PhysicsBaked is cleared to force Activate to bake once on load.
            for (const AZ::Crc32 field :
                 { AZ_CRC_CE("RenderData"), AZ_CRC_CE("BakedBooleanRenderData"), AZ_CRC_CE("BakedBaseRenderData"),
                   AZ_CRC_CE("BakedPhysicsBaseRenderData"), AZ_CRC_CE("BakedPhysicsBooleanRenderData") })
            {
                if (const int idx = classElement.FindElement(field); idx != -1)
                {
                    classElement.RemoveElement(idx);
                }
            }
            if (const int idx = classElement.FindElement(AZ_CRC_CE("PhysicsBaked")); idx != -1)
            {
                classElement.GetSubElement(idx).SetData(context, false);
            }
        }

        return true;
    }

    void EditorWhiteBoxComponent::DrawStairData::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<DrawStairData>()
                ->Version(1)
                ->Field("ByHeight", &DrawStairData::m_byHeight)
                ->Field("Steps", &DrawStairData::m_steps)
                ->Field("StepHeight", &DrawStairData::m_stepHeight)
                ->Field("Rotation", &DrawStairData::m_rotation);

            if (AZ::EditContext* editContext = serializeContext->GetEditContext())
            {
                editContext->Class<DrawStairData>("Stair", "Staircase-specific Draw Shape settings.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &DrawStairData::m_byHeight, "Stair By Step Height",
                        "When on, the Staircase is divided by a fixed step (riser) height; the step count is derived "
                        "from the pull height. When off, a fixed step count is used.")
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, AZ::Edit::PropertyRefreshLevels::EntireTree)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Slider, &DrawStairData::m_steps, "Step Count",
                        "Number of steps the Draw Shape tool builds when the shape is a Staircase.")
                    ->Attribute(AZ::Edit::Attributes::Min, 1)
                    ->Attribute(AZ::Edit::Attributes::Max, 128)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &DrawStairData::StepsVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &DrawStairData::m_stepHeight, "Step Height",
                        "Riser height of each step; the step count is derived from the pull height.")
                    ->Attribute(AZ::Edit::Attributes::Min, 0.01f)
                    ->Attribute(AZ::Edit::Attributes::Max, 1000.0f)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &DrawStairData::StepHeightVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Slider, &DrawStairData::m_rotation, "Stair Rotation (x90)",
                        "Orientation of the Staircase in 90-degree steps about the drawn surface. 2 (180 degrees) puts "
                        "the tall end at the corner you first clicked.")
                    ->Attribute(AZ::Edit::Attributes::Min, 0)
                    ->Attribute(AZ::Edit::Attributes::Max, 3);
            }
        }
    }

    void EditorWhiteBoxComponent::DrawShapeData::Reflect(AZ::ReflectContext* context)
    {
        DrawStairData::Reflect(context);

        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<DrawShapeData>()
                ->Version(3, &DrawShapeDataVersionConverter)
                ->Field("Shape", &DrawShapeData::m_shape)
                ->Field("Sides", &DrawShapeData::m_sides)
                ->Field("HoleRatio", &DrawShapeData::m_holeRatio)
                ->Field("TubeSides", &DrawShapeData::m_tubeSides)
                ->Field("Stair", &DrawShapeData::m_stair)
                ->Field("Carve", &DrawShapeData::m_carve)
                ->Field("MergeUnion", &DrawShapeData::m_mergeUnion)
                ->Field("UnitCube", &DrawShapeData::m_unitCube)
                ->Field("UnitCubeSize", &DrawShapeData::m_unitCubeSize)
                ->Field("UnitCubeShowGrid", &DrawShapeData::m_unitCubeShowGrid);

            if (AZ::EditContext* editContext = serializeContext->GetEditContext())
            {
                editContext->Class<DrawShapeData>("Draw Shape", "Draw Shape tool settings.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::ComboBox, &DrawShapeData::m_shape, "Draw Shape",
                        "Shape the Draw Shape tool builds. Changing this resets Draw Sides to a sensible default.")
                    ->EnumAttribute(DrawShapeType::Box, "Box")
                    ->EnumAttribute(DrawShapeType::Cylinder, "Cylinder")
                    ->EnumAttribute(DrawShapeType::Pyramid, "Pyramid")
                    ->EnumAttribute(DrawShapeType::Cone, "Cone")
                    ->EnumAttribute(DrawShapeType::Sphere, "Sphere")
                    ->EnumAttribute(DrawShapeType::Staircase, "Staircase")
                    ->EnumAttribute(DrawShapeType::Plane, "Plane")
                    ->EnumAttribute(DrawShapeType::Torus, "Torus")
                    ->EnumAttribute(DrawShapeType::Pipe, "Pipe")
                    ->EnumAttribute(DrawShapeType::Polygon, "Freeform Polygon")
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, &DrawShapeData::OnShapeChange)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Slider, &DrawShapeData::m_sides, "Draw Sides",
                        "Number of sides for round / N-gon shapes (4 = box / square), or the subdivision of the Sphere.")
                    ->Attribute(AZ::Edit::Attributes::Min, 3)
                    ->Attribute(AZ::Edit::Attributes::Max, 128)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &DrawShapeData::SidesVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Slider, &DrawShapeData::m_tubeSides, "Tube Sides",
                        "Segments around the torus tube's cross-section. A torus has Draw Sides x Tube Sides "
                        "quads, so this is the other half of its triangle count.")
                    ->Attribute(AZ::Edit::Attributes::Min, MinTubeSides)
                    ->Attribute(AZ::Edit::Attributes::Max, MaxTubeSides)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &DrawShapeData::TubeSidesVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &DrawShapeData::m_stair, "Stair",
                        "Staircase-specific settings.")
                    ->Attribute(AZ::Edit::Attributes::Visibility, &DrawShapeData::StairVisibility)
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true);
            }
        }
    }

    void EditorWhiteBoxComponent::VoxelData::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<VoxelData>()
                ->Version(1)
                ->Field("Cells", &VoxelData::m_cells)
                ->Field("Sizes", &VoxelData::m_sizes)
                ->Field("LegacySize", &VoxelData::m_legacySize)
                ->Field("LegacyMerged", &VoxelData::m_legacyMerged)
                ->Field("LegacyGridData", &VoxelData::m_legacyGridData)
                ->Field("LegacyGridMergedData", &VoxelData::m_legacyGridMergedData);
        }
    }

    void EditorWhiteBoxComponent::BooleanSettings::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<BooleanSettings>()
                ->Version(2)
                ->Field("Source", &BooleanSettings::m_sourceEntity)
                ->Field("Operation", &BooleanSettings::m_operation)
                ->Field("Live", &BooleanSettings::m_live)
                ->Field("AffectActiveOnly", &BooleanSettings::m_affectActiveOnly)
                ->Field("SourceAfterApply", &BooleanSettings::m_sourceAfterApply)
                ->Field("ExcludeFromBoolean", &BooleanSettings::m_excludeFromBoolean)
                ->Field("BooleanOthers", &BooleanSettings::m_booleanOthers)
                ->Field("CutterOperation", &BooleanSettings::m_cutterOperation);
        }
    }

    void EditorWhiteBoxComponent::Reflect(AZ::ReflectContext* context)
    {
        EditorWhiteBoxMeshAsset::Reflect(context);
        DrawShapeData::Reflect(context);
        WhiteBoxLayer::Reflect(context);
        VoxelData::Reflect(context);
        BooleanSettings::Reflect(context);

        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<EditorWhiteBoxComponent, EditorComponentBase>()
                ->Version(6, &EditorWhiteBoxVersionConverter)
                ->Field("WhiteBoxData", &EditorWhiteBoxComponent::m_whiteBoxData)
                ->Field("Layers", &EditorWhiteBoxComponent::m_layers)
                ->Field("ActiveLayer", &EditorWhiteBoxComponent::m_activeLayerIndex)
                ->Field("NextLayerId", &EditorWhiteBoxComponent::m_nextLayerId)
                ->Field("DefaultShape", &EditorWhiteBoxComponent::m_defaultShape)
                ->Field("EditorMeshAsset", &EditorWhiteBoxComponent::m_editorMeshAsset)
                ->Field("Material", &EditorWhiteBoxComponent::m_material)
                ->Field("ComponentMode", &EditorWhiteBoxComponent::m_componentModeDelegate)
                ->Field("FlipYZForExport", &EditorWhiteBoxComponent::m_flipYZForExport)
                ->Field("DrawShapeData", &EditorWhiteBoxComponent::m_drawShapeData)
                ->Field("EdgesOnly", &EditorWhiteBoxComponent::m_edgesOnly)
                ->Field("UseGlobalTint", &EditorWhiteBoxComponent::m_useGlobalTint)
                ->Field("MaterialOverride", &EditorWhiteBoxComponent::m_materialOverrideAssetId)
                ->Field("Voxel", &EditorWhiteBoxComponent::m_voxel)
                ->Field("Boolean", &EditorWhiteBoxComponent::m_boolean)
                // Persist whether this entity is an active global-boolean target so the composed
                // result is the default shown geometry after reload and in game mode (the baked
                // boolean render data carries the geometry; this flag says "show it by default").
                ->Field("GlobalBooleanActive", &EditorWhiteBoxComponent::m_globalBooleanActive)
                ->Field("CsgSolver", &EditorWhiteBoxComponent::m_csgSolver)
                // The evaluated render data and the four game-mode bakes (including the
                // collision-FILTERED physics ones the asset-processor clone needs, since it is never
                // live-edited) are persisted as byte streams rather than as reflected face vectors -
                // see m_renderDataBlob / m_bakedDataBlob for why.
                ->Field("RenderDataBlob", &EditorWhiteBoxComponent::m_renderDataBlob)
                ->Field("BakedDataBlob", &EditorWhiteBoxComponent::m_bakedDataBlob)
                ->Field("PhysicsBaked", &EditorWhiteBoxComponent::m_physicsBaked);

            if (AZ::EditContext* editContext = serializeContext->GetEditContext())
            {
                // The component is only the bridge that binds the White Box data to the entity.
                // ALL editing UI (default shape, layers, draw settings, cube stamp, booleans,
                // material and asset/export operations) lives in the dockable White Box pane
                // (Tools > White Box), driven through the component's Pane API.
                editContext->Class<EditorWhiteBoxComponent>("White Box", "White Box level editing")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Shape")
                    ->Attribute(AZ::Edit::Attributes::Icon, "Editor/Icons/Components/WhiteBox.svg")
                    ->Attribute(AZ::Edit::Attributes::ViewportIcon, "Editor/Icons/Components/Viewport/WhiteBox.svg")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu, AZ_CRC_CE("Game"))
                    ->Attribute(
                        AZ::Edit::Attributes::HelpPageURL, "https://o3de.org/docs/user-guide/components/reference/shape/white-box/")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->UIElement(
                        AZ::Edit::UIHandlers::Button, "",
                        "Open the White Box pane - all White Box editing lives there.")
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, &EditorWhiteBoxComponent::OnOpenPane)
                    ->Attribute(AZ::Edit::Attributes::ButtonText, "Open White Box Pane")
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &EditorWhiteBoxComponent::m_editorMeshAsset, "Editor Mesh Asset",
                        "Editor Mesh Asset")
                    ->Attribute(AZ::Edit::Attributes::Visibility, &EditorWhiteBoxComponent::AssetVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &EditorWhiteBoxComponent::m_componentModeDelegate,
                        "Component Mode", "White Box Tool Component Mode")
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly);
            }
        }
    }

    AZ::Crc32 EditorWhiteBoxComponent::OnAddCollision()
    {
        AZ::Entity* entity = GetEntity();
        if (entity == nullptr)
        {
            return AZ::Edit::PropertyRefreshLevels::None;
        }
        if (entity->FindComponent<EditorWhiteBoxColliderComponent>() != nullptr)
        {
            return AZ::Edit::PropertyRefreshLevels::None; // already has a White Box collider
        }
        AzToolsFramework::ScopedUndoBatch undoBatch("Add White Box Collision");
        AzToolsFramework::EntityCompositionRequests::AddComponentsOutcome outcome =
            AZ::Failure(AZStd::string("uninitialized"));
        AzToolsFramework::EntityCompositionRequestBus::BroadcastResult(
            outcome, &AzToolsFramework::EntityCompositionRequests::AddComponentsToEntities,
            AzToolsFramework::EntityIdList{ GetEntityId() },
            AZ::ComponentTypeList{ azrtti_typeid<EditorWhiteBoxColliderComponent>() });
        undoBatch.MarkEntityDirty(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::EntireTree; // refresh the inspector to show the new component
    }

    void EditorWhiteBoxComponent::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required)
    {
        required.push_back(AZ_CRC_CE("TransformService"));
    }

    void EditorWhiteBoxComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("WhiteBoxService"));
    }

    void EditorWhiteBoxComponent::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
    {
        // Note: NonUniformScaleService is intentionally NOT listed - White Box supports the entity's
        // Non-Uniform Scale component (render, collider and bounds all honour it via the bus).
        incompatible.push_back(AZ_CRC_CE("MeshService"));
        incompatible.push_back(AZ_CRC_CE("WhiteBoxService"));
    }

    EditorWhiteBoxComponent::EditorWhiteBoxComponent() = default;

    EditorWhiteBoxComponent::~EditorWhiteBoxComponent()
    {
        // note: m_editorMeshAsset is (usually) serialized so it is created by the reflection system
        // in Reflect (no explicit `new`) - we must still clean-up the resource on destruction though
        // to not leak resources.
        delete m_editorMeshAsset;
    }

    void EditorWhiteBoxComponent::Init()
    {
        if (m_editorMeshAsset)
        {
            return;
        }

        // if the m_editorMeshAsset has not been created by the serialization system
        // create a new EditorWhiteBoxMeshAsset here
        m_editorMeshAsset = aznew EditorWhiteBoxMeshAsset();
    }

    void EditorWhiteBoxComponent::Activate()
    {
        m_material.m_materialAsset = AZ::Data::Asset<AZ::RPI::MaterialAsset>(
            m_materialOverrideAssetId, azrtti_typeid<AZ::RPI::MaterialAsset>());
        m_material.m_materialAsset.SetAutoLoadBehavior(AZ::Data::AssetLoadBehavior::PreLoad);
        const AZ::EntityId entityId = GetEntityId();
        const AZ::EntityComponentIdPair entityComponentIdPair{entityId, GetId()};

        AzToolsFramework::Components::EditorComponentBase::Activate();
        EditorWhiteBoxComponentRequestBus::Handler::BusConnect(entityComponentIdPair);
        EditorWhiteBoxComponentNotificationBus::Handler::BusConnect(entityComponentIdPair);
        AZ::TransformNotificationBus::Handler::BusConnect(entityId);
        AzFramework::BoundsRequestBus::Handler::BusConnect(entityId);
        AzFramework::VisibleGeometryRequestBus::Handler::BusConnect(entityId);
        // Join the editor's render geometry intersector so White Box meshes answer scene
        // raycasts. Connecting notifies the intersector automatically (see the bus's
        // ConnectionPolicy); geometry changes are signalled from RebuildRenderMesh.
        AzFramework::RenderGeometry::IntersectionRequestBus::Handler::BusConnect(
            AzFramework::RenderGeometry::EntityIdAndContext(entityId, AzToolsFramework::GetEntityContextId()));
        AzFramework::EntityDebugDisplayEventBus::Handler::BusConnect(entityId);
        AzToolsFramework::EditorComponentSelectionRequestsBus::Handler::BusConnect(entityId);
        AzToolsFramework::EditorVisibilityNotificationBus::Handler::BusConnect(entityId);
        // Offer this mesh's vertices to the editor-wide vertex snapper. Connecting is harmless
        // when no snapper gem is installed - nothing will ever call us.
        SnapApi::VertexSourceRequestBus::Handler::BusConnect(entityId);
        AZ::TickBus::Handler::BusConnect();

        m_componentModeDelegate.ConnectWithSingleComponentMode<EditorWhiteBoxComponent, EditorWhiteBoxComponentMode>(
            entityComponentIdPair, this);

        m_worldFromLocal = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(m_worldFromLocal, entityId, &AZ::TransformBus::Events::GetWorldTM);

        m_editorMeshAsset->Associate(entityComponentIdPair);

        // The render data and game-mode bakes are serialized as byte streams; restore them first so a
        // clone that never rebuilds (the asset-processor build) still has them, and so an undo/redo
        // reload comes back with the state it was captured with.
        UnpackRenderDataBlobs();

        // deserialize the white box data into a mesh object or load the serialized asset ref
        // (the serialized stream already contains the full combined geometry - freeform
        // faces plus any voxel-stamped surface - so no voxel regeneration is needed here;
        // regenerating would discard freeform edits made before the level was saved).
        DeserializeWhiteBox();

        // Refresh render/physics/bounds when the entity's non-uniform scale changes (it is separate
        // from the Transform, so it does not raise OnTransformChanged).
        m_nonUniformScaleChangedHandler = AZ::NonUniformScaleChangedEvent::Handler(
            [this]([[maybe_unused]] const AZ::Vector3& scale)
            {
                m_worldAabb.reset();
                InvalidateSnapVertexCache(); // cached snap vertices fold in the non-uniform scale
                // Full rebuild: RebuildCombinedMesh re-bakes the entity non-uniform scale into the
                // physics mesh, RebuildRenderMesh re-applies the render transform (AtomRenderMesh reads
                // the new scale), and the physics/baked passes re-cook the collider from the scaled mesh.
                RebuildWhiteBox();
            });
        AZ::NonUniformScaleRequestBus::Event(
            entityId, &AZ::NonUniformScaleRequests::RegisterScaleChangedEvent, m_nonUniformScaleChangedHandler);

        // re-evaluate the live boolean and listen for the source entity moving
        UpdateBooleanSourceListener();
        EvaluateLiveBoolean();
        RebuildCombinedMesh(); // fold in the stamp/grid layer so it renders on load

        // Ensure the collision-filtered physics bake exists on load. Levels saved before this data
        // was persisted (m_physicsBaked == false) would otherwise fall back to the full visual mesh
        // for physics in game mode, making the runtime collider wireframe show non-collidable layers.
        // Baking here means entering game mode uses the filtered collider geometry immediately, and
        // saving the level persists it for the exported / asset-processor build.
        if (!m_physicsBaked)
        {
            RebuildBakedRenderData();
        }

        if (AzToolsFramework::IsEntityVisible(entityId))
        {
            if (m_edgesOnly)
            {
                HideRenderMesh();
            }
            else
            {
                ShowRenderMesh();
            }
            OnMaterialChange();
        }
    }

    void EditorWhiteBoxComponent::Deactivate()
    {
        m_nonUniformScaleChangedHandler.Disconnect();
        AZ::TickBus::Handler::BusDisconnect();
        SnapApi::VertexSourceRequestBus::Handler::BusDisconnect();
        AzToolsFramework::EditorVisibilityNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::EditorComponentSelectionRequestsBus::Handler::BusDisconnect();
        AzFramework::EntityDebugDisplayEventBus::Handler::BusDisconnect();
        AzFramework::RenderGeometry::IntersectionRequestBus::Handler::BusDisconnect();
        AzFramework::VisibleGeometryRequestBus::Handler::BusDisconnect();
        AzFramework::BoundsRequestBus::Handler::BusDisconnect();
        AZ::TransformNotificationBus::Handler::BusDisconnect();
        EditorWhiteBoxComponentRequestBus::Handler::BusDisconnect();
        EditorWhiteBoxComponentNotificationBus::Handler::BusDisconnect();
        AzToolsFramework::Components::EditorComponentBase::Deactivate();

        m_booleanSourceListener.BusDisconnect();

        m_componentModeDelegate.Disconnect();
        m_editorMeshAsset->Release();
        m_layerRenderMeshes.clear();
        m_renderMesh.reset();
        m_whiteBox.reset();
        m_displayMesh.reset();
    }

    void EditorWhiteBoxComponent::OnTick(float deltaTime, AZ::ScriptTimePoint /*time*/)
    {
        // The reflected layer container's native add / remove / reorder does not reliably invoke
        // the DataElement ChangeNotify, so poll a hash of the layer id order here and do the full
        // structural resync (which also refreshes the property grid) the moment anything changes.
        if (LayerSignature() != m_layerRuntime.m_lastSignature)
        {
            SyncLayerStructure();
        }

        // Coalesced live-boolean rebuild: the boolean source moved (possibly many transform
        // notifications this frame) - re-evaluate ONCE, and skip the physics rebuild while the
        // drag is still streaming (it is deferred until the source has been still briefly).
        if (m_rebuild.m_liveBooleanPending)
        {
            m_rebuild.m_liveBooleanPending = false;
            EvaluateLiveBoolean();
            RebuildCombinedMesh();
            RebuildRenderMesh();
            m_rebuild.m_physicsPending = true;
            m_rebuild.m_physicsTimer = 0.0f;
        }
        else if (m_rebuild.m_physicsPending)
        {
            m_rebuild.m_physicsTimer += deltaTime;
            if (m_rebuild.m_physicsTimer >= 0.25f)
            {
                m_rebuild.m_physicsPending = false;
                RebuildPhysicsMesh();
            }
        }

        // Debounced game-mode bake caches for the DRAG path (boolean source moving): wait for
        // the drag to settle instead of paying for the bake every frame. Discrete edits bake
        // synchronously in RebuildWhiteBox instead.
        if (m_rebuild.m_bakedDataDirty)
        {
            m_rebuild.m_bakedDataDelay += deltaTime;
            if (m_rebuild.m_bakedDataDelay >= 0.5f)
            {
                m_rebuild.m_bakedDataDirty = false;
                m_rebuild.m_bakedDataDelay = 0.0f;
                if (m_rebuild.m_workingMeshUnwritten)
                {
                    SerializeWhiteBox(); // a preview left the working mesh out of the layer's stream
                }
                RebuildBakedRenderData();
                // The bake writes SERIALIZED members; without a dirty mark the prefab state
                // (what play-in-editor spawns from) would keep the stale previous bake.
                AzToolsFramework::ScopedUndoBatch undoBatch("White Box Bake");
                undoBatch.MarkEntityDirty(GetEntityId());
            }
        }
    }

    void EditorWhiteBoxComponent::RebuildWhiteBox()
    {
        EvaluateLiveBoolean(); // refresh m_displayMesh so render/physics/bounds use the latest result
        RebuildCombinedMesh(); // fold the stamp/grid layer into the mesh used for output
        RebuildRenderMesh();
        RebuildPhysicsMesh();

        // Recompute the game-mode bake caches SYNCHRONOUSLY for discrete edits: play-in-editor
        // spawns the game entity from the SERIALIZED (prefab) state, which is captured the
        // moment the edit marks the entity dirty - a debounced bake would land AFTER that
        // capture, leaving game mode one edit behind. RebuildWhiteBox only runs per user
        // gesture, so the cost is acceptable; the per-frame live-boolean drag path (OnTick)
        // bypasses RebuildWhiteBox and keeps the debounce.
        m_rebuild.m_bakedDataDirty = false;
        m_rebuild.m_bakedDataDelay = 0.0f;
        m_rebuild.m_physicsPending = false; // cooked synchronously just above
        RebuildBakedRenderData();
    }

    void EditorWhiteBoxComponent::RebuildWhiteBoxDeferred()
    {
        // The interactive half of RebuildWhiteBox: the geometry and what is on screen update now, while
        // the collider cook and the game-mode bake - the two expensive steps, and the two nothing is
        // looking at mid-drag - are queued for OnTick. Their timers restart on every call, so a stream
        // of edits pays for them once, after the stream stops.
        EvaluateLiveBoolean();
        RebuildCombinedMesh();

        m_rebuild.m_streaming = true;
        RebuildRenderMesh();
        m_rebuild.m_streaming = false;

        m_rebuild.m_physicsPending = true;
        m_rebuild.m_physicsTimer = 0.0f;
        m_rebuild.m_bakedDataDirty = true;
        m_rebuild.m_bakedDataDelay = 0.0f;
    }

    void EditorWhiteBoxComponent::BuildGameEntity(AZ::Entity* gameEntity)
    {
        auto* whiteBoxComponent = gameEntity->CreateComponent<WhiteBoxComponent>();
        if (whiteBoxComponent == nullptr)
        {
            return;
        }

        // Every fallback below reads the live render data, which on a never-activated clone exists
        // only in the serialized blobs.
        EnsureRenderDataUnpacked();

        // note: it is important no edit time only functions are called here as BuildGameEntity
        // will be called by the Asset Processor when creating dynamic slices

        // The bake caches are recomputed debounced on tick; if an edit landed within the debounce
        // window, flush them now so the game entity is built from current data. (Safe on clones:
        // with no live meshes the rebuild simply keeps the serialized caches.)
        if (m_rebuild.m_bakedDataDirty)
        {
            m_rebuild.m_bakedDataDirty = false;
            RebuildBakedRenderData();
        }

        // Bake the base (un-boolean) render geometry. Prefer the live base mesh, but fall back
        // to the cached/serialized BASE render data (not m_renderData - that is the evaluated
        // mesh and would be the CUT result when the live boolean is on, which would make the
        // "base" variant identical to the boolean one).
        if (WhiteBoxMesh* baseMesh = GetWhiteBoxMesh())
        {
            // Per-layer tint / inverted-normal layers must bake the coloured (winding-flipped) faces
            // so the effect survives into game mode; otherwise use the plain CSG-combined faces.
            if (PerLayerRenderActive())
            {
                whiteBoxComponent->GenerateWhiteBoxMesh(BuildColoredRenderData(baseMesh));
            }
            else
            {
                // Full combine (all visible layers, transforms and winding flips) so the game
                // entity matches the editor view - CombinedWithGrid alone only covered the
                // active layer and ignored Invert Normals.
                const Api::WhiteBoxMeshPtr combined = BuildCombined(baseMesh);
                whiteBoxComponent->GenerateWhiteBoxMesh(
                    CreateWhiteBoxRenderData(combined ? *combined : *baseMesh, m_material));
            }
        }
        else if (!m_bakedBaseRenderData.m_faces.empty())
        {
            whiteBoxComponent->GenerateWhiteBoxMesh(m_bakedBaseRenderData);
        }
        else
        {
            whiteBoxComponent->GenerateWhiteBoxMesh(m_renderData);
        }

        // Hand the runtime the BAKED physics render data (used only for the "Draw Collider"
        // debug wireframe; the real body is the cooked collider config). This data already
        // honours each layer's Collision flag - an empty result means "no collision", so it
        // must NOT fall back to the full visual geometry (m_renderData), which is exactly what
        // made the runtime collider wireframe show non-collidable layers. m_physicsBaked (now
        // serialized) is true whenever the filtered physics geometry has been baked, so it is
        // trustworthy even on the clone/asset-processor build; an empty bake then correctly means
        // "collision off" rather than "missing". Only genuinely legacy data (m_physicsBaked false)
        // falls back to the visual geometry.
        if (m_physicsBaked)
        {
            whiteBoxComponent->SetPhysicsGeometryData(m_bakedPhysicsBaseRenderData);
        }
        else
        {
            // Legacy data with no physics bake: fall back to the visual geometry.
            whiteBoxComponent->SetPhysicsGeometryData(m_renderData);
        }
        // Also bake the boolean-evaluated variant. The CSG boolean can only be computed in
        // the Editor (the Manifold/OpenMesh backed Tool API is not linked into the runtime),
        // so we pre-bake both variants here. Use the cached display mesh (evaluated during
        // editing) rather than re-evaluating: the boolean source entity id is not resolvable
        // during the game-mode / spawnable build, so a fresh evaluation here returns nothing.
        // At runtime the component toggles between the variants via the live-boolean
        // parameter (see WhiteBoxComponent::SetLiveBoolean / BakeWhiteBox).
        // Supply the boolean render variant from the (serialized) cached render data. Prefer a
        // freshly built one from the live display mesh, but fall back to the cache so this works
        // even when BuildGameEntity runs on a clone (where m_displayMesh is null but the cached
        // render data survives via serialization).
        WhiteBoxRenderData booleanRenderData;
        if (WhiteBoxMesh* displayMesh = GetLiveBooleanDisplayMesh())
        {
            if (PerLayerRenderActive())
            {
                booleanRenderData = BuildColoredRenderData(displayMesh);
            }
            else
            {
                const Api::WhiteBoxMeshPtr combined = CombinedWithGrid(displayMesh);
                booleanRenderData = CreateWhiteBoxRenderData(combined ? *combined : *displayMesh, m_material);
            }
        }
        else
        {
            booleanRenderData = m_bakedBooleanRenderData;
        }

        if (!booleanRenderData.m_faces.empty())
        {
            whiteBoxComponent->SetBooleanRenderData(booleanRenderData);
            // A single-source target starts live only if its Live flag is set; a global-boolean
            // target shows its composed result by default (the manual refresh is the "apply").
            whiteBoxComponent->SetLiveBooleanState(true, m_boolean.m_live || m_globalBooleanActive);

            if (!m_bakedPhysicsBooleanRenderData.m_faces.empty()) {
                whiteBoxComponent->SetBooleanPhysicsGeometryData(m_bakedPhysicsBooleanRenderData);
            }
        }
        else
        {
            whiteBoxComponent->SetLiveBooleanState(false, false);
        }
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::GetWhiteBoxMesh()
    {
        if (WhiteBoxMesh* whiteBox = m_editorMeshAsset->GetWhiteBoxMesh())
        {
            return whiteBox;
        }

        return m_whiteBox.get();
    }

    void EditorWhiteBoxComponent::OnWhiteBoxMeshModified()
    {
        // if using an asset, notify other editor mesh assets using the same id that
        // the asset has been modified, this will in turn cause all components to update
        // their render and physics meshes
        if (m_editorMeshAsset->InUse())
        {
            WhiteBoxMeshAssetNotificationBus::Event(
                m_editorMeshAsset->GetWhiteBoxMeshAssetId(),
                &WhiteBoxMeshAssetNotificationBus::Events::OnWhiteBoxMeshAssetModified,
                m_editorMeshAsset->GetWhiteBoxMeshAsset());
        }
        // otherwise, update the render and physics mesh immediately
        else
        {
            RebuildWhiteBox();
        }
    }

    void EditorWhiteBoxComponent::SetDefaultShape(const DefaultShapeType defaultShape)
    {
        m_defaultShape = defaultShape;
        OnDefaultShapeChange();
    }

    AZ::Crc32 EditorWhiteBoxComponent::CreateChildLayer()
    {
        // All of the entity/component-mode work lives in the standalone layer utility.
        CreateChildWhiteBoxLayer(GetEntityId());
        return AZ::Edit::PropertyRefreshLevels::None;
    }

    AZ::Crc32 EditorWhiteBoxComponent::OnOpenPane()
    {
        // All White Box editing UI lives in the dockable pane - this button is the only
        // affordance left on the component card (besides entering component mode).
        AzToolsFramework::OpenViewPane("White Box");
        return AZ::Edit::PropertyRefreshLevels::None;
    }

    void EditorWhiteBoxComponent::EnterComponentMode()
    {
        // Enter component mode through the standard "edit selected components of this type" request
        // (the same path the component card's Edit button uses). This routes through each
        // component's ComponentModeDelegate, so the card's enter/exit affordance (the little arrows)
        // stays in sync - issuing BeginComponentMode directly does not update the delegate and the
        // exit button then disappears. The entity must be selected first.
        namespace Cmf = AzToolsFramework::ComponentModeFramework;
        Cmf::ComponentModeSystemRequestBus::Broadcast(
            &Cmf::ComponentModeSystemRequests::AddSelectedComponentModesOfType,
            azrtti_typeid<EditorWhiteBoxComponent>());
    }
} // namespace WhiteBox
