#!/usr/bin/env python2

import math
import random
import numpy as np
import rospy

from drunc import Drunc
import carla
from network_agent_path import NetworkAgentPath
from sidewalk_agent_path import SidewalkAgentPath
from util import *
import carla_connector2.msg
from peds_unity_system.msg import car_info as CarInfo

default_agent_pos = carla.Vector2D(10000, 10000)
default_agent_bbox = []
default_agent_bbox.append(default_agent_pos + carla.Vector2D(1,-1))
default_agent_bbox.append(default_agent_pos + carla.Vector2D(1,1))
default_agent_bbox.append(default_agent_pos + carla.Vector2D(-1,1))
default_agent_bbox.append(default_agent_pos + carla.Vector2D(-1,-1))

class CrowdAgent(object):
    def __init__(self, actor, preferred_speed):
        self.actor = actor
        self.preferred_speed = preferred_speed
        self.actor.set_collision_enabled(False)
    
    def get_id(self):
        return self.actor.id

    def get_velocity(self):
        v = self.actor.get_velocity()
        return carla.Vector2D(v.x, v.y)
    
    def get_transform(self):
        return self.actor.get_transform()
    
    def get_bounding_box(self):
        return self.actor.bounding_box
    
    def get_forward_direction(self):
        forward = self.actor.get_transform().get_forward_vector()
        return carla.Vector2D(forward.x, forward.y)

    def get_position(self):
        pos3D = self.actor.get_location()
        return carla.Vector2D(pos3D.x, pos3D.y)
    
    def get_position3D(self):
        return self.actor.get_location()


class CrowdNetworkAgent(CrowdAgent):
    def __init__(self, actor, path, preferred_speed):
        super(CrowdNetworkAgent, self).__init__(actor, preferred_speed)
        self.path = path

    def get_agent_params(self):
        return carla.AgentParams.get_default('Car') ## to check: where is bicycle?

    def get_bounding_box_corners(self):
        bbox = self.actor.bounding_box
        loc = carla.Vector2D(bbox.location.x, bbox.location.y) + self.get_position()
        forward_vec = self.get_forward_direction().make_unit_vector() # the local x direction (left-handed coordinate system)
        sideward_vec = forward_vec.rotate(np.deg2rad(90)) # the local y direction

        half_y_len = bbox.extent.y
        half_x_len = bbox.extent.x

        corners = []
        corners.append(loc - half_x_len*forward_vec + half_y_len*sideward_vec)
        corners.append(loc + half_x_len*forward_vec + half_y_len*sideward_vec)
        corners.append(loc + half_x_len*forward_vec - half_y_len*sideward_vec)
        corners.append(loc - half_x_len*forward_vec - half_y_len*sideward_vec)
        
        return corners
    
    def get_preferred_velocity(self):
        position = self.get_position()

        ## to check
        if not self.path.resize():
            return None
        self.path.cut(position)
        if not self.path.resize():
            return None

        target_position = self.path.get_position(3) ## to check

        # cut_index = 0
        # for i in range(10):
        #     route_point = self.path.get_position(i)
        #     offset = position - route_point
        #     if offset.length() < 1.0:
        #         cut_index = i + 1
        # target_position = self.path.get_position(cut_index)

        velocity = (target_position - position).make_unit_vector()
        return self.preferred_speed * velocity
    
    def get_control(self, velocity):
        steer = get_signed_angle_diff(velocity, self.get_forward_direction())
        min_steering_angle = -45.0
        max_steering_angle = 45.0
        if steer > max_steering_angle:
            steer = max_steering_angle
        elif steer < min_steering_angle:
            steer = min_steering_angle

        k = 1.0
        steer = k * steer / (max_steering_angle - min_steering_angle) * 2.0
        desired_speed = velocity.length()
        #steer_tmp = get_signed_angle_diff(velocity, self.get_forward_direction())
        cur_speed = self.get_velocity().length()
        control = self.actor.get_control()

        k2 = 1.5
        k3 = 2.5
        if desired_speed - cur_speed > 0:
            control.throttle = k2 * (desired_speed - cur_speed) / desired_speed
            control.brake = 0.0
        elif desired_speed - cur_speed == 0:
            control.throttle = 0.0
            control.brake = 0.0
        else:
            control.throttle = 0
            control.brake = k3 * (cur_speed - desired_speed) / cur_speed

        control.steer = steer
        return control

