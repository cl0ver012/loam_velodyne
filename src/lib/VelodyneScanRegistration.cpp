// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

#include "VelodyneScanRegistration.h"


namespace loam {

VelodyneScanRegistration::VelodyneScanRegistration(const float& scanPeriod,
                                                   const uint16_t& nScans,
                                                   const size_t& imuHistorySize,
                                                   const RegistrationParams& config)
      : ScanRegistration(scanPeriod, nScans, imuHistorySize, config),
        _systemDelay(20)
{

};



bool VelodyneScanRegistration::setup(ros::NodeHandle& node,
                                     ros::NodeHandle& privateNode)
{
  if (!ScanRegistration::setup(node, privateNode)) {
    return false;
  }

  _subLaserCloud = node.subscribe<sensor_msgs::PointCloud2>
      ("/velodyne_points", 2, &VelodyneScanRegistration::processCloudMessage, this);

  return true;
}



void VelodyneScanRegistration::processCloudMessage(const sensor_msgs::PointCloud2ConstPtr& laserCloudMsg)
{
  if (_systemDelay > 0) {
    _systemDelay--;
    return;
  }

  // fetch new input cloud
  pcl::PointCloud<pcl::PointXYZ> laserCloudIn;
  pcl::fromROSMsg(*laserCloudMsg, laserCloudIn);

  process(laserCloudIn, laserCloudMsg->header.stamp);
}



void VelodyneScanRegistration::process(const pcl::PointCloud<pcl::PointXYZ>& laserCloudIn,
                                       const ros::Time& scanTime)
{
  size_t cloudSize = laserCloudIn.points.size();

  // reset internal buffers and set IMU start state based on current scan time
  reset(scanTime);

  // determine scan start and end orientations
  float startOri = -std::atan2(laserCloudIn.points[0].y, laserCloudIn.points[0].x);
  float endOri = -std::atan2(laserCloudIn.points[cloudSize - 1].y,
                             laserCloudIn.points[cloudSize - 1].x) + 2 * float(M_PI);
  if (endOri - startOri > 3 * M_PI) {
    endOri -= 2 * M_PI;
  } else if (endOri - startOri < M_PI) {
    endOri += 2 * M_PI;
  }

  size_t imuIdx = _imuStartIdx;
  bool halfPassed = false;
  pcl::PointXYZI point;
  std::vector<pcl::PointCloud<pcl::PointXYZI> > laserCloudScans(_nScans);

  // extract valid points from input cloud
  for (int i = 0; i < cloudSize; i++) {
    point.x = laserCloudIn.points[i].y;
    point.y = laserCloudIn.points[i].z;
    point.z = laserCloudIn.points[i].x;

    // skip NaN and INF valued points
    if (!pcl_isfinite(point.x) ||
        !pcl_isfinite(point.y) ||
        !pcl_isfinite(point.z)) {
      continue;
    }

    // skip zero valued points
    if (point.x * point.x + point.y * point.y + point.z * point.z < 0.0001) {
      continue;
    }

    // calculate vertical point angle and scan ID
    float angle = std::atan(point.y / std::sqrt(point.x * point.x + point.z * point.z)) * 180 / float(M_PI);
    int roundedAngle = int(angle + (angle < 0.0 ? -0.5 : 0.5));
    int scanID = roundedAngle > 0 ? roundedAngle : roundedAngle + (_nScans - 1);
    if (scanID > (_nScans - 1) || scanID < 0 ){
      continue;
    }

    // calculate horizontal point angle
    float ori = -std::atan2(point.x, point.z);
    if (!halfPassed) {
      if (ori < startOri - M_PI / 2) {
        ori += 2 * M_PI;
      } else if (ori > startOri + M_PI * 3 / 2) {
        ori -= 2 * M_PI;
      }

      if (ori - startOri > M_PI) {
        halfPassed = true;
      }
    } else {
      ori += 2 * M_PI;

      if (ori < endOri - M_PI * 3 / 2) {
        ori += 2 * M_PI;
      } else if (ori > endOri + M_PI / 2) {
        ori -= 2 * M_PI;
      }
    }

    // calculate relative scan time based on point orientation
    float relTime = _scanPeriod * (ori - startOri) / (endOri - startOri);
    point.intensity = scanID + relTime;

    // project point to the start of the sweep using corresponding IMU data
    if (_imuHistory.size() > 0) {
      while (imuIdx < _imuHistory.size() - 1 && (scanTime - _imuHistory[imuIdx].stamp).toSec() + relTime > 0) {
        imuIdx++;
      }

      if (imuIdx == 0 || (scanTime - _imuHistory[imuIdx].stamp).toSec() + relTime > 0) {
        _imuCur = _imuHistory[imuIdx];
      } else {
        float ratio = ((_imuHistory[imuIdx].stamp - scanTime).toSec() - relTime)
                        / (_imuHistory[imuIdx].stamp - _imuHistory[imuIdx - 1].stamp).toSec();
        IMUState::interpolate(_imuHistory[imuIdx], _imuHistory[imuIdx - 1], ratio, _imuCur);
      }

      transformToStartIMU(point, relTime);
    }

    laserCloudScans[scanID].push_back(point);
  }

  // construct sorted full resolution cloud
  cloudSize = 0;
  for (int i = 0; i < _nScans; i++) {
    *_laserCloud += laserCloudScans[i];

    _scanStartIndices[i] = cloudSize;
    cloudSize += laserCloudScans[i].points.size();
    _scanEndIndices[i] = cloudSize - 1;
  }

  // extract features
  extractFeatures();

  // set final IMU transformation information
  setIMUTrans(_scanPeriod);

  // publish result
  publishResult();
}

} // end namespace loam
