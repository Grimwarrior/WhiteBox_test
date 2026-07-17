/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxCsgCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

#include <AzCore/Debug/Trace.h>

// mcut (LGPL-3.0+) - see 3rdParty/Installer/FindMcut.cmake
#include <mcut/mcut.h>

namespace WhiteBox
{
    namespace Csg
    {
        void WeldVertices(TriangleMesh& mesh, const double tolerance)
        {
            const size_t vertexCount = mesh.VertexCount();
            const double invTolerance = 1.0 / tolerance;

            // map quantized position -> first (compacted) vertex index found at that position
            std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> grid;
            std::vector<uint32_t> remap(vertexCount, 0);
            std::vector<double> weldedPositions;
            weldedPositions.reserve(mesh.m_positions.size());

            const auto quantize = [invTolerance](const double value)
            {
                return static_cast<int64_t>(std::llround(value * invTolerance));
            };

            for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                const double x = mesh.m_positions[vertexIndex * 3 + 0];
                const double y = mesh.m_positions[vertexIndex * 3 + 1];
                const double z = mesh.m_positions[vertexIndex * 3 + 2];

                const auto key = std::make_tuple(quantize(x), quantize(y), quantize(z));
                const auto gridIt = grid.find(key);
                if (gridIt != grid.end())
                {
                    remap[vertexIndex] = gridIt->second;
                }
                else
                {
                    const auto weldedIndex = static_cast<uint32_t>(weldedPositions.size() / 3);
                    weldedPositions.push_back(x);
                    weldedPositions.push_back(y);
                    weldedPositions.push_back(z);
                    grid.emplace(key, weldedIndex);
                    remap[vertexIndex] = weldedIndex;
                }
            }

            // remap the index buffer, dropping triangles that have collapsed to a line/point
            std::vector<uint32_t> weldedIndices;
            weldedIndices.reserve(mesh.m_indices.size());
            for (size_t triangleIndex = 0; triangleIndex < mesh.TriangleCount(); ++triangleIndex)
            {
                const uint32_t i0 = remap[mesh.m_indices[triangleIndex * 3 + 0]];
                const uint32_t i1 = remap[mesh.m_indices[triangleIndex * 3 + 1]];
                const uint32_t i2 = remap[mesh.m_indices[triangleIndex * 3 + 2]];
                if (i0 != i1 && i1 != i2 && i2 != i0)
                {
                    weldedIndices.push_back(i0);
                    weldedIndices.push_back(i1);
                    weldedIndices.push_back(i2);
                }
            }

            mesh.m_positions = std::move(weldedPositions);
            mesh.m_indices = std::move(weldedIndices);
        }

        // ─── convex brush subtraction ────────────────────────────────────────────────
        //
        // TrenchBroom-style half-space clipping.  Both meshes must be convex; no
        // mesh-boolean library is involved so the operation is numerically robust.
        //
        // Algorithm overview
        // ──────────────────
        // A plane is stored as {nx,ny,nz,d} with interior defined as n·x ≤ d.
        //
        // Step 1 – clip each triangle of A against B's planes.
        //   We maintain a "remaining" polygon (initially the triangle).  For each B-plane:
        //     • the "outside" piece (n·x > d, i.e. outside B through this face) is kept;
        //     • remaining ← the "inside" piece (n·x ≤ d) and the loop continues.
        //   Any final "remaining" is inside all of B and is discarded.
        //
        // Step 2 – add cap faces.
        //   For each B-plane we compute the polygon that lies on that plane, is inside A
        //   and inside B's other planes.  Its winding is reversed so its outward normal
        //   points away from the A-B solid (into B).

        namespace
        {
            using Vec3d = std::array<double, 3>;
            using Poly  = std::vector<Vec3d>;

            // A plane in the form  n · x = d  (n is a unit outward normal).
            // "Inside" the brush means  n · x ≤ d.
            struct Plane
            {
                double nx, ny, nz, d;
            };

