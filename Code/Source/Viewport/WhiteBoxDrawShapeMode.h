/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/ComponentBus.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzFramework/Entity/EntityDebugDisplayBus.h>
#include <AzToolsFramework/Viewport/ViewportTypes.h>
#include <EditorWhiteBoxComponentModeTypes.h>

namespace WhiteBox
{
    //! DrawShapeMode implements Lumberyard-style click-drag-release-pull shape drawing
    //! inside the White Box component mode.
    //!
    //! Interaction flow:
    //!   1. Left mouse down  → anchor first corner (world hit or ground plane)
    //!   2. Left mouse drag  → size the base rectangle (live preview)
    //!   3. Left mouse up    → lock base, enter height-pull phase
    //!   4. Mouse move       → pull height along up-axis (live preview)
    //!   5. Left mouse down  → commit box to white box mesh + undo batch
    //!   Right-click / Esc  → cancel at any phase
    class DrawShapeMode 
        : private AzFramework::ViewportDebugDisplayEventBus::Handler
    {
    public:
        AZ_CLASS_ALLOCATOR_DECL

        explicit DrawShapeMode(const AZ::EntityComponentIdPair& entityComponentIdPair);
        ~DrawShapeMode();

        //! Forward a raw mouse interaction event.
        //! @return true if the event was consumed (prevents other handlers from seeing it).
        bool HandleMouseInteraction(
            const AzToolsFramework::ViewportInteraction::MouseInteractionEvent& mouseInteraction,
            const AZ::Transform& worldFromLocal,
            const IntersectionAndRenderData& intersectionData);

        //! Draw the ghost preview into the viewport.
        // void Display(
        //     const AzFramework::ViewportInfo& viewportInfo,
        //     AzFramework::DebugDisplayRequests& debugDisplay) const;

        //! Required by EditorWhiteBoxComponentMode variant dispatch.
        void Refresh() {}

        //! Required by EditorWhiteBoxComponentMode variant dispatch.
        AZStd::vector<AzToolsFramework::ActionOverride> PopulateActions(
            const AZ::EntityComponentIdPair& entityComponentIdPair);

    private:
        // Override the global Viewport Display method
        void DisplayViewport(
            const AzFramework::ViewportInfo& viewportInfo,
            AzFramework::DebugDisplayRequests& debugDisplay) override;
        //! Three-phase draw state.
        enum class DrawState
        {
            Idle,           //!< Waiting for first click.
            DraggingBase,   //!< User is dragging out the base rectangle.
            PullingHeight,  //!< Base is locked; user moves mouse to set height.
        };

        //! Raycast the mouse ray against the existing white box mesh polygons,
        //! falling back to an infinite XZ plane through the entity origin.
        //! Returns a position in world space.
        AZ::Vector3 RaycastToSurface(
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
            const AZ::Transform& worldFromLocal,
            const IntersectionAndRenderData& intersectionData,
            AZ::Vector3& outWorldNormal) const;   // NEW out-param

        //! Raycast the mouse ray against a vertical plane that faces the camera
        //! and passes through baseCenter (world space).
        //! Returns the signed height delta above baseCenter.
        float RaycastToHeightPlane(
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
            const AZ::Transform& worldFromLocal,
            const AZ::Vector3& baseCenterWorld) const;

        void CarveAtPolygon(
            const AzToolsFramework::ViewportInteraction::MouseInteraction& mouseInteraction,
            const AZ::Transform& worldFromLocal,
            const IntersectionAndRenderData& intersectionData,
            float insetScale,
            float depth,
            bool openHole);
        
        bool m_carveMode = false;   // set on first click if Ctrl is held
        //! Stamp the current drawn AABB into the white box mesh and record an undo batch.
        void CommitBox(const AZ::Transform& worldFromLocal);

        //! Cancel draw and return to Idle, discarding any in-progress shape.
        void Cancel();

        AZ::EntityComponentIdPair m_entityComponentIdPair;

        DrawState m_state = DrawState::Idle;

        // Add a member to store the anchor's surface frame:
        AZ::Vector3 m_surfaceNormal = AZ::Vector3::CreateAxisZ();

        //! World-space corners of the drawn base rectangle.
        AZ::Vector3 m_worldP0 = AZ::Vector3::CreateZero();
        AZ::Vector3 m_worldP1 = AZ::Vector3::CreateZero();

        //! Height above the base plane pulled during phase 3.
        float m_height = 0.f;

        //! World-space Y (up) value of the ground plane established on first click.
        float m_groundZ = 0.f;
    };
} // namespace WhiteBox
