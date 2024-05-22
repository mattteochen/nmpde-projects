/**
 * @file IdealizedLVFiberGuccione.hpp
 * @brief Header file defining the idealized lv solver class.
 */

#ifndef IDEALIZED_LV_FIBER_GUCCIONE_HPP
#define IDEALIZED_LV_FIBER_GUCCIONE_HPP

#include <cardiac_mechanics/BaseSolverGuccione.hpp>
#include <poisson/Poisson.hpp>

#include <deal.II/base/numbers.h>
#include <deal.II/base/symmetric_tensor.h>
#include <deal.II/fe/mapping_fe.h>
#include <deal.II/lac/trilinos_solver.h>

#include <cmath>
#ifdef BUILD_TYPE_DEBUG
#include <fstream>
#include <string>
#endif

/**
 * @class IdealizedLVFiberGuccione
 * @brief Class representing the an Idealized LV solver with fiber contraction
 * (https://pubmed.ncbi.nlm.nih.gov/26807042/)
 */
template <int dim, typename Scalar>
class IdealizedLVFiberGuccione : public BaseSolverGuccione<dim, Scalar> {
  /**
   * Alias for base class
   */
  using Base = BaseSolverGuccione<dim, Scalar>;
  /**
   * Sacado automatic differentiation type code from
   */
  static constexpr Differentiation::AD::NumberTypes ADTypeCode =
      Differentiation::AD::NumberTypes::sacado_dfad;
  /**
   * Alias for the AD helper
   */
  using ADHelper =
      Differentiation::AD::ResidualLinearization<ADTypeCode, double>;
  /**
   * Alias for the AD number type
   */
  using ADNumberType = typename ADHelper::ad_type;

public:
  /**
   * @brief Constructor
   * @param parameters_file_name_ The parameters file name
   * @param mesh_file_name_ The mesh file name
   * @param problem_name_ The problem name
   */
  IdealizedLVFiberGuccione(const std::string &parameters_file_name_,
                           const std::string &mesh_file_name_,
                           const std::string &problem_name_)
      : Base(parameters_file_name_, mesh_file_name_, problem_name_),
        zero_function(dealii::Functions::ZeroFunction<dim>(dim)),
        poisson_solver(mesh_file_name_, Base::r) {
    Base::prm.enter_subsection("Pressure");
    { fiber_pressure = Base::prm.get_double("FiberValue"); }
    Base::prm.leave_subsection();
    Base::pcout << "Problem pressure configuration" << std::endl;
    Base::pcout << "  Boundary pressure value: " << Base::pressure.value()
                << " Pa" << std::endl;
    Base::pcout << "  Fiber pressure value: " << fiber_pressure << " Pa"
                << std::endl;
    Base::pcout << "==============================================="
                << std::endl;
  }
  /**
   * @brief Initialise boundaries tag. Boundaries are problem specific hence we
   * override the base virtual implementation.
   */
  void initialise_boundaries_tag() override {
    //  Set Newmann boundary faces
    for (auto &t : Base::boundaries_utility.get_newmann_boundaries_tags()) {
      Base::newmann_boundary_faces.insert(t);
    }

    // Set Dirichlet boundary faces
    for (auto &t : Base::boundaries_utility.get_dirichlet_boundaries_tags()) {
      Base::dirichlet_boundary_functions[t] = &zero_function;
    }
  };
  /**
   * @see Base::compute_piola_kirchhoff()
   */
  void compute_piola_kirchhoff(
      Tensor<2, dim, ADNumberType> &out_tensor,
      const Tensor<2, dim, ADNumberType> &solution_gradient_quadrature,
      const unsigned cell_index) override {

    auto norm = [](const auto &v) {
      auto sum = 0.0;
      for (auto &n : v) {
        sum += n * n;
      }
      return static_cast<decltype(v[0])>(std::sqrt(sum));
    };

    auto normalize = [](auto &v, const auto norm) {
      for (auto &n : v) {
        n /= norm;
      }
    };

    const auto &poisson_solution = poisson_solver.get_solution();
    const auto &poisson_dof_indices =
        poisson_solver.get_aggregate_dof_indices();
    Tensor<1, dim, ADNumberType> f;

    for (unsigned i = 0; i < poisson_solver.fe->dofs_per_cell; ++i) {
      const auto global_index = poisson_dof_indices[cell_index][i];
      const auto &support_point = dofs_support_points[global_index];

#ifdef BUILD_TYPE_DEBUG
      Assert(global_index >= 0 && global_index < poisson_solution.size(),
             ExcMessage("global_index out of bounds, global_index = "
                 + std::to_string(global_index) + " poisson_solution size = "
                 + std::to_string(poisson_solution.size()) + "\n"));
#endif
      // retrive the t value
      const Scalar t = poisson_solution[global_index];
      // compute fiber parameters
      // TODO: move them in the config file
      const Scalar d_focal = 45.0;
      const Scalar nu_endo = 0.6;
      const Scalar nu_epi = 0.8;
      const Scalar endo_r_1 = d_focal * std::sinh(nu_endo);
      const Scalar endo_r_2 = d_focal * std::cosh(nu_endo);
      const Scalar epi_r_1 = d_focal * std::sinh(nu_epi);
      const Scalar epi_r_2 = d_focal * std::cosh(nu_epi);
      const Scalar endo_epi_r_1_delta = std::abs(endo_r_1 - epi_r_1);
      const Scalar endo_epi_r_2_delta = std::abs(endo_r_2 - epi_r_2);
      const Scalar r_s = endo_r_1 + endo_epi_r_1_delta * t;
      const Scalar r_e = endo_r_2 + endo_epi_r_2_delta * t;
      const Scalar u_rad = std::acos(support_point[2] / r_e);
      const Scalar v_rad1 =
          std::asin(support_point[1] / (r_s * std::sin(u_rad)));
      const Scalar v_rad2 =
          std::acos(support_point[0] / (r_s * std::sin(u_rad)));
      const Scalar alpha_deg = 90.0 - 180.0 * t;
      const Scalar alpha_rad = alpha_deg * (M_PI / 180.0);

      // sometimes when the argument of the std::asin or std::acos is ~1/~-1 we
      // have detected numerical imprecisions that will lead to NaN. We are
      // skipping those Dofs contributions (from tests we have detected only one
      // point over ~74k total Dofs (with r = 1))
      if ((std::isnan(v_rad1) && std::isnan(v_rad2)) || std::isnan(u_rad)) {
#ifdef BUILD_TYPE_DEBUG
        std::cout << "ERR: rank: " << Base::mpi_rank
                  << " failed point with global index = " << global_index
                  << ": " << support_point[0] << " " << support_point[1] << " "
                  << support_point[2] << std::endl;
#endif
        continue;
      }
      // angle v can be recovered by using 2 equations
      // (https://pubmed.ncbi.nlm.nih.gov/26807042/)
      const Scalar v_rad = std::isnan(v_rad1) ? v_rad2 : v_rad1;

      // declare partial derivative vectors (handmade derivative)
      std::vector<Scalar> dx_du = {r_s * std::cos(u_rad) * std::cos(v_rad),
                                   r_s * std::cos(u_rad) * std::sin(v_rad),
                                   -r_e * std::sin(u_rad)};
      std::vector<Scalar> dx_dv = {-1.0 * r_s * std::sin(u_rad) *
                                       std::sin(v_rad),
                                   r_s * std::sin(u_rad) * std::cos(v_rad), 0};
      normalize(dx_du, norm(dx_du));
      normalize(dx_dv, norm(dx_dv));
      // compute vector f
      for (unsigned i = 0; i < dim; ++i) {
        f[i] +=
            ADNumberType(dx_du[i]) * Sacado::Fad::sin(ADNumberType(alpha_rad)) +
            ADNumberType(dx_dv[i]) * Sacado::Fad::cos(ADNumberType(alpha_rad));
      }
    }
    out_tensor += (ADNumberType(fiber_pressure) * dealii::outer_product(f, f));
    Base::compute_piola_kirchhoff(out_tensor, solution_gradient_quadrature,
                                  cell_index);
  }
  /**
   * @see Base::solve_newton()
   */
  void solve_newton() override {
    Base::pcout << "==============================================="
                << std::endl;
    poisson_solver.assemble();
    poisson_solver.solve();
    Base::solve_newton();
  }

