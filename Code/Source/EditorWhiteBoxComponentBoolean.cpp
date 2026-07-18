/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * EditorWhiteBoxComponent - ENTITY BOOLEAN translation unit: one-shot Apply Boolean, the
 * live (non-destructive) boolean evaluation, the debounced game-mode bake caches and the
 * boolean source listener.
 */

#include "EditorWhiteBoxComponent.h"

#include "Util/WhiteBoxMeshUtil.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityHelpers.h>

namespace WhiteBox
{
    void EditorWhiteBoxComponent::ApplyBoolean()
    {
        if (!m_boolean.m_sourceEntity.IsValid() || m_boolean.m_sourceEntity == GetEntityId())
        {
            AZ_Warning("EditorWhiteBoxComponent", false, "Boolean Source is not set (or is this same entity).");
            return;
        }

        WhiteBoxMesh* freeform = GetWhiteBoxMesh();
        if (freeform == nullptr)
        {
            return;
        }

        AZ::Entity* sourceEntity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            sourceEntity, &AZ::ComponentApplicationRequests::FindEntity, m_boolean.m_sourceEntity);
        if (sourceEntity == nullptr)
        {
            AZ_Warning("EditorWhiteBoxComponent", false, "Boolean Source entity could not be found.");
            return;
        }

        const auto sourceComponents = sourceEntity->FindComponents<EditorWhiteBoxComponent>();
        if (sourceComponents.empty())
        {
            AZ_Warning("EditorWhiteBoxComponent", false, "Boolean Source entity has no White Box component.");
            return;
        }
        // Use the source's EVALUATED mesh so its stamped cubes participate too, not just its freeform.
        WhiteBoxMesh* sourceMesh = sourceComponents[0]->GetEvaluatedWhiteBoxMesh();
        if (sourceMesh == nullptr || Api::MeshFaceHandles(*sourceMesh).empty())
        {
            AZ_Warning("EditorWhiteBoxComponent", false, "Boolean Source mesh is empty.");
            return;
        }

        // Operate on the COMBINED mesh (freeform + stamped-cube grids) so the boolean affects the whole
        // visible solid. For a pure stamped-cube entity the freeform is empty and the cubes live in the
        // (manifold, CSG-merged) grid meshes, so booleaning the freeform alone did nothing.
        Api::WhiteBoxMeshPtr combined = CombinedWithGrid(freeform);
        WhiteBoxMesh* targetMesh = combined ? combined.get() : freeform;
        if (Api::MeshFaceHandles(*targetMesh).empty())
        {
            AZ_Warning("EditorWhiteBoxComponent", false, "This White Box mesh is empty (nothing to boolean).");
            return;
        }

        // Bring the source mesh from its local space into this entity's local space:
        // operandTransform = thisWorldFromLocal^-1 * sourceWorldFromLocal.
        AZ::Transform thisWorldTM = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(thisWorldTM, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
        AZ::Transform sourceWorldTM = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(sourceWorldTM, m_boolean.m_sourceEntity, &AZ::TransformBus::Events::GetWorldTM);
        const AZ::Transform operandTransform = thisWorldTM.GetInverse() * sourceWorldTM;

        AzToolsFramework::ScopedUndoBatch undoBatch("White Box Boolean");

        if (!Api::ApplyMeshBoolean(*targetMesh, *sourceMesh, operandTransform, m_boolean.m_operation, m_csgSolver))
        {
            AZ_Warning(
                "EditorWhiteBoxComponent", false,
                "White Box boolean produced no result (the meshes may not overlap, or are not closed).");
            return;
        }

        Api::CalculateNormals(*targetMesh);
        Api::CalculatePlanarUVs(*targetMesh);

        // If we booleaned the combined solid, bake the result back into the freeform mesh and clear the
        // now-consumed stamp/grid layers (their geometry is folded into the boolean result).
        if (combined)
        {
            Api::WhiteBoxMeshStream stream;
            Api::WriteMesh(*targetMesh, stream);
            Api::ReadMesh(*freeform, stream);

            m_gridMesh = Api::CreateWhiteBoxMesh();
            Api::WriteMesh(*m_gridMesh, m_voxel.m_legacyGridData);
            m_voxel.m_legacyGridMergedData.clear();
            m_voxel.m_cells.clear();
            m_voxel.m_sizes.clear();
            m_voxel.m_legacyMerged.clear();
        }

        SerializeWhiteBox();
        RebuildWhiteBox();
        undoBatch.MarkEntityDirty(GetEntityId());

        // Optionally tidy up the source entity once it has been consumed.
        if (m_boolean.m_sourceAfterApply == SourceAfterApply::Delete)
        {
            const AZ::EntityId sourceId = m_boolean.m_sourceEntity;
            m_boolean.m_sourceEntity = AZ::EntityId{}; // clear the now-dangling reference
            AzToolsFramework::ToolsApplicationRequestBus::Broadcast(
                &AzToolsFramework::ToolsApplicationRequests::DeleteEntityById, sourceId);
        }
        else if (m_boolean.m_sourceAfterApply == SourceAfterApply::Hide)
        {
            AzToolsFramework::SetEntityVisibility(m_boolean.m_sourceEntity, false);
        }
    }

