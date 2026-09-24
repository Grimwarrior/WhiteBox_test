/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "WhiteBoxConvexDecomposition.h"
#include "WhiteBoxColliderConfiguration.h"

#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/MathUtils.h>
#include <AzCore/std/hash.h>
#include <AzCore/std/numeric.h>
#include <cmath>
#include <numeric>

#define ENABLE_VHACD_IMPLEMENTATION 1
#include <VHACD.h>

namespace WhiteBox
{
    namespace
    {
        // PhysX refuses hulls with more than this many vertices.
        constexpr size_t PhysXHullVertexLimit = 255;

        struct Shell
        {
            AZStd::vector<AZ::Vector3> m_points;
            AZStd::vector<AZ::u32> m_triangles; //!< Indices into m_points.
        };

        // Triangles sharing a corner belong to one shell; separate layers and islands come apart.
        AZStd::vector<Shell> SplitShells(const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices)
        {
            AZStd::vector<AZ::u32> parent(vertices.size());
            std::iota(parent.begin(), parent.end(), 0u);
            const auto find = [&parent](AZ::u32 v)
            {
                while (parent[v] != v)
                {
                    parent[v] = parent[parent[v]];
                    v = parent[v];
                }
                return v;
            };
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const AZ::u32 a = find(indices[i]);
                parent[find(indices[i + 1])] = a;
                parent[find(indices[i + 2])] = a;
            }

