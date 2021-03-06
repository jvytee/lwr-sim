#include "lwr_testdriver-component.hpp"
#include <rtt/Component.hpp>
#include <rtt/Logger.hpp>
#include <eigen3/Eigen/Dense>
#include <kdl/frames_io.hpp>


Lwr_testdriver::Lwr_testdriver(std::string const& name) : TaskContext(name) {
    mode = "none";
    this->addOperation("setMode", &Lwr_testdriver::setMode, this).doc("Set position, torque or none mode");

    positioning_torque = 1.0f;
    this->addProperty("positioning_torque", positioning_torque).doc("Torque to be generated in each joint for positioning");

    epsilon = 0.005f;
    this->addProperty("epsilon", epsilon).doc("Desired precision [rad]");

    target_angles.setZero(7);
    target_angles << 70.0f, 12.0f, 90.0f, -80.0f, 0.0f, 60.0f, 0.0f;
    target_angles = target_angles * 3.141f/180.0f;
    this->addProperty("target_angles", target_angles).doc("Target joint angles to be reached [rad]");

    hand_forces.setZero(6);
    elbow_forces.setZero(6);

    this->addProperty("hand_forces", hand_forces).doc("Forces/torques in EE frame");
    this->addProperty("elbow_forces", elbow_forces).doc("Forces/torques in elbow frame");

    this->addOperation("setHandForces", &Lwr_testdriver::setForceAxisUpper, this).doc("Set forces in EE frame");
    this->addOperation("setElbowForces", &Lwr_testdriver::setForceAxisLower, this).doc("Set forces in elbow frame");

    this->addOperation("setHandTorques", &Lwr_testdriver::setTorqueAxisUpper, this).doc("Set torques in EE frame");
    this->addOperation("setElbowTorques", &Lwr_testdriver::setTorqueAxisLower, this).doc("Set torques in elbow frame");

    this->addOperation("loadModel", &Lwr_testdriver::loadModel, this).doc("Load kinematic model from specified URDF file");
    this->addProperty("q_upper", q_upper).doc("Upper chain joint values");
    this->addProperty("q_lower", q_lower).doc("Lower chain joint values");
    this->addProperty("enable_elbow_to_base", elbow_to_base).doc("If true, perform elbow to base transformation on elbow forces/torques");

    this->addOperation("rampHandForces", &Lwr_testdriver::rampForcesUpper, this).doc("Smoothly ramp up forces over given time intervall");
    this->addOperation("rampElbowForces", &Lwr_testdriver::rampForcesLower, this).doc("Smoothly ramp up forces over given time intervall");

    tau.setZero(7);
    this->addProperty("tau", tau).doc("Computed joint torques");

    this->addProperty("enable_pid", enable_pid).doc("Enable PID controller for lower chain");
    this->addProperty("k_proportional", k_p).doc("Proportional PID gain");
    this->addProperty("k_integral", k_i).doc("Integral PID gain");
    this->addProperty("k_derivative", k_d).doc("Derivative PID gain");

    joint_state_in_port.doc("Joint state feedback port");
    joint_state_in_flow = RTT::NoData;
    this->addPort("jointStateIn", joint_state_in_port);

    torques_out_port.doc("Torque output port");
    this->addPort("torquesOut", torques_out_port);

    this->addOperation("averageTau", &Lwr_testdriver::averageTau, this).doc("Print average tau over given number of frames");
    this->addOperation("print", &Lwr_testdriver::printShit, this).doc("Print shit for debugging purposes");

    model_loaded = false;

    RTT::log(RTT::Info) << "Lwr_testdriver constructed" << RTT::endlog();
}


bool Lwr_testdriver::configureHook(){
    torques_out_data.torques.setZero(7);
    torques_out_port.setDataSample(torques_out_data);

    if(!model_loaded) {
        RTT::log(RTT::Error) << "No model loaded" << RTT::endlog();
        return false;
    }

    RTT::log(RTT::Info) << "Lwr_testdriver configured" << RTT::endlog();
    return true;
}


