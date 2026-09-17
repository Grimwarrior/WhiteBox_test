/*
 * White Box - vertex snapping helpers.
 *
 * Thin wrappers over the editor-wide vertex snapping service, shared by every White Box viewport
 * tool that can be dragged: the vertex/edge/polygon modifiers, the transform sub-mode and the
 * draw-mode cube stamp.
 *
 * Every entry point is safe to call when no snapper gem is installed or snapping is switched
 * off - they simply report "no snap" and the caller keeps its original behaviour.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/base.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/optional.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace AzFramework
{
    class DebugDisplayRequests;
}

namespace WhiteBox
{
    //! Vertex snapping helpers shared by the White Box viewport tools.
    namespace SnapUtil
    {
        //! Is the user's Snap to Vertex toggle on (and a snapper gem present).
        //! Cheap - call it to skip snapping work entirely before building exclusion lists.
        bool SnappingActive();

        //!@{
        //! The viewport the mouse is currently interacting with.
        //!
        //! Manipulator callbacks would be the natural place to read this from, but only
        //! MultiLinearManipulator::Action carries an m_viewportId - Linear, Planar and Surface
        //! actions do not. So EditorWhiteBoxComponentMode::HandleMouseInteraction, which does see
        //! the real MouseInteractionEvent, records it here for the manipulator callbacks to read.
        //!
        //! @note Manipulators receive a mouse event before component modes do, so on the very
        //! first event of a session this may still be invalid. In practice hover moves have
        //! already set it long before any drag starts, and a stale or invalid id simply makes the
        //! cursor query return nothing, which disables snapping for that frame rather than
        //! misbehaving.
        void SetActiveViewportId(int viewportId);
        int ActiveViewportId();
        //!@}

        //! Convert between world space and the local space White Box mesh vertices live in.
        //! @note White Box manipulators operate in WorldFromLocalWithUniformScale, so any
        //! Non-Uniform Scale component is applied/divided separately. The vertex source multiplies
        //! it in when publishing world positions, so these two must stay in agreement with it.
        AZ::Vector3 MeshLocalFromWorld(AZ::EntityId entityId, const AZ::Vector3& worldPosition);
        AZ::Vector3 MeshWorldFromLocal(AZ::EntityId entityId, const AZ::Vector3& localPosition);

        //! Index into @p localPositions of the vertex whose screen projection is nearest the cursor.
        //! This is the anchor: the point of a multi-vertex element that will land on the snap
        //! target. Resolved once at mouse-down and held for the drag so it cannot flip mid-drag.
        //! @return Nothing when the cursor is not over a viewport or the list is empty.
        AZStd::optional<size_t> FindAnchorIndex(
            AZ::EntityId entityId, int viewportId, const AZStd::vector<AZ::Vector3>& localPositions);

        //! Convenience overload taking vertex handles and reading their positions from @p whiteBox.
        AZStd::optional<size_t> FindAnchorIndex(
            AZ::EntityId entityId, int viewportId, const WhiteBoxMesh& whiteBox, const Api::VertexHandles& vertexHandles);

        //! The snappable vertex under the cursor, in world space.
        //! @param excludeSourceIndices Vertex handle indices belonging to the element being dragged;
        //! they are ignored so an element cannot snap to itself.
        AZStd::optional<AZ::Vector3> FindSnapTargetWorld(
            AZ::EntityId entityId, int viewportId, const AZStd::vector<AZ::s64>& excludeSourceIndices);

        //! Build an exclusion list from the handles of the element being dragged.
        //! @note Api::VertexHandles is already AZStd::vector<Api::VertexHandle>, so this one
        //! overload covers both spellings.
        AZStd::vector<AZ::s64> ExcludeIndicesFromHandles(const Api::VertexHandles& vertexHandles);

        //! Rigidly translate @p vertexHandles so the vertex at @p anchorLocal lands on @p targetWorld.
        //! @return The local space offset that was applied, so callers can move their manipulator by
        //! the same amount.
        AZ::Vector3 ApplyAnchoredSnap(
            WhiteBoxMesh& whiteBox,
            AZ::EntityId entityId,
            const Api::VertexHandles& vertexHandles,
            const AZ::Vector3& anchorLocal,
            const AZ::Vector3& targetWorld);

        //! Draw the highlight on the vertex currently being snapped to (world space, depth test off).
        void DrawSnapTarget(AzFramework::DebugDisplayRequests& debugDisplay, const AZ::Vector3& worldPosition);

        //!@{
        //! The vertex the drag in progress is snapped to, shared by all White Box tools.
        //!
        //! Most of the draggable tools (edge, polygon, transform sub-mode, cube stamp) are not
        //! themselves viewport display handlers, so rather than making each one into one they
        //! publish their snap target here and EditorWhiteBoxComponentMode::DisplayEntityViewport
        //! draws it once per frame.
        //!
        //! @note Deliberately process-wide mutable state. It is editor-only, touched only from
        //! single-threaded viewport callbacks, and there is only ever one drag in flight. Each
        //! tool must Clear on mouse up / invalidate or the marker will linger.
        void SetActiveSnapTarget(const AZStd::optional<AZ::Vector3>& worldPosition);
        void ClearActiveSnapTarget();
        void DrawActiveSnapTarget(AzFramework::DebugDisplayRequests& debugDisplay);
        //!@}
    } // namespace SnapUtil
} // namespace WhiteBox
