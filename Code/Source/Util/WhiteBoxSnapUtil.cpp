/*
 * White Box - vertex snapping helpers. See WhiteBoxSnapUtil.h.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#include "WhiteBoxSnapUtil.h"

#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/Component/NonUniformScaleBus.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Math/Color.h>
#include <AzCore/Math/Transform.h>
#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzFramework/Viewport/CameraState.h>
#include <AzFramework/Viewport/ScreenGeometry.h>
#include <AzFramework/Viewport/ViewportId.h>
#include <AzFramework/Viewport/ViewportScreen.h>
#include "Util/WhiteBoxEditorUtil.h"
#include <AzToolsFramework/Manipulators/ManipulatorView.h>
#include <AzToolsFramework/Maths/TransformUtils.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>
#include <SnapApi/VertexSnapInterface.h>

#include <limits>

AZ_CVAR(
    AZ::Color, cl_whiteBoxSnapTargetColor, AZ::Color::CreateFromRgba(0, 255, 160, 255), nullptr,
    AZ::ConsoleFunctorFlags::Null, "The color of the highlight drawn on the vertex currently being snapped to");
AZ_CVAR(
    float, cl_whiteBoxSnapTargetSize, 0.06f, nullptr, AZ::ConsoleFunctorFlags::Null,
    "The radius of the highlight drawn on the vertex currently being snapped to");

namespace WhiteBox
{
    namespace SnapUtil
    {
        //! The cursor position for this viewport, or nothing when the cursor is not over it.
        static AZStd::optional<AzFramework::ScreenPoint> CursorPosition(const int viewportId)
        {
            AZStd::optional<AzFramework::ScreenPoint> cursor;
            AzToolsFramework::ViewportInteraction::ViewportMouseCursorRequestBus::EventResult(
                cursor, viewportId, &AzToolsFramework::ViewportInteraction::ViewportMouseCursorRequests::MousePosition);
            return cursor;
        }

        bool SnappingActive()
        {
            return SnapApi::IsSnappingEnabled();
        }

        //! See the note in the header - written by the component mode's mouse handler, read by
        //! manipulator callbacks whose Action type does not carry a viewport id.
        static int s_activeViewportId = AzFramework::InvalidViewportId;

        void SetActiveViewportId(const int viewportId)
        {
            s_activeViewportId = viewportId;
        }

        int ActiveViewportId()
        {
            return s_activeViewportId;
        }

        AZ::Vector3 MeshLocalFromWorld(const AZ::EntityId entityId, const AZ::Vector3& worldPosition)
        {
            const AZ::Transform worldFromLocal = EditorSpaceFromLocal(entityId);
            const AZ::Vector3 scaledLocal = worldFromLocal.GetInverse().TransformPoint(worldPosition);

            AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
            AZ::NonUniformScaleRequestBus::EventResult(nonUniformScale, entityId, &AZ::NonUniformScaleRequests::GetScale);

            return scaledLocal / nonUniformScale;
        }

        AZ::Vector3 MeshWorldFromLocal(const AZ::EntityId entityId, const AZ::Vector3& localPosition)
        {
            AZ::Vector3 nonUniformScale = AZ::Vector3::CreateOne();
            AZ::NonUniformScaleRequestBus::EventResult(nonUniformScale, entityId, &AZ::NonUniformScaleRequests::GetScale);

            const AZ::Transform worldFromLocal = EditorSpaceFromLocal(entityId);
            return worldFromLocal.TransformPoint(localPosition * nonUniformScale);
        }

        AZStd::optional<size_t> FindAnchorIndex(
            const AZ::EntityId entityId, const int viewportId, const AZStd::vector<AZ::Vector3>& localPositions)
        {
            if (localPositions.empty())
            {
                return AZStd::nullopt;
            }

            const auto cursor = CursorPosition(viewportId);
            if (!cursor.has_value())
            {
                return AZStd::nullopt;
            }

            const AzFramework::CameraState cameraState = AzToolsFramework::GetCameraState(viewportId);
            const float cursorX = aznumeric_cast<float>(cursor->m_x);
            const float cursorY = aznumeric_cast<float>(cursor->m_y);

            size_t nearestIndex = 0;
            float nearestDistanceSq = std::numeric_limits<float>::max();

            for (size_t index = 0; index < localPositions.size(); ++index)
            {
                const AzFramework::ScreenPoint projected =
                    AzFramework::WorldToScreen(MeshWorldFromLocal(entityId, localPositions[index]), cameraState);

                const float deltaX = aznumeric_cast<float>(projected.m_x) - cursorX;
                const float deltaY = aznumeric_cast<float>(projected.m_y) - cursorY;
                const float distanceSq = deltaX * deltaX + deltaY * deltaY;

                if (distanceSq < nearestDistanceSq)
                {
                    nearestDistanceSq = distanceSq;
                    nearestIndex = index;
                }
            }

            return nearestIndex;
        }

        AZStd::optional<size_t> FindAnchorIndex(
            const AZ::EntityId entityId,
            const int viewportId,
            const WhiteBoxMesh& whiteBox,
            const Api::VertexHandles& vertexHandles)
        {
            AZStd::vector<AZ::Vector3> localPositions;
            localPositions.reserve(vertexHandles.size());
            for (const Api::VertexHandle vertexHandle : vertexHandles)
            {
                localPositions.push_back(Api::VertexPosition(whiteBox, vertexHandle));
            }

            return FindAnchorIndex(entityId, viewportId, localPositions);
        }

        AZStd::optional<AZ::Vector3> FindSnapTargetWorld(
            const AZ::EntityId entityId, const int viewportId, const AZStd::vector<AZ::s64>& excludeSourceIndices)
        {
            if (!SnappingActive())
            {
                return AZStd::nullopt;
            }

            const auto cursor = CursorPosition(viewportId);
            if (!cursor.has_value())
            {
                return AZStd::nullopt;
            }

            SnapApi::SnapQueryConfig config;
            // Snapping one part of a mesh onto another part of the same mesh is a normal modelling
            // operation, so the owning entity stays in the query - only the dragged geometry itself
            // is excluded.
            config.m_excludeEntity = entityId;
            config.m_includeExcludedEntity = true;
            config.m_excludeSourceIndices = excludeSourceIndices;

            if (const auto snap = SnapApi::QuerySnapTarget(viewportId, cursor.value(), config))
            {
                return snap->m_worldPosition;
            }

            return AZStd::nullopt;
        }

        AZStd::vector<AZ::s64> ExcludeIndicesFromHandles(const Api::VertexHandles& vertexHandles)
        {
            AZStd::vector<AZ::s64> indices;
            indices.reserve(vertexHandles.size());
            for (const Api::VertexHandle vertexHandle : vertexHandles)
            {
                indices.push_back(aznumeric_cast<AZ::s64>(vertexHandle.Index()));
            }
            return indices;
        }

        AZ::Vector3 ApplyAnchoredSnap(
            WhiteBoxMesh& whiteBox,
            const AZ::EntityId entityId,
            const Api::VertexHandles& vertexHandles,
            const AZ::Vector3& anchorLocal,
            const AZ::Vector3& targetWorld)
        {
            // Applied as a post-correction on top of whatever movement the caller already did, so
            // it works for both absolute (polygon: base + offset) and incremental (edge: previous +
            // displacement) drag styles without either having to be restructured.
            const AZ::Vector3 offset = MeshLocalFromWorld(entityId, targetWorld) - anchorLocal;

            for (const Api::VertexHandle vertexHandle : vertexHandles)
            {
                Api::SetVertexPosition(whiteBox, vertexHandle, Api::VertexPosition(whiteBox, vertexHandle) + offset);
            }

            return offset;
        }

        void DrawSnapTarget(AzFramework::DebugDisplayRequests& debugDisplay, const AZ::Vector3& worldPosition)
        {
            // Depth test off: the target often sits behind the geometry being dragged.
            debugDisplay.DepthTestOff();
            debugDisplay.SetColor(cl_whiteBoxSnapTargetColor);
            debugDisplay.DrawBall(worldPosition, cl_whiteBoxSnapTargetSize);
            debugDisplay.DepthTestOn();
        }

        //! See the note in the header - single drag in flight, viewport thread only.
        static AZStd::optional<AZ::Vector3> s_activeSnapTarget;

        void SetActiveSnapTarget(const AZStd::optional<AZ::Vector3>& worldPosition)
        {
            s_activeSnapTarget = worldPosition;
        }

        void ClearActiveSnapTarget()
        {
            s_activeSnapTarget.reset();
        }

        void DrawActiveSnapTarget(AzFramework::DebugDisplayRequests& debugDisplay)
        {
            if (s_activeSnapTarget.has_value())
            {
                DrawSnapTarget(debugDisplay, s_activeSnapTarget.value());
            }
        }
    } // namespace SnapUtil
} // namespace WhiteBox
