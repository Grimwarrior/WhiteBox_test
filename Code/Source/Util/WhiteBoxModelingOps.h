/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/EntityId.h>
#include <AzCore/std/string/string.h>

namespace WhiteBox
{
    //! The Transform-mode modeling operations (Bridge, Weld, Loop Cut, Bevel), factored out of the
    //! White Box pane so the viewport cluster runs the same code rather than a second copy. Each takes
    //! the component to act on and reports what to show the user; neither caller owns the logic.
    namespace ModelingOps
    {
        //! What an operation did, for whichever surface invoked it to display.
        struct Result
        {
            bool m_success = false;
            AZStd::string m_message; //!< The error when it failed, otherwise what just happened.
        };

        //! Whether each operation would do anything with the current selection. The pane uses these to
        //! enable its buttons and the cluster to disable its own, so the two agree on what is possible.
        bool CanBridge(const AZ::EntityComponentIdPair& entityComponentIdPair);
        bool CanWeld(const AZ::EntityComponentIdPair& entityComponentIdPair);
        bool CanLoopCut(const AZ::EntityComponentIdPair& entityComponentIdPair);
        bool CanBevel(const AZ::EntityComponentIdPair& entityComponentIdPair);
        bool HasLiveBevel(const AZ::EntityComponentIdPair& entityComponentIdPair);
        bool CanSelectEdgePattern(const AZ::EntityComponentIdPair& entityComponentIdPair);
        //! Selection only: expands all current edge seeds, without baking or editing geometry.
        Result SelectEdgePattern(const AZ::EntityComponentIdPair& entityComponentIdPair, bool ring);

        //! Connect the selection: two open boundary edges, or two facing polygons.
        Result Bridge(const AZ::EntityComponentIdPair& entityComponentIdPair);
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
