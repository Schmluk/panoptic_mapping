#include "panoptic_mapping/tracking/single_tsdf_tracker.h"

#include <memory>
#include <unordered_set>
#include <utility>

namespace panoptic_mapping {

config_utilities::Factory::RegistrationRos<IDTrackerBase, SingleTSDFTracker,
                                           std::shared_ptr<Globals>>
    SingleTSDFTracker::registration_("single_tsdf");

void SingleTSDFTracker::Config::checkParams() const {
  checkParamConfig(submap_config);
}

void SingleTSDFTracker::Config::setupParamsAndPrinting() {
  setupParam("verbosity", &verbosity);
  setupParam("submap_config", &submap_config);
  setupParam("use_detectron", &use_detectron);
  setupParam("use_instance_classification", &use_instance_classification);
}

SingleTSDFTracker::SingleTSDFTracker(const Config& config,
                                     std::shared_ptr<Globals> globals)
    : config_(config.checkValid()), IDTrackerBase(std::move(globals)) {
  LOG_IF(INFO, config_.verbosity >= 1) << "\n" << config_.toString();
  addRequiredInput(InputData::InputType::kColorImage);
  addRequiredInput(InputData::InputType::kDepthImage);
  if (config_.submap_config.use_class_layer) {
    addRequiredInput(InputData::InputType::kSegmentationImage);
  }
  if (config_.use_detectron) {
    addRequiredInput(InputData::InputType::kDetectronLabels);
  }
}

void SingleTSDFTracker::processInput(SubmapCollection* submaps,
                                     InputData* input) {
  CHECK_NOTNULL(submaps);
  CHECK_NOTNULL(input);
  CHECK(inputIsValid(*input));

  // Check whether the map is already allocated.
  if (!is_setup_) {
    setup(submaps);
  }
}

void SingleTSDFTracker::setup(SubmapCollection* submaps) {
  // Check if there is a loaded map.
  if (submaps->size() > 0) {
    Submap& map = *(submaps->begin());
    if (map.getConfig().voxel_size != config_.submap_config.voxel_size ||
        map.getConfig().voxels_per_side !=
            config_.submap_config.voxels_per_side ||
        map.getConfig().truncation_distance !=
            config_.submap_config.truncation_distance ||
        map.getConfig().use_class_layer !=
            config_.submap_config.use_class_layer) {
      LOG(WARNING)
          << "Loaded submap config does not match the specified config.";
    }
    map.setIsActive(true);
    map_id_ = map.getID();
  } else {
    // Allocate the single map.
    Submap* new_submap = submaps->createSubmap(config_.submap_config);
    new_submap->setLabel(PanopticLabel::kBackground);
    map_id_ = new_submap->getID();
  }
  submaps->setActiveFreeSpaceSubmapID(map_id_);
  is_setup_ = true;
}

}  // namespace panoptic_mapping
