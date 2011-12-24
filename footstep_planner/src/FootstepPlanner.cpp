// SVN $HeadURL$
// SVN $Id$

/*
 * A footstep planner for humanoid robots
 *
 * Copyright 2010-2011 Johannes Garimort, Armin Hornung, University of Freiburg
 * http://www.ros.org/wiki/footstep_planner
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <footstep_planner/FootstepPlanner.h>
#include <time.h>


namespace footstep_planner
{
    FootstepPlanner::FootstepPlanner()
        : ivStartPoseSetUp(false),
          ivGoalPoseSetUp(false),
          ivPlanExists(false),
          ivLastMarkerMsgSize(0),
          ivPathCost(0),
          ivMarkerNamespace("")
    {
        // private NodeHandle for parameters and private messages (debug / info)
        ros::NodeHandle nh_private("~");
        ros::NodeHandle nh_public;

        // ..publishers
        ivExpandedStatesVisPub = nh_private.advertise<
                sensor_msgs::PointCloud>("expanded_states", 1);
        ivFootstepPathVisPub = nh_private.advertise<
                visualization_msgs::MarkerArray>("footsteps_array", 1);
        ivHeuristicPathVisPub = nh_private.advertise<
                nav_msgs::Path>("heuristic_path", 1);
        ivPathVisPub = nh_private.advertise<nav_msgs::Path>("path", 1);
        ivStartPoseVisPub = nh_private.advertise<
                geometry_msgs::PoseStamped>("start", 1);

        // TODO: remove later
        ivChangedStatesVisPub = nh_private.advertise<
                sensor_msgs::PointCloud>("changed_states", 1);

        int max_hash_size;
        int changed_cells_limit;
        std::string heuristic_type;
        double step_cost;
        double diff_angle_cost;

        // read parameters from config file:
        // - planner environment settings
        nh_private.param("heuristic_type", heuristic_type,
        		std::string("EuclideanHeuristic"));
        nh_private.param("max_hash_size", max_hash_size, 65536);
        nh_private.param("accuracy/collision_check", ivCollisionCheckAccuracy, 2);
        nh_private.param("accuracy/cell_size", ivCellSize, 0.01);
        nh_private.param("accuracy/num_angle_bins", ivNumAngleBins, 64);
        nh_private.param("step_cost", step_cost, 0.05);
        nh_private.param("diff_angle_cost", diff_angle_cost, 0.0);

        nh_private.param("planner_type", ivPlannerType,
        		std::string("ARAPlanner"));
        nh_private.param("search_until_first_solution",
        		ivSearchUntilFirstSolution, false);
        nh_private.param("allocated_time", ivMaxSearchTime, 7.0);
        nh_private.param("forward_search", ivForwardSearch, false);
        nh_private.param("initial_epsilon", ivInitialEpsilon, 3.0);
        nh_private.param("changed_cells_limit", changed_cells_limit, 5000);
		ivChangedCellsLimit = (unsigned int) changed_cells_limit;

        // - footstep settings
        nh_private.param("foot/size/x", ivFootsizeX, 0.16);
        nh_private.param("foot/size/y", ivFootsizeY, 0.06);
        nh_private.param("foot/size/z", ivFootsizeZ, 0.015);
        nh_private.param("foot/separation", ivFootSeparation, 0.095);
        nh_private.param("foot/origin_shift/x", ivOriginFootShiftX, 0.02);
        nh_private.param("foot/origin_shift/y", ivOriginFootShiftY, 0.0);
        nh_private.param("foot/max/step/x", ivMaxFootstepX, 0.04);
        nh_private.param("foot/max/step/y", ivMaxFootstepY, 0.04);
        nh_private.param("foot/max/step/theta", ivMaxFootstepTheta, 0.349);
        nh_private.param("foot/max/inverse/step/x", ivMaxInvFootstepX, 0.04);
        nh_private.param("foot/max/inverse/step/y", ivMaxInvFootstepY, 0.01);
        nh_private.param("foot/max/inverse/step/theta", ivMaxInvFootstepTheta, 0.05);

        // - footstep discretisation
        XmlRpc::XmlRpcValue discretization_list_x;
        XmlRpc::XmlRpcValue discretization_list_y;
        XmlRpc::XmlRpcValue discretization_list_theta;
        nh_private.getParam("footsteps/x", discretization_list_x);
        nh_private.getParam("footsteps/y", discretization_list_y);
        nh_private.getParam("footsteps/theta", discretization_list_theta);
        if (discretization_list_x.getType() != XmlRpc::XmlRpcValue::TypeArray)
            ROS_ERROR("Error reading footsteps/x from config file.");
        if (discretization_list_y.getType() != XmlRpc::XmlRpcValue::TypeArray)
            ROS_ERROR("Error reading footsteps/y from config file.");
        if (discretization_list_theta.getType() != XmlRpc::XmlRpcValue::TypeArray)
            ROS_ERROR("Error reading footsteps/theta from config file.");
        // check if received footstep discretization is valid
        int size, size_y, size_t;
        try
        {
            size = discretization_list_x.size();
            size_y = discretization_list_y.size();
            size_t = discretization_list_theta.size();

            if (size != size_y || size != size_t)
            {
                ROS_ERROR("Footstep parameterization has different sizes for x/y/theta, exiting.");
                exit(0);
            }
        }
        catch (const XmlRpc::XmlRpcException& e)
        {
            ROS_ERROR("No footstep parameterization available, exiting.");
            exit(0);
        }

        // create footstep set
        ivFootstepSet.clear();
        double max_step_width = 0;
        for(int i=0; i < size; i++)
        {
            double x = (double)discretization_list_x[i];
            double y = (double)discretization_list_y[i];
            double theta = (double)discretization_list_theta[i];

            Footstep f(x, y, theta, ivCellSize, ivNumAngleBins, max_hash_size,
                       ivFootSeparation);
            ivFootstepSet.push_back(f);

            double cur_step_width = sqrt(x*x + y*y);

            if (cur_step_width > max_step_width)
                max_step_width = cur_step_width;
        }

        // discretise planner settings
        int max_footstep_x = cont_2_disc(ivMaxFootstepX, ivCellSize);
        int max_footstep_y = cont_2_disc(ivMaxFootstepY, ivCellSize);
        int max_footstep_theta = angle_state_2_cell(ivMaxFootstepTheta,
                                                   ivNumAngleBins);
        int max_inv_footstep_x = cont_2_disc(ivMaxInvFootstepX,
                                                      ivCellSize);
        int max_inv_footstep_y = cont_2_disc(ivMaxInvFootstepY,
                                                      ivCellSize);
        int max_inv_footstep_theta = angle_state_2_cell(ivMaxInvFootstepTheta,
                                                       ivNumAngleBins);

        // initialize the heuristic
        boost::shared_ptr<Heuristic> h;
        if (heuristic_type == "EuclideanHeuristic")
        {
        	h.reset(new EuclideanHeuristic(ivCellSize, ivNumAngleBins));
        	ROS_INFO("FootstepPlanner heuristic: euclidean distance");
        }
        else if(heuristic_type == "EuclStepCostHeuristic")
        {
            h.reset(new EuclStepCostHeuristic(ivCellSize, ivNumAngleBins,
                                              step_cost, diff_angle_cost,
                                              max_step_width));
            ROS_INFO("FootstepPlanner heuristic: euclidean distance with step "
                     "costs");
        }
        else if (heuristic_type == "PathCostHeuristic")
        {
            h.reset(new PathCostHeuristic(ivCellSize, ivNumAngleBins, step_cost,
                                          diff_angle_cost, max_step_width));
            ROS_INFO("FootstepPlanner heuristic: 2D path euclidean distance "
                     "with step costs");
            // keep a local ptr for visualization
            ivPathCostHeuristicPtr = boost::dynamic_pointer_cast<PathCostHeuristic>(h);
        }
        else
        {
            ROS_ERROR_STREAM("Heuristic " << heuristic_type << " not available,"
                             " exiting.");
            exit(1);
        }

        // initialize the planner environment
        ivPlannerEnvironmentPtr.reset(
                new FootstepPlannerEnvironment(ivFootstepSet,
                                               h,
                                               ivFootSeparation,
                                               ivOriginFootShiftX,
                                               ivOriginFootShiftY,
                                               ivFootsizeX,
                                               ivFootsizeY,
                                               max_footstep_x,
                                               max_footstep_y,
                                               max_footstep_theta,
                                               max_inv_footstep_x,
                                               max_inv_footstep_y,
                                               max_inv_footstep_theta,
                                               step_cost,
                                               ivCollisionCheckAccuracy,
                                               max_hash_size,
                                               ivCellSize,
                                               ivNumAngleBins,
                                               ivForwardSearch));

        // set up planner
        if (ivPlannerType == "ARAPlanner" || ivPlannerType == "ADPlanner"
        		|| ivPlannerType == "RSTARPlanner"){
            ROS_INFO_STREAM("Planning with " << ivPlannerType);
        }
        else
        {
            ROS_ERROR_STREAM("Planner "<< ivPlannerType <<" not available / "
                             "untested.");
            exit(1);
        }
        if (ivForwardSearch)
            ROS_INFO_STREAM("Search direction: forward planning");
        else
            ROS_INFO_STREAM("Search direction: backward planning");
        setupPlanner();


////TODO: remove when finished
//        PlanningState cur(103, 12, 65, LEFT, ivCellSize, ivNumAngleBins,
//                          max_hash_size);
//        State cur_state;
//        cur_state.x = cell_2_state(cur.getX(), ivCellSize);
//        cur_state.y = cell_2_state(cur.getY(), ivCellSize);
//        cur_state.theta = angle_cell_2_state(cur.getTheta(), ivNumAngleBins);
//        cur_state.leg = cur.getLeg();
//        Footstep fs(0.1, 0.5, -M_PI/8, ivCellSize, ivNumAngleBins, max_hash_size,
//                    ivFootSeparation);
//        PlanningState suc = fs.performMeOnThisState(cur);
//        double suc_state_x = cell_2_state(suc.getX(), ivCellSize);
//        double suc_state_y = cell_2_state(suc.getY(), ivCellSize);
//        double suc_state_theta = angle_cell_2_state(suc.getTheta(),
//                                                    ivNumAngleBins);
//        PlanningState pred = fs.revertMeOnThisState(suc);
//        double pred_state_x = cell_2_state(pred.getX(), ivCellSize);
//        double pred_state_y = cell_2_state(pred.getY(), ivCellSize);
//        double pred_state_theta = angle_cell_2_state(pred.getTheta(),
//                                                     ivNumAngleBins);
////        double suc_footstep_x, suc_footstep_y, suc_footstep_theta;
////        get_footstep(cur.getLeg(), ivFootSeparation,
////                     cur_state.x, cur_state.y, cur_state.theta,
////                     suc_state_x, suc_state_y, suc_state_theta,
////                     suc_footstep_x, suc_footstep_y, suc_footstep_theta);
////        int disc_suc_footstep_x = cont_2_disc(suc_footstep_x, ivCellSize);
////        int disc_suc_footstep_y = cont_2_disc(suc_footstep_y, ivCellSize);
////        int disc_suc_footstep_theta = angle_state_2_cell(suc_footstep_theta,
////                                                         ivNumAngleBins);
//        ROS_INFO("from: x=%f, y=%f, theta=%f (%i, %i, %i)",
//                 cur_state.x, cur_state.y, cur_state.theta,
//                 cur.getX(), cur.getY(), cur.getTheta());
//        ROS_INFO("to: x=%f, y=%f, theta=%f (%i, %i, %i)",
//                 suc_state_x, suc_state_y, suc_state_theta,
//                 suc.getX(), suc.getY(), suc.getTheta());
////        ROS_INFO("footstep (from->to): x=%f, y=%f, theta=%f (%i, %i, %i)",
////                 suc_footstep_x, suc_footstep_y, suc_footstep_theta,
////                 disc_suc_footstep_x, disc_suc_footstep_y,
////                 disc_suc_footstep_theta);
////        ROS_INFO("performable? %i", performable(
////                 disc_suc_footstep_x, disc_suc_footstep_y,
////                 disc_suc_footstep_theta,
////                 max_footstep_x, max_footstep_y, max_footstep_theta,
////                 max_inv_footstep_x, max_inv_footstep_y, max_inv_footstep_theta,
////                 ivNumAngleBins, cur.getLeg()));
//        ROS_INFO("pred: x=%f, y=%f, theta=%f (%i, %i, %i)",
//                pred_state_x, pred_state_y, pred_state_theta,
//                pred.getX(), pred.getY(), pred.getTheta());
//        exit(0);
    }


    FootstepPlanner::~FootstepPlanner()
    {}


    void
    FootstepPlanner::setupPlanner()
    {
        if (ivPlannerType == "ARAPlanner")
        {
            ivPlannerPtr.reset(new ARAPlanner(ivPlannerEnvironmentPtr.get(),
                                              ivForwardSearch));
        }
        else if (ivPlannerType == "ADPlanner")
        {
            ivPlannerPtr.reset(new ADPlanner(ivPlannerEnvironmentPtr.get(),
                                             ivForwardSearch));
        }
        else if (ivPlannerType == "RSTARPlanner")
        {
            ivPlannerPtr.reset(new RSTARPlanner(ivPlannerEnvironmentPtr.get(),
                                                ivForwardSearch));
        }
    }


    bool
    FootstepPlanner::run()
    {
        ROS_DEBUG("Setting up environment");
		ivPlannerEnvironmentPtr->setUp(ivStartFootLeft, ivStartFootRight,
									   ivGoalFootLeft, ivGoalFootRight);
		ROS_DEBUG("Setting up environment done");

        int ret = 0;
        MDPConfig mdp_config;
        std::vector<int> solution_state_ids;

        // NOTE: just for the sake of completeness since this method is
        // currently doing nothing
        ivPlannerEnvironmentPtr->InitializeEnv(NULL);
        ivPlannerEnvironmentPtr->InitializeMDPCfg(&mdp_config);

        // set up planner
        if (ivPlannerPtr->set_start(mdp_config.startstateid) == 0)
        {
            ROS_ERROR("Failed to set start state.");
            return false;
        }
        if (ivPlannerPtr->set_goal(mdp_config.goalstateid) == 0)
        {
            ROS_ERROR("Failed to set goal state\n");
            return false;
        }

        ivPlannerPtr->set_initialsolution_eps(ivInitialEpsilon);
        ivPlannerPtr->set_search_mode(ivSearchUntilFirstSolution);

        ROS_INFO("Start planning (max time: %f, initial eps: %f (%f))\n",
                 ivMaxSearchTime, ivInitialEpsilon, ivPlannerPtr->get_initial_eps());
        int path_cost;
        ros::WallTime startTime = ros::WallTime::now();
        ret = ivPlannerPtr->replan(ivMaxSearchTime, &solution_state_ids,
                                   &path_cost);
        ivPathCost = double(path_cost) / FootstepPlannerEnvironment::cvMmScale;

        ivPlannerEnvironmentPtr->printHashStatistics();

        if (ret && solution_state_ids.size() > 0)
        {
            ROS_INFO("Solution of size %zu found after %f s",
                     solution_state_ids.size(),
            		 (ros::WallTime::now()-startTime).toSec());

            ivPlanExists = extractSolution(solution_state_ids);
            broadcastExpandedNodesVis();

            if (!ivPlanExists)
            {
                ROS_ERROR("extracting path failed\n\n");
                return false;
            }

            ROS_INFO("Expanded states: %i total / %i new",
                     ivPlannerEnvironmentPtr->getNumExpandedStates(),
                     ivPlannerPtr->get_n_expands());
            ROS_INFO("Final eps: %f", ivPlannerPtr->get_final_epsilon());
            ROS_INFO("Path cost: %f (%i)", ivPathCost, path_cost);

            broadcastFootstepPathVis();
            broadcastPathVis();

            return true;
        }
        else
        {
        	ROS_ERROR("No solution found");
            return false;
        }
    }


    bool
    FootstepPlanner::extractSolution(const std::vector<int>& state_ids)
    {
        ivPath.clear();

        State s;
        for(unsigned i = 0; i < state_ids.size(); ++i)
        {
            bool success = ivPlannerEnvironmentPtr->getState(state_ids[i], &s);
            if (!success)
            {
                ivPath.clear();
                return false;
            }
            ivPath.push_back(s);
        }

        return true;
    }


    bool
    FootstepPlanner::plan()
    {
    	if (!ivMapPtr)
    	{
    		ROS_ERROR("FootstepPlanner has no map yet for planning");
    		return false;
    	}
        if (!ivGoalPoseSetUp || !ivStartPoseSetUp)
        {
            ROS_ERROR("FootstepPlanner has no start or goal pose set");
            return false;
        }

        // reset the planner
        ivPlannerEnvironmentPtr->reset();
        setupPlanner();
        //ivPlannerPtr->force_planning_from_scratch();

        // start the planning and return success
        return run();
    }


    bool
    FootstepPlanner::plan(const geometry_msgs::PoseStampedConstPtr& start,
                          const geometry_msgs::PoseStampedConstPtr& goal)
    {
        return plan(start->pose.position.x, start->pose.position.y,
                    tf::getYaw(start->pose.orientation),
                    goal->pose.position.x, goal->pose.position.y,
                    tf::getYaw(goal->pose.orientation));
    }


    bool
    FootstepPlanner::plan(float start_x, float start_y, float start_theta,
                          float goal_x, float goal_y, float goal_theta)
    {
        if (!(setStart(start_x, start_y, start_theta) &&
              setGoal(goal_x, goal_y, goal_theta)))
        {
            return false;
        }

        return plan();
    }


    bool
    FootstepPlanner::replan()
    {
       if (!ivMapPtr)
       {
           ROS_ERROR("FootstepPlanner has no map yet for planning");
           return false;
       }
        if (!ivGoalPoseSetUp || !ivStartPoseSetUp)
        {
            ROS_ERROR("FootstepPlanner has no start or goal pose set");
            return false;
        }

        return run();
    }


    bool
    FootstepPlanner::planService(humanoid_nav_msgs::PlanFootsteps::Request &req,
                                 humanoid_nav_msgs::PlanFootsteps::Response &resp)
    {
    	bool result = plan(req.start.x, req.start.y, req.start.theta,
                           req.goal.x, req.goal.y, req.goal.theta);

    	resp.costs = getPathCosts();
    	resp.footsteps.reserve(getPathSize());

    	humanoid_nav_msgs::StepTarget foot;
    	state_iter_t path_iter;
    	for (path_iter = getPathBegin(); path_iter != getPathEnd(); path_iter++)
    	{
    		foot.pose.x = path_iter->x;
    		foot.pose.y = path_iter->y;
    		foot.pose.theta = path_iter->theta;
    		if (path_iter->leg == LEFT)
    		{
    		    foot.leg = humanoid_nav_msgs::StepTarget::left;
    		}
    		else if (path_iter->leg == RIGHT)
    		{
    		    foot.leg = humanoid_nav_msgs::StepTarget::right;
    		}
    		else
    		{
    			ROS_ERROR("Footstep pose at (%f, %f, %f) is set to NOLEG!",
                          path_iter->x, path_iter->y, path_iter->theta);
    			continue;
    		}

    		resp.footsteps.push_back(foot);
    	}
    	resp.result = result;

    	return result;
    }


    void
    FootstepPlanner::goalPoseCallback(const geometry_msgs::PoseStampedConstPtr& goal_pose)
    {
        bool success = setGoal(goal_pose);
        if (success)
        {
            // NOTE: updates to the goal pose are handled in the run method
            if (ivStartPoseSetUp)
            {
            	assert(ivMapPtr);
            	run();
            }
        }
    }


    void
    FootstepPlanner::startPoseCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr& start_pose)
    {
        bool success = setStart(start_pose->pose.pose.position.x,
                                start_pose->pose.pose.position.y,
                                tf::getYaw(start_pose->pose.pose.orientation));
        if (success)
        {
            // NOTE: updates to the start pose are handled in the run method
            if (ivGoalPoseSetUp)
            {
            	assert(ivMapPtr);
                run();
            }
        }
    }


    void
    FootstepPlanner::mapCallback(const nav_msgs::OccupancyGridConstPtr& occupancy_map)
    {
        boost::shared_ptr<GridMap2D> gridMap(new GridMap2D(occupancy_map));
        setMap(gridMap);
    }


    bool
    FootstepPlanner::setGoal(const geometry_msgs::PoseStampedConstPtr& goal_pose)
    {
        return setGoal(goal_pose->pose.position.x,
                       goal_pose->pose.position.y,
                       tf::getYaw(goal_pose->pose.orientation));
    }


    bool
    FootstepPlanner::setGoal(float x, float y, float theta)
    {
        if (!ivMapPtr)
        {
            ROS_ERROR("Distance map hasn't been initialized yet.");
            return false;
        }

        State goal;
        goal.x = x;
        goal.y = y;
        goal.theta = theta;

        State leftFoot = getFootPosition(goal, LEFT);
        State rightFoot = getFootPosition(goal, RIGHT);

        if (ivPlannerEnvironmentPtr->occupied(leftFoot) ||
            ivPlannerEnvironmentPtr->occupied(rightFoot))
        {
            ROS_ERROR("Goal pose at (%f %f %f) not accessible.", x, y, theta);
            return false;
        }
        ivGoalFootLeft = leftFoot;
        ivGoalFootRight = rightFoot;

        ivGoalPoseSetUp = true;
        ROS_INFO("Goal pose set to (%f %f %f)", x, y, theta);

        return true;
    }


    bool
    FootstepPlanner::setStart(
            const geometry_msgs::PoseStampedConstPtr& start_pose)
    {
        return setStart(start_pose->pose.position.x,
                        start_pose->pose.position.y,
                        tf::getYaw(start_pose->pose.orientation));
    }


    bool
    FootstepPlanner::setStart(const State& right_foot, const State& left_foot)
    {
        if (ivPlannerEnvironmentPtr->occupied(left_foot) ||
            ivPlannerEnvironmentPtr->occupied(right_foot))
        {
            return false;
        }
        ivStartFootLeft = left_foot;
        ivStartFootRight = right_foot;

        ivStartPoseSetUp = true;

        return true;
    }


    bool
    FootstepPlanner::setStart(float x, float y, float theta)
    {
        if (!ivMapPtr)
        {
            ROS_ERROR("Distance map hasn't been initialized yet.");
            return false;
        }

        State start;
        start.x = x;
        start.y = y;
        start.theta = theta;

        State leftFoot = getFootPosition(start, LEFT);
        State rightFoot = getFootPosition(start, RIGHT);

        bool success = setStart(rightFoot, leftFoot);

        if (success)
        	ROS_INFO("Start pose set to (%f %f %f)", x, y, theta);
        else
            ROS_ERROR("Start pose (%f %f %f) not accessible.", x, y, theta);

        // publish visualization:
        geometry_msgs::PoseStamped start_pose;
        start_pose.pose.position.x = x;
        start_pose.pose.position.y = y;
        start_pose.pose.position.z = 0.025;
        start_pose.pose.orientation = tf::createQuaternionMsgFromYaw(theta);
        start_pose.header.frame_id = ivMapPtr->getFrameID();
        start_pose.header.stamp = ros::Time::now();
        ivStartPoseVisPub.publish(start_pose);

        return success;
    }


    void
    FootstepPlanner::setMap(GridMap2DPtr grid_map)
    {
        // TODO: do we need to handle size changes (reinit everything)?

        bool map_exists = ivMapPtr;

        // store old map locally
        GridMap2DPtr old_map = ivMapPtr;
        // store new map
        ivMapPtr.reset();
        ivMapPtr = grid_map;
        // update map of planning environment
        ivPlannerEnvironmentPtr->setMap(grid_map);

        if (map_exists && ivPlanExists)
        {
            updateEnvironment(old_map);
            // TODO: uncomment later
            run(); // plan new path
        }
    }


    void
    FootstepPlanner::updateEnvironment(GridMap2DPtr old_map)
    {

        if (ivPlannerType == "ADPlanner" &&
            ivMapPtr->getResolution() == old_map->getResolution() &&
            ivMapPtr->size().height == old_map->size().height &&
            ivMapPtr->size().width == old_map->size().width)
        {
            ROS_INFO("Received an updated map => change detection");

            std::vector<State> changed_states;
            cv::Mat changed_cells;

            // get new occupied cells only (0: occupied in binary map)
            // changedCells(x,y) = old(x,y) AND NOT(new(x,y))
//          cv::bitwise_not(gridMap->binaryMap(), changedCells);
//          cv::bitwise_and(ivMapPtr->binaryMap(), changedCells, changedCells);

            // to get all changed cells (new free and occupied) use XOR:
            cv::bitwise_xor(old_map->binaryMap(), ivMapPtr->binaryMap(),
                            changed_cells);

            //inflate by outer foot radius:
            cv::bitwise_not(changed_cells, changed_cells); // invert for distanceTransform
            cv::Mat changedDistMap = cv::Mat(changed_cells.size(), CV_32FC1);
            cv::distanceTransform(changed_cells, changedDistMap,
                                  CV_DIST_L2, CV_DIST_MASK_PRECISE);
            double max_foot_radius = sqrt(
                    pow(std::abs(ivOriginFootShiftX)+ivFootsizeX/2.0, 2.0) +
                    pow(std::abs(ivOriginFootShiftY)+ivFootsizeY/2.0, 2.0))
                    / ivMapPtr->getResolution();
            changed_cells = (changedDistMap <= max_foot_radius); // threshold, also invert back

            // loop over changed cells (now marked with 255 in the mask):
            unsigned int num_changed_cells = 0;
            double wx, wy;
            State s;
            for (int y = 0; y < changed_cells.rows; ++y)
            {
                for (int x = 0; x < changed_cells.cols; ++x)
                {
                    if (changed_cells.at<uchar>(x,y) == 255)
                    {
                        num_changed_cells++;
                        ivMapPtr->mapToWorld(x, y, wx, wy);
                        s.x = wx;
                        s.y = wy;
                        // on each grid cell ivNumAngleBins-many planning states
                        // can be placed
                        for (int theta = 0; theta < ivNumAngleBins; ++theta)
                        {
                            s.theta = angle_cell_2_state(theta, ivNumAngleBins);
                            changed_states.push_back(s);
                        }

                        // TODO: state calculation in cases where the grid map
                        // resolution and the planning state resolution don't
                        // match
                    }
                }
            }
            if (num_changed_cells == 0)
            {
                ROS_INFO("old map equals new map; no replanning necessary");
                return;
            }
            ROS_INFO("%d changed map cells found", num_changed_cells);

            // TODO: remove later
            broadcastChangedStatesVis(changed_states);

            if (num_changed_cells <= ivChangedCellsLimit)
            {
                // update planer
                ROS_INFO("Use old information in new planning taks");

                std::vector<int> changed_states_ids;
                if (ivForwardSearch)
                    ivPlannerEnvironmentPtr->getSuccsOfGridCells(changed_states,
                            &changed_states_ids);
                else
                    ivPlannerEnvironmentPtr->getPredsOfGridCells(changed_states,
                            &changed_states_ids);

                boost::shared_ptr<ADPlanner> h =
                        boost::dynamic_pointer_cast<ADPlanner>(ivPlannerPtr);
                h->costs_changed(PlanningStateChangeQuery(&changed_states_ids));
            }
            else
            {
                // reset planner
                ROS_INFO("Reset old information in new planning task");

                ivPlannerEnvironmentPtr->reset();
                setupPlanner();
                //ivPlannerPtr->force_planning_from_scratch();
            }
        }
        else
        {
            // reset planner
            ROS_INFO("Reset old information in new planning task");

            ivPlannerEnvironmentPtr->reset();
            setupPlanner();
            //ivPlannerPtr->force_planning_from_scratch();
        }
    }

    State
    FootstepPlanner::getFootPosition(const State& robot, Leg side)
    {

        double shift_x = -sin(robot.theta) * ivFootSeparation/2;
        double shift_y =  cos(robot.theta) * ivFootSeparation/2;

        double sign = -1.0;
        if (side == LEFT)
        	sign = 1.0;

        State foot;
        foot.x = robot.x + sign * shift_x;
        foot.y = robot.y + sign * shift_y;
        foot.theta = robot.theta;
        foot.leg = side;

        return foot;
    }


    void
    FootstepPlanner::clearFootstepPathVis(unsigned num_footsteps)
    {
        visualization_msgs::Marker marker;
        visualization_msgs::MarkerArray marker_msg;

        marker.header.stamp = ros::Time::now();
        marker.header.frame_id = ivMapPtr->getFrameID();


        if (num_footsteps < 1)
            num_footsteps = ivLastMarkerMsgSize;

        for (unsigned i = 0; i < num_footsteps; i++)
        {
            marker.ns = ivMarkerNamespace;
            marker.id = i;
            marker.action = visualization_msgs::Marker::DELETE;

            marker_msg.markers.push_back(marker);
        }

        ivFootstepPathVisPub.publish(marker_msg);
    }


    // TODO: remove later
    void
    FootstepPlanner::broadcastChangedStatesVis(
            const std::vector<State>& changed_states)
    {
        if (ivChangedStatesVisPub.getNumSubscribers() > 0)
        {
            sensor_msgs::PointCloud cloud_msg;
            geometry_msgs::Point32 point;
            std::vector<geometry_msgs::Point32> points;

            std::vector<State>::const_iterator states_iter;
            for(states_iter = changed_states.begin();
                states_iter != changed_states.end();
                states_iter++)
            {
                point.x = states_iter->x;
                point.y = states_iter->y;
                point.z = 0.01;
                points.push_back(point);
            }
            cloud_msg.header.stamp = ros::Time::now();
            cloud_msg.header.frame_id = ivMapPtr->getFrameID();

            cloud_msg.points = points;

            ivChangedStatesVisPub.publish(cloud_msg);
        }
    }


    void
    FootstepPlanner::broadcastExpandedNodesVis()
    {
        if (ivExpandedStatesVisPub.getNumSubscribers() > 0)
        {
            sensor_msgs::PointCloud cloud_msg;
            geometry_msgs::Point32 point;
            std::vector<geometry_msgs::Point32> points;

            State s;
            FootstepPlannerEnvironment::exp_states_iter_t state_id_iter;
            for(state_id_iter = ivPlannerEnvironmentPtr->getExpandedStatesStart();
                state_id_iter != ivPlannerEnvironmentPtr->getExpandedStatesEnd();
                state_id_iter++)
            {
                ivPlannerEnvironmentPtr->getState(*state_id_iter, &s);
                point.x = s.x;
                point.y = s.y;
                point.z = 0.01;
                points.push_back(point);
            }
            cloud_msg.header.stamp = ros::Time::now();
            cloud_msg.header.frame_id = ivMapPtr->getFrameID();

            cloud_msg.points = points;

            ivExpandedStatesVisPub.publish(cloud_msg);
        }
    }


    void
    FootstepPlanner::broadcastFootstepPathVis()
    {
        if (getPathSize() == 0)
        {
            ROS_INFO("no path has been extracted yet");
            return;
        }

        visualization_msgs::Marker marker;
        visualization_msgs::MarkerArray broadcast_msg;
        std::vector<visualization_msgs::Marker> markers;

        int markers_counter = 0;

        marker.header.stamp = ros::Time::now();
        marker.header.frame_id = ivMapPtr->getFrameID();

		// add the missing start foot to the publish vector for visualization:
        if (ivPath.front().leg == LEFT)
        	footstepToMarker(ivStartFootRight, &marker);
        else
        	footstepToMarker(ivStartFootLeft, &marker);

		marker.id = markers_counter++;
		markers.push_back(marker);

        // add the footsteps of the path to the publish vector
        state_iter_t path_iter = getPathBegin();
        for(; path_iter != getPathEnd(); path_iter++)
        {
            footstepToMarker(*path_iter, &marker);
            marker.id = markers_counter++;
            markers.push_back(marker);
        }
        if (markers_counter < ivLastMarkerMsgSize)
        {
            for(int j = markers_counter; j < ivLastMarkerMsgSize; j++)
            {
                marker.ns = ivMarkerNamespace;
                marker.id = j;
                marker.action = visualization_msgs::Marker::DELETE;

                markers.push_back(marker);
            }
        }

        // add the missing goal foot to the publish vector for visualization:
        if (ivPath.back().leg == LEFT)
        	footstepToMarker(ivGoalFootRight, &marker);
        else
        	footstepToMarker(ivGoalFootLeft, &marker);
        marker.id = markers_counter++;
        markers.push_back(marker);

        broadcast_msg.markers = markers;
        ivLastMarkerMsgSize = markers.size();

        ivFootstepPathVisPub.publish(broadcast_msg);
    }


    void
    FootstepPlanner::broadcastPathVis()
    {
        if (getPathSize() == 0)
        {
            ROS_INFO("no path has been extracted yet");
            return;
        }

        nav_msgs::Path path_msg;
        geometry_msgs::PoseStamped state;

        state.header.stamp = ros::Time::now();
        state.header.frame_id = ivMapPtr->getFrameID();

        state_iter_t path_iter;
        for(path_iter = getPathBegin(); path_iter != getPathEnd(); path_iter++)
        {
            state.pose.position.x = path_iter->x;
            state.pose.position.y = path_iter->y;
            path_msg.poses.push_back(state);
        }

        path_msg.header = state.header;
        ivPathVisPub.publish(path_msg);
    }


    void
    FootstepPlanner::footstepToMarker(const State& footstep,
                                      visualization_msgs::Marker* marker)
    {
        marker->header.stamp = ros::Time::now();
        marker->header.frame_id = ivMapPtr->getFrameID();
        marker->ns = ivMarkerNamespace;
        marker->type = visualization_msgs::Marker::CUBE;
        marker->action = visualization_msgs::Marker::ADD;

        float cos_theta = cos(footstep.theta);
        float sin_theta = sin(footstep.theta);
        float x_shift = cos_theta*ivOriginFootShiftX - sin_theta*ivOriginFootShiftY;
        float y_shift;
        if (footstep.leg == LEFT)
            y_shift = sin_theta*ivOriginFootShiftX + cos_theta*ivOriginFootShiftY;
        else // leg == RLEG
            y_shift = sin_theta*ivOriginFootShiftX - cos_theta*ivOriginFootShiftY;
        marker->pose.position.x = footstep.x + x_shift;
        marker->pose.position.y = footstep.y + y_shift;
        tf::quaternionTFToMsg(tf::createQuaternionFromYaw(footstep.theta),
                              marker->pose.orientation);

        marker->scale.x = ivFootsizeX; // - 0.01;
        marker->scale.y = ivFootsizeY; // - 0.01;
        marker->scale.z = ivFootsizeZ;

        // TODO: make color configurable?
        if (footstep.leg == RIGHT)
        {
            marker->color.r = 0.0f;
            marker->color.g = 1.0f;
        }
        else // leg == LEFT
        {
            marker->color.r = 1.0f;
            marker->color.g = 0.0f;
        }
        marker->color.b = 0.0;
        marker->color.a = 0.4;

        marker->lifetime = ros::Duration();
    }
}
