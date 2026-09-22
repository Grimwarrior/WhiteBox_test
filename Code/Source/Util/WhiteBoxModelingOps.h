/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/ComponentBus.h>
#include <AzCore/std/string/string.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace WhiteBox
{
    //! The Transform-mode modeling operations (Extrude, Inset, Bridge, Weld, Loop Cut, Bevel),
    //! factored out of the White Box pane so the viewport cluster and the floating windows run the same
    //! code rather than a copy each. Each reports what to show the user; no caller owns the logic.
    namespace ModelingOps
    {
        //! What an operation did, for whichever surface invoked it to display.
        struct Result
        {
            bool m_success = false;
            AZStd::string m_message; //!< The error when it failed, otherwise what just happened.
        };

        //! The Transform-mode selection, read once. A polygon handle owns a vector of face handles,
        //! so a caller that asks several of the questions below - the viewport cluster asks five of
        //! them every frame - copies far less than by letting each question fetch its own.
        struct Selection
        {
            Api::PolygonHandles m_polygons;
            Api::EdgeHandles m_edges;
            Api::VertexHandles m_vertices;
            bool m_editable = false;  //!< There is a component with a mesh to act on.
            bool m_liveBevel = false; //!< A bevel is already live, so another cannot start.
        };
        Selection CurrentSelection(const AZ::EntityComponentIdPair& entityComponentIdPair);

        //! Whether each operation would do anything with the given selection. Whichever surface offers
        //! the operation uses these to enable its buttons, so what is clickable and what will actually
        //! run are decided by the same code.
        bool CanBridge(const Selection& selection);
        bool CanFillHole(const Selection& selection);
        bool CanDeletePolygon(const Selection& selection);
        bool CanWeld(const Selection& selection);
        bool CanLoopCut(const Selection& selection);
        bool CanBevel(const Selection& selection);
        bool CanSelectEdgePattern(const Selection& selection);
        //! A typed amount only applies to polygons. Edges extrude by dragging with the latch on.
        bool CanExtrudeInset(const Selection& selection);

        Result ExtrudeInset(const AZ::EntityComponentIdPair& pair, float amount, bool inset);
        bool HasLiveBevel(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Selection only: expands all current edge seeds, without baking or editing geometry.
        Result SelectEdgePattern(const AZ::EntityComponentIdPair& entityComponentIdPair, bool ring);

        //! Connect the selection: two open boundary edges, or two facing polygons.
        Result Bridge(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Close the single planar hole bordered by the selected open edges, and select the new cap.
        Result FillHole(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Delete the selected polygons but keep their vertices, so the opening can be refilled.
        Result DeletePolygon(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Merge the selected vertices into one, at the selection centre or at the last one selected.
        Result Weld(const AZ::EntityComponentIdPair& entityComponentIdPair, bool atLastVertex);
        //! Enter the interactive loop cut. Switches to Transform sub-mode first if something else is active.
        Result BeginLoopCut(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Start a live bevel on the selection using the component's current bevel parameters. The
        //! perimeter edges of a selected polygon region are included; its interior edges are not, so the
        //! region does not gain cuts through the middle of it.
        Result BeginBevel(const AZ::EntityComponentIdPair& entityComponentIdPair);
    } // namespace ModelingOps
} // namespace WhiteBox
