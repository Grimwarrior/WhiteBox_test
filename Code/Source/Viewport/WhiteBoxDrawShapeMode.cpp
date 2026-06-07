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

#include <AzFramework/Render/GeometryIntersectionStructures.h>
#include <AzFramework/Visibility/EntityVisibilityBoundsUnionSystem.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <AzFramework/Render/Intersector.h>           // IntersectorBus / IntersectorInterface
#include <AzFramework/Render/GeometryIntersectionStructures.h>  // RayRequest / RayResult


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
    DrawShapeMode::DrawShapeMode(const AZ::EntityComponentIdPair& entityComponentIdPair)
        : m_entityComponentIdPair(entityComponentIdPair)
    {
        // Connect to the global viewport rendering bus
        AzFramework::ViewportDebugDisplayEventBus::Handler::BusConnect(AzToolsFramework::GetEntityContextId());
    }
    DrawShapeMode::~DrawShapeMode()
    {
        // Disconnect from the bus when mode is exited
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
    
        // 1. White Box mesh
        {
            const AZ::Transform localFromWorld = worldFromLocal.GetInverse();
            const AZ::Vector3 localRayOrigin = localFromWorld.TransformPoint(rayOriginWorld);
            const AZ::Vector3 localRayDir =
                AzToolsFramework::TransformDirectionNoScaling(localFromWorld, rayDirWorld);
    
            float bestDist = AZStd::numeric_limits<float>::max();
            AZ::Vector3 bestLocalHit = AZ::Vector3::CreateZero();
            AZ::Vector3 bestLocalNormal = AZ::Vector3::CreateAxisZ();
            bool hitWhiteBox = false;
    
            for (const auto& polyBound : intersectionData.m_whiteBoxIntersectionData.m_polygonBounds)
            {
                float dist = AZStd::numeric_limits<float>::max();
                int64_t triIdx = 0;
                if (IntersectRayPolygon(polyBound.m_bound, localRayOrigin, localRayDir, dist, triIdx))
                {
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestLocalHit = localRayOrigin + localRayDir * dist;

                        // Compute the normal from the hit triangle's three vertices.
                        const auto& tris = polyBound.m_bound.m_triangles;
                        const size_t base = static_cast<size_t>(triIdx) * 3;
                        if (base + 2 < tris.size())
                        {
                            const AZ::Vector3 e1 = tris[base + 1] - tris[base + 0];
                            const AZ::Vector3 e2 = tris[base + 2] - tris[base + 0];
                            bestLocalNormal = e1.Cross(e2).GetNormalized();
                        }
                        else
                        {
                            bestLocalNormal = AZ::Vector3::CreateAxisZ();
                        }
                        hitWhiteBox = true;
                    }
                }
            }
    
            if (hitWhiteBox)
            {
                AZ::Vector3 worldNormal =
                    AzToolsFramework::TransformDirectionNoScaling(worldFromLocal, bestLocalNormal).GetNormalized();

                // Make the normal face back toward the camera (ray travels camera->surface).
                if (worldNormal.Dot(rayDirWorld) > 0.f)
                {
                    worldNormal = -worldNormal;
                }

                outWorldNormal = worldNormal;
                return worldFromLocal.TransformPoint(bestLocalHit);
            }
        }
    
        // 2. Scene geometry
        {
            AzFramework::RenderGeometry::RayRequest request;
            request.m_startWorldPosition = rayOriginWorld;
            request.m_endWorldPosition   = rayOriginWorld + rayDirWorld * 1000.0f;
            request.m_onlyVisible        = true;
    
            AzFramework::RenderGeometry::RayResult rayResult;
            AzFramework::RenderGeometry::IntersectorBus::EventResult(
                rayResult, AzToolsFramework::GetEntityContextId(),
                &AzFramework::RenderGeometry::IntersectorInterface::RayIntersect, request);
    
            if (rayResult)
            {
                AZ::Vector3 worldNormal = rayResult.m_worldNormal.GetNormalized();
                if (worldNormal.Dot(rayDirWorld) > 0.f)
                {
                    worldNormal = -worldNormal;
                }
                outWorldNormal = worldNormal;
                return rayResult.m_worldPosition;
            }
        }
    
        // 3. Ground plane
        outWorldNormal = AZ::Vector3::CreateAxisZ();
        AZ::Vector3 planePos(0.f, 0.f, m_groundZ);
        AZ::Vector3 planeNormal = AZ::Vector3::CreateAxisZ();
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
                const AZ::Vector3 baseCenterWorld = (m_worldP0 + m_worldP1) * 0.5f;
                m_height = RaycastToHeightPlane(mi, worldFromLocal, baseCenterWorld);
                return true;
            }
            if (leftDown)
            {
                if (AZStd::abs(m_height) < cl_whiteBoxMouseClickDeltaThreshold)
                {
                    Cancel();
                    return false;
                }

                if (m_carveMode)
                {
                    // Map drawn rectangle size -> inset fraction, using the hit face's size.
                    float insetScale = 0.5f;       // fallback
                    float wallThickness = 1.0f;    // fallback

                    WhiteBoxMesh* wb = nullptr;
                    EditorWhiteBoxComponentRequestBus::EventResult(
                        wb, m_entityComponentIdPair, &EditorWhiteBoxComponentRequests::GetWhiteBoxMesh);

                    if (wb)
                    {
                        // Find the polygon we're carving (closest face to P0 in local space).
                        const AZ::Transform localFromWorld = worldFromLocal.GetInverse();
                        const AZ::Vector3 p0Local = localFromWorld.TransformPoint(m_worldP0);

                        Api::FaceHandle bestFace;
                        float faceBest = AZStd::numeric_limits<float>::max();
                        for (const Api::FaceHandle fh : Api::MeshFaceHandles(*wb))
                        {
                            const float d = (Api::FaceMidpoint(*wb, fh) - p0Local).GetLengthSq();
                            if (d < faceBest) { faceBest = d; bestFace = fh; }
                        }

                        if (bestFace.IsValid())
                        {
                            const Api::PolygonHandle target = Api::FacePolygonHandle(*wb, bestFace);
                            if (!target.m_faceHandles.empty())
                            {
                                const Api::VertexHandles border =
                                    Api::PolygonBorderVertexHandlesFlattened(*wb, target);
                                AZ::Aabb faceAabb = AZ::Aabb::CreateNull();
                                for (const auto vh : border)
                                {
                                    faceAabb.AddPoint(Api::VertexPosition(*wb, vh));
                                }
                                const float faceSize = faceAabb.GetExtents().GetMaxElement();
                                const float drawnSize = (m_worldP1 - m_worldP0).GetLength();
                                const float holeFraction =
                                        AZ::GetClamp(drawnSize / AZ::GetMax(faceSize, 0.001f), 0.05f, 0.9f);
                                const float insetScale = holeFraction - 1.0f;

                                // Estimate wall thickness: smallest AABB extent of the whole mesh,
                                // a decent proxy for "how thick is this wall".
                                AZ::Aabb meshAabb = AZ::Aabb::CreateNull();
                                for (const auto& p : Api::MeshVertexPositions(*wb))
                                {
                                    meshAabb.AddPoint(p);
                                }
                                wallThickness = meshAabb.GetExtents().GetMinElement();
                            }
                        }
                    }

                    // Open hole if the pull depth reaches/exceeds the wall thickness.
                    const bool openHole = AZStd::abs(m_height) >= wallThickness * 0.95f;

                    CarveAtPolygon(mi, worldFromLocal, intersectionData,
                                insetScale, AZStd::abs(m_height), openHole);
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

        // Surface basis
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

        // Force the base rectangle to wind CCW about +up so the in-plane orientation
        // is consistent no matter which way the drag went.
        if (uAxis.Cross(vAxis).Dot(up) < 0.f)
        {
            AZStd::swap(uAxis, vAxis);
        }

        const AZ::Vector3 anchor = m_worldP0;
        const AZ::Vector3 b0 = anchor;                    // base rectangle (on the surface)
        const AZ::Vector3 b1 = anchor + uAxis;
        const AZ::Vector3 b2 = anchor + uAxis + vAxis;
        const AZ::Vector3 b3 = anchor + vAxis;

        const AZ::Vector3 ext = up * m_height;            // signed: +out / -into surface

        // The two caps: "plus cap" is whichever cap sits on the +up side.
        // If height is positive, plus-cap = base+ext (the extruded top) and the base is the minus cap.
        // If height is negative, base+ext sits on the -up side, so we flip which cap is which
        // to keep faces wound outward.
        AZ::Vector3 plus[4];   // corners on +up side  -> indices 0..3
        AZ::Vector3 minus[4];  // corners on -up side  -> indices 4..7

        if (m_height >= 0.f)
        {
            plus[0]  = b0 + ext; plus[1]  = b1 + ext; plus[2]  = b2 + ext; plus[3]  = b3 + ext;
            minus[0] = b0;       minus[1] = b1;       minus[2] = b2;       minus[3] = b3;
        }
        else
        {
            // base is now the +up cap; extruded face is the -up cap
            plus[0]  = b0;       plus[1]  = b1;       plus[2]  = b2;       plus[3]  = b3;
            minus[0] = b0 + ext; minus[1] = b1 + ext; minus[2] = b2 + ext; minus[3] = b3 + ext;
        }

        AZ::Vector3 worldCorners[8] = {
            plus[0],  plus[1],  plus[2],  plus[3],   // 0-3  (+up cap)
            minus[0], minus[1], minus[2], minus[3]   // 4-7  (-up cap)
        };

        const AZ::Transform localFromWorld = worldFromLocal.GetInverse();
        Api::VertexHandle v[8];
        for (int i = 0; i < 8; ++i)
        {
            v[i] = Api::AddVertex(*whiteBox, localFromWorld.TransformPoint(worldCorners[i]));
        }

        // Winding is now always outward because 0-3 is guaranteed the +up cap.
        Api::AddQuadPolygon(*whiteBox, v[0], v[1], v[2], v[3]); // +up cap
        Api::AddQuadPolygon(*whiteBox, v[7], v[6], v[5], v[4]); // -up cap
        Api::AddQuadPolygon(*whiteBox, v[4], v[5], v[1], v[0]); // sides
        Api::AddQuadPolygon(*whiteBox, v[5], v[6], v[2], v[1]);
        Api::AddQuadPolygon(*whiteBox, v[6], v[7], v[3], v[2]);
        Api::AddQuadPolygon(*whiteBox, v[7], v[4], v[0], v[3]);

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

    // Carve a hole/doorway through the polygon under the cursor.
    // insetScale: 0..1, fraction of the face kept around the hole (e.g. 0.6 leaves a 60%-size frame).
    // depth: how far to push through; use >= wall thickness to punch all the way through.
    void DrawShapeMode::CarveAtPolygon(
        const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
        const AZ::Transform& worldFromLocal,
        const IntersectionAndRenderData& intersectionData,
        float insetScale,
        float depth,
        bool openHole)   // true = remove caps for a through-hole; false = closed pocket
    {
        WhiteBoxMesh* whiteBox = nullptr;
        EditorWhiteBoxComponentRequestBus::EventResult(
            whiteBox, m_entityComponentIdPair,
            &EditorWhiteBoxComponentRequests::GetWhiteBoxMesh);
        if (!whiteBox)
        {
            return;
        }
    
        // Find polygon under cursor (local space).
        const AZ::Vector3 rayOriginWorld = mouseInteraction.m_mousePick.m_rayOrigin;
        const AZ::Vector3 rayDirWorld    = mouseInteraction.m_mousePick.m_rayDirection;
        const AZ::Transform localFromWorld = worldFromLocal.GetInverse();
        const AZ::Vector3 localRayOrigin = localFromWorld.TransformPoint(rayOriginWorld);
        const AZ::Vector3 localRayDir =
            AzToolsFramework::TransformDirectionNoScaling(localFromWorld, rayDirWorld);
    
        float bestDist = AZStd::numeric_limits<float>::max();
        AZ::Vector3 hitLocal = AZ::Vector3::CreateZero();
        bool found = false;
        for (const auto& pb : intersectionData.m_whiteBoxIntersectionData.m_polygonBounds)
        {
            float dist = AZStd::numeric_limits<float>::max();
            int64_t triIdx = 0;
            if (IntersectRayPolygon(pb.m_bound, localRayOrigin, localRayDir, dist, triIdx))
            {
                if (dist < bestDist) { bestDist = dist; hitLocal = localRayOrigin + localRayDir * dist; found = true; }
            }
        }
        if (!found) { return; }
    
        // Resolve the polygon from the closest face to the hit point.
        Api::FaceHandle bestFace;
        float faceBest = AZStd::numeric_limits<float>::max();
        for (const Api::FaceHandle fh : Api::MeshFaceHandles(*whiteBox))
        {
            const float d = (Api::FaceMidpoint(*whiteBox, fh) - hitLocal).GetLengthSq();
            if (d < faceBest) { faceBest = d; bestFace = fh; }
        }
        if (!bestFace.IsValid()) { return; }
    
        const Api::PolygonHandle target = Api::FacePolygonHandle(*whiteBox, bestFace);
        if (target.m_faceHandles.empty()) { return; }
    
        AzToolsFramework::ScopedUndoBatch undoBatch("Carve White Box");
    
        // Inset -> inner opening polygon.
        // NEW — pass the negative inset directly, clamped to the valid inset range:
        const Api::PolygonHandle inset =
            Api::ScalePolygonAppendRelative(*whiteBox, target, AZ::GetClamp(insetScale, -0.95f, -0.05f));
        if (inset.m_faceHandles.empty()) { return; }
    
        // Extrude inward.
        const Api::PolygonHandle cap =
            Api::TranslatePolygonAppend(*whiteBox, inset, -AZStd::abs(depth));
    
        if (openHole && !cap.m_faceHandles.empty())
        {
            // Remove the inner cap to open the hole.
            Api::RemoveFaces(*whiteBox, cap.m_faceHandles);
            // Note: for a wall, you may also want to remove the matching faces on the
            // back side if the extrude punched through — handle once you see the result.
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
        m_state  = DrawState::Idle;
        m_height = 0.f;
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

        const AZ::Vector3 anchor  = m_worldP0;
        const AZ::Vector3 corner0 = anchor;
        const AZ::Vector3 corner1 = anchor + uAxis;
        const AZ::Vector3 corner2 = anchor + uAxis + vAxis;
        const AZ::Vector3 corner3 = anchor + vAxis;
        const AZ::Vector3 ext     = up * m_height;   // signed — preview follows pull direction

    
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
        debugDisplay.SetColor(outlineColor);
        debugDisplay.SetLineWidth(static_cast<float>(cl_whiteBoxEdgeVisualWidth));
    
        // --- Base rectangle (always drawn) ---
        debugDisplay.DrawLine(corner0, corner1);
        debugDisplay.DrawLine(corner1, corner2);
        debugDisplay.DrawLine(corner2, corner3);
        debugDisplay.DrawLine(corner3, corner0);
    
        // --- Filled base quad for visibility ---
        debugDisplay.SetColor(fillColor);
        debugDisplay.DrawQuad(corner0, corner1, corner2, corner3);
    
        // --- During height pull, draw the extruded top + vertical edges ---
        if (pullingHeight)
        {
            const AZ::Vector3 top0 = corner0 + ext;
            const AZ::Vector3 top1 = corner1 + ext;
            const AZ::Vector3 top2 = corner2 + ext;
            const AZ::Vector3 top3 = corner3 + ext;
    
            debugDisplay.SetColor(outlineColor);
    
            // Top rectangle
            debugDisplay.DrawLine(top0, top1);
            debugDisplay.DrawLine(top1, top2);
            debugDisplay.DrawLine(top2, top3);
            debugDisplay.DrawLine(top3, top0);
    
            // Vertical edges
            debugDisplay.DrawLine(corner0, top0);
            debugDisplay.DrawLine(corner1, top1);
            debugDisplay.DrawLine(corner2, top2);
            debugDisplay.DrawLine(corner3, top3);
    
            // Filled top quad
            debugDisplay.SetColor(fillColor);
            debugDisplay.DrawQuad(top0, top1, top2, top3);
        }
    }
    


    AZStd::vector<AzToolsFramework::ActionOverride> DrawShapeMode::PopulateActions(
        [[maybe_unused]] const AZ::EntityComponentIdPair& entityComponentIdPair)
    {
        return {};
    }

} // namespace WhiteBox