/*
 * EditorWhiteBoxComponent - vertex snapping source.
 *
 * Publishes the mesh's vertices to the editor-wide vertex snapper provided by the
 * SnapApi contract (SnapApi::VertexSourceRequestBus). This is the "provider" half of
 * snapping; the "consumer" half lives in the viewport modifiers, which ask the snapper for
 * a target while a drag is in progress.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include "EditorWhiteBoxComponent.h"

#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/Component/NonUniformScaleBus.h>
#include <AzCore/Debug/Profiler.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace WhiteBox
{
    AZ::Vector3 EditorWhiteBoxComponent::SnapWorldPosition(
        const AZ::Vector3& localPosition, const AZ::Vector3& nonUniformScale) const
    {
        // The world transform carries uniform scale only; a Non-Uniform Scale component is applied
        // separately. Mirrors what GetWorldBounds does, so candidates and bounds agree.
        return m_worldFromLocal.TransformPoint(localPosition * nonUniformScale);
    }

    void EditorWhiteBoxComponent::RebuildSnapVertexCache() const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        // clear() keeps the capacity, so a mesh being dragged re-fills the same allocation every
        // frame rather than churning the heap.
        m_snapVertexCache.clear();
        m_snapVertexCacheBounds = AZ::Aabb::CreateNull();
        m_snapVertexCacheValid = true;

        // EvaluatedMesh is the mesh used for render/collision/bounds/selection, so snapping
        // targets what the user actually sees (including the voxel/grid layer and any live
        // boolean result). It can be null while entities are still activating.
        WhiteBoxMesh* mesh = const_cast<EditorWhiteBoxComponent*>(this)->EvaluatedMesh();
        if (mesh == nullptr)
        {
            return;
        }

        AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
        AZ::NonUniformScaleRequestBus::EventResult(
            nonUniformScale, GetEntityId(), &AZ::NonUniformScaleRequests::GetScale);

        const Api::VertexHandles vertexHandles = Api::MeshVertexHandles(*mesh);
        m_snapVertexCache.reserve(vertexHandles.size());

        const AZ::EntityId entityId = GetEntityId();
        for (const Api::VertexHandle vertexHandle : vertexHandles)
        {
            const AZ::Vector3 worldPosition =
                SnapWorldPosition(Api::VertexPosition(*mesh, vertexHandle), nonUniformScale);

            // The handle index is carried through as the opaque source index so a sub-element
            // tool can exclude the very vertex it is dragging (see SnapQueryConfig).
            m_snapVertexCache.emplace_back(worldPosition, entityId, aznumeric_cast<AZ::s64>(vertexHandle.Index()));
            m_snapVertexCacheBounds.AddPoint(worldPosition);
        }
    }

    AZ::Aabb EditorWhiteBoxComponent::GetSnapVertexBounds() const
    {
        // Taken from the snap cache rather than GetWorldBounds. They describe the same volume,
        // but GetWorldBounds recomputes GetLocalBounds (a full vertex walk) whenever the mesh
        // changes - which during a drag is every frame - so using it here doubled the per-query
        // vertex work. Returns a null Aabb while the mesh is not ready, which is exactly the
        // "reject me" signal the snapper expects.
        if (!m_snapVertexCacheValid)
        {
            RebuildSnapVertexCache();
        }

        return m_snapVertexCacheBounds;
    }

    void EditorWhiteBoxComponent::RebuildSnapEdgeCache() const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        m_snapEdgeCache.clear();
        m_snapEdgeCacheValid = true;

        WhiteBoxMesh* mesh = const_cast<EditorWhiteBoxComponent*>(this)->EvaluatedMesh();
        if (mesh == nullptr)
        {
            return;
        }

        AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
        AZ::NonUniformScaleRequestBus::EventResult(
            nonUniformScale, GetEntityId(), &AZ::NonUniformScaleRequests::GetScale);

        // Polygon border edges only - the wireframe the user actually sees. MeshEdgeHandles would
        // also hand back the internal triangulation edges, and snapping to the diagonal across a
        // quad is never what anyone means by "snap to edge".
        const Api::EdgeHandles edgeHandles = Api::MeshPolygonEdgeHandles(*mesh);
        m_snapEdgeCache.reserve(edgeHandles.size());

        const AZ::EntityId entityId = GetEntityId();
        for (const Api::EdgeHandle edgeHandle : edgeHandles)
        {
            const AZStd::array<AZ::Vector3, 2> positions = Api::EdgeVertexPositions(*mesh, edgeHandle);

            m_snapEdgeCache.emplace_back(
                SnapWorldPosition(positions[0], nonUniformScale), SnapWorldPosition(positions[1], nonUniformScale),
                entityId, aznumeric_cast<AZ::s64>(edgeHandle.Index()));
        }
    }

    void EditorWhiteBoxComponent::RebuildSnapEdgeMidpointCache() const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        m_snapEdgeMidpointCache.clear();
        m_snapEdgeMidpointCacheValid = true;

        WhiteBoxMesh* mesh = const_cast<EditorWhiteBoxComponent*>(this)->EvaluatedMesh();
        if (mesh == nullptr)
        {
            return;
        }

        AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
        AZ::NonUniformScaleRequestBus::EventResult(
            nonUniformScale, GetEntityId(), &AZ::NonUniformScaleRequests::GetScale);

        const Api::EdgeHandles edgeHandles = Api::MeshPolygonEdgeHandles(*mesh);
        m_snapEdgeMidpointCache.reserve(edgeHandles.size());

        const AZ::EntityId entityId = GetEntityId();
        for (const Api::EdgeHandle edgeHandle : edgeHandles)
        {
            m_snapEdgeMidpointCache.emplace_back(
                SnapWorldPosition(Api::EdgeMidpoint(*mesh, edgeHandle), nonUniformScale), entityId,
                aznumeric_cast<AZ::s64>(edgeHandle.Index()));
        }
    }

    void EditorWhiteBoxComponent::RebuildSnapFaceCenterCache() const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        m_snapFaceCenterCache.clear();
        m_snapFaceCenterCacheValid = true;

        WhiteBoxMesh* mesh = const_cast<EditorWhiteBoxComponent*>(this)->EvaluatedMesh();
        if (mesh == nullptr)
        {
            return;
        }

        AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
        AZ::NonUniformScaleRequestBus::EventResult(
            nonUniformScale, GetEntityId(), &AZ::NonUniformScaleRequests::GetScale);

        // Polygons, not triangles - the centre of a quad wall, not the centre of one of the two
        // triangles it happens to be built from.
        const Api::PolygonHandles polygonHandles = Api::MeshPolygonHandles(*mesh);
        m_snapFaceCenterCache.reserve(polygonHandles.size());

        const AZ::EntityId entityId = GetEntityId();
        AZ::s64 polygonIndex = 0;
        for (const Api::PolygonHandle& polygonHandle : polygonHandles)
        {
            m_snapFaceCenterCache.emplace_back(
                SnapWorldPosition(Api::PolygonMidpoint(*mesh, polygonHandle), nonUniformScale), entityId,
                polygonIndex++);
        }
    }

    void EditorWhiteBoxComponent::CollectSnapEdges(
        const SnapApi::SnapQueryVolume& volume, AZStd::vector<SnapApi::SnapEdge>& out) const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        if (!m_snapEdgeCacheValid)
        {
            RebuildSnapEdgeCache();
        }

        for (const SnapApi::SnapEdge& edge : m_snapEdgeCache)
        {
            if (volume.IntersectsSegment(edge.m_start, edge.m_end))
            {
                out.push_back(edge);
            }
        }
    }

    void EditorWhiteBoxComponent::CollectSnapEdgeMidpoints(
        const SnapApi::SnapQueryVolume& volume, AZStd::vector<SnapApi::SnapVertex>& out) const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        if (!m_snapEdgeMidpointCacheValid)
        {
            RebuildSnapEdgeMidpointCache();
        }

        for (const SnapApi::SnapVertex& midpoint : m_snapEdgeMidpointCache)
        {
            if (volume.Contains(midpoint.m_worldPosition))
            {
                out.push_back(midpoint);
            }
        }
    }

    void EditorWhiteBoxComponent::CollectSnapFaceCenters(
        const SnapApi::SnapQueryVolume& volume, AZStd::vector<SnapApi::SnapVertex>& out) const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        if (!m_snapFaceCenterCacheValid)
        {
            RebuildSnapFaceCenterCache();
        }

        for (const SnapApi::SnapVertex& center : m_snapFaceCenterCache)
        {
            if (volume.Contains(center.m_worldPosition))
            {
                out.push_back(center);
            }
        }
    }

    void EditorWhiteBoxComponent::CollectSnapVertices(
        const SnapApi::SnapQueryVolume& volume, AZStd::vector<SnapApi::SnapVertex>& out) const
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);

        if (!m_snapVertexCacheValid)
        {
            RebuildSnapVertexCache();
        }

        // Tight loop over contiguous, already-transformed positions - no allocation, no EBus
        // traffic, no per-vertex API calls. The cone test rejects all but a handful, so the
        // caller only has to project those few into screen space.
        for (const SnapApi::SnapVertex& vertex : m_snapVertexCache)
        {
            if (volume.Contains(vertex.m_worldPosition))
            {
                out.push_back(vertex);
            }
        }
    }
} // namespace WhiteBox