bool Lwr_testdriver::startHook(){
    torques_out_data.torques.setZero(7);

    e_previous.setZero(lower.getNrOfJoints());
    e_current.setZero(lower.getNrOfJoints());
    e_total.setZero(lower.getNrOfJoints());

    RTT::log(RTT::Info) << "Lwr_testdriver started" << RTT::endlog();
    return true;
}


void Lwr_testdriver::updateHook(){
    // Read current state
    joint_state_in_flow = joint_state_in_port.read(joint_state_in_data);

    if(joint_state_in_flow == RTT::NoData) {
        RTT::log(RTT::Error) << "No joint state input" << RTT::endlog();
        return;
    }

    q_lower.data = joint_state_in_data.angles.head(lower.getNrOfJoints()).cast<double>();
    q_upper.data = joint_state_in_data.angles.tail(upper.getNrOfJoints()).cast<double>();

    // If in torque mode, compute torques
    // Else drive to position/do nothing
    if(mode == "torque") {
        tau.setZero(7);

        if(enable_pid) {
            // If PID control enabled, let controler compute torques
            tau.head(lower.getNrOfJoints()) = controlPID(Eigen::VectorXd::Zero(lower.getNrOfJoints()), joint_state_in_data.velocities.head(lower.getNrOfJoints()).cast<double>()).cast<float>();
        } else {
            // Otherwise compute torques by (transformed) jacobian of lower kinematic chain
            if(ramp_forces_elbow) {
                current_time_elbow = RTT::os::TimeService::ticks2nsecs(RTT::os::TimeService::Instance()->getTicks());
                tau.head(lower.getNrOfJoints()) = computeTorquesLower(elbow_forces_diff.cast<double>() * (current_time_elbow - start_time_elbow) / total_time_elbow).cast<float>();
            } else {
                tau.head(lower.getNrOfJoints()) = computeTorquesLower(elbow_forces.cast<double>()).cast<float>();
            }
        }

        if(ramp_forces_hand) {
            current_time_hand = RTT::os::TimeService::ticks2nsecs(RTT::os::TimeService::Instance()->getTicks());
            tau.tail(upper.getNrOfJoints()) = computeTorquesUpper(hand_forces_diff.cast<double>() * (current_time_hand - start_time_hand) / total_time_hand).cast<float>();
        } else {
            tau.tail(upper.getNrOfJoints()) = computeTorquesUpper(hand_forces.cast<double>()).cast<float>();
        }

    } else if(mode == "position"){
        // Use positioning torques to move to target joint angles
        in_position = 0;

        // For all seven joints
        for(joint_counter = 0; joint_counter < 7; joint_counter++) {

            if(target_angles[joint_counter] - joint_state_in_data.angles[joint_counter] > epsilon) {
                tau[joint_counter] = positioning_torque;
            } else if(target_angles[joint_counter] - joint_state_in_data.angles[joint_counter] < -epsilon) {
                tau[joint_counter] = -positioning_torque;
            } else {
                tau[joint_counter] = 0.0f;
                in_position++;
            }
        }
    }

    // Average tau for logging purposes (kinda deprecated)
    if(frames_total > frames_counter) {
       tau_sum += tau;
       frames_counter++;
    } else if(frames_total > 0) {
        RTT::log(RTT::Info) << "Average tau over " << frames_total << " iterations:\n" << tau_sum / frames_total << RTT::endlog();
        frames_total = 0;
        frames_counter = 0;
    }

    // Write torques to their respective output ports
    torques_out_data.torques = tau;
    torques_out_port.write(torques_out_data);
}


void Lwr_testdriver::stopHook() {
    torques_out_data.torques.setZero(7);

    RTT::log(RTT::Info) << "Lwr_testdriver executes stopping" << RTT::endlog();
}


void Lwr_testdriver::cleanupHook() {
    RTT::log(RTT::Info) << "Lwr_testdriver cleaning up" << RTT::endlog();
}


