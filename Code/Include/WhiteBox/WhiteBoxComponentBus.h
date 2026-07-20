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
        //! @param rebakeCollider When true, force the physics collider to rebuild from the
        //! entity's current size/scale even if the live-boolean selection has not changed.
        //! The cooked collision mesh is authored in local (unscaled) space and the runtime
        //! body folds in the entity's current uniform scale when it is (re)built, so this is
        //! how a resized entity gets a matching collider at runtime. Defaults to false so
        //! existing callers keep the original behaviour (only rebuild on a boolean change).
        virtual void BakeWhiteBox([[maybe_unused]] bool rebakeCollider = false) {}

    protected:
        ~WhiteBoxComponentRequests() = default;
    };

    using WhiteBoxComponentRequestBus = AZ::EBus<WhiteBoxComponentRequests>;
} // namespace WhiteBox
