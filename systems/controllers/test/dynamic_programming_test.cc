#include "drake/systems/controllers/dynamic_programming.h"

#include <cmath>

#include <gtest/gtest.h>

#include "drake/common/proto/call_matlab.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/math/barycentric.h"
#include "drake/systems/controllers/linear_quadratic_regulator.h"
#include "drake/systems/primitives/integrator.h"
#include "drake/systems/primitives/linear_system.h"

namespace drake {
namespace systems {
namespace controllers {
namespace {

// Minimum-time problem for the single integrator (which has a trivial solution,
// that can be achieved exactly on a mesh when timestep=1).
// ẋ = u,  u ∈ {-1,0,1}.
// g(x,u) = 0 if x == 0, 1 otherwise.
// The optimal solution is: J(x) = |x|.
GTEST_TEST(FittedValueIterationTest, SingleIntegrator) {
  const int kNumStates = 1;
  Integrator<double> sys(kNumStates);

  Simulator<double> simulator(sys);

  // minimum time cost function (1 for all non-zero states).
  const auto cost_function = [](const Context<double>& context) {
    double x = context.get_continuous_state()[0];
    return (std::abs(x) > 0.1) ? 1. : 0.;
  };

  const math::BarycentricMesh<double>::MeshGrid state_grid(
      {{-4., -3., -2., -1., 0., 1., 2., 3., 4.}});
  const math::BarycentricMesh<double>::MeshGrid input_grid({{-1., 0., 1.}});

  const double timestep = 1.0;

  auto solution = FittedValueIteration(&simulator, cost_function, state_grid,
                                   input_grid, timestep);

  // Optimal cost-to-go is |x|.
  Eigen::RowVectorXd J_expected(static_cast<int>(state_grid[0].size()));
  J_expected << 4., 3., 2., 1., 0., 1., 2., 3., 4.;
  EXPECT_TRUE(CompareMatrices(solution.second, J_expected, 1e-8));

  // Optimal policy is 1 if x < 0, 0 if x = 0, -1 if x > 0.
  EXPECT_EQ(solution.first->get_output_port().size(), 1);
  auto context = solution.first->CreateDefaultContext();
  auto output = solution.first->get_output_port().Allocate(*context);
  for (const double x : state_grid[0]) {
    context->FixInputPort(0, Vector1d{x});
    solution.first->get_output_port().Calc(*context, output.get());
    double y = output->GetValue<BasicVector<double>>()[0];
    EXPECT_EQ(y, (x < 0) - (x > 0));  // implements -sgn(x).
  }
}

// Single integrator minimum time problem, but with the goal at -3, and the
// state wrapped on itself.
GTEST_TEST(FittedValueIterationTest, PeriodicBoundary) {
  const int kNumStates = 1;
  Integrator<double> sys(kNumStates);

  Simulator<double> simulator(sys);

  // minimum time cost function (1 for all non-zero states).
  const auto cost_function = [](const Context<double>& context) {
    double x = context.get_continuous_state()[0];
    return (std::abs(x + 3.) > 0.1) ? 1. : 0.;
  };

  const math::BarycentricMesh<double>::MeshGrid state_grid(
      {{-4., -3., -2., -1., 0., 1., 2., 3., 4.}});
  const math::BarycentricMesh<double>::MeshGrid input_grid({{-1., 0., 1.}});

  const double timestep = 1.0;

  DynamicProgrammingOptions options;
  options.state_indices_with_periodic_boundary_conditions = {0};

  auto solution = FittedValueIteration(&simulator, cost_function, state_grid,
                                   input_grid, timestep, options);

  // Optimal cost-to-go is |x|.
  Eigen::RowVectorXd J_expected(static_cast<int>(state_grid[0].size()));
  J_expected << 1., 0., 1., 2., 3., 4., 3., 2., 1.;
  EXPECT_TRUE(CompareMatrices(solution.second, J_expected, 1e-8));
}

// Linear quadratic regulator for the double integrator.
// q̈ = u,  g(x,u) = x'Qx + u'Ru.
// Note: we only expect the numerical solution to be very approximate, due to
// discretization errors.
GTEST_TEST(FittedValueIteration, DoubleIntegrator) {
  Eigen::Matrix2d A;
  A << 0., 1., 0., 0.;
  const Eigen::Vector2d B{0., 1.};
  const Eigen::Matrix2d C = Eigen::Matrix2d::Identity();
  const Eigen::Vector2d D = Eigen::Vector2d::Zero();
  LinearSystem<double> sys(A, B, C, D);

  const Eigen::Matrix2d Q = Eigen::Matrix2d::Identity();
  const double R = 1.;

  Simulator<double> simulator(sys);

  // minimum time cost function (1 for all non-zero states).
  const auto cost_function = [&sys, &Q, &R](const Context<double>& context) {
    const Eigen::Vector2d x = context.get_continuous_state().CopyToVector();
    const double u = sys.EvalVectorInput(context, 0)->GetAtIndex(0);
    return x.dot(Q * x) + u * R * u;
  };

  math::BarycentricMesh<double>::MeshGrid state_grid(2);
  for (double x = -3.; x <= 3.; x += .2) {
    state_grid[0].insert(x);
  }
  for (double xdot = -4.; xdot <= 4.; xdot += .2) {
    state_grid[1].insert(xdot);
  }

  math::BarycentricMesh<double>::MeshGrid input_grid(1);
  for (double u = -6.; u <= 6.; u += .5) {
    input_grid[0].insert(u);
  }

  const double timestep = .01;

  auto solution = FittedValueIteration(&simulator, cost_function, state_grid,
                                   input_grid, timestep);

  // Note: Compare against continuous time solution, even though we are solving
  // a discretized version.  Confirmed in MATLAB (due to #8034) that cost-to-go
  // is equal to the 3rd decimal, using
  // sys = ss(A,B,eye(2),zero(2,1))
  // dsys = c2d(sys,.01)
  // [K,S] = dlqr(dsys.A,dsys.B,Q*dt,R*dt)
  auto optimal = LinearQuadraticRegulator(A, B, Q, Vector1d(R));

  math::BarycentricMesh<double> state_mesh(state_grid);

  {  // Plot in Matlab.  (Costs little here and is very useful for any future
     // debugging).
    using common::CallMatlab;
    Eigen::VectorXd Qbins(state_grid[0].size());
    Eigen::VectorXd Qdotbins(state_grid[1].size());
    Eigen::MatrixXd J(state_grid[1].size(), state_grid[0].size());
    Eigen::MatrixXd Jstar(state_grid[1].size(), state_grid[0].size());

    int i = 0;
    for (const double q : state_grid[0]) {
      Qbins(i) = q;
      int j = 0;
      for (const double qdot : state_grid[1]) {
        Qdotbins(j) = qdot;
        const Eigen::Vector2d x{q, qdot};
        J(j, i) = state_mesh.Eval(solution.second, x)[0];
        Jstar(j, i) = x.dot(optimal.S * x);
        j++;
      }
      i++;
    }

    CallMatlab("figure", 1);
    CallMatlab("subplot", 1, 2, 1);
    CallMatlab("surf", Qbins, Qdotbins, J);
    CallMatlab("subplot", 1, 2, 2);
    CallMatlab("surf", Qbins, Qdotbins, Jstar);
  }

  for (double q = -2.5; q <= 2.5; q += 1.) {
    for (double qdot = -3.5; qdot <= 3.5; qdot += 1.) {
      const Eigen::Vector2d x{q, qdot};
      const double J = x.dot(optimal.S * x);
      // Note: Magnitudes range from 0 to ~60.
      // We don't expect these solutions to be too similar (the differ mostly at
      // the positive and negative orthants, were the boundary effects have an
      // impact, but also on the total magnitude of the cost through, due to the
      // approximation on the grid and the (more importantly) the discretization
      // of actions.  The matlab plots above are as expected, and this guard
      // will make sure they remain close.
      EXPECT_NEAR(state_mesh.Eval(solution.second, x)[0], J, 1. + .2 * J);
    }
  }
}

}  // namespace
}  // namespace controllers
}  // namespace systems
}  // namespace drake
