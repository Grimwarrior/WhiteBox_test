/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/EntityId.h>

namespace WhiteBox
{
    //! Create a new "layer": a child entity under @p parentEntityId with its own
    //! EditorWhiteBoxComponent, then select it and enter its White Box component (edit) mode so
    //! the next shape/cube is drawn into the new layer. The child inherits the parent's world
    //! transform, so geometry lines up with the parent's origin. Selection + mode entry are
    //! deferred to the next tick (once the entity/component have activated). Runs as one undo step.
    //! @return the created child entity id (invalid on failure).
    AZ::EntityId CreateChildWhiteBoxLayer(AZ::EntityId parentEntityId);
} // namespace WhiteBox
