/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace WhiteBox
{
    namespace Csg
    {
        //! The supported CSG boolean operations.
        enum class BooleanOperation
        {
            Union, //!< A + B
            Subtraction, //!< A - B
            Intersection //!< A & B
        };

        //! An indexed triangle mesh using only std types.
        //! @note This intentionally has no AZ/O3DE dependencies so the CSG core can be
        //! compiled and tested in isolation from the engine (see Tests/WhiteBoxCsgCoreTest).
        struct TriangleMesh
        {
            std::vector<double> m_positions; //!< Flattened vertex positions (x0, y0, z0, x1, y1, z1, ...).
            std::vector<uint32_t> m_indices; //!< Triangle list indices (three per triangle, CCW winding).

            size_t VertexCount() const
            {
                return m_positions.size() / 3;
            }

            size_t TriangleCount() const
            {
                return m_indices.size() / 3;
            }
        };

        //! Merge vertices that lie within tolerance of each other and remap/compact the
        //! index buffer, dropping any triangles that become degenerate.
        //! @note Use this to build a TriangleMesh from a raw (unindexed) triangle soup
        //! and to clean up the output of a boolean operation before rebuilding a mesh.
        void WeldVertices(TriangleMesh& mesh, double tolerance);

        //! Subtract convex mesh B from convex mesh A using the TrenchBroom-style half-space
        //! clipping algorithm. Unlike MeshBoolean (mcut) this never fails on valid convex
        //! input: every WhiteBox default shape (cube, wedge, cylinder approximation) is convex
        //! and therefore suitable. The result is a single closed mesh representing A minus the
        //! intersection with B. Returns false when A and B do not intersect (result equals A
        //! unchanged) or when A is completely contained in B (result is empty).
        bool ConvexSubtract(
            const TriangleMesh& meshA, const TriangleMesh& meshB, TriangleMesh& result);

        //! Perform a CSG boolean operation between two closed (watertight) triangle meshes.
        //! @param meshA The first operand (the mesh being modified).
        //! @param meshB The second operand (the 'tool' mesh).
        //! @param operation Which boolean operation to apply.
        //! @param result The resulting triangle mesh (only valid when returning true).
        //! @return True if the operation succeeded. Fails if the meshes do not intersect
        //! or if either mesh is not a closed two-manifold.
        bool MeshBoolean(
            const TriangleMesh& meshA, const TriangleMesh& meshB, BooleanOperation operation, TriangleMesh& result);
    } // namespace Csg
} // namespace WhiteBox
