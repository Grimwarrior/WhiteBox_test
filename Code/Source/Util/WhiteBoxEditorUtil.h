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

struct IEditor;

namespace WhiteBox
{
    class EditorWhiteBoxComponent;

    //! The White Box component named by @p entityComponentIdPair, or null when the entity is gone or
    //! the id names something else. Shared because the floating tool windows and the modeling
    //! operations all need it - and because four private copies in anonymous namespaces collide the
    //! moment two of them land in the same unity build translation unit.
    EditorWhiteBoxComponent* FindWhiteBoxComponent(const AZ::EntityComponentIdPair& entityComponentIdPair);

    //! The space every White Box editing overlay and manipulator lives in: the entity's world
    //! transform with the ACTIVE LAYER's transform folded in. A layer's transform is applied when the
    //! mesh is combined for display, never to the vertices being edited, so without this the handles
    //! stay on the untransformed geometry until Apply Transform bakes the layer transform in. It is
    //! exactly the entity transform while the layer's own is identity, which is the usual case.
    //! @note Manipulators take an AZ::Transform, which carries a single scale, so a non-uniform layer
    //! scale is folded in as its largest component - the one part of a layer transform the handles
    //! cannot follow exactly.
    AZ::Transform EditorSpaceFromLocal(const AZ::EntityComponentIdPair& entityComponentIdPair);
    //! As above, for the callers that only carry an entity id. Uses the entity's White Box component.
    AZ::Transform EditorSpaceFromLocal(AZ::EntityId entityId);

    //! Small wrapper around an EBus call to request the file at the given path be added to source control.
    void RequestEditSourceControl(const char* absoluteFilePath);

    //! Small wrapper around an EBus call to retrieve the IEditor interface.
    IEditor* GetIEditor();
} // namespace WhiteBox
