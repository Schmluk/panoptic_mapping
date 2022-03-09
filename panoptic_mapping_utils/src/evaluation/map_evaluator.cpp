#include "panoptic_mapping_utils/evaluation/map_evaluator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <panoptic_mapping/3rd_party/config_utilities.hpp>
#include <panoptic_mapping/3rd_party/csv.h>
#include <pcl/features/moment_of_inertia_estimation.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/ply_io.h>
#include <ros/ros.h>
#include <voxblox/interpolator/interpolator.h>
#include <voxblox/io/mesh_ply.h>
#include <voxblox/mesh/mesh_utils.h>

#include "panoptic_mapping_utils/evaluation/progress_bar.h"

namespace {

pcl::PointXYZ get_voxel_centroid(const Eigen::Vector3i& voxel_coordinates,
                                 float voxel_size) {
  auto t = voxel_coordinates.cast<float>().normalized() * (voxel_size / 2);
  return {voxel_coordinates.x() * voxel_size + t.x(),
          voxel_coordinates.y() * voxel_size + t.y(),
          voxel_coordinates.z() * voxel_size + t.z()};
}

}  // namespace

namespace panoptic_mapping {

void MapEvaluator::EvaluationRequest::checkParams() const {
  checkParamGT(maximum_distance, 0.f, "maximum_distance");
  checkParamGT(inlier_distance, 0.f, "inlier_distance");
}

void MapEvaluator::EvaluationRequest::setupParamsAndPrinting() {
  setupParam("verbosity", &verbosity);
  setupParam("map_file", &map_file);
  setupParam("ground_truth_pointcloud_file", &ground_truth_pointcloud_file);
  setupParam("output_suffix", &output_suffix);
  setupParam("maximum_distance", &maximum_distance);
  setupParam("evaluate", &evaluate);
  setupParam("visualize", &visualize);
  setupParam("compute_coloring", &compute_coloring);
  setupParam("color_by_max_error", &color_by_max_error);
  setupParam("color_by_mesh_distance", &color_by_mesh_distance);
  setupParam("ignore_truncated_points", &ignore_truncated_points);
  setupParam("inlier_distance", &inlier_distance);
  setupParam("is_single_tsdf", &is_single_tsdf);
  setupParam("export_mesh", &export_mesh);
  setupParam("export_labeled_pointcloud", &export_labeled_pointcloud);
  setupParam("export_coverage_pointcloud", &export_coverage_pointcloud);
}

MapEvaluator::MapEvaluator(const ros::NodeHandle& nh,
                           const ros::NodeHandle& nh_private)
    : nh_(nh), nh_private_(nh_private) {
  auto config =
      config_utilities::getConfigFromRos<SubmapVisualizer::Config>(nh_private_);
  visualizer_ = std::make_unique<SubmapVisualizer>(config, nullptr);
}

bool MapEvaluator::setupMultiMapEvaluation() {
  // Get evaluation configuration (wait till set).
  while (!nh_private_.hasParam("ground_truth_pointcloud_file")) {
    ros::Duration(0.05).sleep();
  }
  request_ = config_utilities::getConfigFromRos<
      panoptic_mapping::MapEvaluator::EvaluationRequest>(nh_private_);
  LOG_IF(INFO, request_.verbosity >= 1) << "\n" << request_.toString();
  if (!request_.isValid(true)) {
    LOG(ERROR) << "Invalid evaluation request.";
    return false;
  }
  use_voxblox_ = false;

  // Load GT cloud.
  gt_cloud_ptr_ = std::make_unique<pcl::PointCloud<pcl::PointXYZ>>();
  if (pcl::io::loadPLYFile<pcl::PointXYZ>(request_.ground_truth_pointcloud_file,
                                          *gt_cloud_ptr_) != 0) {
    LOG(ERROR) << "Could not load ground truth point cloud from '"
               << request_.ground_truth_pointcloud_file << "'.";
    return false;
  }
  buildKdTree();
  LOG_IF(INFO, request_.verbosity >= 2) << "Loaded ground truth pointcloud";

  // Setup Output File.
  // NOTE(schmluk): The map_file is used to specify the target path here.
  std::string out_file_name =
      request_.map_file + "/" + request_.output_suffix + ".csv";
  output_file_.open(out_file_name, std::ios::out);
  if (!output_file_.is_open()) {
    LOG(ERROR) << "Failed to open output file '" << out_file_name << "'.";
    return false;
  }
  output_file_
      << "MeanGTError [m],StdGTError [m],GTRMSE [m],TotalPoints [1],"
      << "UnknownPoints [1],TruncatedPoints [1],GTInliers [1],MeanMapError [m],"
      << "StdMapError [m],MapRMSE[m],MapInliers[1],MapOutliers[1]\n ";

  // Save the generated eval artifacts also to this directory
  target_directory_ = request_.map_file;

  // Advertise evaluation service.
  process_map_srv_ = nh_private_.advertiseService(
      "process_map", &MapEvaluator::evaluateMapCallback, this);
  return true;
}

bool MapEvaluator::evaluate(const EvaluationRequest& request) {
  if (!request.isValid(true)) {
    return false;
  }
  LOG_IF(INFO, request.verbosity >= 2) << "Processing: \n"
                                       << request.toString();

  // Load the groundtruth pointcloud.
  if (request.evaluate || request.compute_coloring) {
    if (!request.ground_truth_pointcloud_file.empty()) {
      gt_cloud_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr(
          new pcl::PointCloud<pcl::PointXYZ>());
      if (pcl::io::loadPLYFile<pcl::PointXYZ>(
              request.ground_truth_pointcloud_file, *gt_cloud_ptr_) != 0) {
        LOG(ERROR) << "Could not load ground truth point cloud from '"
                   << request.ground_truth_pointcloud_file << "'.";
        gt_cloud_ptr_.reset();
        return false;
      }
      LOG_IF(INFO, request.verbosity >= 2) << "Loaded ground truth pointcloud";
    }
    if (!gt_cloud_ptr_) {
      LOG(ERROR) << "No ground truth pointcloud loaded.";
      return false;
    }
  }

  // Load the map to evaluate.
  if (!request.map_file.empty()) {
    // Load the map.
    const std::string extension =
        request.map_file.substr(request.map_file.find_last_of('.'));
    size_t separator = request.map_file.find_last_of('/');
    target_directory_ = request.map_file.substr(0, separator);
    target_map_name_ = request.map_file.substr(
        separator + 1,
        request.map_file.length() - separator - extension.length() - 1);

    if (extension == ".panmap") {
      // Load panoptic map.
      use_voxblox_ = false;
      submaps_ = std::make_shared<SubmapCollection>();
      if (!submaps_->loadFromFile(request.map_file)) {
        LOG(ERROR) << "Could not load panoptic map from '" << request.map_file
                   << "'.";
        submaps_.reset();
        return false;
      }
      planning_ = std::make_unique<PlanningInterface>(submaps_);
      LOG_IF(INFO, request.verbosity >= 2) << "Loaded the target panoptic map.";
    } else if (extension == ".vxblx") {
      use_voxblox_ = true;
      voxblox::io::LoadLayer<voxblox::TsdfVoxel>(request.map_file, &voxblox_);
      LOG_IF(INFO, request.verbosity >= 2) << "Loaded the target voxblox map.";
    } else {
      LOG(ERROR) << "cannot load file of unknown extension '"
                 << request.map_file << "'.";
      return false;
    }
  }
  if (!submaps_ && !use_voxblox_) {
    LOG(ERROR) << "No panoptic map loaded.";
    return false;
  }

  // Setup output file
  if (request.evaluate) {
    std::string out_file_name = target_directory_ + "/" + target_map_name_ +
                                "_" + request.output_suffix + ".csv";
    output_file_.open(out_file_name, std::ios::out);
    if (!output_file_.is_open()) {
      LOG(ERROR) << "Failed to open output file '" << out_file_name << "'.";
      return false;
    }

    // Evaluate.
    LOG_IF(INFO, request.verbosity >= 2) << "Computing reconstruction error:";
    output_file_ << "MeanError [m],StdError [m],RMSE [m],TotalPoints [1],"
                 << "UnknownPoints [1],TruncatedPoints [1]\n";
    output_file_ << computeReconstructionError(request);
    output_file_.close();
  }

  if (request.export_mesh) {
    exportMesh(request);
  }

  if (request.export_labeled_pointcloud) {
    exportLabeledPointcloud(request);
  }

  // Compute visualization if required.
  if (request.compute_coloring) {
    LOG_IF(INFO, request.verbosity >= 2) << "Computing visualization coloring:";
    visualizeReconstructionError(request);
  }

  // Display the mesh.
  if (request.visualize) {
    LOG_IF(INFO, request.verbosity >= 2) << "Publishing mesh.";
    publishVisualization();
  }

  LOG_IF(INFO, request.verbosity >= 2) << "Done.";
  return true;
}

std::string MapEvaluator::computeReconstructionError(
    const EvaluationRequest& request) {
  // Go through each point, use trilateral interpolation to figure out the
  // distance at that point.

  // Setup.
  uint64_t total_points = 0;
  uint64_t unknown_points = 0;
  uint64_t truncated_points = 0;
  uint64_t inliers = 0;
  std::vector<float> abserror;
  abserror.reserve(gt_cloud_ptr_->size());  // Just reserve the worst case.

  // Setup progress bar.
  const uint64_t interval = gt_cloud_ptr_->size() / 100;
  uint64_t count = 0;
  ProgressBar bar;

  // Evaluate gt pcl based(# gt points within < trunc_dist)
  std::unique_ptr<voxblox::Interpolator<voxblox::TsdfVoxel>> interp;

  if (use_voxblox_) {
    interp.reset(new voxblox::Interpolator<voxblox::TsdfVoxel>(voxblox_.get()));
  }

  for (const auto& pcl_point : *gt_cloud_ptr_) {
    const Point point(pcl_point.x, pcl_point.y, pcl_point.z);
    total_points++;

    // Lookup the distance.
    float distance;
    bool observed;
    if (use_voxblox_) {
      observed = interp->getDistance(point, &distance, true);
    } else {
      if (request.is_single_tsdf) {
        observed = planning_->getDistance(point, &distance, false, true);
      } else {
        observed = planning_->getDistance(point, &distance, true, false);
      }
    }

    // Compute the error.
    if (observed) {
      if (std::abs(distance) > request.maximum_distance) {
        truncated_points++;
        if (!request.ignore_truncated_points) {
          abserror.push_back(request.maximum_distance);
        }
      } else {
        abserror.push_back(std::abs(distance));
      }
      if (std::abs(distance) <= request_.inlier_distance) {
        inliers++;
      }
    } else {
      unknown_points++;
    }

    // Progress bar.
    if (count % interval == 0) {
      bar.display(static_cast<float>(count) / gt_cloud_ptr_->size());
    }
    count++;
  }
  bar.display(1.f);

  // Report summary.
  float mean = 0.0;
  float rmse = 0.0;
  for (auto value : abserror) {
    mean += value;
    rmse += std::pow(value, 2);
  }
  if (!abserror.empty()) {
    mean /= static_cast<float>(abserror.size());
    rmse = std::sqrt(rmse / static_cast<float>(abserror.size()));
  }
  float stddev = 0.0;
  for (auto value : abserror) {
    stddev += std::pow(value - mean, 2.0);
  }
  if (abserror.size() > 2) {
    stddev = sqrt(stddev / static_cast<float>(abserror.size() - 1));
  }

  std::stringstream ss;
  ss << mean << "," << stddev << "," << rmse << "," << total_points << ","
     << unknown_points << "," << truncated_points << "," << inliers;
  return ss.str();
}

std::string MapEvaluator::computeMeshError(const EvaluationRequest& request) {
  // Setup progress bar.
  float counter = 0.f;
  float max_counter = 0.f;
  ProgressBar bar;
  for (auto& submap : *submaps_) {
    voxblox::BlockIndexList block_list;
    submap.getMeshLayer().getAllAllocatedMeshes(&block_list);
    max_counter += block_list.size();
  }

  // Setup error computation.
  uint64_t inliers = 0;
  uint64_t outliers = 0;
  std::vector<float> errors;

  // Parse all submaps
  for (auto& submap : *submaps_) {
    if (!request.is_single_tsdf) {
      if (submap.getLabel() == PanopticLabel::kFreeSpace ||
          submap.getChangeState() == ChangeState::kAbsent ||
          submap.getChangeState() == ChangeState::kUnobserved) {
        voxblox::BlockIndexList block_list;
        submap.getMeshLayer().getAllAllocatedMeshes(&block_list);
        counter += block_list.size();
        bar.display(counter / max_counter);
        continue;
      }
    }

    // Parse all mesh vertices.
    voxblox::BlockIndexList block_list;
    submap.getMeshLayer().getAllAllocatedMeshes(&block_list);
    for (auto& block_index : block_list) {
      if (!ros::ok()) {
        return "";
      }
      for (const Point& point :
           submap.getMeshLayer().getMeshByIndex(block_index).vertices) {
        // Find closest GT point.
        float query_pt[3] = {point.x(), point.y(), point.z()};
        std::vector<size_t> ret_index(1);
        std::vector<float> out_dist_sqr(1);
        int num_results = kdtree_->knnSearch(&query_pt[0], 1, &ret_index[0],
                                             &out_dist_sqr[0]);

        if (num_results != 0) {
          const float error =
              (kdtree_data_.points[ret_index[0]] - point).norm();
          errors.emplace_back(error);
          if (error <= request_.inlier_distance) {
            inliers++;
          } else {
            outliers++;
          }
        }
      }

      // Show progress.
      counter += 1.f;
      bar.display(counter / max_counter);
    }
  }

  // Compute result.
  float mean = 0.0;
  float rmse = 0.0;
  for (auto value : errors) {
    mean += value;
    rmse += std::pow(value, 2);
  }
  if (!errors.empty()) {
    mean /= static_cast<float>(errors.size());
    rmse = std::sqrt(rmse / static_cast<float>(errors.size()));
  }
  float stddev = 0.0;
  for (auto value : errors) {
    stddev += std::pow(value - mean, 2.0);
  }
  if (errors.size() > 2) {
    stddev = sqrt(stddev / static_cast<float>(errors.size() - 1));
  }

  std::stringstream ss;
  ss << mean << "," << stddev << "," << rmse << "," << inliers << ","
     << outliers;
  return ss.str();
}

bool MapEvaluator::evaluateMapCallback(
    panoptic_mapping_msgs::SaveLoadMap::Request& request,
    panoptic_mapping_msgs::SaveLoadMap::Response& response) {
  // Load map.
  submaps_ = std::make_shared<SubmapCollection>();
  if (!submaps_->loadFromFile(request.file_path)) {
    LOG(ERROR) << "Could not load panoptic map from '" << request.file_path
               << "'.";
    submaps_.reset();
    return false;
  }

  if (request_.evaluate) {
    planning_ = std::make_unique<PlanningInterface>(submaps_);

    // Evaluate.
    output_file_ << computeReconstructionError(request_) << ","
                 << computeMeshError(request_) << "\n";
    output_file_.flush();
  }

  const std::string extension =
      request.file_path.substr(request.file_path.find_last_of('.'));
  size_t separator = request.file_path.find_last_of('/');
  target_map_name_ = request.file_path.substr(
      separator + 1,
      request.file_path.length() - separator - extension.length() - 1);

  if (request_.export_mesh) {
    exportMesh(request_);
  }

  if (request_.export_labeled_pointcloud) {
    exportLabeledPointcloud(request_);
  }

  if (request_.export_coverage_pointcloud) {
    exportCoveragePointcloud(request_);
  }

  return true;
}

void MapEvaluator::visualizeReconstructionError(
    const EvaluationRequest& request) {
  // Coloring: grey -> unknown, green -> 0 error, red -> maximum error,
  // purple -> truncated to max error.

  constexpr int max_number_of_neighbors_factor = 25000;  // points per cubic
  // meter depending on voxel size for faster nn search.
  buildKdTree();

  // Remove inactive maps.
  if (!request.is_single_tsdf) {
    std::vector<int> submaps_to_remove;
    for (const Submap& submap : *submaps_) {
      if (submap.getLabel() == PanopticLabel::kFreeSpace ||
          submap.getChangeState() != ChangeState::kPersistent) {
        submaps_to_remove.emplace_back(submap.getID());
      }
    }
    for (int id : submaps_to_remove) {
      submaps_->removeSubmap(id);
    }
  }

  // Setup progress bar.
  float counter = 0.f;
  float max_counter = 0.f;
  ProgressBar bar;
  for (auto& submap : *submaps_) {
    voxblox::BlockIndexList block_list;
    if (request.color_by_mesh_distance) {
      submap.getMeshLayer().getAllAllocatedMeshes(&block_list);
    } else {
      submap.getTsdfLayer().getAllAllocatedBlocks(&block_list);
    }
    max_counter += block_list.size();
  }

  if (request.color_by_mesh_distance) {
    for (auto& submap : *submaps_) {
      submap.updateMesh(false);
      voxblox::BlockIndexList block_list;
      submap.getMeshLayer().getAllAllocatedMeshes(&block_list);

      for (auto& block_id : block_list) {
        auto& mesh = submap.getMeshLayerPtr()->getMeshByIndex(block_id);
        const size_t size = mesh.vertices.size();
        mesh.colors.resize(size);
        for (size_t i = 0; i < size; ++i) {
          const float query_pt[3] = {mesh.vertices[i].x(), mesh.vertices[i].y(),
                                     mesh.vertices[i].z()};
          size_t ret_index;
          float out_dist_sqr;
          int num_results =
              kdtree_->knnSearch(&query_pt[0], 1, &ret_index, &out_dist_sqr);

          const float distance = std::sqrt(out_dist_sqr);
          const float frac = std::min(distance, request.maximum_distance) /
                             request.maximum_distance;
          const float r = std::min((frac - 0.5f) * 2.f + 1.f, 1.f) * 255.f;
          float g = (1.f - frac) * 2.f * 255.f;
          if (frac <= 0.5f) {
            g = 190.f + 130.f * frac;
          }
          mesh.colors[i] = Color(r, g, 0);
        }

        mesh.updated = false;
        bar.display(++counter / max_counter);
      }
    }

    // Store colored submaps.
    submaps_->saveToFile(target_directory_ + "/" + target_map_name_ +
                         "_evaluated.panmap");
  } else {
    // Parse all submaps
    for (auto& submap : *submaps_) {
      const size_t num_voxels_per_block =
          std::pow(submap.getTsdfLayer().voxels_per_side(), 3);
      const float voxel_size = submap.getTsdfLayer().voxel_size();
      const float voxel_size_sqr = voxel_size * voxel_size;
      const float truncation_distance = submap.getConfig().truncation_distance;
      const int max_number_of_neighbors =
          max_number_of_neighbors_factor / std::pow(1.f / voxel_size, 2.f);
      voxblox::Interpolator<TsdfVoxel> interpolator(
          submap.getTsdfLayerPtr().get());

      // Parse all voxels.
      voxblox::BlockIndexList block_list;
      submap.getTsdfLayer().getAllAllocatedBlocks(&block_list);
      int block_count = 0;
      for (auto& block_index : block_list) {
        if (!ros::ok()) {
          return;
        }

        voxblox::Block<TsdfVoxel>& block =
            submap.getTsdfLayerPtr()->getBlockByIndex(block_index);
        for (size_t linear_index = 0; linear_index < num_voxels_per_block;
             ++linear_index) {
          TsdfVoxel& voxel = block.getVoxelByLinearIndex(linear_index);
          if (voxel.distance > truncation_distance ||
              voxel.distance < -truncation_distance) {
            continue;  // these voxels can never be surface.
          }
          Point center = block.computeCoordinatesFromLinearIndex(linear_index);

          // Find surface points within 1 voxel size.
          float query_pt[3] = {center.x(), center.y(), center.z()};
          std::vector<size_t> ret_index(max_number_of_neighbors);
          std::vector<float> out_dist_sqr(max_number_of_neighbors);
          int num_results =
              kdtree_->knnSearch(&query_pt[0], max_number_of_neighbors,
                                 &ret_index[0], &out_dist_sqr[0]);

          if (num_results == 0) {
            // No nearby surface.
            voxel.color = Color(128, 128, 128);
            continue;
          }

          // Get average error.
          float total_error = 0.f;
          float max_error = 0.f;
          int counted_voxels = 0;
          float min_dist_sqr = 1000.f;
          for (int i = 0; i < num_results; ++i) {
            min_dist_sqr = std::min(min_dist_sqr, out_dist_sqr[i]);
            if (out_dist_sqr[i] > voxel_size_sqr) {
              continue;
            }
            voxblox::FloatingPoint distance;
            if (interpolator.getDistance(kdtree_data_.points[ret_index[i]],
                                         &distance, true)) {
              const float error = std::abs(distance);
              total_error += error;
              max_error = std::max(max_error, error);
              counted_voxels++;
            }
          }
          // Coloring.
          if (counted_voxels == 0) {
            counted_voxels = 1;
            total_error += std::sqrt(min_dist_sqr);
            max_error = min_dist_sqr;
          }
          float frac;
          if (request.color_by_max_error) {
            frac = std::min(max_error, request.maximum_distance) /
                   request.maximum_distance;
          } else {
            frac = std::min(total_error / counted_voxels,
                            request.maximum_distance) /
                   request.maximum_distance;
          }

          float r = std::min((frac - 0.5f) * 2.f + 1.f, 1.f) * 255.f;
          float g = (1.f - frac) * 2.f * 255.f;
          if (frac <= 0.5f) {
            g = 190.f + 130.f * frac;
          }
          voxel.color = voxblox::Color(r, g, 0);
        }

        // Show progress.
        counter += 1.f;
        bar.display(counter / max_counter);
      }
      submap.updateMesh(false);
    }

    // Store colored submaps.
    std::string output_name =
        target_directory_ + "/" + target_map_name_ + "_evaluated_" +
        (request.color_by_max_error ? "max" : "mean") + ".panmap";
    submaps_->saveToFile(output_name);
  }
}

void MapEvaluator::buildKdTree() {
  kdtree_data_.points.clear();
  kdtree_data_.points.reserve(gt_cloud_ptr_->size());
  for (const auto& point : *gt_cloud_ptr_) {
    kdtree_data_.points.emplace_back(point.x, point.y, point.z);
  }
  kdtree_.reset(new KDTree(3, kdtree_data_,
                           nanoflann::KDTreeSingleIndexAdaptorParams(10)));
  kdtree_->buildIndex();
}

void MapEvaluator::publishVisualization() {
  // Make sure the tfs arrive otherwise the mesh will be discarded.
  visualizer_->visualizeAll(submaps_.get());
}

void MapEvaluator::exportMesh(const EvaluationRequest& request) {
  // Collect all the meshes
  voxblox::AlignedVector<voxblox::Mesh::ConstPtr> meshes;
  for (auto& submap : *submaps_) {
    auto mesh = voxblox::Mesh::Ptr(new voxblox::Mesh());
    submap.getMeshLayer().getMesh(mesh.get());
    meshes.push_back(std::const_pointer_cast<const voxblox::Mesh>(mesh));
  }

  // Merge all the meshes into one
  auto combined_mesh = voxblox::Mesh::Ptr(new voxblox::Mesh());
  voxblox::createConnectedMesh(meshes, combined_mesh.get());

  // Export the mesh as ply
  const std::string out_mesh_file =
      target_directory_ + "/" + target_map_name_ + ".mesh.ply";
  voxblox::outputMeshAsPly(out_mesh_file, *combined_mesh);
}

void MapEvaluator::exportLabeledPointcloud(const EvaluationRequest& request) {
  pcl::PointCloud<pcl::PointXYZRGBL>::Ptr cloud_ptr(
      new pcl::PointCloud<pcl::PointXYZRGBL>());

  // Convert submaps collection to pointcloud with labels
  for (auto& submap : *submaps_) {
    if (!submap.hasClassLayer()) {
      continue;
    }

    // Parse all mesh vertices.
    voxblox::BlockIndexList block_list;
    submap.getMeshLayer().getAllAllocatedMeshes(&block_list);
    for (auto& block_index : block_list) {
      auto const& mesh = submap.getMeshLayer().getMeshByIndex(block_index);

      ClassBlock::ConstPtr class_block;
      if (submap.getClassLayer().hasBlock(block_index)) {
        class_block =
            submap.getClassLayer().getBlockConstPtrByIndex(block_index);
      }

      for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto const& vertex = mesh.vertices.at(i);
        auto const& color = mesh.colors.at(i);
        std::uint32_t label = 0;
        if (class_block) {
          // Lookup the class voxel
          const ClassVoxel* class_voxel =
              class_block->getVoxelPtrByCoordinates(vertex);

          switch (class_voxel->getVoxelType()) {
            case ClassVoxelType::kBinaryCount:
            case ClassVoxelType::kMovingBinaryCount: {
              // Keep only voxels belonging to the submaps
              if (class_voxel->getBelongingID() == 0) {
                continue;
              }
              label = submap.getClassID() * 1000;
              if (submap.getLabel() == PanopticLabel::kInstance) {
                label += submap.getInstanceID();
              }
              break;
            }
            case ClassVoxelType::kPanopticWeight:
            case ClassVoxelType::kFixedCount:
            case ClassVoxelType::kVariableCount: {
              label = class_voxel->getBelongingID();
              break;
            }
            default: {
              continue;
            }
          }
        }
        // TODO: just a hack to make sure no invalid labels are exported
        if (label > 50000) {
          continue;
        }
        pcl::PointXYZRGBL point(color.r, color.g, color.b, label);
        point.x = vertex.x();
        point.y = vertex.y();
        point.z = vertex.z();

        // Add mesh vertex to the point cloud
        cloud_ptr->push_back(point);
      }
    }
  }