class CrowdSidewalkAgent(CrowdAgent):
    def __init__(self, actor, path, preferred_speed):
        super(CrowdSidewalkAgent, self).__init__(actor, preferred_speed)
        self.path = path
    
    def get_agent_params(self):
        return carla.AgentParams.get_default('People')
    
    def get_bounding_box_corners(self):
        bbox = self.actor.bounding_box
        loc = carla.Vector2D(bbox.location.x, bbox.location.y) + self.get_position()
        forward_vec = self.get_forward_direction().make_unit_vector() # the local x direction (left-handed coordinate system)
        sideward_vec = forward_vec.rotate(np.deg2rad(90)) # the local y direction

        # Hardcoded values for people.
        half_y_len = 0.23
        half_x_len = 0.23

        corners = []
        corners.append(loc - half_x_len*forward_vec + half_y_len*sideward_vec)
        corners.append(loc + half_x_len*forward_vec + half_y_len*sideward_vec)
        corners.append(loc + half_x_len*forward_vec - half_y_len*sideward_vec)
        corners.append(loc - half_x_len*forward_vec - half_y_len*sideward_vec)
        
        return corners
    
    def get_preferred_velocity(self):
        position = self.get_position()

        if not self.path.resize():
            return None

        self.path.cut(position)

        if not self.path.resize():
            return None

        target_position = self.path.get_position(0)
        velocity = (target_position - position).make_unit_vector()
        return self.preferred_speed * velocity
    
    def get_control(self, velocity):
        velocity = velocity.make_unit_vector() * self.preferred_speed
        return carla.WalkerControl(
                carla.Vector3D(velocity.x, velocity.y, 0),
                1.0, False)

