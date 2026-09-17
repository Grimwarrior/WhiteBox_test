/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "Viewport/WhiteBoxManipulatorBounds.h"
#include "Viewport/WhiteBoxViewportConstants.h"

#include <AzCore/Component/ComponentBus.h>
#include <AzToolsFramework/Viewport/ViewportTypes.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace AzToolsFramework
{
    class LinearManipulator;
}

namespace WhiteBox
{
    class ManipulatorViewPolygon;

    //! Provides manipulators for translating a polygon on a white box mesh.
    class PolygonTranslationModifier
    {
    public:
        AZ_CLASS_ALLOCATOR_DECL

        using HandleType = Api::PolygonHandle;

        PolygonTranslationModifier(
            const AZ::EntityComponentIdPair& entityComponentIdPair, const Api::PolygonHandle& polygonHandle,
            const AZ::Vector3& intersectionPoint);
        ~PolygonTranslationModifier();

        bool MouseOver() const;
        void ForwardMouseOverEvent(const AzToolsFramework::ViewportInteraction::MouseInteraction& interaction);

        Api::PolygonHandle GetHandle() const; // Generic context version
        Api::PolygonHandle GetPolygonHandle() const;
        void SetPolygonHandle(const Api::PolygonHandle& polygonHandle);
        void SetColors(const AZ::Color& fillColor, const AZ::Color& outlineColor);

        void Refresh();
        void CreateView();
        bool PerformingAction() const;

        //! Abandon the drag in progress: restore the mesh to its state at mouse down and ignore
        //! any further mouse movement until the button is released. Driven by right click.
        //! @param notify Raise the mesh-modified / intersection-dirty notifications. Pass false
        //! when calling from the destructor - the surrounding component mode may already be part
        //! way through its own teardown, and the rebuild those notifications trigger is redundant
        //! there anyway.
        //! @return True if there was a drag to cancel.
        bool CancelDrag(bool notify = true);

    private:
        void CreateManipulator();
        void DestroyManipulator();

        AZ::EntityComponentIdPair
            m_entityComponentIdPair; //!< The entity and component id this modifier is associated with.
        AZStd::shared_ptr<AzToolsFramework::LinearManipulator>
            m_translationManipulator; //!< The manipulator used to modify the position of the polygon (triangles).
        //! Copy of the mesh taken at mouse down, used to revert the drag when Escape is pressed.
        //! Restores topology too, so it also undoes an extrude performed during the drag.
        Api::WhiteBoxMeshPtr m_dragSnapshot;
        //! The handles as they were at mouse down. An extrude during the drag repoints these at
        //! newly created geometry, which the mesh restore then deletes - so they have to be put
        //! back alongside it or the modifier is left referencing handles that no longer exist.
        Api::PolygonHandle m_dragPolygonHandleSnapshot;
        AZStd::vector<Api::VertexHandle> m_dragVertexHandlesSnapshot;
        //! Set by CancelDrag - suppresses further movement until the mouse button is released.
        bool m_dragCancelled = false;
        AZStd::vector<Api::VertexHandle> m_vertexHandles; //!< The vertex handles associated with this polygon.
        Api::PolygonHandle m_polygonHandle; //!< The polygon handle this modifier is associated with.
        AZStd::shared_ptr<ManipulatorViewPolygon>
            m_polygonView; //!< Manipulator view used to represent a mesh polygon for translation.
        AZ::Color m_fillColor =
            ed_whiteBoxPolygonHover; //!< The color to use for the highlighted filled section of the polygon.
        AZ::Color m_outlineColor =
            ed_whiteBoxOutlineHover; //!< The color to use for the outline of the polygon.
    };

    inline Api::PolygonHandle PolygonTranslationModifier::GetHandle() const
    {
        return GetPolygonHandle();
    }

    inline Api::PolygonHandle PolygonTranslationModifier::GetPolygonHandle() const
    {
        return m_polygonHandle;
    }

    inline void PolygonTranslationModifier::SetColors(const AZ::Color& fillColor, const AZ::Color& outlineColor)
    {
        m_fillColor = fillColor;
        m_outlineColor = outlineColor;
    }
} // namespace WhiteBox