bool Lwr_testdriver::loadModel(const std::string& model_path, const std::string& lower_tip_link, const std::string& upper_root_link) {
    model_loaded = false;

    if(!model.initFile(model_path)) {
        RTT::log(RTT::Error) << "Could not load model from URDF at " << model_path << RTT::endlog();
        return false;
    }

    if(!kdl_parser::treeFromUrdfModel(model, model_tree)) {
        RTT::log(RTT::Error) << "Could not get tree from model" << RTT::endlog();
        return false;
    }

    if(!model_tree.getChain("lwr_arm_base_link", lower_tip_link, lower)) {
        RTT::log(RTT::Error) << "Could not get lower chain from tree" << RTT::endlog();
        return false;
    }

    if(!model_tree.getChain(upper_root_link, "lwr_arm_7_link", upper)) {
        RTT::log(RTT::Error) << "Could not get upper chain from tree" << RTT::endlog();
    }

    q_lower = KDL::JntArray(lower.getNrOfJoints());
    q_upper = KDL::JntArray(upper.getNrOfJoints());
    j_lower = KDL::Jacobian(lower.getNrOfJoints());
    j_upper = KDL::Jacobian(upper.getNrOfJoints());

    fk_solver_pos_lower = std::unique_ptr<KDL::ChainFkSolverPos_recursive>(new KDL::ChainFkSolverPos_recursive(lower));
    fk_solver_pos_upper = std::unique_ptr<KDL::ChainFkSolverPos_recursive>(new KDL::ChainFkSolverPos_recursive(upper));

    jnt_to_jac_solver_lower = std::unique_ptr<KDL::ChainJntToJacSolver>(new KDL::ChainJntToJacSolver(lower));
    jnt_to_jac_solver_upper = std::unique_ptr<KDL::ChainJntToJacSolver>(new KDL::ChainJntToJacSolver(upper));

    model_loaded = true;
    return true;
}


Eigen::VectorXd Lwr_testdriver::computeTorquesUpper(const Eigen::Matrix<double, 6, 1>& axis, const double magnitude) {
    // To set force/torque values in EE frame, perform hand to base transformation
    fk_solver_pos_upper->JntToCart(q_upper, tip_upper);
    inv_upper = tip_upper.Inverse();

    htb_upper.setZero(6, 6);
    for(ind_j = 0; ind_j < 3; ind_j++) {
        for(ind_i = 0; ind_i < 3; ind_i++) {
            htb_upper(ind_i, ind_j) = static_cast<double>(inv_upper(ind_i, ind_j));
            htb_upper(3 + ind_i, 3 + ind_j) = static_cast<double>(inv_upper(ind_i, ind_j));
        }
    }

    jnt_to_jac_solver_upper->JntToJac(q_upper, j_upper);
    j_htb_upper.data = htb_upper * j_upper.data; // HTB-transformed jacobian for EE

    // Compute torques
    return j_htb_upper.data.transpose() * axis * magnitude;
}


Eigen::VectorXd Lwr_testdriver::computeTorquesLower(const Eigen::Matrix<double, 6, 1>& axis, const double magnitude) {
    // To set force/torque values in elbow frame, perform hand (or rather elbow) to base transformation
    fk_solver_pos_lower->JntToCart(q_lower, tip_lower);
    inv_lower = tip_lower.Inverse();

    htb_lower.setZero(6, 6);
    for(ind_j = 0; ind_j < 3; ind_j++) {
        for(ind_i = 0; ind_i < 3; ind_i++) {
            htb_lower(ind_i, ind_j) = static_cast<double>(inv_lower(ind_i, ind_j));
            htb_lower(3 + ind_i, 3 + ind_j) = static_cast<double>(inv_lower(ind_i, ind_j));
        }
    }

    jnt_to_jac_solver_lower->JntToJac(q_lower, j_lower);
    j_htb_lower.data = htb_lower * j_lower.data; // HTB-transformed jacobian for elbow

    // Compute torques
    if(elbow_to_base) {
        return j_htb_lower.data.transpose() * axis * magnitude;
    } else {
        return j_lower.data.transpose() * axis * magnitude;
    }
}


