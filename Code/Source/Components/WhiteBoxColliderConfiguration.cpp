/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxColliderConfiguration.h"

#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>

namespace WhiteBox
{
    AZ_CLASS_ALLOCATOR_IMPL(WhiteBoxColliderConfiguration, AZ::SystemAllocator)

    AZ::Crc32 WhiteBoxColliderConfiguration::SimplifiedVisibility() const
    {
        return m_shape == WhiteBoxColliderShape::SimplifiedMesh ? AZ::Edit::PropertyVisibility::Show : AZ::Edit::PropertyVisibility::Hide;
    }

    AZ::Crc32 WhiteBoxColliderConfiguration::PartsVisibility() const
    {
        return m_shape == WhiteBoxColliderShape::ConvexParts ? AZ::Edit::PropertyVisibility::Show : AZ::Edit::PropertyVisibility::Hide;
    }

    void WhiteBoxColliderConfiguration::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<WhiteBoxColliderConfiguration>()
                ->Version(4)
                ->Field("BodyType", &WhiteBoxColliderConfiguration::m_bodyType)
                ->Field("Shape", &WhiteBoxColliderConfiguration::m_shape)
                ->Field("MaxHullsPerShell", &WhiteBoxColliderConfiguration::m_maxHullsPerShell)
                ->Field("DecompositionResolution", &WhiteBoxColliderConfiguration::m_decompositionResolution)
                ->Field("MaxVerticesPerHull", &WhiteBoxColliderConfiguration::m_maxVerticesPerHull)
                ->Field("MeshResolution", &WhiteBoxColliderConfiguration::m_meshResolution);

            if (AZ::EditContext* editContext = serializeContext->GetEditContext())
            {
                editContext
                    ->Class<WhiteBoxColliderConfiguration>(
                        "White Box Collider Configuration", "White Box collider configuration properties")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->DataElement(
                        AZ::Edit::UIHandlers::ComboBox, &WhiteBoxColliderConfiguration::m_bodyType, "Body Type",
                        "Set if the White Box Collider will be treated as static or kinematic at runtime.")
                    ->EnumAttribute(WhiteBoxBodyType::Static, "Static")
                    ->EnumAttribute(WhiteBoxBodyType::Kinematic, "Kinematic")
                    ->DataElement(
                        AZ::Edit::UIHandlers::ComboBox, &WhiteBoxColliderConfiguration::m_shape, "Collision Shape",
                        "Triangle Mesh follows every face exactly. Triangle Mesh (Simplified) keeps only the share of triangles "
                        "set by Resolution. Convex Hull wraps the whole mesh in one convex shape. "
                        "Convex Parts gives each separate shell (layer or island) its own hull and splits concave ones, so "
                        "doorways and U shapes stay open. PhysX triggers must be convex, so Trigger with Triangle Mesh cooks a hull.")
                    ->EnumAttribute(WhiteBoxColliderShape::TriangleMesh, "Triangle Mesh")
                    ->EnumAttribute(WhiteBoxColliderShape::SimplifiedMesh, "Triangle Mesh (Simplified)")
                    ->EnumAttribute(WhiteBoxColliderShape::ConvexHull, "Convex Hull (Single)")
                    ->EnumAttribute(WhiteBoxColliderShape::ConvexParts, "Convex Parts")
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, AZ::Edit::PropertyRefreshLevels::AttributesAndValues)
                    ->DataElement(
                        AZ::Edit::UIHandlers::Slider, &WhiteBoxColliderConfiguration::m_meshResolution, "Resolution",
                        "Percent of the mesh's triangles the collider keeps. Flat areas and gentle curves go first; faces are "
                        "never folded over, so edges stay where they are.")
                    ->Attribute(AZ::Edit::Attributes::Min, 1)
                    ->Attribute(AZ::Edit::Attributes::Max, 100)
                    ->Attribute(AZ::Edit::Attributes::Suffix, " %")
                    ->Attribute(AZ::Edit::Attributes::Visibility, &WhiteBoxColliderConfiguration::SimplifiedVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::SpinBox, &WhiteBoxColliderConfiguration::m_maxHullsPerShell, "Max Hulls Per Shell",
                        "Most hulls a concave shell may split into. Convex shells always use exactly one.")
                    ->Attribute(AZ::Edit::Attributes::Min, 1)
                    ->Attribute(AZ::Edit::Attributes::Max, 64)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &WhiteBoxColliderConfiguration::PartsVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::SpinBox, &WhiteBoxColliderConfiguration::m_decompositionResolution, "Resolution",
                        "Voxels V-HACD samples each concave shell with. Higher follows small detail more closely but cooks slower.")
                    ->Attribute(AZ::Edit::Attributes::Min, 10000)
                    ->Attribute(AZ::Edit::Attributes::Max, 10000000)
                    ->Attribute(AZ::Edit::Attributes::Step, 10000)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &WhiteBoxColliderConfiguration::PartsVisibility)
                    ->DataElement(
                        AZ::Edit::UIHandlers::SpinBox, &WhiteBoxColliderConfiguration::m_maxVerticesPerHull, "Max Vertices Per Hull",
                        "Vertex cap for each generated hull (PhysX allows at most 255).")
                    ->Attribute(AZ::Edit::Attributes::Min, 8)
                    ->Attribute(AZ::Edit::Attributes::Max, 255)
                    ->Attribute(AZ::Edit::Attributes::Visibility, &WhiteBoxColliderConfiguration::PartsVisibility);
            }
        }
    }
} // namespace WhiteBox
