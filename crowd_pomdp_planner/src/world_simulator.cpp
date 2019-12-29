#include "world_simulator.h"
#include "WorldModel.h"
#include "ped_pomdp.h"

#include <std_msgs/Int32.h>

#include <msg_builder/StartGoal.h>
#include <msg_builder/car_info.h>
#include <msg_builder/peds_info.h>
#include <msg_builder/ped_info.h>
#include <msg_builder/peds_believes.h>
#include "ros/ros.h"

#include <nav_msgs/OccupancyGrid.h>
#include <std_msgs/Bool.h>
#include <msg_builder/TrafficAgentArray.h>
#include <msg_builder/AgentPathArray.h>
#include <msg_builder/Lanes.h>
#include <msg_builder/Obstacles.h>
#include <msg_builder/PomdpCmd.h>


#undef LOG
#define LOG(lv) \
if (despot::logging::level() < despot::logging::ERROR || despot::logging::level() < lv) ; \
else despot::logging::stream(lv)
#include <despot/util/logging.h>

WorldStateTracker* SimulatorBase::stateTracker;

WorldModel SimulatorBase::worldModel;
bool SimulatorBase::agents_data_ready = false;
bool SimulatorBase::agents_path_data_ready = false;

static nav_msgs::OccupancyGrid raw_map_;

double pub_frequency = 9.0;

void pedPoseCallback(msg_builder::ped_local_frame_vector);

void agentArrayCallback(msg_builder::TrafficAgentArray);

void agentPathArrayCallback(msg_builder::AgentPathArray);

void receive_map_callback(nav_msgs::OccupancyGrid map);

COORD poseToCoord(const tf::Stamped<tf::Pose>& pose) {
	COORD coord;
	coord.x = pose.getOrigin().getX();
	coord.y = pose.getOrigin().getY();
	return coord;
}

double poseToHeadingDir(const tf::Stamped<tf::Pose>& pose) {
	/* get yaw angle [-pi, pi)*/
	double yaw;
	yaw = tf::getYaw(pose.getRotation());
	if (yaw < 0)
		yaw += 2 * 3.1415926;
	return yaw;
}

double navposeToHeadingDir(const geometry_msgs::Pose & msg) {
	/* get yaw angle [-pi, pi)*/
	tf::Pose pose;
	tf::poseMsgToTF(msg, pose);

	double yaw;
	yaw = tf::getYaw(pose.getRotation());
	if (yaw < 0)
		yaw += 2 * 3.1415926;
	return yaw;
}

WorldSimulator::WorldSimulator(ros::NodeHandle& _nh, DSPOMDP* model,
		unsigned seed, bool pathplan_ahead, std::string obstacle_file_name, std::string map_location,
		int summit_port,
		COORD car_goal) :
		SimulatorBase(_nh, obstacle_file_name), pathplan_ahead_(pathplan_ahead), model_(
				model), World() {

	map_location_ = map_location;
	summit_port_ = summit_port;

	global_frame_id = ModelParams::rosns + "/map";

	logi << __FUNCTION__ << " global_frame_id: " << global_frame_id << " "
			<< endl;

	ros::NodeHandle n("~");
	n.param<std::string>("goal_file_name", worldModel.goal_file_name_, "null");

	worldModel.InitPedGoals();
	worldModel.InitRVO();
	worldModel.car_goal = car_goal;
	AddObstacle();

	stateTracker = new WorldStateTracker(worldModel);
}

WorldSimulator::~WorldSimulator() {
	geometry_msgs::Twist cmd;
	cmd.angular.z = 0;
	cmd.linear.x = 0;
	cmdPub_.publish(cmd);
}

/**
 * [Essential]
 * Establish connection to simulator or system
 */
