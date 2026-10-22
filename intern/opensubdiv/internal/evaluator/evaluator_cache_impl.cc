/* SPDX-FileCopyrightText: 2021 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "internal/evaluator/evaluator_cache_impl.h"

#ifdef WITH_OPENSUBDIV_GPU_EVALUATOR
#  include "internal/evaluator/eval_output_gpu.h"
#endif

OpenSubdiv_EvaluatorCacheImpl::OpenSubdiv_EvaluatorCacheImpl() = default;

OpenSubdiv_EvaluatorCacheImpl::~OpenSubdiv_EvaluatorCacheImpl()
{
#ifdef WITH_OPENSUBDIV_GPU_EVALUATOR
  delete static_cast<blender::opensubdiv::GpuEvalOutput::EvaluatorCache *>(eval_cache);
#endif
}

OpenSubdiv_EvaluatorCacheImpl *openSubdiv_createEvaluatorCacheInternal(
    eOpenSubdivEvaluator evaluator_type)
{
#ifndef WITH_OPENSUBDIV_GPU_EVALUATOR
  /* Only the GPU evaluator has a cache. */
  (void)evaluator_type;
  return nullptr;
#else
  if (evaluator_type != eOpenSubdivEvaluator::OPENSUBDIV_EVALUATOR_GPU) {
    return nullptr;
  }
  OpenSubdiv_EvaluatorCacheImpl *evaluator_cache;
  evaluator_cache = new OpenSubdiv_EvaluatorCacheImpl;
  blender::opensubdiv::GpuEvalOutput::EvaluatorCache *eval_cache;
  eval_cache = new blender::opensubdiv::GpuEvalOutput::EvaluatorCache();
  evaluator_cache->eval_cache = eval_cache;
  return evaluator_cache;
#endif
}

void openSubdiv_deleteEvaluatorCacheInternal(OpenSubdiv_EvaluatorCacheImpl *evaluator_cache)
{
  delete evaluator_cache;
}