            inline double dot3(const Vec3d& v, double nx, double ny, double nz)
            {
                return v[0]*nx + v[1]*ny + v[2]*nz;
            }

            // Clip poly keeping the half-space  n·x ≤ d  (interior / "inside" side).
            Poly ClipPoly(const Poly& in, const Plane& pl)
            {
                Poly out;
                const int n = static_cast<int>(in.size());
                if (n == 0) return out;

                for (int i = 0; i < n; ++i)
                {
                    const Vec3d& a = in[i];
                    const Vec3d& b = in[(i + 1) % n];
                    const double da = dot3(a, pl.nx, pl.ny, pl.nz) - pl.d; // > 0 = outside
                    const double db = dot3(b, pl.nx, pl.ny, pl.nz) - pl.d;

                    if (da <= 1e-9) // a inside
                        out.push_back(a);

                    const bool aOut = (da >  1e-9);
                    const bool bOut = (db >  1e-9);
                    if (aOut != bOut) // edge crosses plane
                    {
                        const double t = da / (da - db);
                        out.push_back({ a[0] + t*(b[0]-a[0]),
                                        a[1] + t*(b[1]-a[1]),
                                        a[2] + t*(b[2]-a[2]) });
                    }
                }
                return out;
            }

            // Append a fan-triangulated polygon to the result mesh.
            void AppendPoly(const Poly& poly, TriangleMesh& mesh)
            {
                if (poly.size() < 3) return;
                const auto base = static_cast<uint32_t>(mesh.VertexCount());
                for (const Vec3d& v : poly)
                {
                    mesh.m_positions.push_back(v[0]);
                    mesh.m_positions.push_back(v[1]);
                    mesh.m_positions.push_back(v[2]);
                }
                for (uint32_t k = 1; k + 1 < static_cast<uint32_t>(poly.size()); ++k)
                {
                    mesh.m_indices.push_back(base);
                    mesh.m_indices.push_back(base + k);
                    mesh.m_indices.push_back(base + k + 1);
                }
            }

            // Extract unique face planes from a triangle mesh.
            // Two triangles map to the same plane when their normals differ by < eps and their
            // offsets differ by < eps.  For a convex mesh each unique plane is one face.
            std::vector<Plane> ExtractPlanes(const TriangleMesh& mesh, double eps = 1e-5)
            {
                std::vector<Plane> planes;

                for (size_t ti = 0; ti < mesh.TriangleCount(); ++ti)
                {
                    const uint32_t i0 = mesh.m_indices[ti*3+0];
                    const uint32_t i1 = mesh.m_indices[ti*3+1];
                    const uint32_t i2 = mesh.m_indices[ti*3+2];

                    const double x0 = mesh.m_positions[i0*3+0], y0 = mesh.m_positions[i0*3+1], z0 = mesh.m_positions[i0*3+2];
                    const double x1 = mesh.m_positions[i1*3+0], y1 = mesh.m_positions[i1*3+1], z1 = mesh.m_positions[i1*3+2];
                    const double x2 = mesh.m_positions[i2*3+0], y2 = mesh.m_positions[i2*3+1], z2 = mesh.m_positions[i2*3+2];

                    // edge vectors
                    const double ex = x1-x0, ey = y1-y0, ez = z1-z0;
                    const double fx = x2-x0, fy = y2-y0, fz = z2-z0;

                    // cross product (outward normal for CCW winding)
                    double nx = ey*fz - ez*fy;
                    double ny = ez*fx - ex*fz;
                    double nz = ex*fy - ey*fx;
                    const double len = std::sqrt(nx*nx + ny*ny + nz*nz);
                    if (len < 1e-12) continue; // degenerate triangle
                    nx /= len; ny /= len; nz /= len;
                    const double d = nx*x0 + ny*y0 + nz*z0;

                    // Skip if an equivalent plane is already recorded.
                    bool found = false;
                    for (const Plane& p : planes)
                    {
                        if (std::abs(p.nx - nx) < eps && std::abs(p.ny - ny) < eps &&
                            std::abs(p.nz - nz) < eps && std::abs(p.d  - d ) < eps)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        planes.push_back({nx, ny, nz, d});
                }
                return planes;
            }

            // Build two vectors perpendicular to the unit normal n.
            void BuildTangents(double nx, double ny, double nz, Vec3d& t1, Vec3d& t2)
            {
                // Pick any vector not parallel to n.
                Vec3d up = (std::abs(ny) < 0.9) ? Vec3d{0,1,0} : Vec3d{1,0,0};
                // t1 = up × n
                t1 = { up[1]*nz - up[2]*ny,
                       up[2]*nx - up[0]*nz,
                       up[0]*ny - up[1]*nx };
                const double t1len = std::sqrt(t1[0]*t1[0]+t1[1]*t1[1]+t1[2]*t1[2]);
                t1[0]/=t1len; t1[1]/=t1len; t1[2]/=t1len;
                // t2 = n × t1
                t2 = { ny*t1[2] - nz*t1[1],
                       nz*t1[0] - nx*t1[2],
                       nx*t1[1] - ny*t1[0] };
            }
        } // anonymous namespace

