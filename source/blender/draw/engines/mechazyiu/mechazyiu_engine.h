/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "DRW_render.hh"

namespace blender {

struct RenderEngineType;

extern RenderEngineType DRW_engine_viewport_mechazyiu_type;

namespace mechazyiu {

struct Engine : public DrawEngine::Pointer {
  DrawEngine *create_instance() final;

  static void free_static();
};

}  // namespace mechazyiu
}  // namespace blender
