# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# MECHAZYIU (the render engine of the Windows XP build) in the properties of the other modules:
# the panels shown for every engine (those listing both EEVEE and Workbench), and the EEVEE panels
# of the settings MECHAZYIU uses too. MECHAZYIU's own panels are in `properties_render.py`.

import bpy

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


def register():
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