            AZStd::unordered_map<AZ::u32, size_t> shellOf;
            AZStd::vector<AZStd::unordered_map<AZ::u32, AZ::u32>> localOf;
            AZStd::vector<Shell> shells;
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const AZ::u32 root = find(indices[i]);
                auto [it, added] = shellOf.emplace(root, shells.size());
                if (added)
                {
                    shells.emplace_back();
                    localOf.emplace_back();
                }
                Shell& shell = shells[it->second];
                auto& local = localOf[it->second];
                for (size_t c = 0; c < 3; ++c)
                {
                    const AZ::u32 global = indices[i + c];
                    auto [slot, fresh] = local.emplace(global, static_cast<AZ::u32>(shell.m_points.size()));
                    if (fresh)
                    {
                        shell.m_points.push_back(vertices[global]);
                    }
                    shell.m_triangles.push_back(slot->second);
                }
            }
            return shells;
        }

        float Extent(const AZStd::vector<AZ::Vector3>& points)
        {
            AZ::Aabb bounds = AZ::Aabb::CreateNull();
            for (const AZ::Vector3& point : points)
            {
                bounds.AddPoint(point);
            }
            return AZ::GetMax(bounds.GetExtents().GetLength(), 1e-4f);
        }

        // Every point lies on or behind every face plane (winding gives the outside).
        bool IsConvex(const Shell& shell, const float tolerance)
        {
            for (size_t i = 0; i + 2 < shell.m_triangles.size(); i += 3)
            {
                const AZ::Vector3& a = shell.m_points[shell.m_triangles[i]];
                const AZ::Vector3 normal =
                    (shell.m_points[shell.m_triangles[i + 1]] - a).Cross(shell.m_points[shell.m_triangles[i + 2]] - a);
                if (normal.GetLengthSq() <= 1e-16f)
                {
                    continue;
                }
                const AZ::Vector3 unit = normal.GetNormalized();
                for (const AZ::Vector3& point : shell.m_points)
                {
                    if (unit.Dot(point - a) > tolerance)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // The normal of a shell whose points all lie in one plane, or zero when it has volume.
        AZ::Vector3 FlatNormal(const Shell& shell, const float tolerance)
        {
            AZ::Vector3 sum = AZ::Vector3::CreateZero();
            for (size_t i = 0; i + 2 < shell.m_triangles.size(); i += 3)
            {
                const AZ::Vector3& a = shell.m_points[shell.m_triangles[i]];
                sum += (shell.m_points[shell.m_triangles[i + 1]] - a).Cross(shell.m_points[shell.m_triangles[i + 2]] - a);
            }
            if (sum.GetLengthSq() <= 1e-16f)
            {
                return AZ::Vector3::CreateZero();
            }
            const AZ::Vector3 normal = sum.GetNormalized();
            const AZ::Vector3& origin = shell.m_points.front();
            for (const AZ::Vector3& point : shell.m_points)
            {
                if (AZStd::abs(normal.Dot(point - origin)) > tolerance)
                {
                    return AZ::Vector3::CreateZero();
                }
            }
            return normal;
        }

        size_t HashShell(const Shell& shell, const WhiteBoxColliderConfiguration& configuration)
        {
            size_t seed = 0;
            for (const AZ::Vector3& point : shell.m_points)
            {
                // Quantised to a tenth of a millimetre so float noise from re-evaluation does not miss the cache.
                AZStd::hash_combine(seed, static_cast<AZ::s64>(std::llround(point.GetX() * 1e4)));
                AZStd::hash_combine(seed, static_cast<AZ::s64>(std::llround(point.GetY() * 1e4)));
                AZStd::hash_combine(seed, static_cast<AZ::s64>(std::llround(point.GetZ() * 1e4)));
            }
            for (const AZ::u32 index : shell.m_triangles)
            {
                AZStd::hash_combine(seed, index);
            }
            AZStd::hash_combine(seed, configuration.m_maxHullsPerShell);
            AZStd::hash_combine(seed, configuration.m_decompositionResolution);
            AZStd::hash_combine(seed, configuration.m_maxVerticesPerHull);
            return seed;
        }
    } // namespace

    AZStd::vector<HullPoints> ConvexDecomposer::Decompose(
        const AZStd::vector<AZ::Vector3>& vertices, const AZStd::vector<AZ::u32>& indices,
        const WhiteBoxColliderConfiguration& configuration)
    {
        AZStd::vector<HullPoints> hulls;
        for (Shell& shell : SplitShells(vertices, indices))
        {
            const float extent = Extent(shell.m_points);
            const float tolerance = extent * 1e-4f;
            if (const AZ::Vector3 normal = FlatNormal(shell, tolerance); !normal.IsZero())
            {
                // A plane has no volume to wrap, so it gets a thin backing below its surface; the top stays exact.
                const float thickness = AZ::GetClamp(extent * 0.01f, 0.01f, 0.1f);
                HullPoints hull = shell.m_points;
                for (const AZ::Vector3& point : shell.m_points)
                {
                    hull.push_back(point - normal * thickness);
                }
                hulls.push_back(AZStd::move(hull));
                continue;
            }
            if (IsConvex(shell, tolerance) && shell.m_points.size() <= PhysXHullVertexLimit)
            {
                hulls.push_back(AZStd::move(shell.m_points));
                continue;
            }
            // Concave, or convex with too many points for one PhysX hull (V-HACD then simplifies it to one).
            const AZ::u32 maxHulls = IsConvex(shell, tolerance) ? 1u : AZ::GetMax(configuration.m_maxHullsPerShell, 1u);
            const size_t key = HashShell(shell, configuration);
            auto cached = m_cache.find(key);
            if (cached == m_cache.end())
            {
                if (m_cache.size() > 512)
                {
                    m_cache.clear(); // bounded; a stale entry is only a missed shortcut
                }
                cached = m_cache.emplace(key, DecomposeConcave(shell.m_points, shell.m_triangles, maxHulls, configuration)).first;
            }
            if (cached->second.empty())
            {
                // V-HACD gave up; one hull around the shell still collides, just less tightly.
                hulls.push_back(AZStd::move(shell.m_points));
                continue;
            }
            hulls.insert(hulls.end(), cached->second.begin(), cached->second.end());
        }
        return hulls;
    }

    AZStd::vector<HullPoints> ConvexDecomposer::DecomposeConcave(
        const AZStd::vector<AZ::Vector3>& points, const AZStd::vector<AZ::u32>& triangles, const AZ::u32 maxHulls,
        const WhiteBoxColliderConfiguration& configuration)
    {
        AZStd::vector<float> flat;
        flat.reserve(points.size() * 3);
        for (const AZ::Vector3& point : points)
        {
            flat.insert(flat.end(), { point.GetX(), point.GetY(), point.GetZ() });
        }

        VHACD::IVHACD::Parameters parameters;
        parameters.m_maxConvexHulls = maxHulls;
        parameters.m_resolution = AZ::GetClamp(configuration.m_decompositionResolution, 10000u, 10000000u);
        parameters.m_maxNumVerticesPerCH = AZ::GetClamp(configuration.m_maxVerticesPerHull, 8u, static_cast<AZ::u32>(PhysXHullVertexLimit));
        parameters.m_asyncACD = false; // runs on the calling thread; results are cached per shell
        parameters.m_shrinkWrap = true;

        AZStd::vector<HullPoints> hulls;
        VHACD::IVHACD* decomposer = VHACD::CreateVHACD();
        if (decomposer == nullptr)
        {
            return hulls;
        }
        if (decomposer->Compute(
                flat.data(), static_cast<uint32_t>(points.size()), triangles.data(), static_cast<uint32_t>(triangles.size() / 3),
                parameters))
        {
            for (uint32_t i = 0; i < decomposer->GetNConvexHulls(); ++i)
            {
                VHACD::IVHACD::ConvexHull convexHull;
                if (!decomposer->GetConvexHull(i, convexHull) || convexHull.m_points.size() < 4)
                {
                    continue;
                }
                HullPoints hull;
                hull.reserve(convexHull.m_points.size());
                for (const VHACD::Vertex& vertex : convexHull.m_points)
                {
                    hull.emplace_back(static_cast<float>(vertex[0]), static_cast<float>(vertex[1]), static_cast<float>(vertex[2]));
                }
                hulls.push_back(AZStd::move(hull));
            }
        }
        decomposer->Clean();
        decomposer->Release();
        return hulls;
    }
} // namespace WhiteBox