bool WorldSimulator::Connect() {
	cerr << "DEBUG: Connecting with world" << endl;

	cmdPub_ = nh.advertise<msg_builder::PomdpCmd>("cmd_vel_pomdp", 1);
	actionPub_ = nh.advertise<visualization_msgs::Marker>("pomdp_action", 1);
	actionPubPlot_ = nh.advertise<geometry_msgs::Twist>("pomdp_action_plot", 1);

	pa_pub = nh.advertise<geometry_msgs::PoseArray>("my_poses", 1000);
	car_pub = nh.advertise<geometry_msgs::PoseStamped>("car_pose", 1000);
	goal_pub = nh.advertise<visualization_msgs::MarkerArray>("pomdp_goals", 1);

	mapSub_ = nh.subscribe("map", 1, receive_map_callback); // nav_msgs::OccupancyGrid

	speedSub_ = nh.subscribe("odom", 1, &WorldSimulator::speedCallback, this);

	carSub_ = nh.subscribe("ego_state", 1, &WorldSimulator::update_ego_car, this);
	nh.subscribe("ego_dead", 1, &WorldSimulator::ego_dead_callback, this);

    egodeadSub_ = nh.subscribe("ego_dead", 1, &WorldSimulator::ego_car_dead_callback, this); //Bool

	agentSub_ = nh.subscribe("agent_array", 1, agentArrayCallback);
	agentpathSub_ = nh.subscribe("agent_path_array", 1, agentPathArrayCallback);

	logi << "Subscribers and Publishers created at the "
			<< Globals::ElapsedTime() << "th second" << endl;

	auto path_data =
				ros::topic::waitForMessage<nav_msgs::Path>(
						"plan", ros::Duration(300));

	logi << "plan get at the " << Globals::ElapsedTime()
				<< "th second" << endl;

	ros::spinOnce();

	int tick = 0;
	bool agent_data_ok=false, odom_data_ok=false, car_data_ok=false, agent_flag_ok=false;
	while(tick < 28){
		auto agent_data =
					ros::topic::waitForMessage<msg_builder::TrafficAgentArray>(
							"agent_array", ros::Duration(3));

		if (agent_data && !agent_data_ok){
			logi << "agent_array get at the " << Globals::ElapsedTime()
				<< "th second" << endl;
			agent_data_ok = true;
		}
		ros::spinOnce();

		auto odom_data = ros::topic::waitForMessage<nav_msgs::Odometry>(
				"odom", ros::Duration(1));
		if (odom_data && !odom_data_ok){
			logi << "odom get at the " << Globals::ElapsedTime() << "th second"
				<< endl;
			odom_data_ok = true;
		}
		ros::spinOnce();

		auto car_data = ros::topic::waitForMessage<msg_builder::car_info>(
				"ego_state", ros::Duration(1));
		if (car_data && !car_data_ok){
			logi << "ego_state get at the " << Globals::ElapsedTime()
				<< "th second" << endl;
			car_data_ok = true;
		}
		ros::spinOnce();

		auto agent_ready_bool =
				ros::topic::waitForMessage<std_msgs::Bool>(
						"agents_ready", ros::Duration(1));
		if (agent_ready_bool && !agent_flag_ok){
			logi << "agents ready at get at the " << Globals::ElapsedTime()
				<< "th second" << endl;
			agent_flag_ok = true;
		}
		ros::spinOnce();

		if (agent_data_ok && odom_data_ok && car_data_ok && agent_flag_ok)
			tick = 100000;
		else
			tick++;
	}

	if (tick == 28)
		ERR("No agent array messages received after 30 seconds.");

	return true;
}

/**
 * [Essential]
 * Initialize or reset the (simulation) environment, return the start state if applicable
 */
State* WorldSimulator::Initialize() {

	cerr << "DEBUG: Initializing world in WorldSimulator::Initialize" << endl;

	safeAction = 2;
	target_speed_ = 0.0;
	steering_ = 0.0;
	goal_reached = false;
	last_acc_ = 0;

	if(SolverPrior::nn_priors.size() == 0){
		ERR("No nn_prior exist");
	}

	return NULL;
}

tf::Stamped<tf::Pose> WorldSimulator::GetBaseLinkPose() {
	tf::Stamped<tf::Pose> in_pose, out_pose;
	// transpose to laser frame for ped avoidance
	in_pose.setIdentity();
	in_pose.frame_id_ = ModelParams::rosns + ModelParams::laser_frame;

	while (!getObjectPose(global_frame_id, in_pose, out_pose)) {
		logv << "transform error within GetCurrentState" << endl;
		logv << "laser frame " << in_pose.frame_id_ << endl;
		ros::Rate err_retry_rate(10);
		err_retry_rate.sleep();
	}

	return out_pose;
}

/**
 * [Optional]
 * To help construct initial belief to print debug informations in Logger
 */
State* WorldSimulator::GetCurrentState() {

	stateTracker->check_world_status();

	/* Get current car coordinates */

	tf::Stamped<tf::Pose> out_pose = GetBaseLinkPose();

	CarStruct updated_car;
	COORD coord;

	coord = poseToCoord(out_pose);

	logv << "======transformed pose = " << coord.x << " " << coord.y << endl;

	updated_car.pos = coord;
	updated_car.vel = real_speed_;
	updated_car.heading_dir = poseToHeadingDir(out_pose);

	stateTracker->updateCar(updated_car);

	PomdpStateWorld state = stateTracker->getPomdpWorldState();

	current_state.assign(state);

	if (logging::level() >= logging::VERBOSE){
		printf("GetCurrentState start");
		static_cast<PedPomdp*>(model_)->PrintWorldState(current_state);
		printf("GetCurrentState end");
	}
	current_state.time_stamp = Globals::ElapsedTime();

	logv << " current state time stamp " << current_state.time_stamp << endl;

	return static_cast<State*>(&current_state);
}