  // If the label map csv file exists, load it and remap all the ids
  if (request_.is_single_tsdf) {
    // If the label map csv file exists, load it
    const std::string label_map_file_path =
        target_directory_ + "/" + target_map_name_ + ".csv";
    if (std::filesystem::is_regular_file(label_map_file_path)) {
      std::unordered_map<int, int> label_map;
      io::CSVReader<2> in(label_map_file_path);
      in.read_header(io::ignore_extra_column, "InstanceID", "ClassID");
      bool read_row = true;
      while (read_row) {
        int inst = -1, cls = -1;
        read_row = in.read_row(inst, cls);
        if (inst != -1 && cls != -1) {
          label_map[inst] = cls;
        }
      }

      // Now remap the points in the cloud
      for (size_t i = 0; i < cloud_ptr->size(); ++i) {
        auto& point = (*cloud_ptr)[i];
        auto instance_class_id_pair_it =
            label_map.find(static_cast<int>(point.label));
        if (instance_class_id_pair_it != label_map.end()) {
          point.label = instance_class_id_pair_it->second * 1000 +
                        instance_class_id_pair_it->first;
        } else {
          point.label *= 1000;
        }
      }
    }
  }

  // Now save the point cloud as PLY
  const std::string out_pcl_file_name =
      target_directory_ + "/" + target_map_name_ + ".pointcloud.ply";
  pcl::PLYWriter().write(out_pcl_file_name, *cloud_ptr, true /* binary */,
                         false /* use_camers */);
}