        bool ConvexSubtract(const TriangleMesh& meshA, const TriangleMesh& meshB, TriangleMesh& result)
        {
            if (meshA.TriangleCount() == 0 || meshB.TriangleCount() == 0)
                return false;

            const std::vector<Plane> bPlanes = ExtractPlanes(meshB);
            const std::vector<Plane> aPlanes = ExtractPlanes(meshA);

            if (bPlanes.empty() || aPlanes.empty())
                return false;

            result.m_positions.clear();
            result.m_indices.clear();

            // ── Step 1: clip each A-triangle against B ────────────────────────────────
            for (size_t ti = 0; ti < meshA.TriangleCount(); ++ti)
            {
                const uint32_t i0 = meshA.m_indices[ti*3+0];
                const uint32_t i1 = meshA.m_indices[ti*3+1];
                const uint32_t i2 = meshA.m_indices[ti*3+2];

                Poly remaining = {
                    {meshA.m_positions[i0*3+0], meshA.m_positions[i0*3+1], meshA.m_positions[i0*3+2]},
                    {meshA.m_positions[i1*3+0], meshA.m_positions[i1*3+1], meshA.m_positions[i1*3+2]},
                    {meshA.m_positions[i2*3+0], meshA.m_positions[i2*3+1], meshA.m_positions[i2*3+2]},
                };

                for (const Plane& bp : bPlanes)
                {
                    if (remaining.empty()) break;

                    // "outside" piece: n·x ≥ bp.d  →  clip by flipped plane
                    Poly outside = ClipPoly(remaining, {-bp.nx, -bp.ny, -bp.nz, -bp.d});
                    AppendPoly(outside, result);

                    // remaining ← the part inside this B-plane (continues through loop)
                    remaining = ClipPoly(remaining, bp);
                }
                // remaining is entirely inside B — discard it
            }

            // ── Step 2: cap faces (B-plane faces clipped to A ∩ B interior) ──────────
            // For each B-face plane, start with a large quad in that plane then clip it
            // into A's interior and B's other interiors.  The winding is reversed so the
            // outward normal of the cap points into B (= away from the A-B solid).
            constexpr double kCapExtent = 1000.0;

            for (const Plane& bp : bPlanes)
            {
                // Point on the plane closest to the origin.
                const Vec3d center = {bp.nx*bp.d, bp.ny*bp.d, bp.nz*bp.d};

                Vec3d t1, t2;
                BuildTangents(bp.nx, bp.ny, bp.nz, t1, t2);

                Poly cap = {
                    {center[0] - kCapExtent*t1[0] - kCapExtent*t2[0],
                     center[1] - kCapExtent*t1[1] - kCapExtent*t2[1],
                     center[2] - kCapExtent*t1[2] - kCapExtent*t2[2]},
                    {center[0] + kCapExtent*t1[0] - kCapExtent*t2[0],
                     center[1] + kCapExtent*t1[1] - kCapExtent*t2[1],
                     center[2] + kCapExtent*t1[2] - kCapExtent*t2[2]},
                    {center[0] + kCapExtent*t1[0] + kCapExtent*t2[0],
                     center[1] + kCapExtent*t1[1] + kCapExtent*t2[1],
                     center[2] + kCapExtent*t1[2] + kCapExtent*t2[2]},
                    {center[0] - kCapExtent*t1[0] + kCapExtent*t2[0],
                     center[1] - kCapExtent*t1[1] + kCapExtent*t2[1],
                     center[2] - kCapExtent*t1[2] + kCapExtent*t2[2]},
                };

                // Clip to the interior of A (each A-plane clips keeping n·x ≤ d)
                for (const Plane& ap : aPlanes)
                {
                    cap = ClipPoly(cap, ap);
                    if (cap.empty()) break;
                }

                // Clip to the interior of B's other planes
                for (const Plane& obp : bPlanes)
                {
                    if (&obp == &bp) continue;
                    cap = ClipPoly(cap, obp);
                    if (cap.empty()) break;
                }

                if (cap.size() >= 3)
                {
                    // Reverse winding: outward normal of A-B at this cap points into B
                    std::reverse(cap.begin(), cap.end());
                    AppendPoly(cap, result);
                }
            }

            // Weld seam vertices introduced by the clipping
            WeldVertices(result, 1e-6);

            return result.TriangleCount() > 0;
        }

