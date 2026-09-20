/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "EditorWhiteBoxComponent.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/SourceControl/SourceControlAPI.h>

namespace WhiteBox
{
    EditorWhiteBoxComponent* FindWhiteBoxComponent(const AZ::EntityComponentIdPair& entityComponentIdPair)
    {
        AZ::Entity* entity = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(
            entity, &AZ::ComponentApplicationRequests::FindEntity, entityComponentIdPair.GetEntityId());
        return entity != nullptr
            ? azrtti_cast<EditorWhiteBoxComponent*>(entity->FindComponent(entityComponentIdPair.GetComponentId()))
            : nullptr;
    }

    void RequestEditSourceControl(const char* absoluteFilePath)
    {
        bool active = false;
        AzToolsFramework::SourceControlConnectionRequestBus::BroadcastResult(
            active, &AzToolsFramework::SourceControlConnectionRequestBus::Events::IsActive);

        if (active)
        {
            AzToolsFramework::SourceControlCommandBus::Broadcast(
                &AzToolsFramework::SourceControlCommandBus::Events::RequestEdit, absoluteFilePath, true,
                []([[maybe_unused]] bool success, [[maybe_unused]] AzToolsFramework::SourceControlFileInfo info)
                {
                });
        }
    }

    IEditor* GetIEditor()
    {
        IEditor* editor = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);
        return editor;
    }
} // namespace WhiteBox
