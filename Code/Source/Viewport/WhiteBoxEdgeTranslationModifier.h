/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "Viewport/WhiteBoxViewportConstants.h"

#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/Component/ComponentBus.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace AzToolsFramework
{
    class PlanarManipulator;

    namespace ViewportInteraction
    {
        struct MouseInteraction;
    }
} // namespace AzToolsFramework

namespace WhiteBox
{
    class ManipulatorViewEdge;

    //! EdgeTranslationModifier provides the ability to select and draw an edge in the viewport.
    class EdgeTranslationModifier
    {
    public:
        AZ_CLASS_ALLOCATOR_DECL

        using HandleType = Api::EdgeHandle;

        EdgeTranslationModifier(
            const AZ::EntityComponentIdPair& entityComponentIdPair, Api::EdgeHandle edgeHandle,
            const AZ::Vector3& intersectionPoint);
        ~EdgeTranslationModifier();

        bool MouseOver() const;
        void ForwardMouseOverEvent(const AzToolsFramework::ViewportInteraction::MouseInteraction& interaction);

        Api::EdgeHandle GetHandle() const; //!< Return the currently hovered edge (generic context version).
        Api::EdgeHandle GetEdgeHandle() const; //!< Return the currently hovered edge.
        Api::EdgeHandles::const_iterator EdgeHandlesBegin() const; //!< Return begin iterator to edge handles.
        Api::EdgeHandles::const_iterator EdgeHandlesEnd() const; //!< Return end iterator to edge handles.
        void SetEdgeHandle(Api::EdgeHandle edgeHandle);

        void SetColors(const AZ::Color& color, const AZ::Color& hoverColor);
        void SetWidths(float width, float hoverWidth);

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

        //! Copy of the mesh taken at mouse down, used to revert the drag when Escape is pressed.
        //! Restores topology too, so it also undoes an extrude performed during the drag.
        Api::WhiteBoxMeshPtr m_dragSnapshot;
        //! The handles as they were at mouse down. An extrude during the drag repoints these at
        //! newly created geometry, which the mesh restore then deletes - so they have to be put
        //! back alongside it or the modifier is left referencing handles that no longer exist.
        Api::EdgeHandles m_dragEdgeHandlesSnapshot;
        Api::EdgeHandle m_dragHoveredEdgeHandleSnapshot;
        //! Set by CancelDrag - suppresses further movement until the mouse button is released.
        bool m_dragCancelled = false;
        Api::EdgeHandles m_edgeHandles; //!< The edge handles this modifier is currently associated with (edge group).
        Api::EdgeHandle m_hoveredEdgeHandle; //!< The edge handle the mouse is currently over.
        //! The entity and component id this modifier is associated with.
        AZ::EntityComponentIdPair m_entityComponentIdPair;
        //! Manipulators for performing edge translations.
        AZStd::shared_ptr<AzToolsFramework::PlanarManipulator> m_translationManipulator;
        //! Manipulator views used to represent mesh edges for translation.
        AZStd::vector<AZStd::shared_ptr<ManipulatorViewEdge>> m_edgeViews;
        AZ::Color m_color = ed_whiteBoxEdgeDefault; //!< The color to use for the regular edge.
        AZ::Color m_hoverColor = ed_whiteBoxOutlineHover; //!< The color to use for the selected/highlighted edge.
        float m_width = cl_whiteBoxEdgeVisualWidth; //!< The width to use for the regular edge.
        //! The visible width to use for the selected/highlighted edge.
        float m_hoverWidth = cl_whiteBoxSelectedEdgeVisualWidth;
    };

    AZStd::array<AZ::Vector3, 2> GetEdgeNormalAxes(const AZ::Vector3& start, const AZ::Vector3& end);

    inline Api::EdgeHandle EdgeTranslationModifier::GetHandle() const
    {
        return GetEdgeHandle();
    }

    inline Api::EdgeHandle EdgeTranslationModifier::GetEdgeHandle() const
    {
        return m_hoveredEdgeHandle;
    }

    inline Api::EdgeHandles::const_iterator EdgeTranslationModifier::EdgeHandlesBegin() const
    {
        return m_edgeHandles.cbegin();
    }

    inline Api::EdgeHandles::const_iterator EdgeTranslationModifier::EdgeHandlesEnd() const
    {
        return m_edgeHandles.cend();
    }

    inline void EdgeTranslationModifier::SetEdgeHandle(const Api::EdgeHandle edgeHandle)
    {
        m_hoveredEdgeHandle = edgeHandle;
    }
} // namespace WhiteBox