  void setup() override {
    poisson_solver.setup();
    Base::setup();
    switch (Base::triangulation_type) {
    case Base::TriangulationType::T: {
      FE_SimplexP<dim> fe_linear(Base::r);
      MappingFE mapping(fe_linear);
      DoFTools::map_dofs_to_support_points(mapping, poisson_solver.dof_handler,
                                           dofs_support_points);
      break;
    };
    case Base::TriangulationType::Q: {
      FE_Q<dim> fe_linear(Base::r);
      MappingFE mapping(fe_linear);
      DoFTools::map_dofs_to_support_points(mapping, poisson_solver.dof_handler,
                                           dofs_support_points);
      break;
    };
    }
#ifdef BUILD_TYPE_DEBUG
    std::cout << "  rank = " << Base::mpi_rank
              << " dof support points count (based on poisson dofs) = "
              << dofs_support_points.size() << std::endl;
    std::ofstream out_f("dofs_support_points" + std::to_string(Base::mpi_rank) +
                        ".log");
    for (auto &[k, v] : dofs_support_points) {
      out_f << v[0] << " " << v[1] << " " << v[2] << std::endl;
    }
    out_f.close();
#endif
  }

protected:
  /**
   * Utility zero function for Dirichilet boundary
   */
  dealii::Functions::ZeroFunction<dim> zero_function;
  /**
   * Dofs support points
   */
  std::map<dealii::types::global_dof_index, Point<dim>> dofs_support_points;
  /**
   * Utility object to solve Poisson problems
   */
  Poisson<dim, Scalar> poisson_solver;
  /**
   * The fiber pressure T_a value (https://pubmed.ncbi.nlm.nih.gov/26807042/)
   */
  Scalar fiber_pressure;
};

#endif // IDEALIZED_LV_FIBER_GUCCIONE_HPP