        // ─── end convex brush subtraction ────────────────────────────────────────────

        bool MeshBoolean(
            const TriangleMesh& meshA, const TriangleMesh& meshB, const BooleanOperation operation,
            TriangleMesh& result)
        {
            if (meshA.TriangleCount() == 0 || meshB.TriangleCount() == 0)
            {
                return false;
            }

            McContext context = MC_NULL_HANDLE;
            // MC_DEBUG must be passed here for mcDebugMessageCallback to actually invoke the
            // registered callback; without it dbg_cb() is gated out in frontend.h and every
            // mcut diagnostic message is silently discarded even if a callback is registered.
            if (mcCreateContext(&context, MC_DEBUG) != MC_NO_ERROR)
            {
                return false;
            }

            // Forward ALL mcut debug messages to O3DE's warning system so validation
            // failures (invalid mesh, non-manifold edge, etc.) appear in the console.
            mcDebugMessageCallback(
                context,
                [](McDebugSource source, McDebugType type, unsigned int /*id*/, McDebugSeverity severity,
                   size_t /*length*/, const char* message, const McVoid* /*userParam*/)
                {
                    AZ_Warning("WhiteBox", false, "mcut [src=%d type=%d sev=%d]: %s",
                        static_cast<int>(source), static_cast<int>(type),
                        static_cast<int>(severity), message);
                },
                nullptr);

            // mcut dispatch filter flags that reduce the output to exactly the
            // fragment required for each boolean operation (see mcut CSGBoolean tutorial)
            McFlags booleanOpFlags = 0;
            switch (operation)
            {
            case BooleanOperation::Union:
                booleanOpFlags = MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE;
                break;
            case BooleanOperation::Subtraction: // A_NOT_B
                booleanOpFlags = MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE;
                break;
            case BooleanOperation::Intersection:
                booleanOpFlags = MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW;
                break;
            }

            // all faces are triangles
            const std::vector<uint32_t> faceSizesA(meshA.TriangleCount(), 3);
            const std::vector<uint32_t> faceSizesB(meshB.TriangleCount(), 3);

            AZ_Warning("WhiteBox", false,
                "MeshBoolean: meshA verts=%zu tris=%zu | meshB verts=%zu tris=%zu",
                meshA.VertexCount(), meshA.TriangleCount(),
                meshB.VertexCount(), meshB.TriangleCount());

            bool success = false;
            McResult status = mcDispatch(
                context,
                MC_DISPATCH_VERTEX_ARRAY_DOUBLE | MC_DISPATCH_ENFORCE_GENERAL_POSITION | booleanOpFlags,
                meshA.m_positions.data(), meshA.m_indices.data(), faceSizesA.data(),
                static_cast<uint32_t>(meshA.VertexCount()), static_cast<uint32_t>(meshA.TriangleCount()),
                meshB.m_positions.data(), meshB.m_indices.data(), faceSizesB.data(),
                static_cast<uint32_t>(meshB.VertexCount()), static_cast<uint32_t>(meshB.TriangleCount()));

            if (status != MC_NO_ERROR)
            {
                AZ_Warning("WhiteBox", false, "MeshBoolean: mcDispatch failed (mcut error %d)", static_cast<int>(status));
            }

            if (status == MC_NO_ERROR)
            {
                McUint32 connectedComponentCount = 0;
                status = mcGetConnectedComponents(
                    context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, 0, nullptr, &connectedComponentCount);

                if (status == MC_NO_ERROR && connectedComponentCount == 0)
                {
                    AZ_Warning("WhiteBox", false, "MeshBoolean: dispatch succeeded but produced no fragments "
                        "(meshes may not intersect, or one is not a closed manifold)");
                }

                if (status == MC_NO_ERROR && connectedComponentCount > 0)
                {
                    std::vector<McConnectedComponent> connectedComponents(connectedComponentCount, MC_NULL_HANDLE);
                    mcGetConnectedComponents(
                        context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, connectedComponentCount,
                        connectedComponents.data(), nullptr);

                    // with the filter flags above, all returned fragments belong to the result -
                    // append every fragment (a boolean can legitimately produce multiple pieces,
                    // e.g. a cut that splits the source mesh in two)
                    result.m_positions.clear();
                    result.m_indices.clear();
                    success = true;

                    for (const McConnectedComponent component : connectedComponents)
                    {
                        McSize numBytes = 0;

                        // vertices
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, 0, nullptr, &numBytes);
                        std::vector<double> componentPositions(numBytes / sizeof(double), 0.0);
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, numBytes,
                            componentPositions.data(), nullptr);

                        // triangulated face indices
                        numBytes = 0;
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, 0, nullptr, &numBytes);
                        std::vector<uint32_t> componentIndices(numBytes / sizeof(uint32_t), 0);
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, numBytes,
                            componentIndices.data(), nullptr);

