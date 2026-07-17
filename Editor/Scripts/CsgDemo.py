"""
Copyright (c) Contributors to the Open 3D Engine Project.
For complete copyright and license terms please see the LICENSE at the root of this distribution.

SPDX-License-Identifier: Apache-2.0 OR MIT

Usage
-----
    pyRunFile path/to/CsgDemo.py [doorway|ledge|chamfer]

Each mode runs a single CSG subtract on a unit-cube source brush,
demonstrating a shape you cannot easily build with WhiteBox's native
extrude/translate-polygon operations.

IMPORTANT – convex-only constraint
-----------------------------------
ConvexSubtract requires that the SOURCE brush is convex.  Do NOT call
MeshBoolean on a mesh that has already been CSG-subtracted; the result is
non-convex and the algorithm will produce broken geometry (wrong normals,
phantom faces).  This matches TrenchBroom's rule: every brush is always
convex.  For complex shapes, subtract into separate entities and combine
them in the scene, just as you would in TrenchBroom.

Modes
-----
doorway  – punches a rectangular slot through a thick wall block, producing
           the cross-section of a door-frame jamb.  Useful for arches and
           openings in walls.

ledge    – carves a step-notch from one corner to create a shelving ledge
           or a coping stone profile.

chamfer  – carves a 45-degree angled corner using a rotated cutter, giving
           a bevelled edge that native extrusion cannot produce directly.
"""

import math
import WhiteBoxInit as init
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.math

SUBTRACTION = 1

V3  = azlmbr.math.Vector3
Tf  = azlmbr.math.Transform_CreateTranslation
TfS = azlmbr.math.Transform_CreateUniformScale
TfR = azlmbr.math.Transform_CreateRotationY


# ── helpers ───────────────────────────────────────────────────────────────────

def make_cube(name):
    entity    = init.create_white_box_entity(name)
    component = init.create_white_box_component(entity)
    handle    = init.create_white_box_handle(component)
    handle.InitializeAsUnitCube()
    return entity, component, handle


def csg_subtract(target_handle, target_component, cutter_entity, cutter_handle, transform):
    """
    Subtract cutter from target in place, then delete the cutter entity.

    transform  – positions/sizes the cutter relative to the source brush's
                 local origin (the centre of the unit cube, i.e. [0,0,0]).
                   Tf(V3(x,y,z))          pure translation
                   TfS(s)                 uniform scale  (makes cutter s× bigger)
                   TfR(angle_radians)     rotation around Y
                 Compose two transforms by multiplying: TfR(a) * Tf(v)

    The source brush MUST be convex.  Do not call on a mesh that has
    already been CSG-subtracted.
    """
    if target_handle.MeshBoolean(cutter_handle, transform, SUBTRACTION):
        init.update_white_box(target_handle, target_component)
        editor.ToolsApplicationRequestBus(bus.Broadcast, 'DeleteEntityById', cutter_entity)
        return True
    print('csg_subtract: no intersection — check that the meshes overlap')
    return False


# ── modes ─────────────────────────────────────────────────────────────────────

def demo_doorway():
    """
    A rectangular slot punched through the middle of a wall block.

    Source:  unit cube (a thick wall section)
    Cutter:  unit cube, scaled ×0.4 in x (narrow), centred in the wall
    Result:  wall block with a rectangular channel through it —
             the cross-section of a door or window jamb.

    Viewed from the front (x-z plane):
        ┌──┬──┬──┐
        │  │  │  │    ← top of wall above opening
        │  └──┘  │    ← sides of jamb
        │        │    ← opening
        └────────┘
    """
    entity, comp, wb = make_cube('WallJamb')
    # Scale the cutter to 40 % width then lift it so only the top
    # 75 % of the wall height is punched through (leaving a sill).
    # TfS(0.4) makes the cutter [-0.2..0.2] in x, Tf raises it in z.
    transform = Tf(V3(0.0, 0.0, 0.375)) * TfS(0.4)
    e, c, wb_c = make_cube('DoorwayCutter')
    csg_subtract(wb, comp, e, wb_c, transform)
    print('doorway demo: created WallJamb')


def demo_ledge():
    """
    A shelving-ledge profile: a deep notch cut from the top-front corner.

    Source:  unit cube (a block)
    Cutter:  unit cube offset so it overlaps only the top-right (x>0, z>0)
             quadrant of the source.
    Result:  an L-shaped cross section — the same profile used for
             coping stones, skirting boards, or stair nosings.

    Viewed from the side (x-z plane):
        ┌──┐
        │  │   ← back upright
        │  └───┐
        │      │  ← ledge shelf
        └──────┘
    """
    entity, comp, wb = make_cube('Ledge')
    transform = Tf(V3(0.5, 0.0, 0.5))   # cutter → [0..1]×...×[0..1]
    e, c, wb_c = make_cube('LedgeCutter')
    csg_subtract(wb, comp, e, wb_c, transform)
    print('ledge demo: created Ledge')


def demo_chamfer():
    """
    A 45-degree chamfered (bevelled) corner — impossible with extrude alone.

    Source:  unit cube
    Cutter:  unit cube rotated 45° around Y then pushed into the front-right
             edge.  The diagonal face of the rotated cutter shaves off a
             clean 45° bevel.
    Result:  a block with one chamfered vertical edge.
    """
    entity, comp, wb = make_cube('Chamfer')
    # Rotate cutter 45° around Y, scale it up so it's large enough to
    # protrude fully into the target, then move it to the right-front edge.
    rotate    = TfR(math.pi / 4)          # 45°
    scale     = TfS(1.2)                  # slightly oversized so it cuts clean
    translate = Tf(V3(0.7, 0.0, 0.0))    # push into the right edge
    transform = translate * scale * rotate
    e, c, wb_c = make_cube('ChamferCutter')
    csg_subtract(wb, comp, e, wb_c, transform)
    print('chamfer demo: created Chamfer')


# ── entry point ───────────────────────────────────────────────────────────────

MODES = {
    'doorway' : demo_doorway,
    'ledge'   : demo_ledge,
    'chamfer' : demo_chamfer,
}

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='CSG subtract demo.')
    parser.add_argument('mode', nargs='?', default='ledge', choices=sorted(MODES))
    args = parser.parse_args()
    MODES[args.mode]()
