#include "geometry_msgs/PoseStamped.h"
#include "nav_msgs/Odometry.h"
#include "sensor_msgs/Imu.h"
#include "ndtpso_slam/ndtcell.h"
#include "ndtpso_slam/ndtframe.h"
#include "ros/ros.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <eigen3/Eigen/Core>
#include <iostream>
#include <laser_geometry/laser_geometry.h>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/time_synchronizer.h>
#include <mutex>
#include <string>
#include <tf/transform_listener.h>



#define NDTPSO_SLAM_VERSION "1.5.0"

#define SAVE_MAP_DATA_TO_FILE false
#define SAVE_DATA_TO_FILE_EACH_NUM_ITERS 10000
#define SAVE_MAP_IMAGES false
#define SYNC_WITH_LASER_TOPIC false
#define SYNC_WITH_ODOM false
#define WAIT_FOR_TF true
#define DEFAULT_CELL_SIZE_M .5
#define DEFAULT_FRAME_SIZE_M 100
#define DEFAULT_SCAN_TOPIC "/scan"
#define DEFAULT_ODOM_TOPIC "/odom"
#define DEFAULT_LIDAR_FRAME "laser"
#define DEFAULT_OUTPUT_MAP_SIZE_M 25
#define DEFAULT_RATE_HZ 20
#define BUILD_OCCUPANCY_GRID false

#if BUILD_OCCUPANCY_GRID
#define DEFAULT_OCCUPANCY_GRID_CELL_SIZE_M .1
#endif
/**
 * To represent the odometry and the published pose in the same reference frame,
 * the frame_id should be the same reference frame as odometry (in general
 * "odom" for "/odom")
 */
#define DEFAULT_PUBLISHED_POSE_FRAME_ID "map"

using namespace Eigen;
using std::cout;
using std::endl;
static std::mutex matcher_mutex;
static std::chrono::time_point<std::chrono::high_resolution_clock> start_time,
    last_call_time;

static int param_frame_size;
static double param_cell_side;

static bool first_iteration{true};
static unsigned int number_of_iters{0};
static Vector3d previous_pose{Vector3d::Zero()},
    trans_estimate{Vector3d::Zero()}, initial_pose{Vector3d::Zero()},
    current_pose{Vector3d::Zero()};

static NDTFrame *current_frame;
static NDTFrame *ref_frame;

#if SAVE_MAP_DATA_TO_FILE
static NDTFrame *global_map;
#endif

static ros::Publisher pose_pub;
static geometry_msgs::PoseStamped current_pub_pose;

// void imu_callback(const sensor_msgs::ImuConstPtr &imu_data){

//  tf::Quaternion q_imu(imu_data->orientation.x,imu_data->orientation.y,imu_data->orientation.z,imu_data->orientation.w);
//     tf::Matrix3x3 m_imu(q_imu);            
//     double roll_imu, pitch_imu, yaw_imu;
//     m_imu.getRPY(roll_imu, pitch_imu, yaw_imu);  
// }
  