bool Lwr_testdriver::setMode(const std::string& mode) {
    /*
     * Driver knows 3 modes:
     * - none
     *   Will do absolutely nothing
     * - position
     *   Will use positioning torques to move to target joint angles
     * - torque
     *   Will apply torques set manually or computed by PID controller
     */

    if(mode == "position" || mode == "torque" || mode == "none") {
        this->mode = mode;
        return true;
    }

    RTT::log(RTT::Error) << "Available modes are position, torque and none" << RTT::endlog();
    return false;
}


void Lwr_testdriver::setForceAxisUpper(float x, float y, float z){
    hand_forces[0] = x;
    hand_forces[1] = y;
    hand_forces[2] = z;
}


void Lwr_testdriver::setForceAxisLower(float x, float y, float z){
    elbow_forces[0] = x;
    elbow_forces[1] = y;
    elbow_forces[2] = z;
}


void Lwr_testdriver::setTorqueAxisUpper(float x, float y, float z) {
    hand_forces[3] = x;
    hand_forces[4] = y;
    hand_forces[5] = z;
}


void Lwr_testdriver::setTorqueAxisLower(float x, float y, float z) {
    elbow_forces[3] = x;
    elbow_forces[4] = y;
    elbow_forces[5] = z;
}


void Lwr_testdriver::rampForcesUpper(float time, float x, float y, float z) {
    total_time_hand = static_cast<double>(time);
    start_time_hand = RTT::os::TimeService::ticks2nsecs(RTT::os::TimeService::Instance()->getTicks());
    end_time_hand = start_time_hand + total_time_hand;

    hand_forces_diff = hand_forces;

    setForceAxisUpper(x, y, z);
    ramp_forces_hand = true;
}


void Lwr_testdriver::rampForcesLower(float time, float x, float y, float z) {
    total_time_elbow = static_cast<double>(time);
    start_time_elbow = RTT::os::TimeService::ticks2nsecs(RTT::os::TimeService::Instance()->getTicks());
    end_time_elbow = start_time_elbow + total_time_elbow;

    elbow_forces_diff = elbow_forces;

    setForceAxisLower(x, y, z);
    ramp_forces_elbow = true;
}


Eigen::VectorXd Lwr_testdriver::controlPID(const Eigen::VectorXd& target, const Eigen::VectorXd& current) {
    e_previous = e_current;
    e_current = target - current;
    e_total += e_current;

    return k_p * e_current
            + k_i * e_total
            + k_d * (e_current - e_previous);
}


void Lwr_testdriver::averageTau(int frames) {
    frames_total = frames;
    frames_counter = 0;
    tau_sum = Eigen::VectorXf::Zero(tau.size());
}


void Lwr_testdriver::printShit(){
    std::cout<<"---------HTB (upper chain)--------------"<<std::endl;
    std::cout<<htb_upper<<std::endl;

    std::cout<<"---------JAC (upper chain)--------------"<<std::endl;
    std::cout<<j_upper.data<<std::endl;

    std::cout<<"---------INV (upper chain)--------------"<<std::endl;
    std::cout<<inv_upper<<std::endl;

    std::cout<<"---------TAU----------------------------"<<std::endl;
    std::cout<<torques_out_data<<std::endl;

    std::cout<<"---------Segments (upper chain)---------"<<std::endl;
    std::cout<<upper.getNrOfSegments()<<std::endl;
}


/*
 * Using this macro, only one component may live
 * in one library *and* you may *not* link this library
 * with another component library. Use
 * ORO_CREATE_COMPONENT_TYPE()
 * ORO_LIST_COMPONENT_TYPE(Lwr_testdriver)
 * In case you want to link with another library that
 * already contains components.
 *
 * .f you have put your component class
 * in a namespace, don't forget to add it here too:
 */
ORO_CREATE_COMPONENT(Lwr_testdriver)
