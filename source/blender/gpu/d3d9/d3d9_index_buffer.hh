/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_index_buffer.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9IndexBuf : public IndexBuf {
 public:
  ~D3D9IndexBuf() override;

  void upload_data() override;
  void bind_as_ssbo(uint /*binding*/) override {}
  void read(uint32_t *data) const override;
  void update_sub(uint start, uint len, const void *data) override;

  /** Upload if needed and return the device buffer of the source index buffer. */
  IDirect3DIndexBuffer9 *buffer_get();
  /** First index to draw, including the sub-range offset. */
  uint32_t draw_start_get() const
  {
    return index_start_;
  }

 private:
  void strip_restart_indices() override;

  IDirect3DIndexBuffer9 *ibo_ = nullptr;
};

}  // namespace blender::gpu
