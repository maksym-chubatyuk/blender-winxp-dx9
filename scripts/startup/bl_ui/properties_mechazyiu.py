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
    FloatVectorProperty,
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

    # Like EEVEE Legacy's bloom.
    use_bloom: BoolProperty(
        name="Bloom",
        description="Glow around the bright parts of the image",
        default=False,
    )
    bloom_threshold: FloatProperty(
        name="Threshold",
        description="Brightness above which the image glows",
        default=0.8,
        min=0.0,
        soft_max=10.0,
    )
    bloom_knee: FloatProperty(
        name="Knee",
        description="Softness of the threshold",
        default=0.5,
        min=0.0,
        max=1.0,
        subtype='FACTOR',
    )
    bloom_radius: FloatProperty(
        name="Radius",
        description="Spread of the glow",
        default=6.5,
        min=0.0,
        max=10.0,
    )
    bloom_color: FloatVectorProperty(
        name="Color",
        description="Color of the glow",
        default=(1.0, 1.0, 1.0),
        min=0.0,
        max=1.0,
        size=3,
        subtype='COLOR',
    )
    bloom_intensity: FloatProperty(
        name="Intensity",
        description="Strength of the glow",
        default=0.05,
        min=0.0,
        soft_max=1.0,
    )
    bloom_clamp: FloatProperty(
        name="Clamp",
        description="Maximum brightness of the glowing parts (0: none)",
        default=0.0,
        min=0.0,
        soft_max=1000.0,
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