double WorldSimulator::StepReward(PomdpStateWorld& state, ACT_TYPE action) {
	double reward = 0.0;

	if (worldModel.isGlobalGoal(state.car)) {
		reward = ModelParams::GOAL_REWARD;
		return reward;
	}

	PedPomdp* pedpomdp_model = static_cast<PedPomdp*>(model_);

	if (state.car.vel > 0.001 && worldModel.inRealCollision(state, 120.0)) { /// collision occurs only when car is moving
		reward = pedpomdp_model->CrashPenalty(state);
		return reward;
	}

	// Smoothness control
	reward += pedpomdp_model->ActionPenalty(action);

	// Speed control: Encourage higher speed
	reward += pedpomdp_model->MovementPenalty(state,
			pedpomdp_model->GetSteering(action));
	return reward;
}

/**
 * [Essential]
 * send action, receive reward, obs, and terminal
 * @param action Action to be executed in the real-world system
 * @param obs    Observation sent back from the real-world system
 */
bool WorldSimulator::ExecuteAction(ACT_TYPE action, OBS_TYPE& obs) {

	if (action == -1)
		return false;
	logi << "ExecuteAction at the " << Globals::ElapsedTime()
			<< "th second" << endl;

	cout << "[ExecuteAction]" << endl;

	ros::spinOnce();

	cout << "[ExecuteAction] after spin" << endl;

	/* Update state */
	PomdpStateWorld* curr_state =
			static_cast<PomdpStateWorld*>(GetCurrentState());

	if (logging::level() >= logging::INFO)
		static_cast<PedPomdp*>(model_)->PrintWorldState(*curr_state);

	double acc;
	double steer;

	/* Reach goal: no more action sent */
	if (worldModel.isGlobalGoal(curr_state->car)) {
		cout
				<< "--------------------------- goal reached ----------------------------"
				<< endl;
		cout << "" << endl;
		// Stop the car after reaching goal
		acc = static_cast<PedPomdp*>(model_)->GetAccfromAccID(
				PedPomdp::ACT_DEC);
		steer = 0;
		action = static_cast<PedPomdp*>(model_)->GetActionID(steer, acc);
		goal_reached = true;
		//return true;
	}

	/* Collision: slow down the car */
	int collision_peds_id;

	// if( curr_state->car.vel > 0.001 * time_scale_ && worldModel.inCollision(*curr_state,collision_peds_id) ) {
	// 	cout << "--------------------------- pre-collision ----------------------------" << endl;
	// 	cout << "pre-col ped: " << collision_peds_id<<endl;
	// }

	if (curr_state->car.vel > 0.5 * time_scale_
			&& worldModel.inRealCollision(*curr_state, collision_peds_id,
					120.0)) {
		cout
				<< "--------------------------- collision = 1 ----------------------------"
				<< endl;
		cout << "collision ped: " << collision_peds_id << endl;

		acc = static_cast<PedPomdp*>(model_)->GetAccfromAccID(
				PedPomdp::ACT_DEC);
		steer = 0;
		action = static_cast<PedPomdp*>(model_)->GetActionID(steer, acc);

		DEBUG("Termination of episode due to coll.");
	}

	/* Publish action and step reward */

//	logv << "[WorldSimulator::"<<__FUNCTION__<<"] Publish action (for data collection)"<<endl;
//	publishAction(action, step_reward);
	logv << "[WorldSimulator::" << __FUNCTION__
			<< "] Update steering and target speed" << endl;

	/* Publish steering and target velocity to Unity */
	if (goal_reached == true) {
		//ros::shutdown();
		steering_ = 0;
		target_speed_ = real_speed_;

		target_speed_ -= ModelParams::AccSpeed;
		if (target_speed_ <= 0.0)
			target_speed_ = 0.0;

		buffered_action_ = static_cast<PedPomdp*>(model_)->GetActionID(0.0,
				-ModelParams::AccSpeed);

		// shutdown the node after reaching goal
		// TODO consider do this in simulaiton only for safety
		if (real_speed_ <= 0.01 * time_scale_) {
			cout << "After reaching goal: real_spead is already zero" << endl;
			ros::shutdown();
		}
		//return;
	} else if (stateTracker->emergency()) {
		steering_ = 0;
		target_speed_ = -1;

		buffered_action_ = static_cast<PedPomdp*>(model_)->GetActionID(0.0,
				-ModelParams::AccSpeed);

		cout
				<< "--------------------------- emergency ----------------------------"
				<< endl;
	} else if (worldModel.path.size() > 0
			&& COORD::EuclideanDistance(stateTracker->carpos,
					worldModel.path[0]) > 4.0) {
		cerr
				<< "=================== Path offset too high !!! Node shutting down"
				<< endl;
		ros::shutdown();
	} else {
		buffered_action_ = action;
	}

	// Updating cmd steering and speed. They will be send to vel_pulisher by timer
	update_cmds_naive(buffered_action_, false);

	publishCmdAction(buffered_action_);

	double step_reward = StepReward(*curr_state, buffered_action_);
	cout << "action **= " << buffered_action_ << endl;
	cout << "reward **= " << step_reward << endl;
	safeAction = action;

	/* Receive observation.
	 * Caution: obs info is not up-to-date here. Don't use it
	 */

	logv << "[WorldSimulator::" << __FUNCTION__ << "] Generate obs" << endl;
	obs = static_cast<PedPomdp*>(model_)->StateToIndex(GetCurrentState());

	return goal_reached;
}