    WhiteBoxMesh* EditorWhiteBoxComponent::GetLiveBooleanDisplayMesh()
    {
        return m_displayMesh.get();
    }

    Api::WhiteBoxMeshPtr EditorWhiteBoxComponent::EvaluateBooleanMesh(bool physicsPass)
    {
        if (!m_boolean.m_sourceEntity.IsValid() || m_boolean.m_sourceEntity == GetEntityId())
        {
            return nullptr;
        }

        WhiteBoxMesh* baseMesh = GetWhiteBoxMesh();
        if (baseMesh == nullptr)
        {
            return nullptr;
        }

        AZ::Entity* sourceEntity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            sourceEntity, &AZ::ComponentApplicationRequests::FindEntity, m_boolean.m_sourceEntity);
        if (sourceEntity == nullptr)
        {
            return nullptr;
        }
        const auto sourceComponents = sourceEntity->FindComponents<EditorWhiteBoxComponent>();
        if (sourceComponents.empty())
        {
            return nullptr;
        }
        WhiteBoxMesh* sourceMesh = sourceComponents[0]->GetEvaluatedWhiteBoxMesh();
        if (sourceMesh == nullptr)
        {
            return nullptr;
        }

        AZ::Transform thisWorldTM = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(thisWorldTM, GetEntityId(), &AZ::TransformBus::Events::GetWorldTM);
        AZ::Transform sourceWorldTM = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(sourceWorldTM, m_boolean.m_sourceEntity, &AZ::TransformBus::Events::GetWorldTM);
        const AZ::Transform operandTransform = thisWorldTM.GetInverse() * sourceWorldTM;

