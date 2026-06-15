/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxCsgCore.h"

#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/hash.h>
#include <AzCore/std/utils.h>
#include <WhiteBox/WhiteBoxToolApi.h>

namespace WhiteBox
{
    namespace Api
    {
        namespace
        {
            // tolerance (in meters) within which two vertices are considered identical
            constexpr double WeldTolerance = 1e-6;
            // two adjacent triangles are merged into one polygon when their normals deviate
            // by less than this (dot product threshold, ~0.5 degrees)
            constexpr float CoplanarNormalDotThreshold = 0.99996f;

            // convert a white box mesh to an indexed triangle mesh, transforming all
            // positions by the given transform
            Csg::TriangleMesh ToTriangleMesh(const WhiteBoxMesh& whiteBox, const AZ::Transform& transform)
            {
                const Faces faces = MeshFaces(whiteBox);

                Csg::TriangleMesh triangleMesh;
                triangleMesh.m_positions.reserve(faces.size() * 9);
                triangleMesh.m_indices.reserve(faces.size() * 3);

                uint32_t index = 0;
                for (const Face& face : faces)
                {
                    for (const AZ::Vector3& position : face)
                    {
                        const AZ::Vector3 transformedPosition = transform.TransformPoint(position);
                        triangleMesh.m_positions.push_back(transformedPosition.GetX());
                        triangleMesh.m_positions.push_back(transformedPosition.GetY());
                        triangleMesh.m_positions.push_back(transformedPosition.GetZ());
                        triangleMesh.m_indices.push_back(index++);
                    }
                }

                // index the triangle soup so shared vertices are welded
                Csg::WeldVertices(triangleMesh, WeldTolerance);

                return triangleMesh;
            }

            AZ::Vector3 TriangleMeshPosition(const Csg::TriangleMesh& triangleMesh, const uint32_t vertexIndex)
            {
                return AZ::Vector3(
                    static_cast<float>(triangleMesh.m_positions[vertexIndex * 3 + 0]),
                    static_cast<float>(triangleMesh.m_positions[vertexIndex * 3 + 1]),
                    static_cast<float>(triangleMesh.m_positions[vertexIndex * 3 + 2]));
            }

            AZ::Vector3 TriangleNormal(const Csg::TriangleMesh& triangleMesh, const size_t triangleIndex)
            {
                const AZ::Vector3 a = TriangleMeshPosition(triangleMesh, triangleMesh.m_indices[triangleIndex * 3 + 0]);
                const AZ::Vector3 b = TriangleMeshPosition(triangleMesh, triangleMesh.m_indices[triangleIndex * 3 + 1]);
                const AZ::Vector3 c = TriangleMeshPosition(triangleMesh, triangleMesh.m_indices[triangleIndex * 3 + 2]);
                return (b - a).Cross(c - a).GetNormalizedSafe();
            }