void WorldSimulator::update_cmds_naive(ACT_TYPE action, bool buffered) {

	worldModel.path.text();

	cout << "[update_cmds_naive] Buffering action " << action << endl;

	float acc = static_cast<PedPomdp*>(model_)->GetAcceleration(action);

	double speed_step = ModelParams::AccSpeed / ModelParams::control_freq;

	target_speed_ = real_speed_;
	if (acc > 0.0)
		target_speed_ = real_speed_ + speed_step;
	else if (acc < 0.0)
		target_speed_ = real_speed_ - speed_step;

//	int level = real_speed_ / speed_step;
//	if (std::abs(real_speed_ - (level + 1) * speed_step) < speed_step * 0.2) {
//		level = level + 1;
//	}
//
//	int next_level = level;
//	if (acc > 0.0)
//		next_level = level + 1;
//	else if (acc < 0.0)
//		next_level = max(0, level - 1);
//
//	target_speed_ = next_level * speed_step;
//	if (acc == 0.0)
//		target_speed_ = real_speed_;

	target_speed_ = max(min(target_speed_, ModelParams::VEL_MAX),0.0);

	steering_ = static_cast<PedPomdp*>(model_)->GetSteering(action);

	cerr << "DEBUG: Executing action:" << action << " steer/acc = " << steering_
			<< "/" << acc << endl;
}

void WorldSimulator::AddObstacle() {

	if (obstacle_file_name_ == "null") {
		/// for indian_cross
		cout << "Using default obstacles " << endl;

		std::vector<RVO::Vector2> obstacle[4];

		obstacle[0].push_back(RVO::Vector2(-4, -4));
		obstacle[0].push_back(RVO::Vector2(-30, -4));
		obstacle[0].push_back(RVO::Vector2(-30, -30));
		obstacle[0].push_back(RVO::Vector2(-4, -30));

		obstacle[1].push_back(RVO::Vector2(4, -4));
		obstacle[1].push_back(RVO::Vector2(4, -30));
		obstacle[1].push_back(RVO::Vector2(30, -30));
		obstacle[1].push_back(RVO::Vector2(30, -4));

		obstacle[2].push_back(RVO::Vector2(4, 4));
		obstacle[2].push_back(RVO::Vector2(30, 4));
		obstacle[2].push_back(RVO::Vector2(30, 30));
		obstacle[2].push_back(RVO::Vector2(4, 30));

		obstacle[3].push_back(RVO::Vector2(-4, 4));
		obstacle[3].push_back(RVO::Vector2(-4, 30));
		obstacle[3].push_back(RVO::Vector2(-30, 30));
		obstacle[3].push_back(RVO::Vector2(-30, 4));

		int NumThreads = 1/*Globals::config.NUM_THREADS*/;

		for (int tid = 0; tid < NumThreads; tid++) {
			for (int i = 0; i < 4; i++) {
				worldModel.traffic_agent_sim_[tid]->addObstacle(obstacle[i]);
			}

			/* Process the obstacles so that they are accounted for in the simulation. */
			worldModel.traffic_agent_sim_[tid]->processObstacles();
		}
	} else {
		cout << "Skipping obstacle file " << obstacle_file_name_ << endl;
		return;

		cout << "Using obstacle file " << obstacle_file_name_ << endl;

		int NumThreads = 1/*Globals::config.NUM_THREADS*/;

		ifstream file;
		file.open(obstacle_file_name_, std::ifstream::in);

		if (file.fail()) {
			cout << "open obstacle file failed !!!!!!" << endl;
			return;
		}

		std::vector<RVO::Vector2> obstacles[100];

		std::string line;
		int obst_num = 0;
		while (std::getline(file, line)) {
			std::istringstream iss(line);

			double x;
			double y;
			cout << "obstacle shape: ";
			while (iss >> x >> y) {
				cout << x << " " << y << " ";
				obstacles[obst_num].push_back(RVO::Vector2(x, y));
			}
			cout << endl;

			for (int tid = 0; tid < NumThreads; tid++) {
				worldModel.traffic_agent_sim_[tid]->addObstacle(
						obstacles[obst_num]);
			}

			worldModel.AddObstacle(obstacles[obst_num]);

			obst_num++;
			if (obst_num > 99)
				break;
		}

		for (int tid = 0; tid < NumThreads; tid++) {
			worldModel.traffic_agent_sim_[tid]->processObstacles();
		}

		file.close();
	}

}

