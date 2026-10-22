/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Direct3D 9 drawing context, for GPUs limited to Shader Model 2.0 (Windows XP era hardware).
 *
 * All contexts share a single device (created on a hidden window), so GPU resources are shared
 * between windows like with OpenGL shared contexts. Every window context owns an additional swap
 * chain which is presented by #swapBufferRelease.
 */

#pragma once

#ifndef WIN32
#  error WIN32 only!
#endif

#include <d3d9.h>

#include "GHOST_Context.hh"

class GHOST_ContextD3D9 : public GHOST_Context {
 public:
  /** `hwnd` is null for off-screen contexts. */
  GHOST_ContextD3D9(const GHOST_ContextParams &context_params, HWND hwnd);
  ~GHOST_ContextD3D9() override;

  GHOST_TSuccess swapBufferAcquire() override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess swapBufferRelease() override;
  GHOST_TSuccess activateDrawingContext() override;
  GHOST_TSuccess releaseDrawingContext() override;
  GHOST_TSuccess initializeDrawingContext() override;
  GHOST_TSuccess updateDrawingContext() override;
  GHOST_TSuccess releaseNativeHandles() override;
  GHOST_TSuccess setSwapInterval(int interval) override;
  GHOST_TSuccess getSwapInterval(int &interval_out) override;
  GHOST_TSuccess getD3D9Handles(GHOST_D3D9Handles &r_handles) override;

 private:
  bool swap_chain_create(int width, int height);
  void swap_chain_free();

  static bool device_acquire();
  static void device_release();

  HWND hwnd_;
  IDirect3DSwapChain9 *swap_chain_ = nullptr;
  /** Events issued after the presents, to limit the frames queued in the driver (latency). */
  IDirect3DQuery9 *frame_queries_[2] = {nullptr, nullptr};
  int frame_query_index_ = 0;
  IDirect3DSurface9 *back_buffer_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int swap_interval_ = 0;
  bool has_device_ = false;
};