// The odometry is used just for the initial pose to be easily compared with our
// calculated pose
void scan_mathcher(const sensor_msgs::LaserScanConstPtr &scan
#if SYNC_WITH_LASER_TOPIC
                   ,
                   const sensor_msgs::LaserScanConstPtr &scan2
#if __GNUC__ // __attribute__((unused)) is a GCC feature
                   __attribute__((unused)) // scan2 is unused, it
                                           // is present just for
                                           // synchronization at
                                           // calling time
#endif
#endif

#if SYNC_WITH_ODOM
                   ,
                   const sensor_msgs::ImuConstPtr &odom
#endif
) {

#if SAVE_MAP_DATA_TO_FILE
  static unsigned int iter_num = 0;
#endif
  matcher_mutex.lock();
  auto start = std::chrono::high_resolution_clock::now();
  last_call_time = start;
double odom_roll, odom_pitch, odom_orientation; 
odom_roll = 0.0;
odom_pitch = 0.0;
odom_orientation = 0.0;
#if SYNC_WITH_ODOM
  
  tf::Matrix3x3(tf::Quaternion(odom->orientation.x,
                               odom->orientation.y,
                               odom->orientation.z,
                               odom->orientation.w))
      .getRPY(odom_roll, odom_pitch, odom_orientation);
  
  sensor_msgs::LaserScan filtered_scan;
  filtered_scan.ranges = scan->ranges;

  for(int i=0; i< scan->ranges.size();i++){
    filtered_scan.ranges[i] = scan->ranges[i]*cos(odom_roll)*cos(odom_pitch);    
  }
#endif
#if SYNC_WITH_ODOM
  current_frame->loadLaser(filtered_scan.ranges, scan->angle_min, scan->angle_increment,
                           scan->range_max);
#else
current_frame->loadLaser(scan->ranges, scan->angle_min, scan->angle_increment,
                           scan->range_max);                         
#endif



  if (first_iteration) {

    current_pose = previous_pose;
    start_time = std::chrono::high_resolution_clock::now();
    ROS_INFO("Min/Max ranges: %.2f/%.2f", static_cast<double>(scan->range_min),
             static_cast<double>(scan->range_max));
    ROS_INFO("Min/Max angles: %.2f/%.2f", static_cast<double>(scan->angle_min),
             static_cast<double>(scan->angle_max));
  } else {
    current_pose = ref_frame->align(previous_pose, current_frame);
  }

  previous_pose = current_pose;
  ref_frame->update(current_pose, current_frame);

#if SAVE_MAP_DATA_TO_FILE
  if (iter_num == 0)
    global_map->update(current_pose, current_frame);
  iter_num = (iter_num + 1) % SAVE_DATA_TO_FILE_EACH_NUM_ITERS;
  global_map->addPose(scan->header.stamp.toSec(), current_pose

  );
#endif

  // Publish 'pose', using the same timestamp of the laserscan (or use
  // ros::Time::now() !!)
  current_pub_pose.header.stamp = scan->header.stamp;
  current_pub_pose.header.frame_id =
      DEFAULT_PUBLISHED_POSE_FRAME_ID; // we can read it from config
  current_pub_pose.pose.position.x = current_pose.x();
  current_pub_pose.pose.position.y = current_pose.y();
  current_pub_pose.pose.position.z = 0.;
  tf::Quaternion q_ori;
  q_ori.setRPY(0, 0, current_pose.z());
  current_pub_pose.pose.orientation.x = q_ori.getX();
  current_pub_pose.pose.orientation.y = q_ori.getY();
  current_pub_pose.pose.orientation.z = q_ori.getZ();
  current_pub_pose.pose.orientation.w = q_ori.getW();

  // Reallocate the current_frame object, this is much faster than calling
  // current_frame->resetCells()
  delete current_frame;
  current_frame = new NDTFrame(
      initial_pose, static_cast<unsigned short>(param_frame_size),
      static_cast<unsigned short>(param_frame_size), param_frame_size, false);

  auto finish = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = finish - start;

  ++number_of_iters;
  std::chrono::duration<double> current_rate = last_call_time - start_time;

  if (!first_iteration) {
    ROS_INFO("Average publish rate: %.2fHz, matching rate: %.2fHz",
             1. / (current_rate.count() / number_of_iters),
             1. / elapsed.count());
  }

  first_iteration = false;
  matcher_mutex.unlock();
}

int main(int argc, char **argv)