                        // fragments 'below' the cut surface that are sealed with an 'outside' patch
                        // have inverted winding - flip them so all triangles face outward
                        McPatchLocation patchLocation = static_cast<McPatchLocation>(0);
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION, sizeof(McPatchLocation),
                            &patchLocation, nullptr);
                        McFragmentLocation fragmentLocation = static_cast<McFragmentLocation>(0);
                        mcGetConnectedComponentData(
                            context, component, MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION,
                            sizeof(McFragmentLocation), &fragmentLocation, nullptr);

                        if (fragmentLocation == MC_FRAGMENT_LOCATION_BELOW && patchLocation == MC_PATCH_LOCATION_OUTSIDE)
                        {
                            // flip per-triangle winding (swap vertices 1 and 2 in each triangle)
                            for (size_t i = 0; i + 2 < componentIndices.size(); i += 3)
                            {
                                std::swap(componentIndices[i + 1], componentIndices[i + 2]);
                            }
                        }

                        // append to the result, offsetting indices past existing vertices
                        const auto indexOffset = static_cast<uint32_t>(result.VertexCount());
                        result.m_positions.insert(
                            result.m_positions.end(), componentPositions.begin(), componentPositions.end());
                        for (const uint32_t index : componentIndices)
                        {
                            result.m_indices.push_back(index + indexOffset);
                        }
                    }

                    // seam vertices are duplicated between the fragment surface and the sealing
                    // patch - weld them so the rebuilt mesh is a closed two-manifold
                    WeldVertices(result, 1e-6);
                }
            }

            mcReleaseConnectedComponents(context, 0, nullptr);
            mcReleaseContext(context);

            return success;
        }
    } // namespace Csg
} // namespace WhiteBox
