/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_immediate_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

/**
 * Immediate mode: vertices are written to host memory, then converted to the device layout into a
 * dynamic vertex buffer used as a ring.
 */
class D3D9Immediate : public Immediate {
 public:
  ~D3D9Immediate() override;

  uchar *begin() override;
  void end() override;

 private:
  Vector<uchar> host_data_;
  IDirect3DVertexBuffer9 *vbo_ = nullptr;
  uint offset_ = 0;
};

}  // namespace blender::gpu