            // group triangles into connected coplanar regions - each region becomes
            // one white box polygon (interior edges hidden, matching how the white box
            // tool represents quads and n-gons as groups of triangles)
            AZStd::vector<AZStd::vector<size_t>> GroupCoplanarTriangles(const Csg::TriangleMesh& triangleMesh)
            {
                const size_t triangleCount = triangleMesh.TriangleCount();

                // undirected edge (low vertex index, high vertex index) -> adjacent triangles
                using EdgeKey = AZStd::pair<uint32_t, uint32_t>;
                AZStd::unordered_map<EdgeKey, AZStd::vector<size_t>> edgeToTriangles;
                for (size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
                {
                    for (int edge = 0; edge < 3; ++edge)
                    {
                        const uint32_t v0 = triangleMesh.m_indices[triangleIndex * 3 + edge];
                        const uint32_t v1 = triangleMesh.m_indices[triangleIndex * 3 + (edge + 1) % 3];
                        edgeToTriangles[EdgeKey(AZStd::min(v0, v1), AZStd::max(v0, v1))].push_back(triangleIndex);
                    }
                }

                AZStd::vector<AZ::Vector3> triangleNormals;
                triangleNormals.reserve(triangleCount);
                for (size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
                {
                    triangleNormals.push_back(TriangleNormal(triangleMesh, triangleIndex));
                }

                // flood fill across shared edges while normals stay (close to) coplanar
                AZStd::vector<AZStd::vector<size_t>> groups;
                AZStd::vector<bool> visited(triangleCount, false);
                for (size_t seedTriangle = 0; seedTriangle < triangleCount; ++seedTriangle)
                {
                    if (visited[seedTriangle])
                    {
                        continue;
                    }

                    AZStd::vector<size_t> group;
                    AZStd::vector<size_t> stack{ seedTriangle };
                    visited[seedTriangle] = true;

                    while (!stack.empty())
                    {
                        const size_t triangleIndex = stack.back();
                        stack.pop_back();
                        group.push_back(triangleIndex);

                        for (int edge = 0; edge < 3; ++edge)
                        {
                            const uint32_t v0 = triangleMesh.m_indices[triangleIndex * 3 + edge];
                            const uint32_t v1 = triangleMesh.m_indices[triangleIndex * 3 + (edge + 1) % 3];
                            const auto& neighbors =
                                edgeToTriangles[EdgeKey(AZStd::min(v0, v1), AZStd::max(v0, v1))];

                            for (const size_t neighborTriangle : neighbors)
                            {
                                if (!visited[neighborTriangle] &&
                                    triangleNormals[triangleIndex].Dot(triangleNormals[neighborTriangle]) >
                                        CoplanarNormalDotThreshold)
                                {
                                    visited[neighborTriangle] = true;
                                    stack.push_back(neighborTriangle);
                                }
                            }
                        }
                    }

                    groups.push_back(AZStd::move(group));
                }

                return groups;
            }

            // replace the contents of the white box mesh with the given triangle mesh
            void RebuildFromTriangleMesh(WhiteBoxMesh& whiteBox, const Csg::TriangleMesh& triangleMesh)
            {
                Clear(whiteBox);

                AZStd::vector<VertexHandle> vertexHandles;
                vertexHandles.reserve(triangleMesh.VertexCount());
                for (size_t vertexIndex = 0; vertexIndex < triangleMesh.VertexCount(); ++vertexIndex)
                {
                    vertexHandles.push_back(
                        AddVertex(whiteBox, TriangleMeshPosition(triangleMesh, static_cast<uint32_t>(vertexIndex))));
                }

                for (const AZStd::vector<size_t>& group : GroupCoplanarTriangles(triangleMesh))
                {
                    FaceVertHandlesList faceVertHandlesList;
                    faceVertHandlesList.reserve(group.size());
                    for (const size_t triangleIndex : group)
                    {
                        faceVertHandlesList.push_back(FaceVertHandles{
                            vertexHandles[triangleMesh.m_indices[triangleIndex * 3 + 0]],
                            vertexHandles[triangleMesh.m_indices[triangleIndex * 3 + 1]],
                            vertexHandles[triangleMesh.m_indices[triangleIndex * 3 + 2]] });
                    }

                    AddPolygon(whiteBox, faceVertHandlesList);
                }

                CalculateNormals(whiteBox);
                CalculatePlanarUVs(whiteBox);
            }
        } // namespace

        bool MeshBoolean(
            WhiteBoxMesh& whiteBox, const WhiteBoxMesh& operand, const AZ::Transform& operandTransform,
            const BooleanOperation operation)
        {
            const auto csgOperation = [operation]
            {
                switch (operation)
                {
                case BooleanOperation::Union:
                    return Csg::BooleanOperation::Union;
                case BooleanOperation::Intersection:
                    return Csg::BooleanOperation::Intersection;
                case BooleanOperation::Subtraction:
                default:
                    return Csg::BooleanOperation::Subtraction;
                }
            }();

            const Csg::TriangleMesh meshA = ToTriangleMesh(whiteBox, AZ::Transform::CreateIdentity());
            const Csg::TriangleMesh meshB = ToTriangleMesh(operand, operandTransform);

            Csg::TriangleMesh result;

            if (!Csg::MeshBoolean(meshA, meshB, csgOperation, result))
            {
                return false;
            }
            RebuildFromTriangleMesh(whiteBox, result);

            return true;
        }
    } // namespace Api
} // namespace WhiteBox