void WorldSimulator::publishCmdAction(const ros::TimerEvent &e) {
	// for timer
	publishCmdAction(buffered_action_);
}

void WorldSimulator::publishCmdAction(ACT_TYPE action) {
	msg_builder::PomdpCmd cmd;
	cmd.target_speed = target_speed_;
	cmd.cur_speed = real_speed_;
	cmd.acc = static_cast<PedPomdp*>(model_)->GetAcceleration(action);
	cmd.steer = steering_; // GetSteering returns radii value
	cout << "[publishCmdAction] time stamp"
			<< Globals::ElapsedTime() << endl;
	cout << "[publishCmdAction] target speed " << target_speed_ << " steering "
			<< steering_ << endl;
	cmdPub_.publish(cmd);
}

void WorldSimulator::publishROSState() {
	geometry_msgs::Point32 pnt;

	geometry_msgs::Pose pose;

	geometry_msgs::PoseStamped pose_stamped;
	pose_stamped.header.stamp = ros::Time::now();

	pose_stamped.header.frame_id = global_frame_id;

	pose_stamped.pose.position.x = (stateTracker->carpos.x + 0.0);
	pose_stamped.pose.position.y = (stateTracker->carpos.y + 0.0);
	pose_stamped.pose.orientation.w = 1.0;
	car_pub.publish(pose_stamped);

	geometry_msgs::PoseArray pA;
	pA.header.stamp = ros::Time::now();

	pA.header.frame_id = global_frame_id;
	for (int i = 0; i < stateTracker->ped_list.size(); i++) {
		//GetCurrentState(ped_list[i]);
		pose.position.x = (stateTracker->ped_list[i].w + 0.0);
		pose.position.y = (stateTracker->ped_list[i].h + 0.0);
		pose.orientation.w = 1.0;
		pA.poses.push_back(pose);
	}
	for (auto& veh : stateTracker->veh_list) {
		//GetCurrentState(ped_list[i]);
		pose.position.x = (veh.w + 0.0);
		pose.position.y = (veh.h + 0.0);
		pose.orientation.w = veh.heading_dir;
		pA.poses.push_back(pose);
	}

	pa_pub.publish(pA);

	uint32_t shape = visualization_msgs::Marker::CYLINDER;

	if (worldModel.goal_mode == "goal") {

		visualization_msgs::MarkerArray markers;

		for (int i = 0; i < worldModel.goals.size(); i++) {
			visualization_msgs::Marker marker;

			marker.header.frame_id = ModelParams::rosns + "/map";
			marker.header.stamp = ros::Time::now();
			marker.ns = "basic_shapes";
			marker.id = i;
			marker.type = shape;
			marker.action = visualization_msgs::Marker::ADD;

			marker.pose.position.x = worldModel.goals[i].x;
			marker.pose.position.y = worldModel.goals[i].y;
			marker.pose.position.z = 0;
			marker.pose.orientation.x = 0.0;
			marker.pose.orientation.y = 0.0;
			marker.pose.orientation.z = 0.0;
			marker.pose.orientation.w = 1.0;

			marker.scale.x = 1;
			marker.scale.y = 1;
			marker.scale.z = 1;
			marker.color.r = marker_colors[i][0];
			marker.color.g = marker_colors[i][1];
			marker.color.b = marker_colors[i][2];
			marker.color.a = 1.0;

			markers.markers.push_back(marker);
		}
		goal_pub.publish(markers);
	}
}

