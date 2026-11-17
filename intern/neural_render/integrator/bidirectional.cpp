/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/bidirectional.h"

#include "util/log.h"

CCL_NAMESPACE_BEGIN

namespace {

/* The strategies have no kernels yet. Each one only names itself, so the settings, the UI and the
 * calls from PathTrace can be in place before the first one is written. */

/* Light tracing has its kernels (kernel/integrator/init_from_light.h); PathTrace renders its light
 * paths after the camera paths of each work (PathTrace::render_light_paths()). */
class LightTracingStrategy : public BidirectionalStrategyBase {
 public:
  BidirectionalStrategy flag() const override
  {
    return BIDIR_STRATEGY_LIGHT_TRACING;
  }
  bool is_implemented() const override
  {
    return true;
  }
};

/* Photon mapping has its kernels (kernel/integrator/photon_map.h); PathTrace runs its photon pass at
 * the start of each frame (PathTraceWorkGPU::render_photons()). */
class PhotonMappingStrategy : public BidirectionalStrategyBase {
 public:
  BidirectionalStrategy flag() const override
  {
    return BIDIR_STRATEGY_PHOTON_MAPPING;
  }
  bool is_implemented() const override
  {
    return true;
  }
};

class ManifoldStrategy : public BidirectionalStrategyBase {
 public:
  BidirectionalStrategy flag() const override
  {
    return BIDIR_STRATEGY_MANIFOLD;
  }
};

vector<unique_ptr<BidirectionalStrategyBase>> create_strategies()
{
  vector<unique_ptr<BidirectionalStrategyBase>> strategies;
  strategies.push_back(make_unique<LightTracingStrategy>());
  strategies.push_back(make_unique<PhotonMappingStrategy>());
  strategies.push_back(make_unique<ManifoldStrategy>());
  return strategies;
}

}  // namespace

uint bidirectional_implemented_strategies()
{
  uint implemented = BIDIR_STRATEGY_NONE;
  for (const unique_ptr<BidirectionalStrategyBase> &strategy : create_strategies()) {
    if (strategy->is_implemented()) {
      implemented |= strategy->flag();
    }
  }
  return implemented;
}

const char *bidirectional_strategy_name(const BidirectionalStrategy strategy)
{
  switch (strategy) {
    case BIDIR_STRATEGY_LIGHT_TRACING:
      return "Light Tracing";
    case BIDIR_STRATEGY_PHOTON_MAPPING:
      return "Photon Mapping";
    case BIDIR_STRATEGY_MANIFOLD:
      return "Manifold Walks";
    default:
      return "Unknown";
  }
}

Bidirectional::Bidirectional() : strategies_(create_strategies()) {}

Bidirectional::~Bidirectional()
{
  free();
}

void Bidirectional::set_params(const BidirectionalParams &params, const bool reset)
{
  const bool modified = params_.modified(params);
  params_ = params;

  if (modified) {
    warned_strategies_ = BIDIR_STRATEGY_NONE;
  }
  if (modified || reset) {
    for (unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
      strategy->reset();
    }
  }
}

uint Bidirectional::active_strategies() const
{
  uint active = BIDIR_STRATEGY_NONE;
  const uint requested = params_.requested();
  for (const unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
    if ((requested & strategy->flag()) && strategy->is_implemented()) {
      active |= strategy->flag();
    }
  }
  return active;
}

void Bidirectional::warn_not_implemented()
{
  const uint requested = params_.requested();
  for (const unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
    const uint flag = strategy->flag();
    if ((requested & flag) && !strategy->is_implemented() && !(warned_strategies_ & flag)) {
      LOG_WARNING << "Cycles-expansion: bidirectional " << strategy->name()
                  << " is not implemented yet, the image is path traced without it";
      warned_strategies_ |= flag;
    }
  }
}

void Bidirectional::prepare(const BidirectionalContext &context)
{
  if (params_.requested() == BIDIR_STRATEGY_NONE) {
    return;
  }
  warn_not_implemented();

  const uint active = active_strategies();
  for (unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
    if (active & strategy->flag()) {
      strategy->prepare(context);
    }
  }
}

void Bidirectional::finish(const BidirectionalContext &context)
{
  const uint active = active_strategies();
  for (unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
    if (active & strategy->flag()) {
      strategy->finish(context);
    }
  }
}

void Bidirectional::free()
{
  for (unique_ptr<BidirectionalStrategyBase> &strategy : strategies_) {
    strategy->free();
  }
}

CCL_NAMESPACE_END
