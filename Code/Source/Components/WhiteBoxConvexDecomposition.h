/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/functional.h>

namespace WhiteBox
{
    struct WhiteBoxColliderConfiguration;

    //! The points of one convex part; PhysX builds the hull around them.
    using HullPoints = AZStd::vector<AZ::Vector3>;

    //! What Decompose does with a concave shell V-HACD has not finished yet.
    enum class DecomposeWait
    {
        Background, //!< Queue it and stand in a single hull; onReady runs on the main thread when it lands.
        Block       //!< Run it now on the calling thread (game builds, where a stand-in would ship).
    };

    //! Splits collision geometry into convex parts, one connected shell at a time: a convex shell is kept exactly, a flat
    //! one is given a thin backing, and only concave shells go through V-HACD. V-HACD runs on one background worker
    //! shared by every collider, and results are cached by shell content for the session, so edits and undo only pay
    //! for the shells that actually changed.
    class ConvexDecomposer
    {
    public:
        //! @param vertices / indices a welded triangle list (shared corners share an index).
        //! @param requester who is told (once per queued shell) through onReady.
        //! @param pending set when any shell is still a stand-in hull.
        static AZStd::vector<HullPoints> Decompose(
            const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices,
            const WhiteBoxColliderConfiguration& configuration, DecomposeWait wait = DecomposeWait::Block,
            AZ::EntityId requester = AZ::EntityId(), const AZStd::function<void()>& onReady = {}, bool* pending = nullptr);

        //! Cancel queued and running work and join the worker; called before the module unloads.
        static void Shutdown();
    };
} // namespace WhiteBox