class GammaCrowdController(Drunc):
    def __init__(self):
        super(GammaCrowdController, self).__init__()
        self.network_agents = []
        self.sidewalk_agents = []
        self.gamma = carla.RVOSimulator()

        self.walker_blueprints = self.world.get_blueprint_library().filter("walker.pedestrian.*")
        self.vehicles_blueprints = self.world.get_blueprint_library().filter('vehicle.audi.*')
        self.bikes_blueprints = [x for x in self.vehicles_blueprints if int(x.get_attribute('number_of_wheels')) == 2]
        self.cars_blueprints = [x for x in self.vehicles_blueprints if int(x.get_attribute('number_of_wheels')) == 4]
        
        self.num_network_agents = rospy.get_param('~num_network_agents')
        self.num_sidewalk_agents = rospy.get_param('~num_sidewalk_agents')
        self.path_min_points = rospy.get_param('~path_min_points')
        self.path_interval = rospy.get_param('~path_interval')
        self.network_agents_pub = rospy.Publisher(
                '/crowd/network_agents', 
                carla_connector2.msg.CrowdNetworkAgentArray, 
                queue_size=1)
        self.sidewalk_agents_pub = rospy.Publisher(
                '/crowd/sidewalk_agents', 
                carla_connector2.msg.CrowdSidewalkAgentArray, 
                queue_size=1)
        self.il_car_info_sub = rospy.Subscriber(
                '/IL_car_info',
                CarInfo,
                self.il_car_info_callback,
                queue_size=1)

        for i in range(self.num_network_agents):
            self.gamma.add_agent(carla.AgentParams.get_default('Car'), i)
        
        for i in range(self.num_sidewalk_agents):
            # self.gamma.add_agent(carla.AgentParams.get_default('People'), i) ## to check
            self.gamma.add_agent(carla.AgentParams.get_default('People'), i + self.num_network_agents) ## to check
        
        # For ego vehicle.
        self.gamma.add_agent(carla.AgentParams.get_default('Car'), self.num_network_agents + self.num_sidewalk_agents)

        adding_obstacle = False
        if(adding_obstacle):
            self.add_obstacles()

    def add_obstacles(self):
        polygon_table = self.network_occupancy_map.create_polygon_table(self.map_bounds_min,self.map_bounds_max,100,0.1)
        for r in range(polygon_table.rows):
            for c in range(polygon_table.columns):
                for p in polygon_table.get(r, c):
                    obstacle = []
                    for i in reversed(range(len(p))): 
                        v1 = p[i]
                        obstacle.append(carla.Vector2D(v1.x, v1.y))
                    self.gamma.add_obstacle(obstacle)
        self.gamma.process_obstacles()
   
    def dispose(self):
        commands = []
        commands.extend(carla.command.DestroyActor(a.actor.id) for a in self.network_agents)
        commands.extend(carla.command.DestroyActor(a.actor.id) for a in self.sidewalk_agents)
        self.client.apply_batch(commands)
        print('Destroyed crowd actors.')

    def il_car_info_callback(self, car_info):
        self.ego_car_info = car_info
        i = len(self.network_agents) + len(self.sidewalk_agents)
               
        if self.ego_car_info:
            self.gamma.set_agent_position(i, carla.Vector2D(
                self.ego_car_info.car_pos.x,
                self.ego_car_info.car_pos.y))
            self.gamma.set_agent_velocity(i, carla.Vector2D(
                self.ego_car_info.car_vel.x,
                self.ego_car_info.car_vel.y))
            self.gamma.set_agent_heading(i, carla.Vector2D(
                math.cos(np.deg2rad(self.ego_car_info.car_yaw)),
                math.sin(np.deg2rad(self.ego_car_info.car_yaw))))
            self.gamma.set_agent_bounding_box_corners(i, 
                    [carla.Vector2D(v.x, v.y) for v in self.ego_car_info.car_bbox.points])
            self.gamma.set_agent_pref_velocity(i, carla.Vector2D(
                self.ego_car_info.car_pref_vel.x,
                self.ego_car_info.car_pref_vel.y))
        else:
            self.gamma.set_agent_position(i, default_agent_pos)
            self.gamma.set_agent_pref_velocity(i, carla.Vector2D(0, 0))
            self.gamma.set_agent_velocity(i, carla.Vector2D(0, 0))
            self.gamma.set_agent_bounding_box_corners(i, default_agent_bbox)
    
    def update(self):
        while len(self.network_agents) < self.num_network_agents:
            path = NetworkAgentPath.rand_path(self, self.path_min_points, self.path_interval)
            trans = carla.Transform()
            trans.location.x = path.get_position(0).x
            trans.location.y = path.get_position(0).y
            trans.location.z = 0.5
            trans.rotation.yaw = path.get_yaw(0)
            actor = self.world.try_spawn_actor(
                    random.choice(self.cars_blueprints),
                    trans)
            self.world.wait_for_tick(1.0)
            if actor:
                self.network_agents.append(CrowdNetworkAgent(
                    actor, path, 
                    5.0 + random.uniform(0.0, 1.5)))
      
        while len(self.sidewalk_agents) < self.num_sidewalk_agents:
            path = SidewalkAgentPath.rand_path(self, self.path_min_points, self.path_interval)
            trans = carla.Transform()
            trans.location.x = path.get_position(0).x
            trans.location.y = path.get_position(0).y
            trans.location.z = 0.5
            trans.rotation.yaw = path.get_yaw(0)
            actor = self.world.try_spawn_actor(
                    random.choice(self.walker_blueprints),
                    trans)
            self.world.wait_for_tick(1.0)
            if actor:
                self.sidewalk_agents.append(CrowdSidewalkAgent(
                    actor, path, 
                    0.5 + random.uniform(0.0, 1.5)))
    
        commands = []
        
        next_agents = []
        for (i, crowd_agent) in enumerate(self.network_agents + self.sidewalk_agents):
            if not self.in_bounds(crowd_agent.get_position()) or crowd_agent.get_position3D().z < -10:
                next_agents.append(None)
                self.gamma.set_agent_position(i, default_agent_pos)
                self.gamma.set_agent_pref_velocity(i, carla.Vector2D(0, 0))
                self.gamma.set_agent_velocity(i, carla.Vector2D(0, 0))
                self.gamma.set_agent_bounding_box_corners(i, default_agent_bbox)
                commands.append(carla.command.DestroyActor(crowd_agent.actor.id))
                continue

            self.gamma.set_agent(i, crowd_agent.get_agent_params())
            pref_vel = crowd_agent.get_preferred_velocity()
            if pref_vel:
                next_agents.append(crowd_agent)
                self.gamma.set_agent_position(i, crowd_agent.get_position())
                self.gamma.set_agent_velocity(i, crowd_agent.get_velocity())
                self.gamma.set_agent_heading(i, crowd_agent.get_forward_direction())
                self.gamma.set_agent_bounding_box_corners(i, crowd_agent.get_bounding_box_corners())
                self.gamma.set_agent_pref_velocity(i, pref_vel)
            else:
                next_agents.append(None)
                self.gamma.set_agent_position(i, default_agent_pos)
                self.gamma.set_agent_pref_velocity(i, carla.Vector2D(0, 0))
                self.gamma.set_agent_velocity(i, carla.Vector2D(0, 0))
                self.gamma.set_agent_bounding_box_corners(i, default_agent_bbox)
                commands.append(carla.command.DestroyActor(crowd_agent.actor.id))

        self.gamma.do_step()

        for (i, crowd_agent) in enumerate(next_agents):
            if crowd_agent:
                vel_to_exe = self.gamma.get_agent_velocity(i)
                control = crowd_agent.get_control(vel_to_exe)
                if type(crowd_agent) is CrowdNetworkAgent:
                    commands.append(carla.command.ApplyVehicleControl(crowd_agent.actor.id, control))
                elif type(crowd_agent) is CrowdSidewalkAgent:
                    commands.append(carla.command.ApplyWalkerControl(crowd_agent.actor.id, control))
        
        self.network_agents = [a for a in next_agents if a and type(a) is CrowdNetworkAgent]
        self.sidewalk_agents = [a for a in next_agents if a and type(a) is CrowdSidewalkAgent]
        
        self.client.apply_batch(commands)
        self.world.wait_for_tick(1.0)

        network_agents_msg = carla_connector2.msg.CrowdNetworkAgentArray()
        network_agents_msg.header.stamp = rospy.Time.now()
        for a in self.network_agents:
            network_agent_msg = carla_connector2.msg.CrowdNetworkAgent()
            network_agent_msg.id = a.get_id()
            network_agent_msg.type = 'car'
            network_agent_msg.route_point.edge = a.path.route_points[0].edge
            network_agent_msg.route_point.lane = a.path.route_points[0].lane
            network_agent_msg.route_point.segment = a.path.route_points[0].segment
            network_agent_msg.route_point.offset = a.path.route_points[0].offset
            network_agents_msg.agents.append(network_agent_msg)
        self.network_agents_pub.publish(network_agents_msg)
        
        sidewalk_agents_msg = carla_connector2.msg.CrowdSidewalkAgentArray()
        sidewalk_agents_msg.header.stamp = rospy.Time.now()
        for a in self.sidewalk_agents:
            sidewalk_agent_msg = carla_connector2.msg.CrowdSidewalkAgent()
            sidewalk_agent_msg.id = a.get_id()
            sidewalk_agent_msg.type = 'ped'
            sidewalk_agent_msg.route_point.polygon_id = a.path.route_points[0].polygon_id
            sidewalk_agent_msg.route_point.segment_id = a.path.route_points[0].segment_id
            sidewalk_agent_msg.route_point.offset = a.path.route_points[0].offset
            sidewalk_agent_msg.route_orientation = a.path.route_orientation
            sidewalk_agents_msg.agents.append(sidewalk_agent_msg)
        self.sidewalk_agents_pub.publish(sidewalk_agents_msg)
        
            
if __name__ == '__main__':
    rospy.init_node('gamma_crowd_controller')
    gamma_crowd_controller = GammaCrowdController()

    rate = rospy.Rate(100) ## to check
    #rate = rospy.Rate(5)
    while not rospy.is_shutdown():
        gamma_crowd_controller.update()
        rate.sleep()

    gamma_crowd_controller.dispose()
