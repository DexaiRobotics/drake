#include <iostream>
#include <cmath>

#define PI 3.14159265

#include <lcm/lcm-cpp.hpp>

#include "drake/lcmt_iiwa_command.hpp"
#include "drake/lcmt_iiwa_status.hpp"

#include "drake/common/drake_assert.h"
#include "drake/common/drake_path.h"
#include "drake/common/polynomial.h"
#include "drake/examples/kuka_iiwa_arm/iiwa_status.h"
#include "drake/systems/plants/IKoptions.h"
#include "drake/systems/plants/RigidBodyIK.h"
#include "drake/systems/plants/RigidBodyTree.h"
#include "drake/systems/plants/constraint/RigidBodyConstraint.h"
#include "drake/systems/plants/joints/floating_base_types.h"
#include "drake/systems/trajectories/PiecewisePolynomial.h"
#include "drake/systems/vector.h"

namespace drake {
namespace examples {
namespace kuka_iiwa_arm {
namespace {

using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::VectorXi;
using Eigen::Vector2d;
using Eigen::Vector3d;

using drake::Vector1d;

const char* kLcmCommandChannel = "IIWA_COMMAND";

/// This is a really simple demo class to run a trajectory which is
/// the output of an IK plan.  It lacks a lot of useful things, like a
/// controller which does a remotely good job of mapping the
/// trajectory onto the robot.  The paramaters @p nT and @p t are
/// identical to their usage for inverseKinPointwise (@p nT is number
/// of time samples and @p t is an array of times in seconds).
class TrajectoryRunner {
 public:
  TrajectoryRunner(std::shared_ptr<lcm::LCM> lcm, int nT, const double* t,
                   const Eigen::MatrixXd& traj)
      : lcm_(lcm), nT_(nT), t_(t), traj_(traj) {
    lcm_->subscribe(IiwaStatus<double>::channel(),
                    &TrajectoryRunner::HandleStatus, this);
    DRAKE_ASSERT(traj_.cols() == nT);
  }

  void Run() {
    typedef PiecewisePolynomial<double> PPType;
    typedef PPType::PolynomialType PPPoly;
    typedef PPType::PolynomialMatrix PPMatrix;
    std::vector<PPMatrix> polys;
    std::vector<double> times;

    // For each timestep, create a PolynomialMatrix for each joint
    // position.  Each column of traj_ represents a particular time,
    // and the rows of that column contain values for each joint
    // coordinate.
    for (int i = 0; i < nT_; i++) {
      PPMatrix poly_matrix(traj_.rows(), 1);
      const auto traj_now = traj_.col(i);

      // Produce interpolating polynomials for each joint coordinate.
      if (i != nT_ - 1) {
        for (int row = 0; row < traj_.rows(); row++) {
          Eigen::Vector2d coeffs(0, 0);
          coeffs[0] = traj_now(row);

          // Sets the coefficients for a linear polynomial within the interval
          // of time t_[i] < t < t_[i+1] so that the entire piecewise polynomial
          // is continuous at the time instances t_[i].
          // PiecewisePolynomial<T>::value(T t) clamps t to be between t_[0] and
          // t_[nT-1] so that for t > t_[nT-1] the piecewise polynomial always
          // evaluates to the last trajectory instance, traj_.col(nT_-1).
          coeffs[1] = (traj_(row, i + 1) - coeffs[0]) / (t_[i + 1] - t_[i]);
          poly_matrix(row) = PPPoly(coeffs);
        }
        polys.push_back(poly_matrix);
      }
      times.push_back(t_[i]);
    }

    PPType pp_traj(polys, times);

    bool time_initialized = false;
    int64_t start_time_ms = -1;
    int64_t cur_time_ms = -1;
    const int64_t end_time_offset_ms = (t_[nT_ - 1] * 1e3);
    DRAKE_ASSERT(end_time_offset_ms > 0);

    lcmt_iiwa_command iiwa_command;
    iiwa_command.num_joints = kNumJoints;
    iiwa_command.joint_position.resize(kNumJoints, 0.);
    iiwa_command.num_torques = 0;
    iiwa_command.joint_torque.resize(kNumJoints, 0.);

    while (!time_initialized ||
           cur_time_ms < (start_time_ms + end_time_offset_ms)) {
      // The argument to handleTimeout is in msec, and should be
      // safely bigger than e.g. a 200Hz input rate.
      int handled  = lcm_->handleTimeout(10);
      if (handled <= 0) {
        std::cerr << "Failed to receive LCM status." << std::endl;
        return;
      }

      if (!time_initialized) {
        start_time_ms = iiwa_status_.timestamp;
        time_initialized = true;
      }
      cur_time_ms = iiwa_status_.timestamp;

      const double cur_traj_time_s =
          static_cast<double>(cur_time_ms - start_time_ms) / 1e3;
      const auto desired_next = pp_traj.value(cur_traj_time_s);

      iiwa_command.timestamp = iiwa_status_.timestamp;

      // This is totally arbitrary.  There's no good reason to
      // implement this as a maximum delta to submit per tick.  What
      // we actually need is something like a proper
      // planner/interpolater which spreads the motion out over the
      // entire duration from current_t to next_t, and commands the
      // next position taking into account the velocity of the joints
      // and the distance remaining.
      const double max_joint_delta = 0.1;
      for (int joint = 0; joint < kNumJoints; joint++) {
        double joint_delta =
            desired_next(joint) - iiwa_status_.joint_position_measured[joint];
        joint_delta = std::max(-max_joint_delta,
                               std::min(max_joint_delta, joint_delta));
        iiwa_command.joint_position[joint] =
            iiwa_status_.joint_position_measured[joint] + joint_delta;
      }

      lcm_->publish(kLcmCommandChannel, &iiwa_command);
    }
  }

