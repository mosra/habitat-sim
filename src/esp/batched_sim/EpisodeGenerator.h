// Copyright (c) Facebook, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef ESP_BATCHEDSIM_EPISODEGENERATOR_H_
#define ESP_BATCHEDSIM_EPISODEGENERATOR_H_

#include "esp/batched_sim/EpisodeSet.h"
#include "esp/core/esp.h"
#include "esp/batched_sim/configure.h"

namespace esp {
namespace batched_sim {

struct EpisodeGeneratorConfig {
  int numEpisodes = 100;
  int seed = 0;
  int numStageVariations = 84;  // see selectedReplicaCadBakedStages
  int numObjectVariations = 9;  // see selectedYCBObjects
  int minNontargetObjects = 27;
  int maxNontargetObjects = 32;
  bool useFixedRobotStartPos = false;
  bool useFixedRobotStartYaw = false;
  bool useFixedRobotJointStartPositions = false;
  ESP_SMART_POINTERS(EpisodeGeneratorConfig);
};

EpisodeSet generateBenchmarkEpisodeSet(const EpisodeGeneratorConfig& config,
                                       #ifndef MAGNUM_RENDERER
                                       const BpsSceneMapping& sceneMapping,
                                       #endif
                                       const serialize::Collection& collection);

}  // namespace batched_sim
}  // namespace esp

#endif
