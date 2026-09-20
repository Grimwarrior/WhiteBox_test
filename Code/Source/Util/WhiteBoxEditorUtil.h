/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/ComponentBus.h>

struct IEditor;

namespace WhiteBox
{
    class EditorWhiteBoxComponent;

    //! The White Box component named by @p entityComponentIdPair, or null when the entity is gone or
    //! the id names something else. Shared because the floating tool windows and the modeling
    //! operations all need it - and because four private copies in anonymous namespaces collide the
    //! moment two of them land in the same unity build translation unit.
    EditorWhiteBoxComponent* FindWhiteBoxComponent(const AZ::EntityComponentIdPair& entityComponentIdPair);

    //! Small wrapper around an EBus call to request the file at the given path be added to source control.
    void RequestEditSourceControl(const char* absoluteFilePath);

    //! Small wrapper around an EBus call to retrieve the IEditor interface.
    IEditor* GetIEditor();
} // namespace WhiteBox