{
  ros::init(argc, argv, "ndtpso_slam");
  ros::NodeHandle nh("~");

  // Print some useful information
  ROS_INFO("NDTPSO Scan Matcher v%s", NDTPSO_SLAM_VERSION);
#pragma GCC diagnostic push
  /* Disable the warning which says that date and time aren't reproducible
   * => normal; it is the expected behaviour
   */
#pragma GCC diagnostic ignored "-Wdate-time"
  ROS_INFO("Compiled on %s at %s", __DATE__, __TIME__);
#pragma GCC diagnostic pop

#if __GNUC__
  ROS_INFO("Compiled with GCC %d.%d.%d", __GNUC__, __GNUC_MINOR__,
           __GNUC_PATCHLEVEL__);
#elif __clang__
  ROS_INFO("Compiled with Clang %s", __clang_version__);
#endif

  NDTPSOConfig ndtpso_conf; // Initally, the object helds the default values

  // Read parameters
  std::string param_scan_topic, param_lidar_frame;

  int param_map_size, param_rate;
  double x_offset_, y_offset_, z_offset_, roll_offset_, pitch_offset_, yaw_offset_;


  nh.param<std::string>("scan_topic", param_scan_topic, DEFAULT_SCAN_TOPIC);
#if SYNC_WITH_ODOM
  std::string param_odom_topic;
  nh.param<std::string>("odom_topic", param_odom_topic, DEFAULT_ODOM_TOPIC);
#endif
  nh.param<std::string>("scan_frame", param_lidar_frame, DEFAULT_LIDAR_FRAME);
  nh.param("map_size", param_map_size, DEFAULT_OUTPUT_MAP_SIZE_M);
  nh.param("num_threads", ndtpso_conf.psoConfig.num_threads, -1);
  nh.param("iterations", ndtpso_conf.psoConfig.iterations, PSO_ITERATIONS);
  nh.param("population", ndtpso_conf.psoConfig.populationSize,
           PSO_POPULATION_SIZE);
  nh.param("rate", param_rate, DEFAULT_RATE_HZ);
  nh.param("cell_side", param_cell_side, DEFAULT_CELL_SIZE_M);
  nh.param<int>("frame_size", param_frame_size, DEFAULT_FRAME_SIZE_M);
  nh.param<double>("x_offset", x_offset_, 0.0);
  nh.param<double>("y_offset", y_offset_, 0.0);
  nh.param<double>("z_offset", z_offset_, 0.0);
  nh.param<double>("roll_offset", roll_offset_, 0.0);
  nh.param<double>("pitch_offset", pitch_offset_, 0.0);
  nh.param<double>("yaw_offset", yaw_offset_, 0.0);
  
#if BUILD_OCCUPANCY_GRID
  double param_occupancy_grid_cell_side;
  nh.param("og_cell_side", param_occupancy_grid_cell_side,
           DEFAULT_OCCUPANCY_GRID_CELL_SIZE_M);
#endif
#if SYNC_WITH_LASER_TOPIC
  std::string param_sync_topic;
  nh.param<std::string>("sync_topic", param_sync_topic, param_scan_topic);
  ROS_INFO("sync_topic:= \"%s\"", param_sync_topic.c_str());
#endif

  // Print patameters
  ROS_INFO("scan_topic:= \"%s\"", param_scan_topic.c_str());

#if SYNC_WITH_ODOM
  ROS_INFO("odom_topic:= \"%s\"", param_odom_topic.c_str());
#endif

#if WAIT_FOR_TF
  ROS_INFO("scan_frame:= \"%s\"", param_lidar_frame.c_str());
#endif

  ROS_INFO("rate:= %dHz", param_rate);

  ROS_INFO("Config [PSO Number of Iterations: %d]",
           ndtpso_conf.psoConfig.iterations);
  ROS_INFO("Config [PSO Population Size: %d]",
           ndtpso_conf.psoConfig.populationSize);
  ROS_INFO("Config [PSO Threads: %d]", ndtpso_conf.psoConfig.num_threads);
  ROS_INFO("Config [NDT Cell Size: %.2fm]", param_cell_side);
  ROS_INFO("Config [NDT Frame Size: %dx%dm]", param_frame_size,
           param_frame_size);
  ROS_INFO("Config [NDT Window Size: %d]", NDT_WINDOW_SIZE);
  ROS_INFO("Config [Max Map Size: %dx%dm]", param_map_size, param_map_size);
#if BUILD_OCCUPANCY_GRID
  ROS_INFO("Config [Occupancy Grid Cell Size: %.2fm]",
           param_occupancy_grid_cell_side);
#endif

  // The reference frame which will be used for all the matching operations,
  // It is the only frame which needs to be set to the correct cell and
  // occupancy grid sizes
  ref_frame = new NDTFrame(Vector3d::Zero(),
                           static_cast<unsigned short>(param_frame_size),
                           static_cast<unsigned short>(param_frame_size),
                           param_cell_side, true, ndtpso_conf
#if BUILD_OCCUPANCY_GRID
                           ,
                           param_occupancy_grid_cell_side
#endif
  );

#if SAVE_MAP_DATA_TO_FILE
  global_map = new NDTFrame(
      Vector3d::Zero(), static_cast<unsigned short>(param_map_size),
      static_cast<unsigned short>(param_map_size), param_map_size, false);
#endif

  current_frame = new NDTFrame(
      initial_pose, static_cast<unsigned short>(param_frame_size),
      static_cast<unsigned short>(param_frame_size), param_cell_side, false);

  pose_pub = nh.advertise<geometry_msgs::PoseStamped>("pose", 1);

#if WAIT_FOR_TF
  tf::TransformListener tf_listener(ros::Duration(1));
  tf::StampedTransform transform;
  ROS_INFO("Waiting for tf \"base_link\" -> \"%s\"", param_lidar_frame.c_str());
  while (!tf_listener.waitForTransform("base_link", param_lidar_frame,
                                       ros::Time(0), ros::Duration(2)))
    ;
  ROS_INFO("Got a tf \"base_link\" -> \"%s\"", param_lidar_frame.c_str());
  tf_listener.lookupTransform("base_link", param_lidar_frame, ros::Time(0),
                              transform);

  auto init_trans = transform.getOrigin();
  auto initial_trans = Vector3d(init_trans.getX(), init_trans.getY(),
                                tf::getYaw(transform.getRotation()));

  ROS_INFO("tf = (%.5f, %.5f, %.5f)", initial_trans.x(), initial_trans.y(),
           initial_trans.z());

  current_frame->setTrans(initial_trans);
#endif

  previous_pose = current_pose = initial_pose = Vector3d::Zero();

  ROS_INFO("Starting from initial pose (%.5f, %.5f, %.5f)", initial_pose.x(),
           initial_pose.y(), initial_pose.z());

#if SYNC_WITH_ODOM || SYNC_WITH_LASER_TOPIC
  typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::LaserScan
#if SYNC_WITH_LASER_TOPIC
                                                          ,
                                                          sensor_msgs::LaserScan
#endif
#if SYNC_WITH_ODOM
                                                          ,
                                                          sensor_msgs::Imu
#endif
                                                          >
      ApproxSyncPolicy;

  message_filters::Subscriber<sensor_msgs::LaserScan> laser_sub(
      nh, param_scan_topic, 10);
#if SYNC_WITH_LASER_TOPIC
  message_filters::Subscriber<sensor_msgs::LaserScan> laser_sub_sync(
      nh, param_sync_topic, 10);
#endif
#if SYNC_WITH_ODOM
  // message_filters::Subscriber<nav_msgs::Odometry> odom_sub(nh, param_odom_topic,
  //                                                          10);
  message_filters::Subscriber<sensor_msgs::Imu> odom_sub(nh, param_odom_topic,
                                                           10);
#endif

  message_filters::Synchronizer<ApproxSyncPolicy> sync(ApproxSyncPolicy(10),
                                                       laser_sub
#if SYNC_WITH_LASER_TOPIC
                                                       ,
                                                       laser_sub_sync
#endif
#if SYNC_WITH_ODOM
                                                       ,
                                                       odom_sub
#endif
  );

  sync.registerCallback(boost::bind(&scan_mathcher, _1, _2
#if SYNC_WITH_ODOM && SYNC_WITH_LASER_TOPIC
                                    ,
                                    _3
#endif
                                    ));
#else
  ros::Subscriber laser_sub =
      nh.subscribe<sensor_msgs::LaserScan>(param_scan_topic, 1, &scan_mathcher);
  // ros::Subscriber imu_sub =
  //     nh.subscribe<sensor_msgs::Imu>("mavros/imu/data", 1, &imu_callback);
#endif

  ROS_INFO("NDTPSO node started successfuly");

  // Using the ros::Rate + ros::spinOnce can slows down the ApproxSyncPolicy if
  // no sufficient buffer
  ros::Rate loop_rate(param_rate);

  while (ros::ok()) {
    ros::spinOnce();
    pose_pub.publish(current_pub_pose);
    loop_rate.sleep();
  }

  char time_formated[80];
  time_t rawtime;
  time(&rawtime);

  tm *timeinfo = localtime(&rawtime);

  strftime(time_formated, sizeof(time_formated), "%Y%m%d-%H%M%S", timeinfo);

  cout << endl
       << "Exporting results "
       << "[.pose.csv, .map.csv, .png, .gnuplot]" << endl;

  size_t pos = 0;

  do {
    param_scan_topic.replace(pos, 1, "_");
    pos = param_scan_topic.find("/", pos);
  } while (pos != std::string::npos);

  char filename[512];

  sprintf(filename, "%s-%s", param_scan_topic.c_str(), time_formated);

#if SAVE_MAP_DATA_TO_FILE
  global_map->dumpMap(filename, true, true, SAVE_MAP_IMAGES, 100
#if BUILD_OCCUPANCY_GRID
                      ,
                      true
#endif
  );

  cout << "Map saved to file " << filename
       << "[.pose.csv, .map.csv, .png, .gnuplot]" << endl;
#endif

  sprintf(filename, "%s-%s-ref-frame", param_scan_topic.c_str(), time_formated);

  ref_frame->dumpMap(filename, false, true, SAVE_MAP_IMAGES, 100
#if BUILD_OCCUPANCY_GRID
                     ,
                     true
#endif
  );

  cout << "Map saved to file " << filename
       << "[.pose.csv, .map.csv, .png, .gnuplot]" << endl;
}
