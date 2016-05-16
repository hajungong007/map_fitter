/*
 * MapFitter.cpp
 *
 *  Created on: Apr 04, 2016
 *      Author: Roman Käslin
 *   Institute: ETH Zurich, Autonomous Systems Lab
 *
 */

#include <map_fitter/MapFitter.h>

namespace map_fitter {

MapFitter::MapFitter(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle), isActive_(false)
{
  ROS_INFO("Map fitter node started, ready to match some grid maps.");
  readParameters();
  shiftedPublisher_ = nodeHandle_.advertise<grid_map_msgs::GridMap>(shiftedMapTopic_,1);   // publisher for shifted_map
  correlationPublisher_ = nodeHandle_.advertise<grid_map_msgs::GridMap>(correlationMapTopic_,1);    // publisher for correlation_map
  activityCheckTimer_ = nodeHandle_.createTimer(activityCheckDuration_,
                                                &MapFitter::updateSubscriptionCallback,
                                                this);
  broadcastTimer_ = nodeHandle_.createTimer(ros::Duration(0.01), &MapFitter::tfBroadcast, this);
  corrPointPublisher_ = nodeHandle_.advertise<geometry_msgs::PointStamped>("/corrPoint",1);
  SSDPointPublisher_ = nodeHandle_.advertise<geometry_msgs::PointStamped>("/SSDPoint",1);
  SADPointPublisher_ = nodeHandle_.advertise<geometry_msgs::PointStamped>("/SADPoint",1);
  MIPointPublisher_ = nodeHandle_.advertise<geometry_msgs::PointStamped>("/MIPoint",1);
  correctPointPublisher_ = nodeHandle_.advertise<geometry_msgs::PointStamped>("/correctPoint",1);
}

MapFitter::~MapFitter()
{
}

bool MapFitter::readParameters()
{
  nodeHandle_.param("map_topic", mapTopic_, std::string("/elevation_mapping_long_range/elevation_map"));
  nodeHandle_.param("reference_map_topic", referenceMapTopic_, std::string("/uav_elevation_mapping/uav_elevation_map"));
  //nodeHandle_.param("reference_map_topic", referenceMapTopic_, std::string("/elevation_mapping/elevation_map"));
  nodeHandle_.param("shifted_map_topic", shiftedMapTopic_, std::string("/elevation_mapping_long_range/shifted_map"));
  nodeHandle_.param("correlation_map_topic", correlationMapTopic_, std::string("/correlation_best_rotation/correlation_map"));

  nodeHandle_.param("angle_increment", angleIncrement_, 360);
  nodeHandle_.param("position_increment_search", searchIncrement_, 5);
  nodeHandle_.param("position_increment_correlation", correlationIncrement_, 5);
  nodeHandle_.param("required_overlap", requiredOverlap_, float(0.75));
  nodeHandle_.param("correlation_threshold", corrThreshold_, float(0)); //0.65 weighted, 0.75 unweighted
  nodeHandle_.param("SSD_threshold", SSDThreshold_, float(10));
  nodeHandle_.param("SAD_threshold", SADThreshold_, float(10));
  nodeHandle_.param("MI_threshold", MIThreshold_, float(-10));

  double activityCheckRate;
  nodeHandle_.param("activity_check_rate", activityCheckRate, 1.0);
  activityCheckDuration_.fromSec(1.0 / activityCheckRate);
  cumulativeErrorCorr_ = 0;
  cumulativeErrorSSD_ = 0;
  cumulativeErrorSAD_ = 0;
  cumulativeErrorMI_ = 0;
  correctMatchesCorr_ = 0;
  correctMatchesSSD_ = 0;
  correctMatchesSAD_ = 0;
  correctMatchesMI_ = 0;

  /*weightedHist_ = cv::Mat::zeros(256, 256, cv::DataType<float>::type);
  for (int i=0; i < 256; i++)
  {
    for (int j=i; j < 256; j++)
    {
      weightedHist_.at<float>(i,j) = float( ((j-i)+1) )/12;
      weightedHist_.at<float>(j,i) = weightedHist_.at<float>(i,j);
    }
  }*/
}

void MapFitter::updateSubscriptionCallback(const ros::TimerEvent&)
{
  if (!isActive_) {
    mapSubscriber_ = nodeHandle_.subscribe(mapTopic_, 1, &MapFitter::callback, this);
    isActive_ = true;
    ROS_DEBUG("Subscribed to grid map at '%s'.", mapTopic_.c_str());
  }
}

void MapFitter::callback(const grid_map_msgs::GridMap& message)
{
  ROS_INFO("Map fitter received a map (timestamp %f) for matching.",
            message.info.header.stamp.toSec());
  grid_map::GridMapRosConverter::fromMessage(message, map_);

  grid_map::GridMapRosConverter::loadFromBag("/home/parallels/rosbags/reference_map_last.bag", referenceMapTopic_, referenceMap_);
//grid_map::GridMapRosConverter::loadFromBag("/home/parallels/rosbags/source/asl_walking_uav/uav_elevation_map_merged.bag", referenceMapTopic_, referenceMap_);

  exhaustiveSearch();
}

void MapFitter::exhaustiveSearch()
{
  // initialize correlationMap
  grid_map::GridMap correlationMap({"correlation","rotationNCC","SSD","rotationSSD","SAD","rotationSAD", "MI", "rotationMI"});
  correlationMap.setGeometry(referenceMap_.getLength(), referenceMap_.getResolution()*searchIncrement_,
                              referenceMap_.getPosition()); //TODO only use submap
  correlationMap.setFrameId("grid_map");

  //initialize parameters
  grid_map::Size reference_size = referenceMap_.getSize();
  int rows = reference_size(0);
  int cols = reference_size(1);
  grid_map::Matrix acceptedThetas = grid_map::Matrix::Constant(reference_size(0), reference_size(1), 0);

  float best_corr[int(360/angleIncrement_)];
  int corr_row[int(360/angleIncrement_)];
  int corr_col[int(360/angleIncrement_)];

  float best_SSD[int(360/angleIncrement_)];
  int SSD_row[int(360/angleIncrement_)];
  int SSD_col[int(360/angleIncrement_)];

  float best_SAD[int(360/angleIncrement_)];
  int SAD_row[int(360/angleIncrement_)];
  int SAD_col[int(360/angleIncrement_)];

  float best_MI[int(360/angleIncrement_)];
  int MI_row[int(360/angleIncrement_)];
  int MI_col[int(360/angleIncrement_)];

  grid_map::Position correct_position = map_.getPosition();
  grid_map::Position position = referenceMap_.getPosition();
  grid_map::Size size = referenceMap_.getSize();
  grid_map::Index start_index = referenceMap_.getStartIndex();
  //float correlation[referenceMapImage_.rows][referenceMapImage_.cols][int(360/angleIncrement_)];

  //ros::Time time = ros::Time::now();
  ros::Duration duration;
  duration.sec = 0;
  duration.nsec = 0;
  duration1_.sec = 0;
  duration1_.nsec = 0:
  duration2_.sec = 0;
  duration2_.nsec = 0;

  grid_map::Matrix& reference_data = referenceMap_["elevation"];
  grid_map::Matrix& data = map_["elevation"];
  grid_map::Matrix& variance_data = map_["variance"];
  for (float theta = 0; theta < 360; theta+=angleIncrement_)
  {
    best_corr[int(theta/angleIncrement_)] = -1;
    best_SSD[int(theta/angleIncrement_)] = 10;
    best_SAD[int(theta/angleIncrement_)] = 10;
    best_MI[int(theta/angleIncrement_)] = -10;

    // iterate sparsely through search area
    for (grid_map::GridMapIteratorSparse iterator(referenceMap_, searchIncrement_); !iterator.isPastEnd(); ++iterator) {
      grid_map::Index index(*iterator);
      index = grid_map::getIndexFromBufferIndex(index, size, start_index);
      float errSAD = 10;
      float errSSD = 10;
      float corrNCC = -1;
      float mutInfo = -10;

      ros::Time time = ros::Time::now();
      bool success = findMatches(data, variance_data, reference_data, index, theta);
      duration += ros::Time::now() - time;

      if (success) 
      {
        errSAD = errorSAD();
        errSSD = errorSSD();
        corrNCC = correlationNCC();
        //mutInfo = mutualInformation();

        //errSAD = weightedErrorSAD();
        //errSSD = weightedErrorSSD();
        //corrNCC = weightedCorrelationNCC();
        //mutInfo = weightedMutualInformation();
        /*for (int i = 0; i < matches_; i++)
        {
          std::cout << xy_reference_[i] << std::endl;
        }*/


        acceptedThetas(index(0), index(1)) += 1;

        grid_map::Position xy_position;
        referenceMap_.getPosition(index, xy_position);
        if (correlationMap.isInside(xy_position))
        {
          grid_map::Index correlation_index;
          correlationMap.getIndex(xy_position, correlation_index);

          bool valid = correlationMap.isValid(correlation_index, "correlation");
          // if no value so far or correlation smaller or correlation higher than for other thetas
          if (((valid == false) || (corrNCC+1.5 > correlationMap.at("correlation", correlation_index) ))) 
          {
            correlationMap.at("correlation", correlation_index) = corrNCC+1.5;  //set correlation
            correlationMap.at("rotationNCC", correlation_index) = theta;    //set theta
          }

          valid = correlationMap.isValid(correlation_index, "SSD");
          // if no value so far or correlation smaller or correlation higher than for other thetas
          if (((valid == false) || (errSSD*5 < correlationMap.at("SSD", correlation_index) ))) 
          {
            correlationMap.at("SSD", correlation_index) = errSSD*5;  //set correlation
            correlationMap.at("rotationSSD", correlation_index) = theta;    //set theta
          }

          valid = correlationMap.isValid(correlation_index, "SAD");
          // if no value so far or correlation smaller or correlation higher than for other thetas
          if (((valid == false) || (errSSD*5 < correlationMap.at("SAD", correlation_index) ))) 
          {
            correlationMap.at("SAD", correlation_index) = errSAD*5;  //set correlation
            correlationMap.at("rotationSAD", correlation_index) = theta;    //set theta
          }

          valid = correlationMap.isValid(correlation_index, "MI");
          // if no value so far or correlation smaller or correlation higher than for other thetas
          if (((valid == false) || (mutInfo > correlationMap.at("MI", correlation_index) ))) 
          {
            correlationMap.at("MI", correlation_index) = mutInfo;  //set correlation
            correlationMap.at("rotationMI", correlation_index) = theta;    //set theta
          }
        }
                  
        // save best correlation for each theta
        if (corrNCC > best_corr[int(theta/angleIncrement_)])
        {
          best_corr[int(theta/angleIncrement_)] = corrNCC;
          corr_row[int(theta/angleIncrement_)] = index(0);
          corr_col[int(theta/angleIncrement_)] = index(1);
        }
        if (errSSD < best_SSD[int(theta/angleIncrement_)])
        {
          best_SSD[int(theta/angleIncrement_)] = errSSD;
          SSD_row[int(theta/angleIncrement_)] = index(0);
          SSD_col[int(theta/angleIncrement_)] = index(1);
        }
        if (errSAD < best_SAD[int(theta/angleIncrement_)])
        {
          best_SAD[int(theta/angleIncrement_)] = errSAD;
          SAD_row[int(theta/angleIncrement_)] = index(0);
          SAD_col[int(theta/angleIncrement_)] = index(1);
        }
        if (mutInfo > best_MI[int(theta/angleIncrement_)])
        {
          best_MI[int(theta/angleIncrement_)] = mutInfo;
          MI_row[int(theta/angleIncrement_)] = index(0);
          MI_col[int(theta/angleIncrement_)] = index(1);
        }
      }
    }
    // publish correlationMap for each theta
    grid_map_msgs::GridMap correlation_msg;
    grid_map::GridMapRosConverter::toMessage(correlationMap, correlation_msg);
    correlationPublisher_.publish(correlation_msg);
  }
  
  //find highest correlation over all theta
  float bestCorr = -1.0;
  float bestSSD = 10;
  float bestSAD = 10;
  float bestMI = -10;
  int bestThetaCorr;
  int bestThetaSSD;
  int bestThetaSAD;
  int bestThetaMI;
  float bestXCorr;
  float bestYCorr;
  float bestXSSD;
  float bestYSSD;
  float bestXSAD;
  float bestYSAD;
  float bestXMI;
  float bestYMI;

  for (int i = 0; i < int(360/angleIncrement_); i++)
  {
    if (best_corr[i] > bestCorr && best_corr[i] >= corrThreshold_) 
    {
      std::cout << int(acceptedThetas(corr_row[i], corr_col[i])) << " " << int(360/angleIncrement_) << std::endl;
      if (int(acceptedThetas(corr_row[i], corr_col[i])) == int(360/angleIncrement_))
      {
        bestCorr = best_corr[i];
        bestThetaCorr = i*angleIncrement_;
        grid_map::Position best_pos;
        referenceMap_.getPosition(grid_map::Index(corr_row[i], corr_col[i]), best_pos);
        bestXCorr = best_pos(0);
        bestYCorr = best_pos(1);
      }
    }
    if (best_SSD[i] < bestSSD && best_SSD[i] <= SSDThreshold_) 
    {
      if (acceptedThetas(SSD_row[i], SSD_col[i]) == int(360/angleIncrement_))
      {
        bestSSD = best_SSD[i];
        bestThetaSSD = i*angleIncrement_;
        grid_map::Position best_pos;
        referenceMap_.getPosition(grid_map::Index(SSD_row[i], SSD_col[i]), best_pos);
        bestXSSD = best_pos(0);
        bestYSSD = best_pos(1);
      }
    }
    if (best_SAD[i] < bestSAD && best_SAD[i] <= SADThreshold_) 
    {
      if (acceptedThetas(SAD_row[i], SAD_col[i]) == int(360/angleIncrement_))
      {
        bestSAD = best_SAD[i];
        bestThetaSAD = i*angleIncrement_;
        grid_map::Position best_pos;
        referenceMap_.getPosition(grid_map::Index(SAD_row[i], SAD_col[i]), best_pos);
        bestXSAD = best_pos(0);
        bestYSAD = best_pos(1);
      }
    }
    if (best_MI[i] > bestMI && best_MI[i] >= MIThreshold_) 
    {
      if (acceptedThetas(MI_row[i], MI_col[i]) == int(360/angleIncrement_))
      {
        bestMI = best_MI[i];
        bestThetaMI = i*angleIncrement_;
        grid_map::Position best_pos;
        referenceMap_.getPosition(grid_map::Index(MI_row[i], MI_col[i]), best_pos);
        bestXMI = best_pos(0);
        bestYMI = best_pos(1);
      }
    }
  }
  float z = findZ(bestXCorr, bestYCorr, bestThetaCorr);

  ros::Time pubTime = ros::Time::now();
  // output best correlation and time used
  if (bestCorr != -1) 
  {
    cumulativeErrorCorr_ += sqrt((bestXCorr - correct_position(0))*(bestXCorr - correct_position(0)) + (bestYCorr - correct_position(1))*(bestYCorr - correct_position(1)));
    if (sqrt((bestXCorr - correct_position(0))*(bestXCorr - correct_position(0)) + (bestYCorr - correct_position(1))*(bestYCorr - correct_position(1))) < 0.5 && fabs(bestThetaCorr - (360-int(templateRotation_))%360) < angleIncrement_) {correctMatchesCorr_ += 1;}
    geometry_msgs::PointStamped corrPoint;
    corrPoint.point.x = bestXCorr;
    corrPoint.point.y = bestYCorr;
    corrPoint.point.z = bestThetaCorr;
    corrPoint.header.stamp = pubTime;
    corrPointPublisher_.publish(corrPoint);
  }
  if (bestSSD != 10) 
  {
    cumulativeErrorSSD_ += sqrt((bestXSSD - correct_position(0))*(bestXSSD - correct_position(0)) + (bestYSSD - correct_position(1))*(bestYSSD - correct_position(1)));
    if (sqrt((bestXSSD - correct_position(0))*(bestXSSD - correct_position(0)) + (bestYSSD - correct_position(1))*(bestYSSD - correct_position(1))) < 0.5 && fabs(bestThetaSSD - (360-int(templateRotation_))%360) < angleIncrement_) {correctMatchesSSD_ += 1;}
    geometry_msgs::PointStamped SSDPoint;
    SSDPoint.point.x = bestXSSD;
    SSDPoint.point.y = bestYSSD;
    SSDPoint.point.z = bestThetaSSD;
    SSDPoint.header.stamp = pubTime;
    SSDPointPublisher_.publish(SSDPoint);
  }
  if (bestSAD != 10) 
  {
    cumulativeErrorSAD_ += sqrt((bestXSAD - correct_position(0))*(bestXSAD - correct_position(0)) + (bestYSAD - correct_position(1))*(bestYSAD - correct_position(1)));
    if (sqrt((bestXSAD - correct_position(0))*(bestXSAD - correct_position(0)) + (bestYSAD - correct_position(1))*(bestYSAD - correct_position(1))) < 0.5 && fabs(bestThetaSAD - (360-int(templateRotation_))%360) < angleIncrement_) {correctMatchesSAD_ += 1;}
    geometry_msgs::PointStamped SADPoint;
    SADPoint.point.x = bestXSAD;
    SADPoint.point.y = bestYSAD;
    SADPoint.point.z = bestThetaSAD;
    SADPoint.header.stamp = pubTime;
    SADPointPublisher_.publish(SADPoint);
  }
  if (bestMI != 0) 
  {
    cumulativeErrorMI_ += sqrt((bestXMI - correct_position(0))*(bestXMI - correct_position(0)) + (bestYMI - correct_position(1))*(bestYMI - correct_position(1)));
    if (sqrt((bestXMI - correct_position(0))*(bestXMI - correct_position(0)) + (bestYMI - correct_position(1))*(bestYMI - correct_position(1))) < 0.5 && fabs(bestThetaMI - (360-int(templateRotation_))%360) < angleIncrement_) {correctMatchesMI_ += 1;}
    geometry_msgs::PointStamped MIPoint;
    MIPoint.point.x = bestXMI;
    MIPoint.point.y = bestYMI;
    MIPoint.point.z = bestThetaMI;
    MIPoint.header.stamp = pubTime;
    MIPointPublisher_.publish(MIPoint);
  }
  geometry_msgs::PointStamped correctPoint;
  correctPoint.point.x = correct_position(0);
  correctPoint.point.y = correct_position(1);
  correctPoint.point.z = 0;
  correctPoint.header.stamp = pubTime;
  correctPointPublisher_.publish(correctPoint);


  //ros::Duration duration = ros::Time::now() - time;
  std::cout << "Best correlation " << bestCorr << " at " << bestXCorr << ", " << bestYCorr << " and theta " << bestThetaCorr << " and z: " << z << std::endl;
  std::cout << "Best SSD " << bestSSD << " at " << bestXSSD << ", " << bestYSSD << " and theta " << bestThetaSSD << std::endl;
  std::cout << "Best SAD " << bestSAD << " at " << bestXSAD << ", " << bestYSAD << " and theta " << bestThetaSAD << std::endl;
  std::cout << "Best MI " << bestMI << " at " << bestXMI << ", " << bestYMI << " and theta " << bestThetaMI << std::endl;
  std::cout << "Correct position " << correct_position.transpose() << " and theta " << (360-int(templateRotation_))%360 << std::endl;
  std::cout << "Time used: " << duration.toSec() << " Sekunden" << " 1: " << duration1_.toSec() << " 2: " << duration2_.toSec() << std::endl;
  std::cout << "Cumulative error NCC: " << cumulativeErrorCorr_ << " matches: " << correctMatchesCorr_ << " SSD: " << cumulativeErrorSSD_ << " matches: " << correctMatchesSSD_ << " SAD: " << cumulativeErrorSAD_ << " matches: " << correctMatchesSAD_ << " MI: " << cumulativeErrorMI_ << " matches: " << correctMatchesMI_ << std::endl;
  ROS_INFO("done");
  isActive_ = false;
}

float MapFitter::findZ(float x, float y, int theta)
{
  // initialize
  float shifted_mean = 0;
  float reference_mean = 0;
  int matches = 0;

  grid_map::Matrix& data = map_["elevation"];
  for (grid_map::GridMapIteratorSparse iterator(map_, correlationIncrement_); !iterator.isPastEnd(); ++iterator) {
    const grid_map::Index index(*iterator);
    float shifted = data(index(0), index(1));
    if (shifted == shifted) {   // check if point is defined, if nan f!= f 
      grid_map::Position xy_position;
      map_.getPosition(index, xy_position);  // get coordinates
      tf::Vector3 xy_vector = tf::Vector3(xy_position(0), xy_position(1), 0.0);

      // transform coordinates from /map_rotated to /grid_map
      tf::Transform transform = tf::Transform(tf::Quaternion(0.0, 0.0, sin(theta/180*M_PI/2), cos(theta/180*M_PI/2)), tf::Vector3(x, y, 0.0));
      tf::Vector3 map_vector = transform*(xy_vector); // apply transformation
      grid_map::Position map_position;
      map_position(0) = map_vector.getX();
      map_position(1) = map_vector.getY();

      // check if point is within reference_map
      if (referenceMap_.isInside(map_position)) {
        float reference = referenceMap_.atPosition("elevation", map_position);
        if (reference == reference) {   // check if point is defined, if nan f!= f 
          matches += 1;
          shifted_mean += shifted;
          reference_mean += reference;
        }
      }
    }
  }
  // calculate mean
  shifted_mean = shifted_mean/matches;
  reference_mean = reference_mean/matches;

  return reference_mean - shifted_mean;
}

bool MapFitter::findMatches(grid_map::Matrix& data, grid_map::Matrix& variance_data, grid_map::Matrix& reference_data, grid_map::Index reference_index, float theta)
{
  // initialize
  int points = 0;
  matches_ = 0;

  shifted_mean_ = 0;
  reference_mean_ = 0;
  xy_shifted_.clear();
  xy_reference_.clear();
  xy_shifted_var_.clear();
  //xy_reference_var_.clear();

  grid_map::Size size = map_.getSize();
  grid_map::Index start_index = map_.getStartIndex();
  grid_map::Size reference_size = referenceMap_.getSize();
  grid_map::Index reference_start_index = referenceMap_.getStartIndex();

  for (int i = 0; i <= size(0)-correlationIncrement_; i += correlationIncrement_)
  {
    for (int j = 0; j<= size(1)-correlationIncrement_; j += correlationIncrement_)
    {
      grid_map::Index index = start_index + grid_map::Index(i,j);
      grid_map::mapIndexWithinRange(index, size);
      ros::Time time = ros::Time::now();
      float mapHeight = data(index(0), index(1));
      duration1_ += ros::Time::now() - time;
      if (mapHeight == mapHeight)
      {
        points += 1;
        grid_map::Index reference_buffer_index = reference_size - reference_start_index + reference_index;
        grid_map::mapIndexWithinRange(reference_buffer_index, reference_size);
        grid_map::Index shifted_index;
        shifted_index(0) = reference_buffer_index(0) - cos(theta/180*M_PI)*(float(size(0))/2-i)+sin(theta/180*M_PI)*(float(size(1))/2-j);
        shifted_index(1) = reference_buffer_index(1) - sin(theta/180*M_PI)*(float(size(0))/2-i)-cos(theta/180*M_PI)*(float(size(1))/2-j);

        if (grid_map::checkIfIndexWithinRange(shifted_index, reference_size))
        {
          shifted_index = shifted_index + reference_start_index;
          grid_map::mapIndexWithinRange(shifted_index, reference_size);
          time = ros::Time::now();
          float referenceHeight = reference_data(shifted_index(0), shifted_index(1));
          duration2_ += ros::Time::now() - time;
          //std::cout << referenceHeight << " " << shifted_index_x <<", " << shifted_index_y << std::endl;
          if (referenceHeight == referenceHeight)
          {
            matches_ += 1;
            shifted_mean_ += mapHeight;
            reference_mean_ += referenceHeight;
            xy_shifted_.push_back(mapHeight);
            xy_reference_.push_back(referenceHeight);
            float mapVariance = variance_data(index(0), index(1));
            xy_shifted_var_.push_back(1 / mapVariance);
          }
        }
      }
    }
  }

  // check if required overlap is fulfilled
  if (matches_ > points*requiredOverlap_) 
  { 
    shifted_mean_ = shifted_mean_/matches_;
    reference_mean_ = reference_mean_/matches_;
    return true; 
  }
  else { return false; }
}

/*float MapFitter::correlationNCC(grid_map::Position position, int theta)
{
  float correlation = 0;  // initialization
  float shifted_mean = 0;
  float reference_mean = 0;
  std::vector<float> xy_shifted;
  std::vector<float> xy_reference;
  float shifted_normal = 0;
  float reference_normal = 0;
  int points = 0;
  int matches = 0;

  
  grid_map::Matrix& data = map_["elevation"];

  // iterate sparsely through template points
  for (grid_map::GridMapIteratorSparse iterator(map_, correlationIncrement_); !iterator.isPastEnd(); ++iterator) {
    const grid_map::Index index(*iterator);

  
    float shifted = data(index(0), index(1));
  
    
    if (shifted == shifted) {   // check if point is defined, if nan f!= f
      points += 1;    // increase number of valid points
      grid_map::Position xy_position;
      map_.getPosition(index, xy_position);  // get coordinates 30% time
      tf::Vector3 xy_vector = tf::Vector3(xy_position(0), xy_position(1), 0.0);

      // transform coordinates from /map_rotated to /map
      tf::Transform transform = tf::Transform(tf::Quaternion(0.0, 0.0, sin(theta/180*M_PI/2), cos(theta/180*M_PI/2)), tf::Vector3(position(0), position(1), 0.0));
      tf::Vector3 map_vector = transform*(xy_vector); // apply transformation
      grid_map::Position map_position;
      map_position(0) = map_vector.getX();
      map_position(1) = map_vector.getY();

  ros::Time correlation_time = ros::Time::now();
      // check if point is within reference_map
      if (referenceMap_.isInside(map_position)) { // 15% time
  correlationDur_ += ros::Time::now() - correlation_time;
        float reference = referenceMap_.atPosition("elevation", map_position); // 45% time

        if (reference == reference) {   // check if point is defined, if nan f!= f 
          matches += 1;   // increase number of matched points
          shifted_mean += shifted;
          reference_mean += reference;
      correlation_time = ros::Time::now();
          xy_shifted.push_back(shifted);
          xy_reference.push_back(reference);
      correlationDur2_ += ros::Time::now() - correlation_time;
        }
      }
    }
  }
  
  
  // check if required overlap is fulfilled
  if (matches > points*requiredOverlap_) 
  { 
    // calculate Normalized Cross Correlation (NCC)
    shifted_mean = shifted_mean/matches;
    reference_mean = reference_mean/matches;
    for (int i = 0; i < matches; i++) {
      float shifted_corr = (xy_shifted[i]-shifted_mean);
      float reference_corr = (xy_reference[i]-reference_mean);
      correlation += shifted_corr*reference_corr;
      shifted_normal += shifted_corr*shifted_corr;
      reference_normal += reference_corr*reference_corr;
    }
    correlation = correlation/sqrt(shifted_normal*reference_normal);
    return 1 - correlation; 
  }
  else { return 1.0; }
}*/

float MapFitter::mutualInformation()
{
  /*cv::Mat hist( 256, 1, cv::DataType<float>::type, 0.0); 
  cv::Mat referenceHist( 256, 1, cv::DataType<float>::type, 0.0);
  cv::Mat jointHist( 256, 256, cv::DataType<float>::type, 0.0);

  for (int k=0; k < matches_; k++)
  {
      int i1 = xy_shifted_[k]/65536*127 - (shifted_mean_+1)/65536*127 + 127;
      int i2 = xy_reference_[k]/65536*127 - (reference_mean_+1)/65536*127 + 127;
      hist.at<float>(i1, 0) += 1;
      referenceHist.at<float>(i2, 0) += 1;
      jointHist.at<float>(i1, i2) += 1;
  }
  hist = hist/matches_;
  referenceHist = referenceHist/matches_;
  jointHist = jointHist/matches_;
  
  cv::Mat logP;
  cv::log(hist,logP);
  cv::Mat referenceLogP;
  cv::log(referenceHist,referenceLogP);
  cv::Mat jointLogP;
  cv::log(jointHist,jointLogP);

  float entropy = -1*cv::sum(hist.mul(logP)).val[0];
  float referenceEntropy = -1*cv::sum(referenceHist.mul(referenceLogP)).val[0];
  float jointEntropy = -1*cv::sum(jointHist.mul(jointLogP)).val[0];*/

  /*cv::Mat divLogP;
  cv::gemm(hist, referenceHist, 1, cv::Mat(), 0, divLogP, cv::GEMM_2_T);
  cv::divide(jointHist, divLogP, divLogP);
  cv::log(divLogP, divLogP);
  float mutualDiv = cv::sum(weightedHist.mul(divLogP)).val[0];*/

  //std::cout << " Mutual information: " << entropy+referenceEntropy-jointEntropy << " by division: " << mutualDiv <<std::endl;
  //std::cout << count << " template entropy: " << entropy << " reference entropy: " << referenceEntropy << " joint entropy: " << jointEntropy << " Mutual information: " << entropy+referenceEntropy-jointEntropy <<std::endl;

  /*jointHist = jointHist * 255;

  cv::Mat histImage( 256, 256, cv::DataType<float>::type, 0.0);
  cv::Mat histImage2( 256, 256, cv::DataType<float>::type, 0.0);

  cv::normalize(hist, hist, 0, histImage.rows-1, cv::NORM_MINMAX, -1, cv::Mat() );
  cv::normalize(referenceHist, referenceHist, 0, histImage2.rows-1, cv::NORM_MINMAX, -1, cv::Mat() );

  for( int i = 1; i < 256; i++ )
  {
    cv::line( histImage, cv::Point( (i-1), 255 - cvRound(hist.at<float>(i-1)) ) ,
                     cv::Point(i, 255 - cvRound(hist.at<float>(i)) ),
                     cv::Scalar( 255, 0, 0), 1, 8, 0  );
    cv::line( histImage2, cv::Point( (i-1), 255 - cvRound(referenceHist.at<float>(i-1)) ) ,
                     cv::Point( i, 255 - cvRound(referenceHist.at<float>(i)) ),
                     cv::Scalar( 255, 0, 0), 1, 8, 0  );
  }
  std::vector<cv::Mat> channels; 
  channels.push_back(histImage);
  channels.push_back(histImage2);
  channels.push_back(jointHist);
  cv::merge(channels, histImage);
  cv::namedWindow("calcHist", CV_WINDOW_AUTOSIZE );
  cv::imshow("calcHist", histImage );
  cv::waitKey(0);*/

  //return (entropy+referenceEntropy-jointEntropy); // jointEntropy or sqrt(entropy*referenceEntropy);
}

float MapFitter::weightedMutualInformation()
{
  /*cv::Mat hist( 256, 1, cv::DataType<float>::type, 0.0);
  cv::Mat referenceHist( 256, 1, cv::DataType<float>::type, 0.0);
  cv::Mat jointHist( 256, 256, cv::DataType<float>::type, 0.0);

  for (int k=0; k < matches_; k++)
  {
      int i1 = xy_shifted_[k]/65536*127 - (shifted_mean_+1)/65536*127 + 127;
      int i2 = xy_reference_[k]/65536*127 - (reference_mean_+1)/65536*127 + 127;
      hist.at<float>(i1, 0) += 1;
      referenceHist.at<float>(i2, 0) += 1;
      jointHist.at<float>(i1, i2) += 1;
  }
  hist = hist/matches_;
  referenceHist = referenceHist/matches_;
  jointHist = jointHist/matches_;
  
  cv::Mat logP;
  cv::log(hist,logP);
  float entropy = -1*cv::sum(hist.mul(logP)).val[0];

  cv::log(referenceHist,logP);
  float referenceEntropy = -1*cv::sum(referenceHist.mul(logP)).val[0];

  cv::Mat jointLogP;
  cv::log(jointHist,jointLogP);

  //cv::Mat weightedHist;
  //float norm = cv::norm(weightedHist, cv::NORM_L1);
  //weightedHist = weightedHist/norm;
  //std::cout << weightedHist <<std::endl;
  jointHist = weightedHist_.mul(jointHist);

  float jointEntropy = -1*cv::sum(jointHist.mul(jointLogP)).val[0];*/

  //std::cout << " template entropy: " << entropy << " reference entropy: " << referenceEntropy << " joint entropy: " << jointEntropy << " Mutual information: " << entropy+referenceEntropy-jointEntropy <<std::endl;

  /*cv::divide(jointHist, weightedHist_, jointHist);
  jointHist = jointHist * 255;

  cv::Mat histImage( 256, 256, cv::DataType<float>::type, 0.0);
  cv::Mat histImage2( 256, 256, cv::DataType<float>::type, 0.0);

  cv::normalize(hist, hist, 0, histImage.rows-1, cv::NORM_MINMAX, -1, cv::Mat() );
  cv::normalize(referenceHist, referenceHist, 0, histImage2.rows-1, cv::NORM_MINMAX, -1, cv::Mat() );

  for( int i = 1; i < 256; i++ )
  {
    cv::line( histImage, cv::Point( (i-1), 255 - cvRound(hist.at<float>(i-1)) ) ,
                     cv::Point(i, 255 - cvRound(hist.at<float>(i)) ),
                     cv::Scalar( 255, 0, 0), 1, 8, 0  );
    cv::line( histImage2, cv::Point( (i-1), 255 - cvRound(referenceHist.at<float>(i-1)) ) ,
                     cv::Point( i, 255 - cvRound(referenceHist.at<float>(i)) ),
                     cv::Scalar( 255, 0, 0), 1, 8, 0  );
  }
  std::vector<cv::Mat> channels; 
  channels.push_back(histImage);
  channels.push_back(histImage2);
  channels.push_back(jointHist);
  cv::merge(channels, histImage);
  cv::namedWindow("calcHist", CV_WINDOW_AUTOSIZE );
  cv::imshow("calcHist", histImage );
  cv::waitKey(0);*/

  //return (entropy+referenceEntropy-jointEntropy); //-cv::sum(jointHist).val[0]+2; // jointEntropy or sqrt(entropy*referenceEntropy);
}

float MapFitter::errorSAD()
{
  float error = 0;
  for (int i = 0; i < matches_; i++) 
  {
    float shifted = (xy_shifted_[i]-shifted_mean_)/std::numeric_limits<unsigned short>::max();
    float reference = (xy_reference_[i]-reference_mean_)/std::numeric_limits<unsigned short>::max();
    error += fabs(shifted-reference);
  }
  // divide error by number of matches
  //std::cout << error/matches_ <<std::endl;
  return error/matches_;
}

float MapFitter::weightedErrorSAD()
{
  float error = 0;
  float normalization = 0;
  for (int i = 0; i < matches_; i++) 
  {
    float shifted = (xy_shifted_[i]-shifted_mean_)/std::numeric_limits<unsigned short>::max();
    float reference = (xy_reference_[i]-reference_mean_)/std::numeric_limits<unsigned short>::max();
    error += fabs(shifted-reference) * xy_shifted_var_[i];// * xy_reference_var_[i];
    normalization += xy_shifted_var_[i];// * (xy_reference_var_[i]/std::numeric_limits<unsigned short>::max());
  }
  // divide error by number of matches
  //std::cout << error/normalization <<std::endl;
  return error/normalization;
}

float MapFitter::errorSSD()
{
  float error = 0;
  for (int i = 0; i < matches_; i++) 
  {
    float shifted = (xy_shifted_[i]-shifted_mean_)/std::numeric_limits<unsigned short>::max();
    float reference = (xy_reference_[i]-reference_mean_)/std::numeric_limits<unsigned short>::max();
    error += (shifted-reference)*(shifted-reference); //sqrt(fabs(shifted-reference)) instead of (shifted-reference)*(shifted-reference), since values are in between 0 and 1
  }
  // divide error by number of matches
  //std::cout << error/matches_ <<std::endl;
  return error/matches_;
}

float MapFitter::weightedErrorSSD()
{
  float error = 0;
  float normalization = 0;
  for (int i = 0; i < matches_; i++) 
  {
    float shifted = (xy_shifted_[i]-shifted_mean_)/std::numeric_limits<unsigned short>::max();
    float reference = (xy_reference_[i]-reference_mean_)/std::numeric_limits<unsigned short>::max();
    error += (shifted-reference)*(shifted-reference) * xy_shifted_var_[i]*xy_shifted_var_[i];// * xy_reference_var_[i]; //sqrt(fabs(shifted-reference)) instead of (shifted-reference)*(shifted-reference), since values are in between 0 and 1
    normalization += xy_shifted_var_[i]*xy_shifted_var_[i];// * (xy_reference_var_[i]/std::numeric_limits<unsigned short>::max());
  }
  // divide error by number of matches
  //std::cout << error/normalization <<std::endl;
  return error/normalization;
}

float MapFitter::correlationNCC()
{
  float shifted_normal = 0;
  float reference_normal = 0;
  float correlation = 0;
  for (int i = 0; i < matches_; i++) 
  {
    float shifted_corr = (xy_shifted_[i]-shifted_mean_);
    float reference_corr = (xy_reference_[i]-reference_mean_);
    correlation += shifted_corr*reference_corr;
    shifted_normal += shifted_corr*shifted_corr;
    reference_normal += reference_corr*reference_corr;
  }
  return correlation/sqrt(shifted_normal*reference_normal);
}

float MapFitter::weightedCorrelationNCC()
{
  // calculate Normalized Cross Correlation (NCC)
  float shifted_normal = 0;
  float reference_normal = 0;
  float correlation = 0;
  for (int i = 0; i < matches_; i++) 
  {
    //for 1/variance
    float shifted_corr = (xy_shifted_[i]-shifted_mean_); // * xy_shifted_var_[i]
    float reference_corr = (xy_reference_[i]-reference_mean_);// * xy_reference_var_[i];
    correlation += shifted_corr*reference_corr * xy_shifted_var_[i];
    shifted_normal += xy_shifted_var_[i]*shifted_corr*shifted_corr;// shifted_corr*shifted_corr * xy_shifted_var_[i];
    reference_normal += xy_shifted_var_[i]*reference_corr*reference_corr;// reference_corr*reference_corr * xy_shifted_var_[i];
    
    //for 1 - variance
    /*float shifted_corr = (xy_shifted_[i]-shifted_mean_)* xy_shifted_var_[i];
    float reference_corr = (xy_reference_[i]-reference_mean_);// * xy_reference_var_[i];
    correlation += shifted_corr*reference_corr;
    shifted_normal += shifted_corr * shifted_corr;
    reference_normal += reference_corr * reference_corr;*/
  }
  return correlation/sqrt(shifted_normal*reference_normal);
}

void MapFitter::tfBroadcast(const ros::TimerEvent&) {
  broadcaster_.sendTransform(
      tf::StampedTransform(
        tf::Transform(tf::Quaternion(0, 0, 0, 1), tf::Vector3(0.0, 0.0, 0.0)), 
          ros::Time::now(),"/map", "/grid_map"));
}

} /* namespace */