extern double marker_colors[20][3];
void WorldSimulator::publishAction(int action, double reward) {
	if (action == -1)
		return;
	logv << "[WorldSimulator::" << __FUNCTION__ << "] Prepare markers" << endl;

	uint32_t shape = visualization_msgs::Marker::CUBE;
	visualization_msgs::Marker marker;

	marker.header.frame_id = global_frame_id;
	marker.header.stamp = ros::Time::now();
	marker.ns = "basic_shapes";
	marker.id = 0;
	marker.type = shape;
	marker.action = visualization_msgs::Marker::ADD;
	logv << "[WorldSimulator::" << __FUNCTION__ << "] Prepare markers check 1"
			<< endl;
	double px, py;
	px = stateTracker->carpos.x;
	py = stateTracker->carpos.y;
	marker.pose.position.x = px + 1;
	marker.pose.position.y = py + 1;
	marker.pose.position.z = 0;
	marker.pose.orientation.x = 0.0;
	marker.pose.orientation.y = 0.0;
	marker.pose.orientation.z = 0.0;
	marker.pose.orientation.w = 1.0;
	logv << "[WorldSimulator::" << __FUNCTION__ << "] Prepare markers check 2"
			<< endl;

	// Set the scale of the marker in meters
	marker.scale.x = 0.6;
	marker.scale.y = 3;
	marker.scale.z = 0.6;

	int aid = action;
	aid = max(aid, 0) % 3;
	logv << "[WorldSimulator::" << __FUNCTION__ << "] color id" << aid
			<< " action " << action << endl;

	marker.color.r = marker_colors[action_map[aid]][0];
	marker.color.g = marker_colors[action_map[aid]][1];
	marker.color.b = marker_colors[action_map[aid]][2];
	marker.color.a = 1.0;

	ros::Duration d(1 / ModelParams::control_freq);
	logv << "[WorldSimulator::" << __FUNCTION__ << "] Publish marker" << endl;
	marker.lifetime = d;
	actionPub_.publish(marker);
	logv << "[WorldSimulator::" << __FUNCTION__ << "] Publish action comand"
			<< endl;
	geometry_msgs::Twist action_cmd;
	action_cmd.linear.x = action;
	action_cmd.linear.y = reward;
	actionPubPlot_.publish(action_cmd);
}

bool WorldSimulator::getObjectPose(string target_frame,
		tf::Stamped<tf::Pose>& in_pose, tf::Stamped<tf::Pose>& out_pose) const {
	out_pose.setIdentity();

	logv << "laser frame " << in_pose.frame_id_ << endl;

	logv << "getting transform of frame " << target_frame << endl;

	try {
		tf_.transformPose(target_frame, in_pose, out_pose);
	} catch (tf::LookupException& ex) {
		ROS_ERROR("No Transform available Error: %s\n", ex.what());
		return false;
	} catch (tf::ConnectivityException& ex) {
		ROS_ERROR("Connectivity Error: %s\n", ex.what());
		return false;
	} catch (tf::ExtrapolationException& ex) {
		ROS_ERROR("Extrapolation Error: %s\n", ex.what());
		return false;
	}
	return true;
}

//=============== from pedpomdpnode ==============

void WorldSimulator::speedCallback(nav_msgs::Odometry odo) {

//	DEBUG(string_sprintf(" ts = %f", Globals::ElapsedTime()));

	odom_vel_ = COORD(odo.twist.twist.linear.x, odo.twist.twist.linear.y);
	// real_speed_=sqrt(odo.twist.twist.linear.x * odo.twist.twist.linear.x + 
	// odo.twist.twist.linear.y * odo.twist.twist.linear.y);
	odom_heading_ = tf::getYaw(odo.pose.pose.orientation);

	COORD along_dir(cos(odom_heading_), sin(odom_heading_));

	real_speed_ = odom_vel_.dot(along_dir);

	tf::Stamped<tf::Pose> out_pose = GetBaseLinkPose();
	baselink_heading_ = poseToHeadingDir(out_pose);

	stateTracker->car_odom_heading = baselink_heading_;

	if (real_speed_ > ModelParams::VEL_MAX * 1.3) {
		ERR(string_sprintf("Unusual car vel (too large): %f", real_speed_));
	}
}

bool sortFn(Pedestrian p1, Pedestrian p2) {
	return p1.id < p2.id;
}


//void laneCallback(msg_builder::Lanes data){
//	double data_time_sec = Globals::ElapsedTime();
//	DEBUG(
//			string_sprintf("receive %d lanes at time %f",
//					data.lane_segments.size(), Globals::ElapsedTime()));
//}
//
//void obstacleCallback(msg_builder::Obstacles data){
//	double data_time_sec = Globals::ElapsedTime();
//	DEBUG(
//			string_sprintf("receive %d obstacle contours at time %f",
//					data.contours.size(), Globals::ElapsedTime()));
//}