 private:
  void HandleStatus(const lcm::ReceiveBuffer* rbuf, const std::string& chan,
                    const lcmt_iiwa_status* status) {
    iiwa_status_ = *status;
  }

  static const int kNumJoints = 7;
  std::shared_ptr<lcm::LCM> lcm_;
  const int nT_;
  const double* t_;
  const Eigen::MatrixXd& traj_;
  lcmt_iiwa_status iiwa_status_;
};

int main(int argc, const char* argv[]) {
  std::shared_ptr<lcm::LCM> lcm = std::make_shared<lcm::LCM>();

  RigidBodyTree tree(
      drake::GetDrakePath() + "/examples/kuka_iiwa_arm/urdf/iiwa14.urdf",
      drake::systems::plants::joints::kFixed);

  // Create a basic pointwise IK trajectory for moving the iiwa arm.
  // We start in the zero configuration (straight up).


  const int kNumTimesteps = 2; // must be even
  double dt = 4;
  double t[kNumTimesteps];

  VectorXd zero_conf = tree.getZeroConfiguration();
  MatrixXd q0(tree.get_num_positions(), kNumTimesteps);
  std::vector<RigidBodyConstraint*> constraint_array;
  for (int i=0; i<kNumTimesteps; i++){
    q0.col(i) = zero_conf;
    t[i] = dt*i;
    if (i%2 == 1){
      Vector3d pos_end;
      pos_end << 0.3*cos(((double)i)*PI/kNumTimesteps), 0.3*sin(((double)i)*PI/kNumTimesteps), .5;

      printf("pos_end: %f, %f, %f\n",pos_end[0],pos_end[1],pos_end[2]);
      Vector2d tspan(dt*((double)i-0.5), dt*((double)i+0.5));
      printf("tspan: %f, %f\n", tspan[0], tspan[1]);
      Vector3d pos_lb = pos_end - Vector3d::Constant(0.05);
      Vector3d pos_ub = pos_end + Vector3d::Constant(0.05);
      WorldPositionConstraint wpc(&tree, tree.FindBodyIndex("iiwa_link_ee"),
                                  Vector3d::Zero(), pos_lb, pos_ub,
                                  tspan);
      constraint_array.push_back(&wpc);      
    }
  }

  IKoptions ikoptions(&tree);
  int info[kNumTimesteps];
  MatrixXd q_sol(tree.get_num_positions(), kNumTimesteps);
  std::vector<std::string> infeasible_constraint;

  std::cout << "Num constraints imposed: " << constraint_array.size() << std::endl;
  std::cout << "q0: " << q0 << std::endl;
  // std::cout << "constraint array: " << constraint_array.data() << std::endl;
  

  inverseKinPointwise(&tree, kNumTimesteps, t, q0, q0, constraint_array.size(),
                      constraint_array.data(), ikoptions, &q_sol, info,
                      &infeasible_constraint);
  
  std::cout << "Num infeasible constraints: " << infeasible_constraint.size() << std::endl;
  for (unsigned int i = 0; i < infeasible_constraint.size(); i++){
    std::cout << infeasible_constraint[i] << std::endl;
  }
  
  bool info_good = true;
  for (int i = 0; i < kNumTimesteps; ++i) {
    printf("INFO[%d] = %d ", i, info[i]);
    if (info[i] != 1) {
      info_good = false;
    }
  }
  printf("\n");

  if (!info_good) {
    std::cerr << "Solution failed, not sending." << std::endl;
    return 1;
  }

  std::cout << q_sol << std::endl;

  // Now run through the plan.
  TrajectoryRunner runner(lcm, kNumTimesteps, t, q_sol);
  runner.Run();
  return 0;
}

}  // namespace
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake


int main(int argc, const char* argv[]) {
  return drake::examples::kuka_iiwa_arm::main(argc, argv);
}
