/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxDrawShapeMode.h"

#include "EditorWhiteBoxComponentModeTypes.h"
#include "Viewport/WhiteBoxManipulatorBounds.h"
#include "Viewport/WhiteBoxViewportConstants.h"
#include "Util/WhiteBoxMathUtil.h"

#include <AzCore/Math/IntersectSegment.h>
#include <AzCore/Math/Plane.h>
#include <AzCore/Math/MathStringConversions.h>
#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzToolsFramework/Maths/TransformUtils.h>
#include <AzToolsFramework/Undo/UndoSystem.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>
#include <WhiteBox/EditorWhiteBoxComponentBus.h>
#include <WhiteBox/WhiteBoxToolApi.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/ComponentMode/EditorComponentModeBus.h>

#include <AzFramework/Render/GeometryIntersectionStructures.h>
#include <AzFramework/Visibility/EntityVisibilityBoundsUnionSystem.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <AzFramework/Render/Intersector.h>           // IntersectorBus / IntersectorInterface
#include <AzFramework/Render/GeometryIntersectionStructures.h>  // RayRequest / RayResult
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/ActionManager/HotKey/HotKeyManagerInterface.h>
#include <AzToolsFramework/API/ComponentModeCollectionInterface.h>
#include <AzToolsFramework/Editor/ActionManagerIdentifiers/EditorContextIdentifiers.h>
#include <AzToolsFramework/ViewportUi/ViewportUiRequestBus.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include "EditorWhiteBoxComponent.h"
#include <cmath>


namespace WhiteBox
{
    AZ_CLASS_ALLOCATOR_IMPL(DrawShapeMode, AZ::SystemAllocator)
    // Helper: build a right-handed basis with `up` as the Z-equivalent (the normal).
    void BasisFromNormal(const AZ::Vector3& n, AZ::Vector3& right, AZ::Vector3& fwd, AZ::Vector3& up)
    {
        up = n.GetNormalized();

        const AZ::Vector3 ref = (AZStd::abs(up.GetZ()) < 0.99f)
            ? AZ::Vector3::CreateAxisZ()
            : AZ::Vector3::CreateAxisY();   // use Y (not X) so a vertical normal still gives a stable frame

        right = ref.Cross(up).GetNormalized();
        fwd   = up.Cross(right).GetNormalized();
    }

    static const char* DrawShapeName(DrawShapeType shape)
    {
        switch (shape)
        {
        case DrawShapeType::Box:      return "Box";
        case DrawShapeType::Cylinder: return "Cylinder";
        case DrawShapeType::Pyramid:  return "Pyramid";
        case DrawShapeType::Cone:     return "Cone";
        default:                      return "Shape";
        }
    }

    // Add one (convex, planar) face from an ordered loop of vertex handles,
    // fan-triangulated and wound so its normal points AWAY from the solid centre
    // (outward). Robust for any convex polygon - no manual per-shape winding.
    static void AddOutwardFace(
        WhiteBoxMesh& whiteBox,
        const AZStd::vector<Api::VertexHandle>& handles,
        const AZStd::vector<AZ::Vector3>& localPositions,
        const AZStd::vector<AZ::u32>& loop,
        const AZ::Vector3& solidCentroid)
    {
        if (loop.size() < 3)
        {
            return;
        }

        AZ::Vector3 faceCentroid = AZ::Vector3::CreateZero();
        for (const AZ::u32 idx : loop)
        {
            faceCentroid += localPositions[idx];
        }
        faceCentroid /= aznumeric_cast<float>(loop.size());

        const AZ::Vector3 a = localPositions[loop[0]];
        const AZ::Vector3 b = localPositions[loop[1]];
        const AZ::Vector3 c = localPositions[loop[2]];
        const AZ::Vector3 normal = (b - a).Cross(c - a);
        const bool flip = normal.Dot(faceCentroid - solidCentroid) < 0.0f;

        Api::FaceVertHandlesList faces;
        faces.reserve(loop.size() - 2);
        for (size_t i = 1; i + 1 < loop.size(); ++i)
        {
            Api::VertexHandle v0 = handles[loop[0]];
            Api::VertexHandle v1 = handles[loop[i]];
            Api::VertexHandle v2 = handles[loop[i + 1]];
            if (flip)
            {
                AZStd::swap(v1, v2);
            }
            faces.push_back(Api::FaceVertHandles{ {v0, v1, v2} });
        }
        Api::AddPolygon(whiteBox, faces);
    }

