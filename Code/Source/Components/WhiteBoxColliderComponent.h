/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "WhiteBoxColliderConfiguration.h"

#include <AzCore/Component/Component.h>
#include <AzCore/Component/TransformBus.h>
#include <AzFramework/Physics/Shape.h>
#include <AzFramework/Physics/Common/PhysicsTypes.h>
#include <AzFramework/Physics/SimulatedBodies/RigidBody.h>
#include <WhiteBox/WhiteBoxColliderBus.h>

namespace WhiteBox
{
    //! Component that provides a White box Collider.
    //! It covers the rigid body functionality as well, but it can be refactored out
    //! once EditorStaticRigidBodyComponent handles the creation of the simulated body.
    class WhiteBoxColliderComponent
        : public AZ::Component
        , private AZ::TransformNotificationBus::Handler
        , public WhiteBoxColliderRequestBus::Handler
    {
    public:
        AZ_COMPONENT(WhiteBoxColliderComponent, "{B60C4D82-3299-414A-B91B-0299AA51BEF6}");
        static void Reflect(AZ::ReflectContext* context);

        WhiteBoxColliderComponent() = default;
        WhiteBoxColliderComponent(
            const Physics::CookedMeshShapeConfiguration& meshShape,
            const Physics::ColliderConfiguration& physicsColliderConfiguration,
            const WhiteBoxColliderConfiguration& whiteBoxColliderConfiguration,
            const Physics::CookedMeshShapeConfiguration& booleanMeshShape = {},
            bool hasBooleanMesh = false,
            bool useBooleanMesh = false);
        WhiteBoxColliderComponent(const WhiteBoxColliderComponent&) = delete;
        WhiteBoxColliderComponent& operator=(const WhiteBoxColliderComponent&) = delete;

    private:
        static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided);
        static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required);
        static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible);

        // AZ::Component ...
        void Activate() override;
        void Deactivate() override;

        // AZ::TransformNotificationBus ...
        void OnTransformChanged(const AZ::Transform& local, const AZ::Transform& world) override;

        // WhiteBoxColliderRequestBus ...
        void BakeCollider(bool useBooleanMesh) override;

        //! Create the simulated body from the currently selected cooked mesh (removing any
        //! existing body first).
        void RebuildBody();
        //! Remove the current simulated body if one exists.
        void DestroyBody();
        //! The cooked mesh currently selected by m_useBooleanMesh.
        const Physics::CookedMeshShapeConfiguration& ActiveShapeConfiguration() const;

        Physics::CookedMeshShapeConfiguration m_shapeConfiguration; //!< The base physics representation of the mesh.
        Physics::CookedMeshShapeConfiguration
            m_booleanShapeConfiguration; //!< The boolean-evaluated physics representation (pre-baked at build time).
        bool m_hasBooleanMesh = false; //!< Whether a boolean-evaluated cooked mesh was baked.
        bool m_useBooleanMesh = false; //!< Whether the boolean-evaluated mesh is currently in use.
        Physics::ColliderConfiguration
            m_physicsColliderConfiguration; //!< General physics collider configuration information.
        AzPhysics::SimulatedBodyHandle m_simulatedBodyHandle = AzPhysics::InvalidSimulatedBodyHandle; //!< Simulated body to represent the White Box Mesh at runtime.
        WhiteBoxColliderConfiguration
            m_whiteBoxColliderConfiguration; //!< White Box specific collider configuration information.
    };
} // namespace WhiteBox