void agentArrayCallback(msg_builder::TrafficAgentArray data) {

	double data_sec = data.header.stamp.sec;  // std_msgs::time
	double data_nsec = data.header.stamp.nsec;

	double data_ros_time_sec = data_sec + data_nsec * 1e-9;

	double data_time_sec = Globals::ElapsedTime();

	WorldSimulator::stateTracker->latest_time_stamp = data_time_sec;

	DEBUG(
			string_sprintf("receive agent num %d at time %f",
					data.agents.size(), Globals::ElapsedTime()));

	vector<Pedestrian> ped_list;
	vector<Vehicle> veh_list;

	for (msg_builder::TrafficAgent& agent : data.agents) {
		std::string agent_type = agent.type;
		// cout << "get agent: " << agent.id <<" "<< agent_type << endl;

		if (agent_type == "car" || agent_type == "bike") {
			Vehicle world_veh;

			world_veh.time_stamp = data_time_sec;
			world_veh.ros_time_stamp = data_ros_time_sec;
			world_veh.id = agent.id;
			world_veh.w = agent.pose.position.x;
			world_veh.h = agent.pose.position.y;
			world_veh.heading_dir = navposeToHeadingDir(agent.pose);

			for (auto& corner : agent.bbox.points) {
				world_veh.bb.emplace_back(corner.x, corner.y);
			}

			veh_list.push_back(world_veh);
		} else if (agent_type == "ped") {
			Pedestrian world_ped;

			world_ped.time_stamp = data_time_sec;
			world_ped.ros_time_stamp = data_ros_time_sec;
			world_ped.id = agent.id;
			world_ped.w = agent.pose.position.x;
			world_ped.h = agent.pose.position.y;

			ped_list.push_back(world_ped);
		} else {
			ERR(string_sprintf("Unsupported type %s", agent_type));
		}
	}

	for (auto& ped : ped_list) {
		WorldSimulator::stateTracker->updatePedState(ped);
	}

	for (auto& veh : veh_list) {
		WorldSimulator::stateTracker->updateVehState(veh);
	}

	WorldSimulator::stateTracker->cleanAgents();

//	DEBUG(
//				string_sprintf("Finish agent update at time %f",
//						Globals::ElapsedTime()));

	WorldSimulator::stateTracker->model.print_path_map();

	WorldSimulator::stateTracker->text(ped_list);
	WorldSimulator::stateTracker->text(veh_list);

	// DEBUG(string_sprintf("ped_list len %d", ped_list.size()));
	// DEBUG(string_sprintf("veh_list len %d", veh_list.size()));
//	logv << "====================[ agentArrayCallback end ]================="
//			<< endl;

	SimulatorBase::agents_data_ready = true;
}

void agentPathArrayCallback(msg_builder::AgentPathArray data) {
	double data_sec = data.header.stamp.sec;  // std_msgs::time
	double data_nsec = data.header.stamp.nsec;

	double data_time_sec = data_sec + data_nsec * 1e-9;

	WorldSimulator::stateTracker->latest_path_time_stamp = data_time_sec;

	DEBUG(
			string_sprintf("receive agent num %d at time %f",
					data.agents.size(), Globals::ElapsedTime()));

	vector<Pedestrian> ped_list;
	vector<Vehicle> veh_list;

	for (msg_builder::AgentPaths& agent : data.agents) {
		std::string agent_type = agent.type;

		if (agent_type == "car" || agent_type == "bike") {
			Vehicle world_veh;

			world_veh.time_stamp = data_time_sec;
			world_veh.id = agent.id;
			world_veh.reset_intention = agent.reset_intention;
			for (auto& nav_path : agent.path_candidates) {
				Path new_path;
				world_veh.paths.emplace_back(new_path);
				for (auto& pose : nav_path.poses) {
					world_veh.paths.back().emplace_back(pose.pose.position.x,
							pose.pose.position.y);
				}
			}

			// cout << "vehicle has "<< agent.path_candidates.size() << " path_candidates" << endl;
			veh_list.push_back(world_veh);
		} else if (agent_type == "ped") {
			Pedestrian world_ped;

			world_ped.time_stamp = data_time_sec;
			world_ped.id = agent.id;
			world_ped.reset_intention = agent.reset_intention;
			world_ped.cross_dir = agent.cross_dirs[0];
			for (auto& nav_path : agent.path_candidates) {
				Path new_path;
				world_ped.paths.emplace_back(new_path);
				for (auto& pose : nav_path.poses) {
					world_ped.paths.back().emplace_back(pose.pose.position.x,
							pose.pose.position.y);
				}
			}

			ped_list.push_back(world_ped);
		} else {
			ERR(string_sprintf("Unsupported type %s", agent_type));
		}
	}

	for (auto& ped : ped_list) {
		WorldSimulator::stateTracker->updatePedPaths(ped);
	}

	for (auto& veh : veh_list) {
		WorldSimulator::stateTracker->updateVehPaths(veh);
	}

//	DEBUG(
//			string_sprintf("Finish agent paths update at time %f",
//					Globals::ElapsedTime()));

	SimulatorBase::agents_path_data_ready = true;
}

