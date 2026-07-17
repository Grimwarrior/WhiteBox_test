/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "EditorWhiteBoxColliderComponent.h"
#include "EditorWhiteBoxComponent.h"
#include "WhiteBoxColliderComponent.h"

#include <AzCore/Component/TransformBus.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzFramework/Physics/PhysicsScene.h>
#include <AzFramework/Physics/SystemBus.h>
#include <AzFramework/Physics/Configuration/StaticRigidBodyConfiguration.h>
#include <WhiteBox/EditorWhiteBoxComponentBus.h>
#include <numeric>

namespace WhiteBox
{
    void EditorWhiteBoxColliderComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<EditorWhiteBoxColliderComponent, EditorComponentBase>()
                ->Version(1)
                ->Field("Configuration", &EditorWhiteBoxColliderComponent::m_physicsColliderConfiguration)
                ->Field("MeshData", &EditorWhiteBoxColliderComponent::m_meshShapeConfiguration)
                ->Field("WhiteBoxConfiguration", &EditorWhiteBoxColliderComponent::m_whiteBoxColliderConfiguration);

            if (auto editContext = serializeContext->GetEditContext())
            {
                editContext
                    ->Class<EditorWhiteBoxColliderComponent>(
                        "White Box Collider", "Physics collider for White Box Component")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "Shape")
                    ->Attribute(AZ::Edit::Attributes::Icon, "Editor/Icons/Components/WhiteBox_collider.svg")
                    ->Attribute(
                        AZ::Edit::Attributes::ViewportIcon, "Editor/Icons/Components/Viewport/WhiteBox_collider.png")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu, AZ_CRC_CE("Game"))
                    ->Attribute(
                        AZ::Edit::Attributes::HelpPageURL,
                        "https://o3de.org/docs/user-guide/components/reference/shape/white-box-collider/")
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default, &EditorWhiteBoxColliderComponent::m_physicsColliderConfiguration,
                        "Configuration", "Collider configuration")
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Default,
                        &EditorWhiteBoxColliderComponent::m_whiteBoxColliderConfiguration,
                        "White Box Collider Configuration", "White Box collider configuration properties")
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly);
            }
        }
    }

    void EditorWhiteBoxColliderComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("WhiteBoxColliderService"));
    }

    void EditorWhiteBoxColliderComponent::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required)
    {
        required.push_back(AZ_CRC_CE("TransformService"));
        required.push_back(AZ_CRC_CE("WhiteBoxService"));
    }

    void EditorWhiteBoxColliderComponent::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
    {
        incompatible.push_back(AZ_CRC_CE("NonUniformScaleService"));
        incompatible.push_back(AZ_CRC_CE("WhiteBoxColliderService"));
        // Incompatible with other rigid bodies because it handles its own rigid body
        // internally and it would conflict if another rigid body is added to the entity.
        incompatible.push_back(AZ_CRC_CE("PhysicsRigidBodyService"));
    }

    void EditorWhiteBoxColliderComponent::Activate()
    {
        AzToolsFramework::Components::EditorComponentBase::Activate();
        EditorWhiteBoxColliderRequestBus::Handler::BusConnect(GetEntityId());
        AZ::TransformNotificationBus::Handler::BusConnect(GetEntityId());

        // hide collider properties we do not care about for white box
        m_physicsColliderConfiguration.SetPropertyVisibility(Physics::ColliderConfiguration::Offset, false);
        m_physicsColliderConfiguration.SetPropertyVisibility(Physics::ColliderConfiguration::IsTrigger, false);

        m_sceneInterface = AZ::Interface<AzPhysics::SceneInterface>::Get();
        if (m_sceneInterface)
        {
            m_editorSceneHandle = m_sceneInterface->GetSceneHandle(AzPhysics::EditorPhysicsSceneName);
        }

        // can't use buses here as EditorWhiteBoxComponentBus is addressed using component id. How do get component id?
        if (auto whiteBoxComponent = GetEntity()->FindComponent<WhiteBox::EditorWhiteBoxComponent>())
        {
            // use the evaluated mesh so the edit-time collider matches the (possibly
            // live-boolean) geometry that is rendered.
            if (auto whiteBoxMesh = whiteBoxComponent->GetEvaluatedWhiteBoxMesh())
            {
                CreatePhysics(*whiteBoxMesh);
            }
        }
    }

    void EditorWhiteBoxColliderComponent::Deactivate()
    {
        AZ::TransformNotificationBus::Handler::BusDisconnect();
        EditorWhiteBoxColliderRequestBus::Handler::BusDisconnect();
        AzToolsFramework::Components::EditorComponentBase::Deactivate();

        DestroyPhysics();

        m_sceneInterface = nullptr;
        m_editorSceneHandle = AzPhysics::InvalidSceneHandle;
    }

    void EditorWhiteBoxColliderComponent::BuildGameEntity(AZ::Entity* gameEntity)
    {
        // Pre-bake both collider variants: the base mesh and (if a boolean source is set)
        // the boolean-evaluated mesh. CSG cannot run at runtime, so baking both here lets
        // the runtime component toggle its collider between them via the live-boolean param.
        Physics::CookedMeshShapeConfiguration baseConfiguration;
        Physics::CookedMeshShapeConfiguration booleanConfiguration;
        bool hasBooleanMesh = false;
        bool liveBoolean = false;

        if (auto* whiteBoxComponent = GetEntity()->FindComponent<WhiteBox::EditorWhiteBoxComponent>())
        {
            if (auto* baseMesh = whiteBoxComponent->GetWhiteBoxMesh())
            {
                CookToConfiguration(*baseMesh, baseConfiguration);
            }

            if (Api::WhiteBoxMeshPtr evaluated = whiteBoxComponent->EvaluateBooleanMesh())
            {
                hasBooleanMesh = CookToConfiguration(*evaluated, booleanConfiguration);
            }

            liveBoolean = whiteBoxComponent->GetLiveBoolean();
        }

        gameEntity->CreateComponent<WhiteBoxColliderComponent>(
            baseConfiguration, m_physicsColliderConfiguration, m_whiteBoxColliderConfiguration, booleanConfiguration,
            hasBooleanMesh, hasBooleanMesh && liveBoolean);
    }

    void EditorWhiteBoxColliderComponent::OnTransformChanged(
        [[maybe_unused]] const AZ::Transform& local, const AZ::Transform& world)
    {
        if (m_sceneInterface)
        {
            if (auto* rigidBody = m_sceneInterface->GetSimulatedBodyFromHandle(m_editorSceneHandle, m_rigidBodyHandle))
            {
                rigidBody->SetTransform(world);
            }
        }
    }

    void EditorWhiteBoxColliderComponent::CreatePhysics(const WhiteBoxMesh& whiteBox)
    {
        if (Api::MeshFaceCount(whiteBox) == 0)
        {
            return;
        }

        ConvertToPhysicsMesh(whiteBox);

        AzPhysics::StaticRigidBodyConfiguration bodyConfiguration;
        bodyConfiguration.m_debugName = GetEntity()->GetName().c_str();
        bodyConfiguration.m_entityId = GetEntityId();
        bodyConfiguration.m_orientation = GetTransform()->GetWorldRotationQuaternion();
        bodyConfiguration.m_position = GetTransform()->GetWorldTranslation();
        bodyConfiguration.m_colliderAndShapeData = AzPhysics::ShapeColliderPair(
            AZStd::make_shared<Physics::ColliderConfiguration>(m_physicsColliderConfiguration),
            AZStd::make_shared<Physics::CookedMeshShapeConfiguration>(m_meshShapeConfiguration));

        if (m_sceneInterface)
        {
            DestroyPhysics();
            m_rigidBodyHandle = m_sceneInterface->AddSimulatedBody(m_editorSceneHandle, &bodyConfiguration);
        }
    }

    void EditorWhiteBoxColliderComponent::DestroyPhysics()
    {
        if (m_sceneInterface && m_rigidBodyHandle != AzPhysics::InvalidSimulatedBodyHandle)
        {
            m_sceneInterface->RemoveSimulatedBody(m_editorSceneHandle, m_rigidBodyHandle);
            m_rigidBodyHandle = AzPhysics::InvalidSimulatedBodyHandle;
        }
    }

    static bool ConvertToTriangles(
        const WhiteBoxMesh& whiteBox, AZStd::vector<AZ::Vector3>& vertices, AZStd::vector<AZ::u32>& indices)
    {
        // Build the vertex and index buffers together, one face at a time. Do NOT size the
        // vertex buffer from MeshHalfedgeCount and assume 3 vertices per face: after a CSG
        // boolean the mesh can contain boundary half-edges and merged (n-gon) polygon faces,
        // which made the old approach emit a mismatched buffer with uninitialised trailing
        // vertices - PhysX then rejected the cooked data ("Unable to create a mesh object
        // from the CookedMeshShapeConfiguration buffer"). Triangle-fanning each face handles
        // both plain triangles and any n-gon faces safely.
        vertices.clear();
        indices.clear();

        const auto faceHandles = Api::MeshFaceHandles(whiteBox);
        for (const auto& faceHandle : faceHandles)
        {
            const auto faceVertexHandles = Api::FaceVertexHandles(whiteBox, faceHandle);
            if (faceVertexHandles.size() < 3)
            {
                continue; // degenerate face, skip
            }

            const AZ::u32 base = static_cast<AZ::u32>(vertices.size());
            for (const auto& vertexHandle : faceVertexHandles)
            {
                vertices.push_back(Api::VertexPosition(whiteBox, vertexHandle));
            }

            // fan the face into triangles (base, i, i+1)
            for (size_t i = 1; i + 1 < faceVertexHandles.size(); ++i)
            {
                indices.push_back(base);
                indices.push_back(base + static_cast<AZ::u32>(i));
                indices.push_back(base + static_cast<AZ::u32>(i + 1));
            }
        }

        return !indices.empty();
    }

    bool EditorWhiteBoxColliderComponent::CookToConfiguration(
        const WhiteBoxMesh& whiteBox, Physics::CookedMeshShapeConfiguration& outConfiguration)
    {
        AZStd::vector<AZ::Vector3> vertices;
        AZStd::vector<AZ::u32> indices;
        // convert white box mesh to vertices
        if (!ConvertToTriangles(whiteBox, vertices, indices))
        {
            // if there are no valid triangles then do not attempt to create a physics mesh
            return false;
        }

        if (auto* physicsSystem = AZ::Interface<Physics::System>::Get())
        {
            AZStd::vector<AZ::u8> bytes;
            const bool result = physicsSystem->CookTriangleMeshToMemory(
                vertices.data(), (AZ::u32)vertices.size(), indices.data(), (AZ::u32)indices.size(), bytes);

            AZ_Warning(
                "EditorWhiteBoxColliderComponent", result, "Failed to cook mesh data (verts=%zu, indices=%zu)",
                vertices.size(), indices.size());

            if (result)
            {
                outConfiguration.SetCookedMeshData(
                    bytes.data(), bytes.size(), Physics::CookedMeshShapeConfiguration::MeshType::TriangleMesh);
                return true;
            }
        }
        else
        {
            AZ_Warning(
                "EditorWhiteBoxColliderComponent", false, "No physics backend enabled - please ensure one is provided");
        }

        return false;
    }

    void EditorWhiteBoxColliderComponent::ConvertToPhysicsMesh(const WhiteBoxMesh& whiteBox)
    {
        CookToConfiguration(whiteBox, m_meshShapeConfiguration);
    }
} // namespace WhiteBox
