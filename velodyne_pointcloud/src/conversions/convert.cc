/*
 *  Copyright (C) 2009, 2010 Austin Robot Technology, Jack O'Quin
 *  Copyright (C) 2011 Jesse Vera
 *  Copyright (C) 2012 Austin Robot Technology, Jack O'Quin
 *  License: Modified BSD Software License Agreement
 *
 *  $Id$
 */

/** @file

    This class converts raw Velodyne 3D LIDAR packets to PointCloud2.

*/

#include "velodyne_pointcloud/convert.h"

#include <velodyne_pointcloud/pointcloudXYZIR.h>
#include <velodyne_pointcloud/organized_cloudXYZIR.h>

namespace velodyne_pointcloud
{
  /** @brief Constructor. */
  Convert::Convert(ros::NodeHandle node, ros::NodeHandle private_nh):
    data_(new velodyne_rawdata::RawData()), first_rcfg_call(true)
  {

    boost::optional<velodyne_pointcloud::Calibration> calibration = data_->setup(private_nh);
    if(calibration)
    {
        ROS_DEBUG_STREAM("Calibration file loaded.");
        config_.num_lasers = static_cast<uint16_t>(calibration.get().num_lasers);
    }
    else
    {
        ROS_ERROR_STREAM("Could not load calibration file!");
    }

    // std::vector< std::string > v;
    // private_nh.getParamNames(v);
    // for(std::vector<std::string>::size_type i = 0; i != v.size(); i++) {
    // /* std::cout << v[i]; ... */
    //    ROS_INFO_STREAM(v[i]);
    // }

    // std::string s;
    // private_nh.param("frame_id", s, std::string("stupid_1"));
    // ROS_INFO_STREAM(s);
    // std::string t;
    // private_nh.param("/superframe_nodelet_manager_driver/frame_id", t, std::string("stupid_2"));
    // ROS_INFO_STREAM(t);
    // std::string r;
    // private_nh.param("frame_id", r, std::string("stupid_3"));
    // ROS_INFO_STREAM(r);

    // use private node handle to get parameters
    private_nh.param("frame_id", config_.target_frame, std::string("velodyne"));
    std::string tf_prefix = tf::getPrefixParam(private_nh);
    ROS_DEBUG_STREAM("tf_prefix: " << tf_prefix);
    config_.target_frame = tf::resolve(tf_prefix, config_.target_frame);

    config_.fixed_frame = config_.target_frame;
    // config_.target_frame = config_.fixed_frame = "velodyne";

    if(config_.organize_cloud)
    {
      container_ptr_ = boost::shared_ptr<OrganizedCloudXYZIR>(
          new OrganizedCloudXYZIR(config_.max_range, config_.min_range,
              config_.target_frame, config_.fixed_frame,
              config_.num_lasers, data_->scansPerPacket()));
    }
    else
    {
      container_ptr_ = boost::shared_ptr<PointcloudXYZIR>(
          new PointcloudXYZIR(config_.max_range, config_.min_range,
              config_.target_frame, config_.fixed_frame,
              data_->scansPerPacket()));
    }


    // advertise output point cloud (before subscribing to input data)
    output_ =
      node.advertise<sensor_msgs::PointCloud2>(config_.target_frame, 10);

    srv_ = boost::make_shared <dynamic_reconfigure::Server<velodyne_pointcloud::
      CloudNodeConfig> > (private_nh);
    dynamic_reconfigure::Server<velodyne_pointcloud::CloudNodeConfig>::
      CallbackType f;
    f = boost::bind (&Convert::callback, this, _1, _2);
    srv_->setCallback (f);

    // subscribe to VelodyneScan packets
    velodyne_scan_ =
      node.subscribe("velodyne_packets", 10,
                     &Convert::processScan, (Convert *) this,
                     ros::TransportHints().tcpNoDelay(true));

    // Diagnostics
    diagnostics_.setHardwareID("Velodyne Convert");
    // Arbitrary frequencies since we don't know which RPM is used, and are only
    // concerned about monitoring the frequency.
    diag_min_freq_ = 2.0;
    diag_max_freq_ = 20.0;
    using namespace diagnostic_updater;
    diag_topic_.reset(new TopicDiagnostic("velodyne_points", diagnostics_,
                                       FrequencyStatusParam(&diag_min_freq_,
                                                            &diag_max_freq_,
                                                            0.1, 10),
                                       TimeStampStatusParam()));
  }

  void Convert::callback(velodyne_pointcloud::CloudNodeConfig &config,
                uint32_t level)
  {
    // ROS_INFO("Reconfigure Request");
    data_->setParameters(config.min_range, config.max_range, config.view_direction,
                         config.view_width);
    config_.min_range = config.min_range;
    config_.max_range = config.max_range;

    if(first_rcfg_call || config.organize_cloud != config_.organize_cloud){
        first_rcfg_call = false;
        config_.organize_cloud = config.organize_cloud;
        if(config_.organize_cloud) // TODO only on change
        {
            // ROS_INFO_STREAM("Using the organized cloud format...");
            container_ptr_ = boost::shared_ptr<OrganizedCloudXYZIR>(
                new OrganizedCloudXYZIR(config_.max_range, config_.min_range,
                    config_.target_frame, config_.fixed_frame,
                    config_.num_lasers, data_->scansPerPacket()));
        }
        else
        {
            container_ptr_ = boost::shared_ptr<PointcloudXYZIR>(
                new PointcloudXYZIR(config_.max_range, config_.min_range,
                    config_.target_frame, config_.fixed_frame,
                    data_->scansPerPacket()));
        }
    }

    container_ptr_->configure(config_.max_range, config_.min_range, config_.fixed_frame, config_.target_frame);

  }

  /** @brief Callback for raw scan messages. */
  void Convert::processScan(const velodyne_msgs::VelodyneScan::ConstPtr &scanMsg)
  {
    if (output_.getNumSubscribers() == 0)         // no one listening?
      return;                                     // avoid much work

    boost::lock_guard<boost::mutex> guard(reconfigure_mtx_);
    // allocate a point cloud with same time and frame ID as raw data
    container_ptr_->setup(scanMsg);

    // process each packet provided by the driver
    for (size_t i = 0; i < scanMsg->packets.size(); ++i)
    {
      data_->unpack(scanMsg->packets[i], *container_ptr_);
    }

    // publish the accumulated cloud message
    diag_topic_->tick(scanMsg->header.stamp);
    diagnostics_.update();
    output_.publish(container_ptr_->finishCloud());

    messages_published_since_last_log++;
    ros::Time current_log_time = ros::Time::now();
    ros::Duration diff = current_log_time - last_log_time;
    double diff_sec = diff.toSec();
    if (diff_sec > 5) {
      std::ostringstream oss;
      oss << "[ " << config_.target_frame << "_publisher ]" << " [ fps:  " << std::fixed << std::setprecision(2) << messages_published_since_last_log/diff_sec << " ]";
      std::string sdt_str = oss.str();
      const char* log_msg = sdt_str.c_str();
      ROS_INFO("%s", log_msg);
      last_log_time = current_log_time;
      messages_published_since_last_log = 0;
    }
  }

} // namespace velodyne_pointcloud