void MapEvaluator::exportCoveragePointcloud(const EvaluationRequest& request) {
  if (!gt_cloud_ptr_) {
    gt_cloud_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr(
        new pcl::PointCloud<pcl::PointXYZ>());
    if (pcl::io::loadPLYFile<pcl::PointXYZ>(
            request_.ground_truth_pointcloud_file, *gt_cloud_ptr_) == -1) {
      gt_cloud_ptr_.reset();
      LOG(FATAL) << "Could not load ground truth point cloud from '"
                 << request_.ground_truth_pointcloud_file << "'.";
    }
  }

  if (!gt_voxel_grid_ptr_) {
    gt_voxel_grid_ptr_ =
        pcl::VoxelGrid<pcl::PointXYZ>::Ptr(new pcl::VoxelGrid<pcl::PointXYZ>());
    // Save leaf layout for later access
    gt_voxel_grid_ptr_->setSaveLeafLayout(true);
    // Voxelize the gt poincloud to compute the coverage mask later
    filtered_gt_cloud_ptr_ = pcl::PointCloud<pcl::PointXYZ>::Ptr(
        new pcl::PointCloud<pcl::PointXYZ>());
    gt_voxel_grid_ptr_->setInputCloud(
        boost::const_pointer_cast<const pcl::PointCloud<pcl::PointXYZ>>(
            gt_cloud_ptr_));
    gt_voxel_grid_ptr_->setLeafSize(
        kCoverageGridVoxelSize, kCoverageGridVoxelSize, kCoverageGridVoxelSize);
    gt_voxel_grid_ptr_->filter(*filtered_gt_cloud_ptr_);
  }

  auto planning_interface_ptr = std::make_unique<PlanningInterface>(submaps_);

  // Iterate over the gt voxel grid and add only voxel centroids that have
  // been observed in the current submap collection to the coverage pcd
  pcl::PointCloud<pcl::PointXYZ>::Ptr coverage_cloud_ptr(
      new pcl::PointCloud<pcl::PointXYZ>());
  const auto min_box_coordinates = gt_voxel_grid_ptr_->getMinBoxCoordinates();
  const auto max_box_coordinates = gt_voxel_grid_ptr_->getMaxBoxCoordinates();
  for (int i = min_box_coordinates.x(); i <= max_box_coordinates.x(); ++i) {
    for (int j = min_box_coordinates.y(); j <= max_box_coordinates.y(); ++j) {
      for (int k = min_box_coordinates.z(); k <= max_box_coordinates.z(); ++k) {
        int centroid_index = gt_voxel_grid_ptr_->getCentroidIndexAt({i, j, k});
        if (centroid_index == -1) {
          auto empty_voxel_centroid =
              get_voxel_centroid({i, j, k}, kCoverageGridVoxelSize);
          if (planning_interface_ptr->isObserved({empty_voxel_centroid.x,
                                                  empty_voxel_centroid.y,
                                                  empty_voxel_centroid.z})) {
            coverage_cloud_ptr->push_back(empty_voxel_centroid);
          }
        } else {
          auto voxel_centroid = filtered_gt_cloud_ptr_->at(centroid_index);
          if (planning_interface_ptr->isObserved(
                  {voxel_centroid.x, voxel_centroid.y, voxel_centroid.z})) {
            coverage_cloud_ptr->push_back(voxel_centroid);
          }
        }
      }
    }
  }
  std::string coverage_pcl_file_name =
      target_directory_ + "/" + target_map_name_ + ".coverage.ply";
  pcl::PLYWriter().write(coverage_pcl_file_name, *coverage_cloud_ptr,
                         true /* binary_mode */, false /* use_camera */);
}

}  // namespace panoptic_mapping
