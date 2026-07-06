/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/Component/ComponentBus.h>

namespace WhiteBox
{
    //! WhiteBoxComponent requests.
    //! @note Addressed by EntityId (a single runtime White Box per entity) so it can be
    //! called ergonomically from runtime Lua, e.g. WhiteBoxComponentRequestBus.Event.BakeWhiteBox(entityId).
    class WhiteBoxComponentRequests : public AZ::ComponentBus
    {
    public:
        // EBusTraits overrides ...
        static const AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;

        virtual bool WhiteBoxIsVisible() const = 0;

        //! Select whether the runtime White Box uses its baked live-boolean geometry.
        //! @note The CSG boolean is evaluated in the Editor when the game entity is built,
        //! so this switches between the pre-baked base and boolean-evaluated variants rather
        //! than re-running the boolean. Call BakeWhiteBox to apply the change to the render
        //! mesh and physics collider. If no boolean variant was baked this has no effect.
        virtual void SetLiveBoolean([[maybe_unused]] bool enabled) {}

        //! Whether the runtime White Box is currently set to use its baked boolean geometry.
        virtual bool GetLiveBoolean() const
        {
            return false;
        }

        //! Apply the current live-boolean selection: rebuild the render mesh and re-create
        //! the physics collider from the matching pre-baked geometry. This is the "bake"
        //! command callable from Lua at runtime.
        virtual void BakeWhiteBox() {}

    protected:
        ~WhiteBoxComponentRequests() = default;
    };

    using WhiteBoxComponentRequestBus = AZ::EBus<WhiteBoxComponentRequests>;
} // namespace WhiteBox
