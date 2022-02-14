#ifndef PANOPTIC_MAPPING_UTILS_EVALUATION_MAP_EVALUATOR_H_
#define PANOPTIC_MAPPING_UTILS_EVALUATION_MAP_EVALUATOR_H_

#include <memory>
#include <string>
#include <vector>

#include <panoptic_mapping/3rd_party/config_utilities.hpp>
#include <panoptic_mapping/3rd_party/nanoflann.hpp>
#include <panoptic_mapping/common/common.h>
#include <panoptic_mapping/map/submap_collection.h>
#include <panoptic_mapping/tools/planning_interface.h>
#include <panoptic_mapping_msgs/SaveLoadMap.h>
#include <panoptic_mapping_ros/visualization/submap_visualizer.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <ros/ros.h>
#include <voxblox_ros/tsdf_server.h>

namespace panoptic_mapping {

/**
 * @brief Evaluation tools in a ROS node. Initially based on the
 * voxblox_ros/voxblox_eval.cc code.
 */
class MapEvaluator {
 public:
  struct EvaluationRequest
      : public config_utilities::Config<EvaluationRequest> {
    int verbosity = 4;

    // Data handling.
    std::string map_file;
    std::string ground_truth_pointcloud_file = "/home/giuliano/mt_ipp_panoptic_mapping/datasets/scannetv2-dvc/scans/scene0587_02/scene0587_02_vh_clean_2.pointcloud.ply";
    std::string output_suffix = "evaluation_data";

    // Evaluation
    float maximum_distance = 0.2;  // m
    float inlier_distance = 0.1;   // m
    bool visualize = true;
    bool evaluate = true;
    bool compute_coloring = false;  // Use map_file to load and display.
    bool ignore_truncated_points = false;
    bool color_by_max_error = false;  // false: color by average error
    bool color_by_mesh_distance =
        true;  // true: iterate through mesh, false: iterate over gt points.
    bool is_single_tsdf = false;

    bool export_mesh = false;
    bool export_labeled_pointcloud = false;
    bool export_coverage_pointcloud = false;

    EvaluationRequest() { setConfigName("MapEvaluator::EvaluationRequest"); }

   protected:
    void setupParamsAndPrinting() override;
    void checkParams() const override;
  };

  // nanoflann pointcloud adapter.
  struct TreeData {
    std::vector<Point> points;

    inline std::size_t kdtree_get_point_count() const { return points.size(); }

    inline float kdtree_get_pt(const size_t idx, const size_t dim) const {
      if (dim == 0)
        return points[idx].x();
      else if (dim == 1)
        return points[idx].y();
      else
        return points[idx].z();
    }

    template <class BBOX>
    bool kdtree_get_bbox(BBOX& /* bb */) const {
      return false;
    }
  };
  typedef nanoflann::KDTreeSingleIndexAdaptor<
      nanoflann::L2_Simple_Adaptor<float, TreeData>, TreeData, 3>
      KDTree;

  // Constructor.
  MapEvaluator(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private);
  virtual ~MapEvaluator() = default;

  // Access.
  bool evaluate(const EvaluationRequest& request);
  void publishVisualization();
  bool setupMultiMapEvaluation();

  // Services.
  bool evaluateMapCallback(
      panoptic_mapping_msgs::SaveLoadMap::Request& request,     // NOLINT
      panoptic_mapping_msgs::SaveLoadMap::Response& response);  // NOLINT

 private:
  std::string computeReconstructionError(const EvaluationRequest& request);
  std::string computeMeshError(const EvaluationRequest& request);
  void visualizeReconstructionError(const EvaluationRequest& request);
  void buildKdTree();
  void exportMesh(const EvaluationRequest& request);
  void exportLabeledPointcloud(const EvaluationRequest& request);
  void exportCoveragePointcloud(const EvaluationRequest& request);

 private:
  // ROS.
  ros::NodeHandle nh_;
  ros::NodeHandle nh_private_;

  // Files.
  std::ofstream output_file_;

  // Stored data.
  static constexpr float kCoverageGridVoxelSize = 0.05f;
  pcl::PointCloud<pcl::PointXYZ>::Ptr gt_cloud_ptr_;
  pcl::VoxelGrid<pcl::PointXYZ>::Ptr gt_voxel_grid_ptr_;
  pcl::PointCloud<pcl::PointXYZ>::Ptr filtered_gt_cloud_ptr_;
  std::shared_ptr<SubmapCollection> submaps_;
  std::shared_ptr<TsdfLayer> voxblox_;
  bool use_voxblox_;
  std::string target_directory_;
  std::string target_map_name_;
  std::unique_ptr<PlanningInterface> planning_;
  std::unique_ptr<SubmapVisualizer> visualizer_;
  TreeData kdtree_data_;
  std::unique_ptr<KDTree> kdtree_;

  // Multi Map Evaluations.
  ros::ServiceServer process_map_srv_;
  EvaluationRequest request_;
};

}  // namespace panoptic_mapping

#endif  // PANOPTIC_MAPPING_UTILS_EVALUATION_MAP_EVALUATOR_H_