        // Evaluate into a fresh mesh so the editable base is never modified. By default the cut
        // applies to the WHOLE combined geometry (every visible layer, its grid/stamps and its
        // transform); "Affect only the active layer" restricts it to the active layer's base mesh.
        Api::WhiteBoxMeshPtr evaluated;
        if (m_boolean.m_affectActiveOnly)
        {
            evaluated = Api::CloneMesh(*baseMesh);
        }
        else
        {
            // Pass the physicsPass flag down to BuildCombined!
            evaluated = BuildCombined(baseMesh, physicsPass); // full base combined (no live boolean -> no recursion)
            if (!evaluated)
            {
                evaluated = Api::CloneMesh(*baseMesh);
            }
        }
        if (!evaluated)
        {
            return nullptr;
        }
        if (Api::ApplyMeshBoolean(*evaluated, *sourceMesh, operandTransform, m_boolean.m_operation, m_csgSolver))
        {
            Api::CalculateNormals(*evaluated);
            Api::CalculatePlanarUVs(*evaluated);
            return evaluated;
        }
        // on failure (no overlap) return null -> callers fall back to the base mesh.
        return nullptr;
    }

    void EditorWhiteBoxComponent::EvaluateLiveBoolean()
    {
        // Always evaluate the boolean whenever a source is set, independent of the live flag,
        // so the baked game-mode boolean variant exists even when the live boolean is off.
        // This is what lets runtime Lua toggle the boolean on from a base start. The live flag
        // only affects what EvaluatedMesh() returns (what is displayed / used by the edit-time
        // collider); the game entity always receives both variants when a source is present.
        m_displayMesh = EvaluateBooleanMesh();
        m_physicsDisplayMesh = EvaluateBooleanMesh(true); // Physics pass
        
        // The game-mode bake caches (m_bakedBaseRenderData / m_bakedBooleanRenderData) each cost a
        // FULL recombine of every layer, so computing them inline here made every edit (and every
        // frame of a live-boolean drag) pay for the game-mode bake. Recompute them debounced on
        // tick instead (and on demand in BuildGameEntity); they only need to be current when the
        // level saves or a game entity is built.
        m_rebuild.m_bakedDataDirty = true;
        m_rebuild.m_bakedDataDelay = 0.0f;
    }

    void EditorWhiteBoxComponent::RebuildBakedRenderData()
    {
        // Cache the true (uncut) base render data (serialized) so the game-mode bake always has
        // the base variant, even on a clone where GetWhiteBoxMesh() is null. Use the full combined
        // base (all visible layers, grids and transforms) so the bake matches the editor view.
        if (WhiteBoxMesh* baseMesh = GetWhiteBoxMesh())
        {
            // --- VISUAL BAKE ---
            if (PerLayerRenderActive()) {
                m_bakedBaseRenderData = BuildColoredRenderData(baseMesh);
            } else {
                const Api::WhiteBoxMeshPtr combined = BuildCombined(baseMesh, false);
                m_bakedBaseRenderData = CreateWhiteBoxRenderData(combined ? *combined : *baseMesh, m_material);
            }

            // --- PHYSICS BAKE (ADD THIS) ---
            // Physics doesn't care about per-layer vertex colors, just raw geometry without collision=false layers.
            // BuildCombined(physicsPass=true) never returns null, so an empty result genuinely means "no
            // collidable layers" (collision disabled) rather than "fall back to the full base mesh".
            const Api::WhiteBoxMeshPtr physicsCombined = BuildCombined(baseMesh, true);
            m_bakedPhysicsBaseRenderData =
                physicsCombined ? CreateWhiteBoxRenderData(*physicsCombined, m_material) : WhiteBoxRenderData{};
            // Mark that the physics geometry has been baked (even when empty) so the game-mode build can
            // trust an empty result as "collision off" instead of falling back to the full visual mesh.
            m_physicsBaked = true;
        }

        // Cache the boolean-evaluated render data (serialized) so the game-mode bake can supply the
        // boolean render variant even when BuildGameEntity runs on a cloned entity (m_displayMesh
        // null). In whole-mesh mode m_displayMesh is already the full combined result; in
        // active-only mode fold the grids/other layers around the active boolean result.
        if (m_displayMesh)
        {
            if (PerLayerRenderActive())
            {
                m_bakedBooleanRenderData = BuildColoredRenderData(m_displayMesh.get());
            }
            else
            {
                const Api::WhiteBoxMeshPtr combined =
                    m_boolean.m_affectActiveOnly ? BuildCombined(m_displayMesh.get()) : nullptr;
                m_bakedBooleanRenderData =
                    CreateWhiteBoxRenderData(combined ? *combined : *m_displayMesh, m_material);
            }
            // --- PHYSICS BAKE (ADD THIS) ---
            if (m_physicsDisplayMesh)
            {
                const Api::WhiteBoxMeshPtr physicsCombined =
                    m_boolean.m_affectActiveOnly ? BuildCombined(m_physicsDisplayMesh.get(), true) : nullptr;
                m_bakedPhysicsBooleanRenderData =
                    CreateWhiteBoxRenderData(physicsCombined ? *physicsCombined : *m_physicsDisplayMesh, m_material);
            }
        }
        else if (!m_boolean.m_sourceEntity.IsValid() || m_boolean.m_sourceEntity == GetEntityId())
        {
            // no boolean source -> there is no boolean variant; clear the cache
            m_bakedBooleanRenderData = WhiteBoxRenderData{};
            m_bakedPhysicsBooleanRenderData = WhiteBoxRenderData{};
        }
        // else: a source is set but evaluation transiently failed (e.g. the source entity is
        // not active yet at load time). Keep any previously cached boolean render data.
    }

    void EditorWhiteBoxComponent::UpdateBooleanSourceListener()
    {
        m_booleanSourceListener.BusDisconnect();
        // listen whenever a source is set (not just while live) so the baked boolean stays
        // current when the source entity moves, even with the live boolean off.
        if (m_boolean.m_sourceEntity.IsValid() && m_boolean.m_sourceEntity != GetEntityId())
        {
            m_booleanSourceListener.m_owner = this;
            m_booleanSourceListener.BusConnect(m_boolean.m_sourceEntity);
        }
    }

    AZ::u32 EditorWhiteBoxComponent::OnLiveBooleanChange()
    {
        UpdateBooleanSourceListener();
        RebuildWhiteBox(); // evaluates the live boolean (or reverts to base) + rebuilds render/physics
        return AZ::Edit::PropertyRefreshLevels::ValuesOnly;
    }

    AZ::u32 EditorWhiteBoxComponent::OnBooleanSourceChange()
    {
        OnLiveBooleanChange();
        // The Boolean group's visibility depends on whether a source is set. EntityId
        // fields don't reliably honor the ChangeNotify refresh-level return value, so
        // force the property tree to rebuild explicitly so the group shows/hides.
        AzToolsFramework::ToolsApplicationEvents::Bus::Broadcast(
            &AzToolsFramework::ToolsApplicationEvents::InvalidatePropertyDisplay,
            AzToolsFramework::Refresh_EntireTree);
        return AZ::Edit::PropertyRefreshLevels::EntireTree;
    }

    AZ::Crc32 EditorWhiteBoxComponent::BooleanGroupVisibility() const
    {
        return m_boolean.m_sourceEntity.IsValid() ? AZ::Edit::PropertyVisibility::Show
                                               : AZ::Edit::PropertyVisibility::Hide;
    }

    void EditorWhiteBoxComponent::BooleanSourceListener::OnTransformChanged(
        const AZ::Transform& /*local*/, const AZ::Transform& /*world*/)
    {
        if (m_owner != nullptr)
        {
            // Source moved: rebuild ONCE on the next tick. Transform notifications can arrive
            // many times per frame while the source entity is dragged, and each rebuild costs a
            // full recombine + CSG boolean - rebuilding per notification drove the framerate to
            // zero once real geometry (e.g. cube stamps) existed.
            m_owner->m_rebuild.m_liveBooleanPending = true;
        }
    }
} // namespace WhiteBox
