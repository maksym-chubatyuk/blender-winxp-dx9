# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# MECHAZYIU (the render engine of the Windows XP build) in the properties of the other modules:
# the panels shown for every engine (those listing both EEVEE and Workbench), and the EEVEE panels
# of the settings MECHAZYIU uses too. MECHAZYIU's own panels are in `properties_render.py`.

import bpy
from bpy.props import (
    BoolProperty,
    FloatProperty,
    PointerProperty,
)

ENGINE = 'BLENDER_MECHAZYIU'

# EEVEE panels whose settings MECHAZYIU reads.
EEVEE_PANELS = (
    "DATA_PT_EEVEE_light",
    "DATA_PT_EEVEE_light_distance",
    "EEVEE_MATERIAL_PT_context_material",
    "EEVEE_MATERIAL_PT_surface",
    "EEVEE_MATERIAL_PT_settings",
    "EEVEE_MATERIAL_PT_settings_surface",
    "MATERIAL_PT_preview",
    "EEVEE_WORLD_PT_surface",
)


def _subclasses(cls):
    for sub in cls.__subclasses__():
        yield sub
        yield from _subclasses(sub)


class MechazyiuSceneSettings(bpy.types.PropertyGroup):
    """MECHAZYIU's settings of a scene (``Scene.mechazyiu``, read by the engine)."""

    use_ambient_occlusion: BoolProperty(
        name="Ambient Occlusion",
        description="Darken the world lighting where nearby surfaces block it",
        default=True,
    )
    ambient_occlusion_distance: FloatProperty(
        name="Distance",
        description="Distance of the surfaces that occlude",
        default=0.2,
        min=0.0,
        soft_max=10.0,
        subtype='DISTANCE',
    )
    use_bounce_light: BoolProperty(
        name="Bounce Light",
        description="Light the occluded parts with the light of the surfaces occluding them "
        "(one more bounce every sample)",
        default=True,
    )
    ambient_occlusion_factor: FloatProperty(
        name="Factor",
        description="Strength of the occlusion",
        default=1.0,
        min=0.0,
        soft_max=2.0,
        subtype='FACTOR',
    )


def register():
    bpy.utils.register_class(MechazyiuSceneSettings)
    bpy.types.Scene.mechazyiu = PointerProperty(type=MechazyiuSceneSettings)

    for base in (bpy.types.Panel, bpy.types.Menu, bpy.types.UIList):
        for cls in _subclasses(base):
            engines = getattr(cls, "COMPAT_ENGINES", None)
            if engines is None or ENGINE in engines:
                continue
            generic = 'BLENDER_EEVEE' in engines and 'BLENDER_WORKBENCH' in engines
            if generic or cls.__name__ in EEVEE_PANELS:
                # A set of its own: subclasses can share their base's set.
                cls.COMPAT_ENGINES = set(engines) | {ENGINE}


classes = ()
