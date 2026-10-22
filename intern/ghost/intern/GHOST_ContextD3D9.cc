/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <cstdio>

#include "GHOST_ContextD3D9.hh"

/* -------------------------------------------------------------------- */
/** \name Shared Device
 * \{ */

static IDirect3D9 *g_d3d = nullptr;
static IDirect3DDevice9 *g_device = nullptr;
static HWND g_device_window = nullptr;
static int g_device_users = 0;

bool GHOST_ContextD3D9::device_acquire()
{
  if (g_device) {
    g_device_users++;
    return true;
  }

  g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (g_d3d == nullptr) {
    fprintf(stderr, "GHOST: Direct3DCreate9 failed, DirectX 9.0c is required.\n");
    return false;
  }

  /* Windowed devices only use their window for the implicit swap chain, which is never presented:
   * keep it hidden and minimal. */
  g_device_window = CreateWindowA("STATIC",
                                  "Blender D3D9",
                                  WS_POPUP,
                                  0,
                                  0,
                                  1,
                                  1,
                                  nullptr,
                                  nullptr,
                                  GetModuleHandle(nullptr),
                                  nullptr);

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 1;
  pp.BackBufferHeight = 1;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_device_window;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  /* Blender relies on double precision math: keep the FPU state. Resources are created from worker
   * threads too, so let the runtime serialize access. */
  const DWORD common_flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
  const DWORD vertex_processing[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };
  HRESULT hr = E_FAIL;
  for (const DWORD vp : vertex_processing) {
    hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT,
                             D3DDEVTYPE_HAL,
                             g_device_window,
                             common_flags | vp,
                             &pp,
                             &g_device);
    if (SUCCEEDED(hr)) {
      break;
    }
  }
  if (FAILED(hr)) {
    fprintf(stderr, "GHOST: IDirect3D9::CreateDevice failed (0x%08lx).\n", (unsigned long)hr);
    g_device = nullptr;
    DestroyWindow(g_device_window);
    g_device_window = nullptr;
    g_d3d->Release();
    g_d3d = nullptr;
    return false;
  }
  g_device_users = 1;
  return true;
}

void GHOST_ContextD3D9::device_release()
{
  if (--g_device_users > 0) {
    return;
  }
  g_device->Release();
  g_device = nullptr;
  g_d3d->Release();
  g_d3d = nullptr;
  DestroyWindow(g_device_window);
  g_device_window = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context
 * \{ */

GHOST_ContextD3D9::GHOST_ContextD3D9(const GHOST_ContextParams &context_params, HWND hwnd)
    : GHOST_Context(context_params), hwnd_(hwnd)
{
}

GHOST_ContextD3D9::~GHOST_ContextD3D9()
{
  swap_chain_free();
  if (has_device_) {
    device_release();
  }
}

bool GHOST_ContextD3D9::swap_chain_create(int width, int height)
{
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = UINT(width);
  pp.BackBufferHeight = UINT(height);
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd_;
  pp.Windowed = TRUE;
  pp.PresentationInterval = swap_interval_ ? D3DPRESENT_INTERVAL_ONE :
                                             D3DPRESENT_INTERVAL_IMMEDIATE;

  HRESULT hr = g_device->CreateAdditionalSwapChain(&pp, &swap_chain_);
  if (FAILED(hr)) {
    fprintf(stderr,
            "GHOST: CreateAdditionalSwapChain %dx%d failed (0x%08lx).\n",
            width,
            height,
            (unsigned long)hr);
    swap_chain_ = nullptr;
    return false;
  }
  swap_chain_->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back_buffer_);
  width_ = width;
  height_ = height;
  return true;
}

void GHOST_ContextD3D9::swap_chain_free()
{
  if (back_buffer_) {
    back_buffer_->Release();
    back_buffer_ = nullptr;
  }
  if (swap_chain_) {
    swap_chain_->Release();
    swap_chain_ = nullptr;
  }
}

GHOST_TSuccess GHOST_ContextD3D9::initializeDrawingContext()
{
  if (!device_acquire()) {
    return GHOST_kFailure;
  }
  has_device_ = true;
  if (context_params_.vsync != GHOST_kVSyncModeUnset) {
    swap_interval_ = context_params_.vsync == GHOST_kVSyncModeOff ? 0 : 1;
  }
  return updateDrawingContext();
}

GHOST_TSuccess GHOST_ContextD3D9::updateDrawingContext()
{
  if (hwnd_ == nullptr) {
    return GHOST_kSuccess;
  }
  RECT rect;
  GetClientRect(hwnd_, &rect);
  /* A minimized window has an empty client area: keep the previous swap chain. */
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    return swap_chain_ ? GHOST_kSuccess : (swap_chain_create(1, 1) ? GHOST_kSuccess :
                                                                     GHOST_kFailure);
  }
  if (swap_chain_ && width == width_ && height == height_) {
    return GHOST_kSuccess;
  }
  swap_chain_free();
  return swap_chain_create(width, height) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_ContextD3D9::swapBufferRelease()
{
  if (swap_chain_ == nullptr) {
    return GHOST_kFailure;
  }
  const HRESULT hr = swap_chain_->Present(nullptr, nullptr, nullptr, nullptr, 0);
  return SUCCEEDED(hr) ? GHOST_kSuccess : GHOST_kFailure;
}

GHOST_TSuccess GHOST_ContextD3D9::activateDrawingContext()
{
  active_context_ = this;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextD3D9::releaseDrawingContext()
{
  active_context_ = nullptr;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextD3D9::releaseNativeHandles()
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextD3D9::setSwapInterval(int interval)
{
  if (swap_interval_ == interval) {
    return GHOST_kSuccess;
  }
  swap_interval_ = interval;
  if (swap_chain_) {
    swap_chain_free();
    width_ = height_ = 0;
    return updateDrawingContext();
  }
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextD3D9::getSwapInterval(int &interval_out)
{
  interval_out = swap_interval_;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_ContextD3D9::getD3D9Handles(GHOST_D3D9Handles &r_handles)
{
  updateDrawingContext();
  r_handles.device = g_device;
  r_handles.back_buffer = back_buffer_;
  r_handles.width = width_;
  r_handles.height = height_;
  return g_device ? GHOST_kSuccess : GHOST_kFailure;
}

/** \} */
