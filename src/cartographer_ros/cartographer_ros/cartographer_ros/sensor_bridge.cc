/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer_ros/sensor_bridge.h"

#include "cartographer/common/make_unique.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros/time_conversion.h"


namespace cartographer_ros {

namespace carto = ::cartographer;

using carto::transform::Rigid3d;

namespace {


std::string CheckNoLeadingSlash(const std::string& frame_id) {
  std::string frame_id_out = frame_id;
  if (frame_id.size() > 0) {
    if(frame_id[0] == '/'){
      // LOG(WARNING)<< "The frame_id " << frame_id
      //             << " should not start with a /. See 1.7 in "
      //                 "http://wiki.ros.org/tf2/Migration.";
      if(frame_id.size() > 1){
        frame_id_out = frame_id.substr(1);
      }else{
        LOG(ERROR)<< "The frame_id " << frame_id
                  << " should not start with a /. See 1.7 in "
                      "http://wiki.ros.org/tf2/Migration.";
      }
    } 
  }
  return frame_id_out;
}

}  // namespace

SensorBridge::SensorBridge(
    const int num_subdivisions_per_laser_scan,
    const std::string& tracking_frame,
    const double lookup_transform_timeout_sec, tf2_ros::Buffer* const tf_buffer,
    carto::mapping::TrajectoryBuilderInterface* const trajectory_builder)
    : num_subdivisions_per_laser_scan_(num_subdivisions_per_laser_scan),
      tf_bridge_(tracking_frame, lookup_transform_timeout_sec, tf_buffer),
      trajectory_builder_(trajectory_builder) {}

std::unique_ptr<carto::sensor::OdometryData> SensorBridge::ToOdometryData(
    const nav_msgs::Odometry::ConstPtr& msg) {
  const carto::common::Time time = FromRos(msg->header.stamp);
  const auto sensor_to_tracking = tf_bridge_.LookupToTracking(
      time, CheckNoLeadingSlash(msg->child_frame_id));
  if (sensor_to_tracking == nullptr) {
    return nullptr;
  }
  return carto::common::make_unique<carto::sensor::OdometryData>(
      carto::sensor::OdometryData{
          time, ToRigid3d(msg->pose.pose) * sensor_to_tracking->inverse()});
}

void SensorBridge::HandleOdometryMessage(
    const std::string& sensor_id, const nav_msgs::Odometry::ConstPtr& msg) {
  std::unique_ptr<carto::sensor::OdometryData> odometry_data =
      ToOdometryData(msg);
  if (odometry_data != nullptr) {
    trajectory_builder_->AddSensorData(
        sensor_id,
        carto::sensor::OdometryData{odometry_data->time, odometry_data->pose});
  }
}

void SensorBridge::HandleNavSatFixMessage(
    const std::string& sensor_id, const sensor_msgs::NavSatFix::ConstPtr& msg) {
  const carto::common::Time time = FromRos(msg->header.stamp);
  if (msg->status.status == sensor_msgs::NavSatStatus::STATUS_NO_FIX) {
    trajectory_builder_->AddSensorData(
        sensor_id, carto::sensor::FixedFramePoseData{
                       time, carto::common::optional<Rigid3d>()});
    return;
  }

  if (!ecef_to_local_frame_.has_value()) {
    ecef_to_local_frame_ =
        ComputeLocalFrameFromLatLong(msg->latitude, msg->longitude);
    LOG(INFO) << "Using NavSatFix. Setting ecef_to_local_frame with lat = "
              << msg->latitude << ", long = " << msg->longitude << ".";
  }

  trajectory_builder_->AddSensorData(
      sensor_id,
      carto::sensor::FixedFramePoseData{
          time, carto::common::optional<Rigid3d>(Rigid3d::Translation(
                    ecef_to_local_frame_.value() *
                    LatLongAltToEcef(msg->latitude, msg->longitude,
                                     msg->altitude)))});
}

void SensorBridge::HandleLandmarkMessage(
    const std::string& sensor_id,
    const cartographer_ros_msgs::LandmarkList::ConstPtr& msg) {
  trajectory_builder_->AddSensorData(sensor_id, ToLandmarkData(*msg));
}

std::unique_ptr<carto::sensor::ImuData> SensorBridge::ToImuData(
    const sensor_msgs::Imu::ConstPtr& msg) {
  CHECK_NE(msg->linear_acceleration_covariance[0], -1)
      << "Your IMU data claims to not contain linear acceleration measurements "
         "by setting linear_acceleration_covariance[0] to -1. Cartographer "
         "requires this data to work. See "
         "http://docs.ros.org/api/sensor_msgs/html/msg/Imu.html.";
  CHECK_NE(msg->angular_velocity_covariance[0], -1)
      << "Your IMU data claims to not contain angular velocity measurements "
         "by setting angular_velocity_covariance[0] to -1. Cartographer "
         "requires this data to work. See "
         "http://docs.ros.org/api/sensor_msgs/html/msg/Imu.html.";
  const carto::common::Time time = FromRos(msg->header.stamp);
  const auto sensor_to_tracking = tf_bridge_.LookupToTracking(
      time, CheckNoLeadingSlash(msg->header.frame_id));
  if (sensor_to_tracking == nullptr) {
    return nullptr;
  }
  CHECK(sensor_to_tracking->translation().norm() < 1e-5)
      << "The IMU frame must be colocated with the tracking frame. "
         "Transforming linear acceleration into the tracking frame will "
         "otherwise be imprecise.";
  return carto::common::make_unique<carto::sensor::ImuData>(
      carto::sensor::ImuData{
          time,
          sensor_to_tracking->rotation() * ToEigen(msg->linear_acceleration),
          sensor_to_tracking->rotation() * ToEigen(msg->angular_velocity)});
}

void SensorBridge::HandleImuMessage(const std::string& sensor_id,
                                    const sensor_msgs::Imu::ConstPtr& msg) {
  std::unique_ptr<carto::sensor::ImuData> imu_data = ToImuData(msg);
  if (imu_data != nullptr) {
    trajectory_builder_->AddSensorData(
        sensor_id,
        carto::sensor::ImuData{imu_data->time, imu_data->linear_acceleration,
                               imu_data->angular_velocity});
  }
}

void SensorBridge::HandleLaserScanMessage(
    const std::string& sensor_id, const sensor_msgs::LaserScan::ConstPtr& msg) {
  carto::sensor::PointCloudWithIntensities point_cloud;
  carto::common::Time time;
  std::tie(point_cloud, time) = ToPointCloudWithIntensities(*msg);
  HandleLaserScan(sensor_id, time, msg->header.frame_id, point_cloud);
}

void SensorBridge::HandleMultiEchoLaserScanMessage(
    const std::string& sensor_id,
    const sensor_msgs::MultiEchoLaserScan::ConstPtr& msg) {
  carto::sensor::PointCloudWithIntensities point_cloud;
  carto::common::Time time;
  std::tie(point_cloud, time) = ToPointCloudWithIntensities(*msg);
  HandleLaserScan(sensor_id, time, msg->header.frame_id, point_cloud);
}

void SensorBridge::HandlePointCloud2Message(
    const std::string& sensor_id,
    const sensor_msgs::PointCloud2::ConstPtr& msg,
    const std::string& sensor_type) {
  carto::sensor::TimedPointCloud point_cloud;
  double rel_time_last = 0.;
  carto::common::Time point_cloud_stamp;
  if(sensor_type == "ouster"){
    pcl::PointCloud<OusterPointXYZIRT> pcl_point_cloud;
    pcl::fromROSMsg(*msg, pcl_point_cloud);
   
    rel_time_last = pcl_point_cloud.points.back().t * 1e-9f;
    for (const auto& point : pcl_point_cloud.points) {
      if(isnan(point) || isinf(point)) continue;
      point_cloud.emplace_back(
        point.x, point.y, point.z, point.t * 1e-9f - rel_time_last);
    }
    point_cloud_stamp = FromRos(msg->header.stamp) + 
      carto::common::FromSeconds(rel_time_last);
  }else if(sensor_type == "velodyne"){
    pcl::PointCloud<PointXYZIRT> pcl_point_cloud;
    pcl::fromROSMsg(*msg, pcl_point_cloud);
    
    //注意：Velodyne ROS消息的stamp记录的是第一个点的采集时间，
    //而carto里面的TimedPointCloud中每一个元素的最后一维记录的是相对最后一个点的采集时间
    rel_time_last = pcl_point_cloud.points.back().time;
    for (const auto& point : pcl_point_cloud.points) {
      if(isnan(point) || isinf(point)) continue;
      point_cloud.emplace_back(
        point.x, point.y, point.z, point.time - rel_time_last);
    }
    point_cloud_stamp = FromRos(msg->header.stamp) + 
      carto::common::FromSeconds(rel_time_last);
  }else if(sensor_type == "robosense"){
    pcl::PointCloud<RsPointXYZIRT> pcl_point_cloud;
    pcl::fromROSMsg(*msg, pcl_point_cloud);
    double st = msg->header.stamp.toSec();
    rel_time_last = pcl_point_cloud.points.back().timestamp;
    // double rel_time_first = pcl_point_cloud.points.front().timestamp;
    // LOG(INFO) << "Stamp - end: "<< st - rel_time_last;
    // LOG(INFO) << "Stamp - start:"<< st - rel_time_first;
    
    //carto里面的TimedPointCloud中每一个元素的最后一维记录的是相对最后一个点的采集时间
    for (const auto& point : pcl_point_cloud.points) {
      if(isnan(point) || isinf(point)) continue;
      point_cloud.emplace_back(
        point.x, point.y, point.z, point.timestamp - rel_time_last);
    }
    //注意：Robosense ROS消息的stamp记录的是最后点的采集时间...
    point_cloud_stamp = FromRos(msg->header.stamp);
  }else{
    pcl::PointCloud<pcl::PointXYZI> pcl_point_cloud;
    pcl::fromROSMsg(*msg, pcl_point_cloud);
        
    for (const auto& point : pcl_point_cloud.points) {
      if(isnan(point) || isinf(point)) continue;
      point_cloud.emplace_back(point.x, point.y, point.z, 0.f);
    }
    point_cloud_stamp = FromRos(msg->header.stamp);
  }
  
  //wz: 这里的时间改为最后一个点的时间戳
  HandleRangefinder(
    sensor_id, point_cloud_stamp, msg->header.frame_id, point_cloud);
}

const TfBridge& SensorBridge::tf_bridge() const { return tf_bridge_; }

void SensorBridge::HandleLaserScan(
    const std::string& sensor_id, const carto::common::Time time,
    const std::string& frame_id,
    const carto::sensor::PointCloudWithIntensities& points) {
  if (points.points.empty()) {
    return;
  }
  CHECK_LE(points.points.back()[3], 0);
  // TODO(gaschler): Use per-point time instead of subdivisions.
  for (int i = 0; i != num_subdivisions_per_laser_scan_; ++i) {
    const size_t start_index =
        points.points.size() * i / num_subdivisions_per_laser_scan_;
    const size_t end_index =
        points.points.size() * (i + 1) / num_subdivisions_per_laser_scan_;
    carto::sensor::TimedPointCloud subdivision(
        points.points.begin() + start_index, points.points.begin() + end_index);
    if (start_index == end_index) {
      continue;
    }
    const double time_to_subdivision_end = subdivision.back()[3];
    // `subdivision_time` is the end of the measurement so sensor::Collator will
    // send all other sensor data first.
    const carto::common::Time subdivision_time =
        time + carto::common::FromSeconds(time_to_subdivision_end);
    auto it = sensor_to_previous_subdivision_time_.find(sensor_id);
    if (it != sensor_to_previous_subdivision_time_.end() &&
        it->second >= subdivision_time) {
      LOG(WARNING) << "Ignored subdivision of a LaserScan message from sensor "
                   << sensor_id << " because previous subdivision time "
                   << it->second << " is not before current subdivision time "
                   << subdivision_time;
      continue;
    }
    sensor_to_previous_subdivision_time_[sensor_id] = subdivision_time;
    for (Eigen::Vector4f& point : subdivision) {
      point[3] -= time_to_subdivision_end;
    }
    CHECK_EQ(subdivision.back()[3], 0);
    HandleRangefinder(sensor_id, subdivision_time, frame_id, subdivision);
  }
}
//在这里把传感器数据统一到了一个坐标系下了
void SensorBridge::HandleRangefinder(
    const std::string& sensor_id, const carto::common::Time time,
    const std::string& frame_id, const carto::sensor::TimedPointCloud& ranges) {
  const auto sensor_to_tracking =
      tf_bridge_.LookupToTracking(time, CheckNoLeadingSlash(frame_id));
  // LOG(INFO)<<sensor_to_tracking->inverse();
  if (sensor_to_tracking != nullptr) {
    trajectory_builder_->AddSensorData(
        sensor_id, carto::sensor::TimedPointCloudData{
                       time, sensor_to_tracking->translation().cast<float>(),
                       carto::sensor::TransformTimedPointCloud(
                           ranges, sensor_to_tracking->cast<float>())});
  }
}

}  // namespace cartographer_ros
