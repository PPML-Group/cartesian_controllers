// -- BEGIN LICENSE BLOCK -----------------------------------------------------
// -- END LICENSE BLOCK -------------------------------------------------------

//-----------------------------------------------------------------------------
/*!\file    cartesian_controller_base.hpp
 *
 * \author  Stefan Scherzinger <scherzin@fzi.de>
 * \date    2017/07/27
 *
 */
//-----------------------------------------------------------------------------

#ifndef CARTESIAN_CONTROLLER_BASE_HPP_INCLUDED
#define CARTESIAN_CONTROLLER_BASE_HPP_INCLUDED

// Project
#include <cartesian_controller_base/cartesian_controller_base.h>

// KDL
#include <kdl/tree.hpp>
#include <kdl_parser/kdl_parser.hpp>
#include <kdl/jntarray.hpp>

// URDF
#include <urdf/model.h>

namespace cartesian_controller_base
{

template <class HardwareInterface>
CartesianControllerBase<HardwareInterface>::
CartesianControllerBase()
: m_already_initialized(false)
{
}

template <class HardwareInterface>
bool CartesianControllerBase<HardwareInterface>::
init(HardwareInterface* hw, ros::NodeHandle& nh)
{
  if (m_already_initialized)
  {
    return true;
  }

  std::string robot_description;
  urdf::Model robot_model;
  KDL::Tree   robot_tree;
  KDL::Chain  robot_chain;

  // Get controller specific configuration
  if (!nh.getParam("/robot_description",robot_description))
  {
    ROS_ERROR("Failed to load '/robot_description' from parameter server");
    return false;
  }
  if (!nh.getParam("robot_base_link",m_robot_base_link))
  {
    ROS_ERROR_STREAM("Failed to load " << nh.getNamespace() + "/robot_base_link" << " from parameter server");
    return false;
  }
  if (!nh.getParam("end_effector_link",m_end_effector_link))
  {
    ROS_ERROR_STREAM("Failed to load " << nh.getNamespace() + "/end_effector_link" << " from parameter server");
    return false;
  }

  // Build a kinematic chain of the robot
  if (!robot_model.initString(robot_description))
  {
    ROS_ERROR("Failed to parse urdf model from 'robot_description'");
    return false;
  }
  if (!kdl_parser::treeFromUrdfModel(robot_model,robot_tree))
  {
    const std::string error = ""
      "Failed to parse KDL tree from urdf model";
    ROS_ERROR_STREAM(error);
    throw std::runtime_error(error);
  }
  if (!robot_tree.getChain(m_robot_base_link,m_end_effector_link,robot_chain))
  {
    const std::string error = ""
      "Failed to parse robot chain from urdf model. "
      "Are you sure that both your 'robot_base_link' and 'end_effector_link' exist?";
    ROS_ERROR_STREAM(error);
    throw std::runtime_error(error);
  }

  // Get names of controllable joints from the parameter server
  if (!nh.getParam("joints",m_joint_names))
  {
    const std::string error = ""
    "Failed to load " + nh.getNamespace() + "/joints" + " from parameter server";
    ROS_ERROR_STREAM(error);
    throw std::runtime_error(error);
  }

  // Parse joint limits
  KDL::JntArray upper_pos_limits(m_joint_names.size());
  KDL::JntArray lower_pos_limits(m_joint_names.size());
  for (size_t i = 0; i < m_joint_names.size(); ++i)
  {
    if (!robot_model.getJoint(m_joint_names[i]))
    {
      const std::string error = ""
        "Joint " + m_joint_names[i] + " does not appear in /robot_description";
      ROS_ERROR_STREAM(error);
      throw std::runtime_error(error);
    }
    upper_pos_limits(i) = robot_model.getJoint(m_joint_names[i])->limits->upper;
    lower_pos_limits(i) = robot_model.getJoint(m_joint_names[i])->limits->lower;
  }

  // Get the joint handles to use in the control loop
  for (size_t i = 0; i < m_joint_names.size(); ++i)
  {
    m_joint_handles.push_back(hw->getHandle(m_joint_names[i]));
  }

  // Initialize solvers
  m_forward_dynamics_solver.init(robot_chain,upper_pos_limits,lower_pos_limits);
  KDL::Tree tmp("not_relevant");
  tmp.addChain(robot_chain,"not_relevant");
  m_forward_kinematics_solver.reset(new KDL::TreeFkSolverPos_recursive(tmp));

  // Initialize Cartesian pid controllers
  m_spatial_controller.init(nh);

  m_already_initialized = true;

  // Start with normal ROS control behavior
  m_paused = false;

  return true;
}

template <class HardwareInterface>
void CartesianControllerBase<HardwareInterface>::
starting(const ros::Time& time)
{
  // Copy joint state to internal simulation
  m_forward_dynamics_solver.setStartState(m_joint_handles);
}

template <class HardwareInterface>
void CartesianControllerBase<HardwareInterface>::
pause(const ros::Time& time)
{
  m_paused = true;
}

template <class HardwareInterface>
bool CartesianControllerBase<HardwareInterface>::
resume(const ros::Time& time)
{
  m_paused = false;
  return true;
}

template <>
void CartesianControllerBase<hardware_interface::PositionJointInterface>::
writeJointControlCmds()
{
  // Don't update position commands when paused.
  // Note: CartesianMotionControllers don't take any feedback from the joint
  // handles. These controllers will drift if the target frame they are
  // following isn't also paused.
  if (m_paused)
  {
    return;
  }

  // Take position commands
  for (size_t i = 0; i < m_joint_handles.size(); ++i)
  {
    m_joint_handles[i].setCommand(m_simulated_joint_motion.positions[i]);
  }
}

template <>
void CartesianControllerBase<hardware_interface::VelocityJointInterface>::
writeJointControlCmds()
{
  // Don't update velocity commands when paused.
  // Note: CartesianMotionControllers don't take any feedback from the joint
  // handles. These controllers will drift if the target frame they are
  // following isn't also paused.
  if (m_paused)
  {
    return;
  }

  // Take velocity commands
  for (size_t i = 0; i < m_joint_handles.size(); ++i)
  {
    m_joint_handles[i].setCommand(m_simulated_joint_motion.velocities[i]);
  }
}

template <class HardwareInterface>
void CartesianControllerBase<HardwareInterface>::
computeJointControlCmds(const ctrl::Vector6D& error, const ros::Duration& period)
{
  if (m_paused)
  {
    return;
  }

  // PID controlled system input
  m_cartesian_input = m_spatial_controller(error,period);

  m_simulated_joint_motion = m_forward_dynamics_solver.getJointControlCmds(
      period,
      m_cartesian_input);
}

template <class HardwareInterface>
ctrl::Vector6D CartesianControllerBase<HardwareInterface>::
displayInBaseLink(const ctrl::Vector6D& vector, const std::string& from)
{
  // Adjust format
  KDL::Wrench wrench_kdl;
  for (int i = 0; i < 6; ++i)
  {
    wrench_kdl(i) = vector[i];
  }

  KDL::Frame transform_kdl;
  m_forward_kinematics_solver->JntToCart(
      m_forward_dynamics_solver.getPositions(),
      transform_kdl,
      from);

  // Rotate into new reference frame
  wrench_kdl = transform_kdl.M * wrench_kdl;

  // Reassign
  ctrl::Vector6D out;
  for (int i = 0; i < 6; ++i)
  {
    out[i] = wrench_kdl(i);
  }

  return out;
}

template <class HardwareInterface>
ctrl::Matrix6D CartesianControllerBase<HardwareInterface>::
displayInBaseLink(const ctrl::Matrix6D& tensor, const std::string& from)
{
  // Get rotation to base
  KDL::Frame R_kdl;
  m_forward_kinematics_solver->JntToCart(
      m_forward_dynamics_solver.getPositions(),
      R_kdl,
      from);

  // Adjust format
  ctrl::Matrix3D R;
  R <<
      R_kdl.M.data[0],
      R_kdl.M.data[1],
      R_kdl.M.data[2],
      R_kdl.M.data[3],
      R_kdl.M.data[4],
      R_kdl.M.data[5],
      R_kdl.M.data[6],
      R_kdl.M.data[7],
      R_kdl.M.data[8];

  // Treat diagonal blocks as individual 2nd rank tensors.
  // Display in base frame.
  ctrl::Matrix6D tmp = ctrl::Matrix6D::Zero();
  tmp.topLeftCorner<3,3>() = R * tensor.topLeftCorner<3,3>() * R.transpose();
  tmp.bottomRightCorner<3,3>() = R * tensor.bottomRightCorner<3,3>() * R.transpose();

  return tmp;
}

template <class HardwareInterface>
ctrl::Vector6D CartesianControllerBase<HardwareInterface>::
displayInTipLink(const ctrl::Vector6D& vector, const std::string& to)
{
  // Adjust format
  KDL::Wrench wrench_kdl;
  for (int i = 0; i < 6; ++i)
  {
    wrench_kdl(i) = vector[i];
  }

  KDL::Frame transform_kdl;
  m_forward_kinematics_solver->JntToCart(
      m_forward_dynamics_solver.getPositions(),
      transform_kdl,
      to);

  // Rotate into new reference frame
  wrench_kdl = transform_kdl.M.Inverse() * wrench_kdl;

  // Reassign
  ctrl::Vector6D out;
  for (int i = 0; i < 6; ++i)
  {
    out[i] = wrench_kdl(i);
  }

  return out;
}

} // namespace

#endif