void pedPoseCallback(msg_builder::ped_local_frame_vector lPedLocal) {
	logv << "======================[ pedPoseCallback ]= ts "
			<< Globals::ElapsedTime() << " ==================" << endl;

	DEBUG(string_sprintf("receive peds num %d", lPedLocal.ped_local.size()));

	if (lPedLocal.ped_local.size() == 0)
		return;

	vector<Pedestrian> ped_list;
	for (int ii = 0; ii < lPedLocal.ped_local.size(); ii++) {
		Pedestrian world_ped;
		msg_builder::ped_local_frame ped = lPedLocal.ped_local[ii];
		world_ped.id = ped.ped_id;
		world_ped.w = ped.ped_pose.x;
		world_ped.h = ped.ped_pose.y;
		ped_list.push_back(world_ped);
	}

	for (int i = 0; i < ped_list.size(); i++) {
		WorldSimulator::stateTracker->updatePed(ped_list[i]);
	}

	logv << "====================[ pedPoseCallback end ]================="
			<< endl;

	SimulatorBase::agents_data_ready = true;
}

void receive_map_callback(nav_msgs::OccupancyGrid map) {
	logi << "[receive_map_callback] ts: " << Globals::ElapsedTime() << endl;

	raw_map_ = map;

	logi << "[receive_map_callback] end ts: " << Globals::ElapsedTime() << endl;
}

void WorldSimulator::ego_car_dead_callback(std_msgs::Bool::ConstPtr data){

	ERR("Ego car is dead, reached the edge of the current map");
}

geometry_msgs::PoseStamped WorldSimulator::getPoseAhead(
		const tf::Stamped<tf::Pose>& carpose) {
	static int last_i = -1;
	static double last_yaw = 0;
	static COORD last_ahead;
	auto& path = worldModel.path;
	COORD coord = poseToCoord(carpose);

	int i = path.nearest(coord);
	COORD ahead;
	double yaw;

	if (i == last_i) {
		yaw = last_yaw;
		ahead = last_ahead;
	} else {
		int j = path.forward(i, pathplan_ahead_);
		//yaw = (path.getYaw(j) + path.getYaw(j-1) + path.getYaw(j-2)) / 3;
		yaw = path.getYaw(j);
		ahead = path[j];
		last_yaw = yaw;
		last_ahead = ahead;
		last_i = i;
	}

	auto q = tf::createQuaternionFromYaw(yaw);
	tf::Pose p(q, tf::Vector3(ahead.x, ahead.y, 0));
	tf::Stamped<tf::Pose> pose_ahead(p, carpose.stamp_, carpose.frame_id_);
	geometry_msgs::PoseStamped posemsg;
	tf::poseStampedTFToMsg(pose_ahead, posemsg);
	return posemsg;
}

double xylength(geometry_msgs::Point32 p) {
	return sqrt(p.x * p.x + p.y * p.y);
}
bool car_data_ready = false;

void WorldSimulator::ego_dead_callback(const std_msgs::Bool ego_dead){
	ERR("Ego vehicle killed in ego_vehicle.py");
}

void WorldSimulator::update_ego_car(const msg_builder::car_info::ConstPtr car) {

	if (true) {
		const msg_builder::car_info& ego_car = *car;
		ModelParams::CAR_FRONT = COORD(
				ego_car.front_axle_center.x - ego_car.car_pos.x,
				ego_car.front_axle_center.y - ego_car.car_pos.y).Length();
		ModelParams::CAR_REAR = COORD(
				ego_car.rear_axle_center.y - ego_car.car_pos.y,
				ego_car.rear_axle_center.y - ego_car.car_pos.y).Length();
		ModelParams::CAR_WHEEL_DIST = ModelParams::CAR_FRONT
				+ ModelParams::CAR_REAR;

		ModelParams::CAR_WIDTH = 0;
		ModelParams::CAR_LENGTH = 0;
		float car_yaw = ego_car.car_yaw;

		if (fabs(car_yaw - odom_heading_) > 0.6
				&& fabs(car_yaw - odom_heading_) < 2 * M_PI - 0.6)
			if (odom_heading_ != 0)
				DEBUG(string_sprintf(
						"Il topic car_yaw incorrect: %f , truth %f",
						car_yaw, odom_heading_));

		COORD tan_dir(-sin(car_yaw), cos(car_yaw));
		COORD along_dir(cos(car_yaw), sin(car_yaw));
		for (auto& point : ego_car.car_bbox.points) {
			COORD p(point.x - ego_car.car_pos.x, point.y - ego_car.car_pos.y);
			double proj = p.dot(tan_dir);
			ModelParams::CAR_WIDTH = max(ModelParams::CAR_WIDTH, fabs(proj));
			proj = p.dot(along_dir);
			ModelParams::CAR_LENGTH = max(ModelParams::CAR_LENGTH, fabs(proj));
		}
		ModelParams::CAR_WIDTH = ModelParams::CAR_WIDTH * 2;
		ModelParams::CAR_LENGTH = ModelParams::CAR_LENGTH * 2;
		ModelParams::CAR_FRONT = ModelParams::CAR_LENGTH / 2.0;

		car_data_ready = true;
	}
}


void WorldSimulator::setCarGoal(COORD car_goal) {
	worldModel.car_goal = car_goal;
}