    // Add a round cap as one polygon, triangulated as an "umbrella" from a centre
    // vertex (uniform triangles, no slivers - unlike a corner fan). The centre and
    // the spokes are interior to the polygon, so they don't render as edges; the
    // cap still reads as a clean disc but has good underlying topology.
    static void AddOutwardCap(
        WhiteBoxMesh& whiteBox,
        const AZStd::vector<Api::VertexHandle>& handles,
        const AZStd::vector<AZ::Vector3>& localPositions,
        const AZStd::vector<AZ::u32>& rim,
        const AZ::u32 centerIdx,
        const AZ::Vector3& solidCentroid)
    {
        const size_t n = rim.size();
        if (n < 3)
        {
            return;
        }

        const AZ::Vector3 a = localPositions[centerIdx];
        const AZ::Vector3 b = localPositions[rim[0]];
        const AZ::Vector3 c = localPositions[rim[1]];
        const AZ::Vector3 normal = (b - a).Cross(c - a);
        const bool flip = normal.Dot(localPositions[centerIdx] - solidCentroid) < 0.0f;

        Api::FaceVertHandlesList faces;
        faces.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            Api::VertexHandle v0 = handles[centerIdx];
            Api::VertexHandle v1 = handles[rim[i]];
            Api::VertexHandle v2 = handles[rim[(i + 1) % n]];
            if (flip)
            {
                AZStd::swap(v1, v2);
            }
            faces.push_back(Api::FaceVertHandles{ {v0, v1, v2} });
        }
        Api::AddPolygon(whiteBox, faces);
    }

    // Generate the N-gon footprint ring (world space) for the current shape:
    // round shapes inscribe the ellipse; angular shapes fill the rectangle (4 =
    // exact corners). ru/rv are the half-extent vectors along the two in-plane axes.
    static AZStd::vector<AZ::Vector3> ComputeFootprintRing(
        const AZ::Vector3& center, const AZ::Vector3& ru, const AZ::Vector3& rv, const bool round, const int sidesIn)
    {
        const int sides = AZ::GetClamp(sidesIn, 3, 256);
        AZStd::vector<AZ::Vector3> ring;
        ring.reserve(sides);
        if (round)
        {
            for (int i = 0; i < sides; ++i)
            {
                const float t = (AZ::Constants::TwoPi * static_cast<float>(i)) / static_cast<float>(sides);
                ring.push_back(center + ru * std::cos(t) + rv * std::sin(t));
            }
        }
        else
        {
            const float phase = AZ::Constants::Pi / static_cast<float>(sides);
            float maxX = 0.0f, maxY = 0.0f;
            for (int i = 0; i < sides; ++i)
            {
                const float t = phase + (AZ::Constants::TwoPi * static_cast<float>(i)) / static_cast<float>(sides);
                maxX = AZ::GetMax(maxX, std::abs(std::cos(t)));
                maxY = AZ::GetMax(maxY, std::abs(std::sin(t)));
            }
            for (int i = 0; i < sides; ++i)
            {
                const float t = phase + (AZ::Constants::TwoPi * static_cast<float>(i)) / static_cast<float>(sides);
                ring.push_back(center + ru * (std::cos(t) / maxX) + rv * (std::sin(t) / maxY));
            }
        }
        return ring;
    }

    // Build a closed shape solid into @p mesh from a footprint in the surface
    // plane (centre + in-plane axes uAxis/vAxis) extruded along @p up between the
    // base plane (centre + up*baseUp) and the top plane / apex (centre + up*topUp).
    // Shared by the draw-commit (visible shape) and the carve/add cutter so they
    // always produce the same geometry. All faces are wound outward.
    static void BuildShapeSolid(
        WhiteBoxMesh& mesh, const AZ::Transform& localFromWorld,
        const AZ::Vector3& center, const AZ::Vector3& uAxis, const AZ::Vector3& vAxis, const AZ::Vector3& up,
        const float baseUp, const float topUp, const DrawShapeType shapeType, const int sidesIn)
    {
        const bool pointed = (shapeType == DrawShapeType::Pyramid || shapeType == DrawShapeType::Cone);
        const bool round   = (shapeType == DrawShapeType::Cylinder || shapeType == DrawShapeType::Cone);
        const int  sides   = AZ::GetClamp(sidesIn, 3, 256);

        const AZ::Vector3 ru = uAxis * 0.5f;
        const AZ::Vector3 rv = vAxis * 0.5f;
        const AZ::Vector3 baseCenter = center + up * baseUp;
        const AZ::Vector3 topPos     = center + up * topUp;   // top-cap centre OR apex
        const AZ::Vector3 ext        = up * (topUp - baseUp);

        // footprint ring at the base plane
        const AZStd::vector<AZ::Vector3> baseWorld = ComputeFootprintRing(baseCenter, ru, rv, round, sides);

        const size_t n = baseWorld.size();
        const bool centerCap = (n > 4);

        AZStd::vector<AZ::Vector3> localPos;
        AZStd::vector<Api::VertexHandle> vh;
        localPos.reserve(n * 2 + 2);
        vh.reserve(n * 2 + 2);

        const auto addVertex = [&](const AZ::Vector3& world) -> AZ::u32
        {
            const AZ::Vector3 local = localFromWorld.TransformPoint(world);
            localPos.push_back(local);
            vh.push_back(Api::AddVertex(mesh, local));
            return aznumeric_cast<AZ::u32>(vh.size() - 1);
        };

        AZStd::vector<AZ::u32> baseRing;
        baseRing.reserve(n);
        for (const AZ::Vector3& w : baseWorld) { baseRing.push_back(addVertex(w)); }

        if (pointed)
        {
            const AZ::u32 apex = addVertex(topPos);
            const AZ::u32 baseCenterIdx = centerCap ? addVertex(baseCenter) : 0u;

            AZ::Vector3 centroid = AZ::Vector3::CreateZero();
            for (const AZ::Vector3& p : localPos) { centroid += p; }
            centroid /= aznumeric_cast<float>(localPos.size());

            if (centerCap) { AddOutwardCap(mesh, vh, localPos, baseRing, baseCenterIdx, centroid); }
            else           { AddOutwardFace(mesh, vh, localPos, baseRing, centroid); }
            for (size_t i = 0; i < n; ++i)
            {
                const AZStd::vector<AZ::u32> tri = { baseRing[i], baseRing[(i + 1) % n], apex };
                AddOutwardFace(mesh, vh, localPos, tri, centroid);
            }
        }
        else
        {
            AZStd::vector<AZ::u32> topRing;
            topRing.reserve(n);
            for (const AZ::Vector3& w : baseWorld) { topRing.push_back(addVertex(w + ext)); }
            const AZ::u32 baseCenterIdx = centerCap ? addVertex(baseCenter)       : 0u;
            const AZ::u32 topCenterIdx  = centerCap ? addVertex(baseCenter + ext) : 0u;

            AZ::Vector3 centroid = AZ::Vector3::CreateZero();
            for (const AZ::Vector3& p : localPos) { centroid += p; }
            centroid /= aznumeric_cast<float>(localPos.size());

            if (centerCap)
            {
                AddOutwardCap(mesh, vh, localPos, baseRing, baseCenterIdx, centroid);
                AddOutwardCap(mesh, vh, localPos, topRing,  topCenterIdx,  centroid);
            }
            else
            {
                AddOutwardFace(mesh, vh, localPos, baseRing, centroid);
                AddOutwardFace(mesh, vh, localPos, topRing,  centroid);
            }
            for (size_t i = 0; i < n; ++i)
            {
                const size_t j = (i + 1) % n;
                const AZStd::vector<AZ::u32> quad = { baseRing[i], baseRing[j], topRing[j], topRing[i] };
                AddOutwardFace(mesh, vh, localPos, quad, centroid);
            }
        }
    }

    DrawShapeMode::DrawShapeMode(const AZ::EntityComponentIdPair& entityComponentIdPair)
        : m_entityComponentIdPair(entityComponentIdPair)
    {
        // Connect to the global viewport rendering bus
        AzFramework::ViewportDebugDisplayEventBus::Handler::BusConnect(AzToolsFramework::GetEntityContextId());
        // Receive numeric-depth keyboard actions dispatched to this component.
        EditorWhiteBoxDrawShapeModeRequestBus::Handler::BusConnect(entityComponentIdPair);
    }
    DrawShapeMode::~DrawShapeMode()
    {
        // Disconnect from the bus when mode is exited
        EditorWhiteBoxDrawShapeModeRequestBus::Handler::BusDisconnect();
        AzFramework::ViewportDebugDisplayEventBus::Handler::BusDisconnect();
    }
    AZ::Vector3 DrawShapeMode::RaycastToSurface(
        const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
        const AZ::Transform& worldFromLocal,
        const IntersectionAndRenderData& intersectionData,
        AZ::Vector3& outWorldNormal) const
    {
        const AZ::Vector3 rayOriginWorld = mouseInteraction.m_mousePick.m_rayOrigin;
        const AZ::Vector3 rayDirWorld    = mouseInteraction.m_mousePick.m_rayDirection;

        // Track the closest hit across every surface type so white box meshes,
        // other white box meshes, and ordinary scene geometry are all candidates
        // and all use a consistent flat (per-triangle) surface normal.
        float bestDist = AZStd::numeric_limits<float>::max();
        AZ::Vector3 bestHit = AZ::Vector3::CreateZero();
        AZ::Vector3 bestNormal = AZ::Vector3::CreateAxisZ();
        bool found = false;

        const auto consider = [&](const AZ::Vector3& worldHit, AZ::Vector3 worldNormal)
        {
            const float d = (worldHit - rayOriginWorld).Dot(rayDirWorld); // distance along the ray
            if (d < 0.0f || d >= bestDist)
            {
                return;
            }
            bestDist = d;
            bestHit = worldHit;
            worldNormal = worldNormal.GetNormalizedSafe();
            if (worldNormal.Dot(rayDirWorld) > 0.0f) // face back toward the camera
            {
                worldNormal = -worldNormal;
            }
            bestNormal = worldNormal;
            found = true;
        };

        // Ray-test a white box mesh's local triangles (flat normals) and report hits.
        const auto raycastWhiteBox = [&](WhiteBoxMesh& mesh, const AZ::Transform& meshWorldFromLocal)
        {
            const AZ::Transform localFromWorld = meshWorldFromLocal.GetInverse();
            const AZ::Vector3 lo = localFromWorld.TransformPoint(rayOriginWorld);
            const AZ::Vector3 ld = AzToolsFramework::TransformDirectionNoScaling(localFromWorld, rayDirWorld);
            const float rayLen = 100000.0f;
            const AZ::Vector3 le = lo + ld * rayLen;

            AZ::Intersect::SegmentTriangleHitTester hitTester(lo, le);
            for (const Api::Face& f : Api::MeshFaces(mesh))
            {
                float t = 0.0f;
                AZ::Vector3 n;
                if (hitTester.IntersectSegmentTriangle(f[0], f[1], f[2], n, t))
                {
                    const AZ::Vector3 localHit = lo + (le - lo) * t;
                    consider(
                        meshWorldFromLocal.TransformPoint(localHit),
                        AzToolsFramework::TransformDirectionNoScaling(meshWorldFromLocal, n));
                }
            }
        };

        // 1. the current white box mesh (its precomputed intersection data, flat normal)
        {
            const AZ::Transform localFromWorld = worldFromLocal.GetInverse();
            const AZ::Vector3 lo = localFromWorld.TransformPoint(rayOriginWorld);
            const AZ::Vector3 ld = AzToolsFramework::TransformDirectionNoScaling(localFromWorld, rayDirWorld);

            for (const auto& polyBound : intersectionData.m_whiteBoxIntersectionData.m_polygonBounds)
            {
                float dist = AZStd::numeric_limits<float>::max();
                int64_t triIdx = 0;
                if (IntersectRayPolygon(polyBound.m_bound, lo, ld, dist, triIdx))
                {
                    const AZ::Vector3 localHit = lo + ld * dist;
                    AZ::Vector3 localNormal = AZ::Vector3::CreateAxisZ();
                    const auto& tris = polyBound.m_bound.m_triangles;
                    const size_t base = static_cast<size_t>(triIdx) * 3;
                    if (base + 2 < tris.size())
                    {
                        localNormal =
                            (tris[base + 1] - tris[base + 0]).Cross(tris[base + 2] - tris[base + 0]).GetNormalizedSafe();
                    }
                    consider(
                        worldFromLocal.TransformPoint(localHit),
                        AzToolsFramework::TransformDirectionNoScaling(worldFromLocal, localNormal));
                }
            }
        }

        // 2. every OTHER white box mesh in the scene (not in the scene intersector)
        {
            const AZ::EntityId selfEntity = m_entityComponentIdPair.GetEntityId();
            AZ::ComponentApplicationBus::Broadcast(
                &AZ::ComponentApplicationRequests::EnumerateEntities,
                [&](AZ::Entity* entity)
                {
                    if (entity == nullptr || entity->GetId() == selfEntity)
                    {
                        return;
                    }
                    for (EditorWhiteBoxComponent* comp : entity->FindComponents<EditorWhiteBoxComponent>())
                    {
                        WhiteBoxMesh* mesh = comp->GetWhiteBoxMesh();
                        if (mesh == nullptr)
                        {
                            continue;
                        }
                        AZ::Transform worldTM = AZ::Transform::CreateIdentity();
                        AZ::TransformBus::EventResult(worldTM, entity->GetId(), &AZ::TransformBus::Events::GetWorldTM);
                        raycastWhiteBox(*mesh, worldTM);
                    }
                });
        }

        // 3. ordinary scene render geometry (non white box)
        {
            // Cast a parallel ray offset by `offset`; returns true + the world hit.
            const auto castScene = [&](const AZ::Vector3& offset, AZ::Vector3& outHit) -> bool
            {
                AzFramework::RenderGeometry::RayRequest request;
                request.m_startWorldPosition = rayOriginWorld + offset;
                request.m_endWorldPosition   = rayOriginWorld + offset + rayDirWorld * 100000.0f;
                request.m_onlyVisible        = true;

                AzFramework::RenderGeometry::RayResult rayResult;
                AzFramework::RenderGeometry::IntersectorBus::EventResult(
                    rayResult, AzToolsFramework::GetEntityContextId(),
                    &AzFramework::RenderGeometry::IntersectorInterface::RayIntersect, request);
                if (rayResult)
                {
                    outHit = rayResult.m_worldPosition;
                    return true;
                }
                return false;
            };

            AZ::Vector3 hit0;
            if (castScene(AZ::Vector3::CreateZero(), hit0))
            {
                // Derive the surface normal from world-space positions of two nearby
                // parallel rays. This reflects the true surface orientation regardless
                // of how the intersector reports its normal (which can come back in
                // model space, so it ignores the entity's rotation).
                const float dist = (hit0 - rayOriginWorld).GetLength();
                const float eps = AZ::GetMax(0.01f, dist * 0.004f);

                AZ::Vector3 perpA = rayDirWorld.Cross(AZ::Vector3::CreateAxisX());
                if (perpA.GetLengthSq() < 1e-6f)
                {
                    perpA = rayDirWorld.Cross(AZ::Vector3::CreateAxisY());
                }
                perpA.Normalize();
                const AZ::Vector3 perpB = rayDirWorld.Cross(perpA).GetNormalizedSafe();

                AZ::Vector3 hitA, hitB;
                AZ::Vector3 sceneNormal = AZ::Vector3::CreateAxisZ();
                if (castScene(perpA * eps, hitA) && castScene(perpB * eps, hitB))
                {
                    const AZ::Vector3 n = (hitA - hit0).Cross(hitB - hit0);
                    if (n.GetLengthSq() > 1e-10f)
                    {
                        sceneNormal = n.GetNormalizedSafe();
                    }
                    else
                    {
                        sceneNormal = -rayDirWorld; // degenerate (e.g. grazing) - face the camera
                    }
                }
                else
                {
                    sceneNormal = -rayDirWorld; // offset rays missed (near an edge) - face the camera
                }

                consider(hit0, sceneNormal);
            }
        }

        if (found)
        {
            outWorldNormal = bestNormal;
            return bestHit;
        }

        // 4. ground plane fallback
        outWorldNormal = AZ::Vector3::CreateAxisZ();
        const AZ::Vector3 planePos(0.f, 0.f, m_groundZ);
        const AZ::Vector3 planeNormal = AZ::Vector3::CreateAxisZ();
        float t = 0.f;
        AZ::Intersect::IntersectRayPlane(rayOriginWorld, rayDirWorld, planePos, planeNormal, t);
        if (t < 0.f) { t = 0.f; }
        return rayOriginWorld + rayDirWorld * t;
    }
    
    

    float DrawShapeMode::RaycastToHeightPlane(
        const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
        [[maybe_unused]] const AZ::Transform& worldFromLocal,
        const AZ::Vector3& baseCenterWorld) const
    {
        const AZ::Vector3 rayOriginWorld = mouseInteraction.m_mousePick.m_rayOrigin;
        const AZ::Vector3 rayDirWorld    = mouseInteraction.m_mousePick.m_rayDirection;
    
        const AZ::Vector3 up = m_surfaceNormal.GetNormalized();
    
        // A fallback in-plane vector, perpendicular to up.
        const AZ::Vector3 ref = (AZStd::abs(up.GetZ()) < 0.99f)
            ? AZ::Vector3::CreateAxisZ()
            : AZ::Vector3::CreateAxisX();
        const AZ::Vector3 inPlane = ref.Cross(up).GetNormalized();
    
        // Plane that contains `up` and faces the camera: remove the up-component of the ray dir.
        AZ::Vector3 planeNormal = rayDirWorld - up * rayDirWorld.Dot(up);
        if (planeNormal.GetLengthSq() < 0.0001f)
        {
            planeNormal = inPlane;
        }
        planeNormal.Normalize();
    
        float t = 0.f;
        AZ::Intersect::IntersectRayPlane(rayOriginWorld, rayDirWorld, baseCenterWorld, planeNormal, t);
    
        if (t <= 0.f)
        {
            return m_height; // keep current height rather than snapping
        }
    
        const AZ::Vector3 hitWorld = rayOriginWorld + rayDirWorld * t;
        const float heightDelta = (hitWorld - baseCenterWorld).Dot(up); // signed
        return heightDelta;
    }

    bool DrawShapeMode::HandleMouseInteraction(
        const AzToolsFramework::ViewportInteraction::MouseInteractionEvent& mouseInteraction,
        const AZ::Transform& worldFromLocal,
        const IntersectionAndRenderData& intersectionData)
    {
        using MouseEvent = AzToolsFramework::ViewportInteraction::MouseEvent;

        // Cache so a keyboard-driven numeric confirm (which carries no transform)
        // can commit using the current frame's transform.
        m_worldFromLocal = worldFromLocal;

        const auto& mi     = mouseInteraction.m_mouseInteraction;
        const bool  leftDown  = mi.m_mouseButtons.Left() && mouseInteraction.m_mouseEvent == MouseEvent::Down;
        const bool  leftUp    = mi.m_mouseButtons.Left() && mouseInteraction.m_mouseEvent == MouseEvent::Up;
        const bool  rightDown = mi.m_mouseButtons.Right() && mouseInteraction.m_mouseEvent == MouseEvent::Down;
        const bool  moved     = mouseInteraction.m_mouseEvent == MouseEvent::Move;

        if (rightDown)
        {
            Cancel();
            return m_state != DrawState::Idle;
        }

        switch (m_state)
        {
        case DrawState::Idle:
        {
            if (leftDown)
            {
                // Ctrl+click = carve a hole through the polygon under the cursor.
                m_carveMode = mi.m_keyboardModifiers.Ctrl();   // Ctrl = carve, plain = draw
                AZ::Vector3 hitNormal;
                const AZ::Vector3 hitWorld = RaycastToSurface(mi, worldFromLocal, intersectionData, hitNormal);
                m_surfaceNormal = hitNormal;            // remember the surface orientation
                m_groundZ = hitWorld.GetZ();
                m_worldP0 = hitWorld;
                m_worldP1 = hitWorld;
                m_height  = 0.f;
                m_state   = DrawState::DraggingBase;
                return true;
            }
            return false;
        }

        case DrawState::DraggingBase:
        {
            if (moved)
            {
                AZ::Vector3 dummyNormal;
                const AZ::Vector3 rawHit = RaycastToSurface(mi, worldFromLocal, intersectionData, dummyNormal);

                // Project the raw hit onto the anchor's surface plane so the base rectangle
                // always lies flat on that surface — works for horizontal, vertical, or tilted.
                const AZ::Vector3 up = m_surfaceNormal.GetNormalized();
                const float distFromPlane = (rawHit - m_worldP0).Dot(up);
                m_worldP1 = rawHit - up * distFromPlane;

                return true;
            }
            if (leftUp)
            {
                // (no SetZ — P1 is already on the surface plane from the move handler)
                const AZ::Vector3 baseDelta = m_worldP1 - m_worldP0;

                // Reject a near-zero base measured IN THE SURFACE PLANE, not by world XZ.
                if (baseDelta.GetLength() < cl_whiteBoxMouseClickDeltaThreshold)
                {
                    Cancel();
                    return false;
                }

                m_height = 0.f;
                m_state  = DrawState::PullingHeight;
                return true;
            }
            return true;
        }

        case DrawState::PullingHeight:
        {
            if (moved)
            {
                // Once the depth is being typed, the mouse no longer drives it.
                if (!m_numericInput.IsActive())
                {
                    const AZ::Vector3 baseCenterWorld = (m_worldP0 + m_worldP1) * 0.5f;
                    m_height = RaycastToHeightPlane(mi, worldFromLocal, baseCenterWorld);
                }
                return true;
            }
            if (leftDown)
            {
                // A click also commits (using the typed depth if numeric is active).
                m_numericInput.Reset();

                if (AZStd::abs(m_height) < cl_whiteBoxMouseClickDeltaThreshold)
                {
                    Cancel();
                    return false;
                }

                if (m_carveMode)
                {
                    // Ctrl + draw = CSG boolean. Pull direction decides:
                    // pull in -> carve (subtract), pull out -> add (union).
                    BooleanAtPolygon(worldFromLocal, m_height);
                }
                else
                {
                    CommitBox(worldFromLocal);
                }

                m_state = DrawState::Idle;
                return true;
            }
            return true;
        }
        }

        return false;
    }
    
    

    void DrawShapeMode::CommitBox(const AZ::Transform& worldFromLocal)
    {
        WhiteBoxMesh* whiteBox = nullptr;
        EditorWhiteBoxComponentRequestBus::EventResult(
            whiteBox, m_entityComponentIdPair,
            &EditorWhiteBoxComponentRequests::GetWhiteBoxMesh);

        if (!whiteBox)
        {
            return;
        }

        AzToolsFramework::ScopedUndoBatch undoBatch("Draw White Box Shape");

        // Surface basis (up = outward normal of the surface drawn on).
        AZ::Vector3 right, fwd, up;
        BasisFromNormal(m_surfaceNormal, right, fwd, up);

        // Drag extents in the surface plane.
        const AZ::Vector3 drag = m_worldP1 - m_worldP0;
        AZ::Vector3 uAxis = right * drag.Dot(right);
        AZ::Vector3 vAxis = fwd   * drag.Dot(fwd);

        // Reject degenerate base / height.
        if (uAxis.GetLength() < 0.0001f || vAxis.GetLength() < 0.0001f ||
            AZStd::abs(m_height) < 0.0001f)
        {
            return;
        }

        // Force CCW winding about +up so the in-plane orientation is consistent.
        if (uAxis.Cross(vAxis).Dot(up) < 0.f)
        {
            AZStd::swap(uAxis, vAxis);
        }

        const AZ::Vector3 center = m_worldP0 + (uAxis + vAxis) * 0.5f;

        // Build the chosen shape directly into the mesh: base on the surface
        // (up offset 0), top/apex at the pull height.
        BuildShapeSolid(
            *whiteBox, worldFromLocal.GetInverse(), center, uAxis, vAxis, up,
            0.0f, m_height, CurrentShape(), CurrentSides());

        Api::CalculateNormals(*whiteBox);
        Api::CalculatePlanarUVs(*whiteBox);

        EditorWhiteBoxComponentRequestBus::Event(
            m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::SerializeWhiteBox);
        EditorWhiteBoxComponentRequestBus::Event(
            m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::RebuildWhiteBox);
        EditorWhiteBoxComponentModeRequestBus::Event(
            m_entityComponentIdPair,
            &EditorWhiteBoxComponentModeRequestBus::Events::MarkWhiteBoxIntersectionDataDirty);
        AzToolsFramework::ToolsApplicationRequests::Bus::Broadcast(
            &AzToolsFramework::ToolsApplicationRequests::AddDirtyEntity,
            m_entityComponentIdPair.GetEntityId());
    }

    // Apply a CSG boolean using a cutter prism built from the drawn footprint
    // (m_worldP0..m_worldP1) extruded along the surface normal by the pull depth.
    // The PULL DIRECTION picks the operation:
    //   pull INTO the surface (height < 0) -> Subtraction (carve a pocket/hole)
    //   pull OUT of the surface (height > 0) -> Union       (add a boss/extrusion)
    // Uses Manifold rather than the legacy inset/extrude/remove-cap trick, so a
    // carve becomes a closed pocket when shallow and a clean through-hole when it
    // exceeds the wall thickness - automatically, no special-casing.
    void DrawShapeMode::BooleanAtPolygon(const AZ::Transform& worldFromLocal, float height)
    {
        WhiteBoxMesh* whiteBox = nullptr;
        EditorWhiteBoxComponentRequestBus::EventResult(
            whiteBox, m_entityComponentIdPair,
            &EditorWhiteBoxComponentRequests::GetWhiteBoxMesh);
        if (!whiteBox)
        {
            return;
        }

        // Surface basis: up = outward normal of the face the user drew on.
        AZ::Vector3 right, fwd, up;
        BasisFromNormal(m_surfaceNormal, right, fwd, up);

        // Drawn rectangle in the surface plane.
        const AZ::Vector3 drag = m_worldP1 - m_worldP0;
        AZ::Vector3 uAxis = right * drag.Dot(right);
        AZ::Vector3 vAxis = fwd   * drag.Dot(fwd);

        const float depth = AZStd::abs(height);
        if (uAxis.GetLength() < 0.0001f || vAxis.GetLength() < 0.0001f || depth < 0.0001f)
        {
            return;
        }

        // Force CCW winding about +up so the cutter is wound outward consistently
        // (same convention as CommitBox).
        if (uAxis.Cross(vAxis).Dot(up) < 0.f)
        {
            AZStd::swap(uAxis, vAxis);
        }

        // Pull direction decides the operation.
        const bool carve = (height < 0.f);
        const Api::BooleanOperation operation =
            carve ? Api::BooleanOperation::Subtraction : Api::BooleanOperation::Union;

        // The cutter is the SELECTED shape (box/cylinder/pyramid/cone, N sides),
        // not just a box. It must overlap the surface slightly (never sit exactly
        // coplanar with the target face - coplanar faces make the boolean fragile)
        // and overshoot the far end so the cut/weld is clean:
        //   carve: base just OUTSIDE the surface (+eps), extrude to -(depth+eps)
        //   add:   base just INSIDE the surface (-eps), extrude to +(depth+eps)
        const float surfaceSpan = AZ::GetMax(uAxis.GetLength(), vAxis.GetLength());
        const float eps = AZ::GetMax(surfaceSpan * 0.02f, 0.001f);

        const AZ::Vector3 center = m_worldP0 + (uAxis + vAxis) * 0.5f;
        const float baseUp = carve ?  eps           : -eps;
        const float topUp  = carve ? -(depth + eps) : (depth + eps);

        // Build the cutter as its own watertight white box mesh, expressed in the
        // TARGET's local space so we can boolean with an identity transform.
        Api::WhiteBoxMeshPtr cutter = Api::CreateWhiteBoxMesh();
        BuildShapeSolid(
            *cutter, worldFromLocal.GetInverse(), center, uAxis, vAxis, up,
            baseUp, topUp, CurrentShape(), CurrentSides());
        Api::CalculateNormals(*cutter);

        AzToolsFramework::ScopedUndoBatch undoBatch(carve ? "Carve White Box" : "Add White Box");

        // Apply the boolean (identity transform: the cutter is already built in
        // the target's local space).
        if (!Api::MeshBoolean(*whiteBox, *cutter, AZ::Transform::CreateIdentity(), operation))
        {
            // No intersection / empty result - nothing to do.
            return;
        }

        Api::CalculateNormals(*whiteBox);
        Api::CalculatePlanarUVs(*whiteBox);

        EditorWhiteBoxComponentRequestBus::Event(
            m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::SerializeWhiteBox);
        EditorWhiteBoxComponentRequestBus::Event(
            m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::RebuildWhiteBox);
        EditorWhiteBoxComponentModeRequestBus::Event(
            m_entityComponentIdPair,
            &EditorWhiteBoxComponentModeRequestBus::Events::MarkWhiteBoxIntersectionDataDirty);
        AzToolsFramework::ToolsApplicationRequests::Bus::Broadcast(
            &AzToolsFramework::ToolsApplicationRequests::AddDirtyEntity,
            m_entityComponentIdPair.GetEntityId());
    }

    void DrawShapeMode::Cancel()
    {
        m_numericInput.Reset();
        m_state  = DrawState::Idle;
        m_height = 0.f;
    }

    bool DrawShapeMode::BeginNumericIfPulling()
    {
        // Numeric depth is only meaningful once the base is locked and we're
        // pulling height.
        if (m_state != DrawState::PullingHeight)
        {
            return false;
        }
        if (!m_numericInput.IsActive())
        {
            m_numericInput.Begin(NumericOpMode::Draw);
        }
        return true;
    }

    void DrawShapeMode::SyncPreviewHeight()
    {
        // Live-preview the typed expression as the pull depth.
        if (m_numericInput.IsActive() && !m_numericInput.IsEmpty())
        {
            m_height = m_numericInput.GetValue();
        }
    }

    void DrawShapeMode::NumericConfirm()
    {
        if (!m_numericInput.IsActive() || m_state != DrawState::PullingHeight)
        {
            return;
        }

        const float value = m_numericInput.GetValue();
        m_numericInput.Reset();
        m_height = value;

        if (AZStd::abs(m_height) < 0.0001f)
        {
            Cancel();
            return;
        }

        // Ctrl gates the boolean; the sign of the depth picks add vs carve.
        if (m_carveMode)
        {
            BooleanAtPolygon(m_worldFromLocal, m_height);
        }
        else
        {
            CommitBox(m_worldFromLocal);
        }
        m_state = DrawState::Idle;
    }

    void DrawShapeMode::NumericCancel()
    {
        // First Escape drops just the numeric entry (back to mouse pull);
        // a second one (nothing typed) cancels the whole draw.
        if (m_numericInput.IsActive())
        {
            m_numericInput.Reset();
        }
        else
        {
            Cancel();
        }
    }

    void DrawShapeMode::DisplayViewport(
        [[maybe_unused]] const AzFramework::ViewportInfo& viewportInfo,
        AzFramework::DebugDisplayRequests& debugDisplay)
    {
        if (m_state == DrawState::Idle)
        {
            return;
        }
    
        // Build the surface-aligned frame from the normal captured at the anchor.
        AZ::Vector3 right, fwd, up;
        BasisFromNormal(m_surfaceNormal, right, fwd, up);

        const AZ::Vector3 drag = m_worldP1 - m_worldP0;
        AZ::Vector3 uAxis = right * drag.Dot(right);
        AZ::Vector3 vAxis = fwd   * drag.Dot(fwd);
        if (uAxis.Cross(vAxis).Dot(up) < 0.f)
        {
            AZStd::swap(uAxis, vAxis);
        }

        const AZ::Vector3 center = m_worldP0 + (uAxis + vAxis) * 0.5f;
        const AZ::Vector3 ru     = uAxis * 0.5f;
        const AZ::Vector3 rv     = vAxis * 0.5f;
        const AZ::Vector3 ext    = up * m_height;   // signed — preview follows pull direction

        const DrawShapeType shape = CurrentShape();
        const bool pointed = (shape == DrawShapeType::Pyramid || shape == DrawShapeType::Cone);
        const bool round   = (shape == DrawShapeType::Cylinder || shape == DrawShapeType::Cone);

        // Preview the ACTUAL shape footprint (N-gon), matching what will be built.
        const int sides = CurrentSides();
        const AZStd::vector<AZ::Vector3> ring = ComputeFootprintRing(center, ru, rv, round, sides);
        const size_t n = ring.size();

        const bool pullingHeight = (m_state == DrawState::PullingHeight);

        const AZ::Color fillColor = pullingHeight
            ? AZ::Color(static_cast<AZ::Color>(ed_whiteBoxPolygonSelection).GetR(),
                        static_cast<AZ::Color>(ed_whiteBoxPolygonSelection).GetG(),
                        static_cast<AZ::Color>(ed_whiteBoxPolygonSelection).GetB(), 0.25f)
            : AZ::Color(static_cast<AZ::Color>(ed_whiteBoxPolygonHover).GetR(),
                        static_cast<AZ::Color>(ed_whiteBoxPolygonHover).GetG(),
                        static_cast<AZ::Color>(ed_whiteBoxPolygonHover).GetB(), 0.25f);

        const AZ::Color outlineColor = m_carveMode
        ? AZ::Color(1.0f, 0.25f, 0.25f, 1.0f)   // red = carve
        : (pullingHeight
            ? static_cast<AZ::Color>(ed_whiteBoxOutlineSelection)
            : static_cast<AZ::Color>(ed_whiteBoxOutlineHover));

        debugDisplay.DepthTestOff();
        debugDisplay.SetLineWidth(static_cast<float>(cl_whiteBoxEdgeVisualWidth));

        // --- Base footprint outline + fill (always drawn) ---
        debugDisplay.SetColor(outlineColor);
        for (size_t i = 0; i < n; ++i)
        {
            debugDisplay.DrawLine(ring[i], ring[(i + 1) % n]);
        }
        {
            AZStd::vector<AZ::Vector3> baseTris;
            baseTris.reserve(n * 3);
            for (size_t i = 0; i < n; ++i)
            {
                baseTris.push_back(center);
                baseTris.push_back(ring[i]);
                baseTris.push_back(ring[(i + 1) % n]);
            }
            debugDisplay.DrawTriangles(baseTris, fillColor);
        }

        // --- During height pull, draw the extrusion (prism) or the apex (pointed) ---
        if (pullingHeight)
        {
            debugDisplay.SetColor(outlineColor);
            if (pointed)
            {
                const AZ::Vector3 apex = center + ext;
                for (size_t i = 0; i < n; ++i)
                {
                    debugDisplay.DrawLine(ring[i], apex);
                }
            }
            else
            {
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t j = (i + 1) % n;
                    debugDisplay.DrawLine(ring[i] + ext, ring[j] + ext); // top outline
                    debugDisplay.DrawLine(ring[i], ring[i] + ext);       // vertical edge
                }
                const AZ::Vector3 topCenter = center + ext;
                AZStd::vector<AZ::Vector3> topTris;
                topTris.reserve(n * 3);
                for (size_t i = 0; i < n; ++i)
                {
                    topTris.push_back(topCenter);
                    topTris.push_back(ring[i] + ext);
                    topTris.push_back(ring[(i + 1) % n] + ext);
                }
                debugDisplay.DrawTriangles(topTris, fillColor);
            }
        }

        // --- Live shape + side-count readout (the toolbar can't show text) ---
        {
            const AZStd::string shapeLabel = AZStd::string::format("%s  |  %d sides", DrawShapeName(shape), sides);
            debugDisplay.SetColor(AZ::Color(1.0f, 1.0f, 1.0f, 1.0f));
            debugDisplay.DrawTextLabel(center, 1.3f, shapeLabel.c_str(), true, 0, 0);
        }

        // --- Numeric depth overlay (Blender-style) ---
        if (m_numericInput.IsActive())
        {
            const AZ::Vector3 labelPos = center + ext;
            const AZStd::string status = m_carveMode
                ? (AZStd::string("[Ctrl] ") + m_numericInput.GetStatusText())
                : m_numericInput.GetStatusText();
            debugDisplay.SetColor(AZ::Color(1.0f, 1.0f, 1.0f, 1.0f));
            debugDisplay.DrawTextLabel(labelPos, 1.5f, status.c_str(), true, 0, 0);
        }
    }
    


    AZStd::vector<AzToolsFramework::ActionOverride> DrawShapeMode::PopulateActions(
        [[maybe_unused]] const AZ::EntityComponentIdPair& entityComponentIdPair)
    {
        return {};
    }

    // ----------------------------------------------------------------------- //
    // Shape + side count (sourced from the component's Inspector properties)    //
    // ----------------------------------------------------------------------- //
    DrawShapeType DrawShapeMode::CurrentShape() const
    {
        DrawShapeType shape = DrawShapeType::Box;
        EditorWhiteBoxComponentRequestBus::EventResult(
            shape, m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::GetDrawShape);
        return shape;
    }

    int DrawShapeMode::CurrentSides() const
    {
        int sides = 4;
        EditorWhiteBoxComponentRequestBus::EventResult(
            sides, m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::GetDrawSides);
        return AZ::GetClamp(sides, 3, 256);
    }

    // ----------------------------------------------------------------------- //
    // Numeric-depth keyboard actions (Blender-style), scoped to the draw mode  //
    // ----------------------------------------------------------------------- //
    namespace
    {
        constexpr AZStd::string_view DrawNumericConfirmId   = "o3de.action.whiteBoxDraw.numeric.confirm";
        constexpr AZStd::string_view DrawNumericCancelId    = "o3de.action.whiteBoxDraw.numeric.cancel";
        constexpr AZStd::string_view DrawNumericBackspaceId = "o3de.action.whiteBoxDraw.numeric.backspace";
        constexpr AZStd::string_view DrawNumericDecimalId   = "o3de.action.whiteBoxDraw.numeric.decimal";
        constexpr AZStd::string_view DrawNumericNegateId    = "o3de.action.whiteBoxDraw.numeric.negate";
        constexpr AZStd::string_view DrawNumericOpPlusId    = "o3de.action.whiteBoxDraw.numeric.opPlus";
        constexpr AZStd::string_view DrawNumericOpMultId    = "o3de.action.whiteBoxDraw.numeric.opMult";
        constexpr AZStd::string_view DrawNumericOpDivId     = "o3de.action.whiteBoxDraw.numeric.opDiv";
        constexpr AZStd::string_view DrawNumericDigitIds[10] = {
            "o3de.action.whiteBoxDraw.numeric.digit0", "o3de.action.whiteBoxDraw.numeric.digit1",
            "o3de.action.whiteBoxDraw.numeric.digit2", "o3de.action.whiteBoxDraw.numeric.digit3",
            "o3de.action.whiteBoxDraw.numeric.digit4", "o3de.action.whiteBoxDraw.numeric.digit5",
            "o3de.action.whiteBoxDraw.numeric.digit6", "o3de.action.whiteBoxDraw.numeric.digit7",
            "o3de.action.whiteBoxDraw.numeric.digit8", "o3de.action.whiteBoxDraw.numeric.digit9",
        };
    } // namespace

    void DrawShapeMode::RegisterActions()
    {
        auto actionManagerInterface = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        AZ_Assert(actionManagerInterface, "WhiteBoxDrawShapeMode - could not get ActionManagerInterface on RegisterActions.");
        auto hotKeyManagerInterface = AZ::Interface<AzToolsFramework::HotKeyManagerInterface>::Get();
        AZ_Assert(hotKeyManagerInterface, "WhiteBoxDrawShapeMode - could not get HotKeyManagerInterface on RegisterActions.");

        // Dispatch a no-arg request to every active White Box component in draw mode.
        auto dispatchToDrawModes = [](auto fn)
        {
            auto cmci = AZ::Interface<AzToolsFramework::ComponentModeCollectionInterface>::Get();
            AZ_Assert(cmci, "Could not retrieve component mode collection.");
            cmci->EnumerateActiveComponents(
                [fn](const AZ::EntityComponentIdPair& id, const AZ::Uuid&)
                {
                    EditorWhiteBoxDrawShapeModeRequestBus::Event(id, fn);
                });
        };

        const auto registerSimple =
            [&](AZStd::string_view id, const char* name, const char* hotkey, auto fn)
        {
            AzToolsFramework::ActionProperties p;
            p.m_name = name;
            p.m_category = "White Box Component Mode - Draw Shape";
            actionManagerInterface->RegisterAction(
                EditorIdentifiers::MainWindowActionContextIdentifier, id, p,
                [dispatchToDrawModes, fn] { dispatchToDrawModes(fn); });
            hotKeyManagerInterface->SetActionHotKey(id, hotkey);
        };

        registerSimple(DrawNumericConfirmId,   "Draw Numeric Confirm",   "Return",    &EditorWhiteBoxDrawShapeModeRequests::NumericConfirm);
        registerSimple(DrawNumericCancelId,    "Draw Numeric Cancel",    "Escape",    &EditorWhiteBoxDrawShapeModeRequests::NumericCancel);
        registerSimple(DrawNumericBackspaceId, "Draw Numeric Backspace", "Backspace", &EditorWhiteBoxDrawShapeModeRequests::NumericBackspace);
        registerSimple(DrawNumericDecimalId,   "Draw Numeric Decimal",   ".",         &EditorWhiteBoxDrawShapeModeRequests::NumericAppendDecimal);
        registerSimple(DrawNumericNegateId,    "Draw Numeric Minus",     "-",         &EditorWhiteBoxDrawShapeModeRequests::NumericNegate);
        registerSimple(DrawNumericOpPlusId,    "Draw Numeric Plus",      "+",         &EditorWhiteBoxDrawShapeModeRequests::NumericAppendOperatorPlus);
        registerSimple(DrawNumericOpMultId,    "Draw Numeric Multiply",  "*",         &EditorWhiteBoxDrawShapeModeRequests::NumericAppendOperatorMult);
        registerSimple(DrawNumericOpDivId,     "Draw Numeric Divide",    "/",         &EditorWhiteBoxDrawShapeModeRequests::NumericAppendOperatorDiv);

        // Digits 0-9 (NumericAppendDigit takes a char, so inline the dispatch).
        const char* digitKeys[10] = {"0","1","2","3","4","5","6","7","8","9"};
        for (int d = 0; d <= 9; ++d)
        {
            AzToolsFramework::ActionProperties p;
            p.m_name = AZStd::string::format("Draw Numeric Digit %d", d).c_str();
            p.m_category = "White Box Component Mode - Draw Shape";
            const char digit = static_cast<char>('0' + d);
            actionManagerInterface->RegisterAction(
                EditorIdentifiers::MainWindowActionContextIdentifier, DrawNumericDigitIds[d], p,
                [digit]
                {
                    auto cmci = AZ::Interface<AzToolsFramework::ComponentModeCollectionInterface>::Get();
                    AZ_Assert(cmci, "Could not retrieve component mode collection.");
                    cmci->EnumerateActiveComponents(
                        [digit](const AZ::EntityComponentIdPair& id, const AZ::Uuid&)
                        {
                            EditorWhiteBoxDrawShapeModeRequestBus::Event(
                                id, &EditorWhiteBoxDrawShapeModeRequests::NumericAppendDigit, digit);
                        });
                });
            hotKeyManagerInterface->SetActionHotKey(DrawNumericDigitIds[d], digitKeys[d]);
        }
    }

    void DrawShapeMode::BindActionsToModes(const AZStd::string& modeIdentifier)
    {
        auto actionManagerInterface = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
        AZ_Assert(actionManagerInterface, "WhiteBoxDrawShapeMode - could not get ActionManagerInterface on BindActionsToModes.");

        for (const auto& id :
             { DrawNumericConfirmId, DrawNumericCancelId, DrawNumericBackspaceId, DrawNumericDecimalId,
               DrawNumericNegateId, DrawNumericOpPlusId, DrawNumericOpMultId, DrawNumericOpDivId })
        {
            actionManagerInterface->AssignModeToAction(modeIdentifier, id);
        }
        for (const auto& digitId : DrawNumericDigitIds)
        {
            actionManagerInterface->AssignModeToAction(modeIdentifier, digitId);
        }
    }

} // namespace WhiteBox