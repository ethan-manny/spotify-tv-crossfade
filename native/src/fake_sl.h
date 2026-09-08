#pragma once
#include "backend.h"
#include "pipeline.h"

namespace xfade {
using BackendFactory = Backend* (*)();
using PipelineFactory = Pipeline* (*)(const PcmFormat&);
// Called by the library (Task 9) and by tests to choose what a realized player is wired to.
void setFactories(BackendFactory backend, PipelineFactory pipeline);
}  // namespace xfade
