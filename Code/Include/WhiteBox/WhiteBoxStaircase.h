/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include <AzCore/std/containers/array.h>
#include <AzCore/std/containers/fixed_vector.h>

namespace WhiteBox
{
    struct StairTread
    {
        float m_end;    //!< Distance from the start of the flight to the end of this tread.
        float m_height; //!< Walking surface elevation.
    };

    //! Extrude a strictly ascending tread profile as one closed, editable shell.
    //! A lower landing is a first tread at landing height, above baseHeight.
    //! addVertex takes (width side 0/1, run, height). Reverse winding for a
    //! left-handed mapping of those coordinates. addPolygon receives one triangulated face.
    template<class TreadAt, class AddVertex, class AddPolygon>
    void BuildStaircaseShell(size_t treadCount, float baseHeight, bool reverseWinding,
        TreadAt treadAt, AddVertex addVertex, AddPolygon addPolygon)
    {
        if (treadCount == 0)
        {
            return;
        }
        using Handle = decltype(addVertex(0, 0.0f, 0.0f));
        using Loop = AZStd::fixed_vector<Handle, 5>;
        auto face = [&](const Loop& loop, bool flip)
        {
            AZStd::fixed_vector<AZStd::array<Handle, 3>, 3> triangles;
            const bool reverse = flip != reverseWinding;
            for (size_t i = 1; i + 1 < loop.size(); ++i)
            {
                triangles.push_back({ loop[0], loop[reverse ? i + 1 : i], loop[reverse ? i : i + 1] });
            }
            addPolygon(triangles);
        };

        float start = 0.0f;
        AZStd::array<Handle, 2> bottom{}, previousTop{};
        for (size_t i = 0; i < treadCount; ++i)
        {
            const StairTread tread = treadAt(i);
            AZStd::array<Handle, 2> front, back, nextBottom;
            for (int side = 0; side < 2; ++side)
            {
                // Keep the Draw Shape builder's vertex and polygon creation order.
                nextBottom[side] = addVertex(side, tread.m_end, baseHeight);
                back[side] = addVertex(side, tread.m_end, tread.m_height);
                front[side] = addVertex(side, start, tread.m_height);
                if (i == 0)
                {
                    bottom[side] = addVertex(side, start, baseHeight);
                    previousTop[side] = bottom[side];
                }
                // Split the front edge where the previous tread meets this riser.
                // Fan from the opposite bottom corner to avoid collinear triangles.
                Loop cap{ nextBottom[side], back[side], front[side] };
                if (i > 0)
                {
                    cap.push_back(previousTop[side]);
                }
                cap.push_back(bottom[side]);
                face(cap, side == 0);
            }
            face({ bottom[0], bottom[1], nextBottom[1], nextBottom[0] }, true);
            face({ front[0], front[1], back[1], back[0] }, false);
            face({ previousTop[0], previousTop[1], front[1], front[0] }, false);
            bottom = nextBottom;
            previousTop = back;
            start = tread.m_end;
        }
        face({ bottom[0], bottom[1], previousTop[1], previousTop[0] }, true);
    }
} // namespace WhiteBox
