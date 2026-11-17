/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon Cycles-expansion: the framework for bidirectional light transport.
 *
 * Cycles-expansion traces camera paths the same way Cycles does (with the Neural Rendering
 * sampling). Bidirectional strategies add paths that start at the lights:
 *
 *   - Light tracing (BDPT): light subpaths connected to the camera and to camera subpath vertices.
 *   - Photon mapping: photons from the lights, gathered at camera subpath vertices (caustics).
 *   - Manifold walks (SMS): specular chains found by Newton steps, in place of MNEE.
 *
 * Each strategy is turned on by itself (scene.cycles.use_bidirectional and one setting per
 * strategy); with none of them, the render is the plain path tracing.
 *
 * How a strategy plugs in:
 *   - Integrator::get_bidirectional_params() reads the settings.
 *   - Session hands the params to PathTrace::set_bidirectional_params() before rendering.
 *   - PathTrace::render_pipeline() calls Bidirectional::prepare() before the camera paths of an
 *     iteration (build the light-side data: light subpaths, photon map, ...) and
 *     Bidirectional::finish() after them (e.g. add the light tracing splats to the passes).
 *   - The kernels read KernelIntegrator::bidirectional_strategies, which only holds strategies
 *     that are both asked for and implemented (bidirectional_implemented_strategies()).
 *
 * A new strategy derives from BidirectionalStrategyBase, returns true from is_implemented() once
 * its kernels exist, and is added in Bidirectional::Bidirectional(). */

#pragma once

#include "kernel/types.h"

#include "util/unique_ptr.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;

/* The strategies that have kernels in this build (BidirectionalStrategy flags). */
uint bidirectional_implemented_strategies();
/* Name of one strategy flag, for the log and the UI. */
const char *bidirectional_strategy_name(BidirectionalStrategy strategy);

struct BidirectionalParams {
  /* scene.cycles.use_bidirectional. */
  bool use = false;
  /* The asked for strategies (BidirectionalStrategy flags). */
  uint strategies = BIDIR_STRATEGY_NONE;
  /* The longest light subpath (number of bounces after leaving the light). */
  int max_light_bounces = 8;

  BidirectionalParams() = default;

  /* Strategies asked for (zero when the bidirectional switch is off). */
  uint requested() const
  {
    return use ? strategies : uint(BIDIR_STRATEGY_NONE);
  }

  bool modified(const BidirectionalParams &other) const
  {
    return !(use == other.use && strategies == other.strategies &&
             max_light_bounces == other.max_light_bounces);
  }
};

/* What a strategy gets for one rendering iteration. */
struct BidirectionalContext {
  Device *device = nullptr;
  DeviceScene *device_scene = nullptr;
  const BidirectionalParams *params = nullptr;
  /* Samples of this iteration. */
  int start_sample = 0;
  int num_samples = 0;
  int sample_offset = 0;
};

class BidirectionalStrategyBase {
 public:
  virtual ~BidirectionalStrategyBase() = default;

  virtual BidirectionalStrategy flag() const = 0;
  /* Name for the log and the UI. */
  const char *name() const
  {
    return bidirectional_strategy_name(flag());
  }
  /* False until the kernels of the strategy exist. While false, asking for the strategy changes
   * nothing in the render (a warning is logged). */
  virtual bool is_implemented() const
  {
    return false;
  }

  /* The settings changed, or the scene was reset: drop what was built for the last ones. */
  virtual void reset() {}
  /* Before the camera paths of an iteration: build the light-side data. */
  virtual void prepare(const BidirectionalContext & /*context*/) {}
  /* After the camera paths of an iteration. */
  virtual void finish(const BidirectionalContext & /*context*/) {}
  /* The device memory of the strategy is freed (the scene or the device goes away). */
  virtual void free() {}
};

class Bidirectional {
 public:
  Bidirectional();
  ~Bidirectional();

  void set_params(const BidirectionalParams &params, const bool reset);
  const BidirectionalParams &get_params() const
  {
    return params_;
  }

  /* Strategies that are asked for and implemented. */
  uint active_strategies() const;
  bool is_active() const
  {
    return active_strategies() != BIDIR_STRATEGY_NONE;
  }

  void prepare(const BidirectionalContext &context);
  void finish(const BidirectionalContext &context);
  void free();

 protected:
  void warn_not_implemented();

  BidirectionalParams params_;
  vector<unique_ptr<BidirectionalStrategyBase>> strategies_;
  /* Strategies already warned about, so the log is not repeated for every iteration. */
  uint warned_strategies_ = BIDIR_STRATEGY_NONE;
};

CCL_NAMESPACE_END
