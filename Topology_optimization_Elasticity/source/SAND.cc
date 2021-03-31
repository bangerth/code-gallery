/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2021 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE.md at
 * the top level directory of deal.II.
 *
 * ---------------------------------------------------------------------

 *
 * Author: Justin O'Connor, Colorado State University, 2021.
 */


// @sect3{Preliminaries}

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/tensor.h>
#include <deal.II/base/timer.h>

#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/linear_operator.h>
#include <deal.II/lac/packaged_operation.h>
#include <deal.II/lac/sparse_direct.h>
#include <deal.II/lac/affine_constraints.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_refinement.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_q.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>

#include <iostream>
#include <fstream>
#include <algorithm>

// Above are fairly normal files to include.  These also include the
// one for the sparse direct class SparseDirectUMFPACK. This is not
// the most efficient way to solve large linear problems, but it will
// do for now.
//
// As usual, we put everything into a common namespace. We then start
// by declaring a number of symbolic names for numbers that will be
// used throughout. Specifically, we have a *lot* of variables in this
// program (of course the density and the displacement, but also the
// unfiltered density and quite a number of Lagrange multipliers. It
// is easy to forget which of these variables is at which position in
// the solution vector, and trying to use numbers for these vector
// components is a prescription for bugs. Rather, we define static
// variables that can be used in all of these places and that have to
// be initialized only once. In practice, this will lead to some
// lengthy expressions, but they are more readable and less likely to
// be wrong.
//
// A similar issue arises with the ordering of blocks in the system
// matrix and in vectors. The matrices have $9\times 9$ blocks, and
// it's difficult to remember which is which. It is far easier to just
// use symbolic names for those as well.
//
// Finally, while we're at it, we introduce symbolic names also for
// the boundary indicators we will use, in the same spirit as was done
// in step-19.
//
// In all of these cases, we declare these variables as members in a
// namespace. In the case of the solution components, the concrete
// values of these variables depend on the space dimension, so we use
// [template
// variables](https://en.cppreference.com/w/cpp/language/variable_template)
// to make the value of the variable depend on a template argument in
// the same way as we often use template functions.
namespace SAND {
    using namespace dealii;
    
    namespace SolutionComponents
    {
      template <int dim> constexpr unsigned int density = 0;
      template <int dim> constexpr unsigned int displacement =1;
      template <int dim> constexpr unsigned int unfiltered_density = 1+dim;
      template <int dim> constexpr unsigned int displacement_multiplier = 2+dim;
      template <int dim> constexpr unsigned int unfiltered_density_multiplier = 2+2*dim;
      template <int dim> constexpr unsigned int density_lower_slack =  3+2*dim;
      template <int dim> constexpr unsigned int density_lower_slack_multiplier =  4+2*dim;
      template <int dim> constexpr unsigned int density_upper_slack =  5+2*dim;
      template <int dim> constexpr unsigned int density_upper_slack_multiplier =  6+2*dim;
    }

    namespace SolutionBlocks
    {
        constexpr unsigned int density = 0;
        constexpr unsigned int displacement = 1;
        constexpr unsigned int unfiltered_density = 2;
        constexpr unsigned int displacement_multiplier = 3;
        constexpr unsigned int unfiltered_density_multiplier = 4;
        constexpr unsigned int density_lower_slack = 5;
        constexpr unsigned int density_lower_slack_multiplier = 6;
        constexpr unsigned int density_upper_slack = 7;
        constexpr unsigned int density_upper_slack_multiplier = 8;
    }

    namespace BoundaryIds
    {
        constexpr types::boundary_id no_force = 101;
        constexpr types::boundary_id down_force = 102;
        constexpr types::boundary_id held_still = 103;
    }


  // @sect3{The SANDTopOpt main class}
  
  // Next up is the main class for this problem. The majority of functions
  // follow the usual naming schemes of tutorial programs, though there
  // are a couple that have been broken out of what is usually called
  // the `setup_system()` function because of their length, and there
  // are also a number that deal with various aspects of the
  // optimization algorithm.
  //
  // As an added bonus, the program writes the computed design as an STL
  // file that one can, for example, send to a 3d printer.
    template<int dim>
    class SANDTopOpt
    {
    public:
        SANDTopOpt();

        void
        run();

    private:
        void
        create_triangulation();

        void
        setup_boundary_values();

        void
        setup_block_system();

        void
        setup_filter_matrix();

        void
        assemble_system(const double barrier_size);

        void
        solve();
        std::pair<double,double>
        calculate_max_step_size(const BlockVector<double> &state, const BlockVector<double> &step, const double barrier_size) const;

        BlockVector<double>
        calculate_test_rhs(const BlockVector<double> &test_solution, const double barrier_size) const;

        double
        calculate_exact_merit(const BlockVector<double> &test_solution, const double barrier_size);

        BlockVector<double>
        find_max_step(const BlockVector<double> &state, const double barrier_size);

        BlockVector<double>
        take_scaled_step(const BlockVector<double> &state, const BlockVector<double> &step, const double descent_requirement,const double barrier_size);

        bool
        check_convergence(const BlockVector<double> &state, const double barrier_size);

        void
        output_results(const unsigned int j) const;

        void
        write_as_stl();


      // Most of the member variables are also standard. There are,
      // however, a number of variables that are specifically related
      // to the optimization algorithm (such the various scalar
      // factors below) as well as the filter matrix to ensure that
      // the design remains smooth.
        Triangulation<dim> triangulation;
        FESystem<dim> fe;
        DoFHandler<dim> dof_handler;
        AffineConstraints<double> constraints;

        std::map<types::global_dof_index, double> boundary_values;
      
        BlockSparsityPattern sparsity_pattern;
        BlockSparseMatrix<double> system_matrix;
      
        SparsityPattern filter_sparsity_pattern;
        SparseMatrix<double> filter_matrix;
      
        BlockVector<double> linear_solution;
        BlockVector<double> system_rhs;
        BlockVector<double> nonlinear_solution;
      
        const double density_ratio;
        const double density_penalty_exponent;
        const double filter_r;
        double penalty_multiplier;


        TimerOutput timer;
    };


  // @sect3{Constructor and set up functions}

  
    // This problem has quite a lot of variables. We initialize a
    // FESystem composed of 2×dim `FE_Q(1)` elements for the
    // displacement variable and its Lagrange multiplier, and 7
    // `FE_DGQ(0)` elements.  These piecewise constant functions are
    // for density-related variables: the density itself, the
    // unfiltered density, the slack variables for the lower and upper
    // bounds on the unfiltered density, and then Lagrange multipliers
    // for the connection between filtered and unfiltered densities as
    // well as for the inequality constraints.
    //
    // The order in which these elements appear is documented above.
    template<int dim>
    SANDTopOpt<dim>::SANDTopOpt()
            :
            fe(FE_DGQ<dim>(0) ^ 1,
               (FESystem<dim>(FE_Q<dim>(1) ^ dim)) ^ 1,
               FE_DGQ<dim>(0) ^ 1,
               (FESystem<dim>(FE_Q<dim>(1) ^ dim)) ^ 1,
               FE_DGQ<dim>(0) ^ 5),
            dof_handler(triangulation),
            density_ratio (.5),
            density_penalty_exponent (3),
            filter_r (.251),
            penalty_multiplier (1),
            timer(std::cout,
                  TimerOutput::summary,
                  TimerOutput::wall_times)
    {}


    // The first step then is to create the triangulation that matches
    // the problem description in the introduction -- a 6-by-1
    // rectangle (or a 6-by-1-by-1 box in 3d) where a force will be
    // applied in the top center. This triangulation is then uniformly
    // refined a number of times.
    //
    // In contrast to nearly the entire rest of this program, this
    // function specifically assumes that we are in 2d and will
    // require changes if we wanted to move to 3d simulations. We
    // ensure that nobody tries to accidentally run in 3d without such
    // modifications through an assertion at the top of the function.
    template<int dim>
    void
    SANDTopOpt<dim>::create_triangulation() {
      Assert (dim==2, ExcNotImplemented());
      GridGenerator::subdivided_hyper_rectangle (triangulation, 
                                                 {6,1},
                                                 Point<dim>(0,0),
                                                 Point<dim>(6,1));
              
      triangulation.refine_global(3);

        // The second step is to apply boundary indicators to parts of
        // the boundary. The following code assigns boundary
        // indicators to the bottom, top, left, and right boundaries
        // of the box, respectively. The center region of the top
        // boundary is given a separate boundary indicator: This is
        // where we will apply the down force.
        for (const auto &cell : triangulation.active_cell_iterators()) {
          for (const auto &face : cell->face_iterators())
            {
                if (face->at_boundary()) {
                    const auto center = face->center();
                    if (std::fabs(center(1) - 0) < 1e-12) {
                        face->set_boundary_id(BoundaryIds::no_force);
                    }
                    else if (std::fabs(center(1) - 1) < 1e-12) {
                        if ((std::fabs(center(0) - 3) < .3)) {
                            face->set_boundary_id(BoundaryIds::down_force);
                        } else {
                            face->set_boundary_id(BoundaryIds::no_force);
                        }
                    }
                    else if (std::fabs(center(0) - 0) < 1e-12) {
                        face->set_boundary_id(BoundaryIds::no_force);
                    }
                    else if (std::fabs(center(0) - 6) < 1e-12) {
                        face->set_boundary_id(BoundaryIds::no_force);
                    }
                }
            }
        }
    }


  // Next, determine the constraints due to boundary values.  The
  // bottom corners of the domain are kept in place in the $y$
  // direction -- the bottom left also in the $x$ direction. deal.II
  // generally thinks of boundary values as attached to pieces of the
  // boundary, i.e., faces, rather than individual vertices. Indeed,
  // mathematically speaking, one can not assign boundary values to
  // individual points for the infinite-dimensional partial
  // differential equation. But, since we are trying to reproduce a
  // widely used benchmark, we will do so anyway and keep in mind that
  // we have a finite-dimensional problem for which imposing boundary
  // conditions at a single node is valid.
    template<int dim>
    void
    SANDTopOpt<dim>::setup_boundary_values() {
      boundary_values.clear();
        for (const auto &cell : dof_handler.active_cell_iterators()) {
          for (const auto &face : cell->face_iterators()) {
                if (face->at_boundary()) {
                    const auto center = face->center();

                    // Check whether the current face is on the bottom
                    // boundary, and if it is whether one of its
                    // vertices might be the bottom left or bottom
                    // right vertex:
                    if (std::fabs(center(1) - 0) < 1e-12) {

                        for (unsigned int vertex_number = 0;
                             vertex_number < GeometryInfo<dim>::vertices_per_cell;
                             ++vertex_number) {
                            const auto vert = cell->vertex(vertex_number);

                            if (std::fabs(vert(0) - 0) < 1e-12 && std::fabs(vert(1) - 0) < 1e-12)
                              {
                                const unsigned int x_displacement =
                                        cell->vertex_dof_index(vertex_number, 0);
                                const unsigned int y_displacement =
                                        cell->vertex_dof_index(vertex_number, 1);
                                const unsigned int x_displacement_multiplier =
                                        cell->vertex_dof_index(vertex_number, 2);
                                const unsigned int y_displacement_multiplier =
                                        cell->vertex_dof_index(vertex_number, 3);
                               
                                boundary_values[x_displacement] = 0;
                                boundary_values[y_displacement] = 0;
                                boundary_values[x_displacement_multiplier] = 0;
                                boundary_values[y_displacement_multiplier] = 0;
                            }
                            
                            else if (std::fabs(vert(0) - 6) < 1e-12 && std::fabs(vert(1)- 0) < 1e-12) {
                                const unsigned int y_displacement =
                                        cell->vertex_dof_index(vertex_number, 1);
                                const unsigned int y_displacement_multiplier =
                                        cell->vertex_dof_index(vertex_number, 3);

                                boundary_values[y_displacement] = 0;
                                boundary_values[y_displacement_multiplier] = 0;
                            }
                        }
                    }
                }
            }
        }
    }

  // @sect3{Setting up block matrices and vectors}


    // The next function makes a giant 9-by-9 block matrix, and also
    // sets up the necessary block vectors.  The sparsity pattern for
    // this matrix includes the sparsity pattern for the filter
    // matrix. It also initializes any block vectors we will use.
    //
    // Setting up the blocks by themselves is not overly complicated
    // and follows what is already done in programs such as step-22,
    // for example.
    template<int dim>
    void
    SANDTopOpt<dim>::setup_block_system() {
        std::vector<unsigned int> block_component(9, 2);
        block_component[0] = 0;
        block_component[1] = 1;
        const std::vector<types::global_dof_index> dofs_per_block =
                DoFTools::count_dofs_per_fe_block(dof_handler, block_component);

        const unsigned int n_p = dofs_per_block[0];
        const unsigned int n_u = dofs_per_block[1];
        const std::vector<unsigned int> block_sizes = {n_p, n_u, n_p, n_u, n_p, n_p, n_p, n_p, n_p};

        BlockDynamicSparsityPattern dsp(9, 9);
        for (unsigned int k = 0; k < 9; ++k)
            for (unsigned int j = 0; j < 9; ++j)
                dsp.block(j, k).reinit(block_sizes[j], block_sizes[k]);
        dsp.collect_sizes();


        // The bulk of the function is in setting up which of these
        // blocks will actually contain anything, i.e., which
        // variables couple with which other variables. This is
        // cumbersome but necessary to ensure that we don't just
        // allocate a very large number of entries for our matrix that
        // will then end up being zero.
        //
        // The concrete pattern you see below is something one
        // probably has to draw once on a piece of paper, but follows
        // in an otherwise relatively straightforward way from looking
        // through the many terms of the bilinear form we will have to
        // assemble in each nonlinear iteration.
        //
        // The use of the symbolic names defined in namespace
        // `SolutionComponents` helps understand what each of the
        // following terms corresponds to, but it also makes the
        // expressions lengthy and unwieldy: An term such as
        // `coupling[SolutionComponents::density_upper_slack_multiplier<dim>][SolutionComponents::density<dim>]`
        // just doesn't read very well, and would either have to be
        // split over several lines or run off the right edge of
        // nearly every screen. As a consequence, we open a
        // curly-brace enclosed code block in which we temporarily
        // make the names in namespace `SolutionComponents` available
        // without the namespace qualifier, by saying `using namespace
        // SolutionComponents`.
        Table<2, DoFTools::Coupling> coupling(2 * dim + 7, 2 * dim + 7);
        {
          using namespace SolutionComponents;
          
          coupling[density<dim>][density<dim>] = DoFTools::always;

          for (unsigned int i = 0; i < dim; ++i) {
            coupling[density<dim>][displacement<dim> + i] = DoFTools::always;
            coupling[displacement<dim> + i][density<dim>] = DoFTools::always;
          }

          coupling[density<dim>][unfiltered_density<dim>] = DoFTools::none;
          coupling[unfiltered_density<dim>][density<dim>] = DoFTools::none;

          for (unsigned int i = 0; i < dim; ++i) {
            coupling[density<dim>][displacement_multiplier<dim> + i] = DoFTools::always;
            coupling[displacement_multiplier<dim> + i][density<dim>] = DoFTools::always;
          }

          coupling[density<dim>][unfiltered_density_multiplier<dim>] = DoFTools::always;
          coupling[unfiltered_density_multiplier<dim>][density<dim>] = DoFTools::always;


          coupling[density<dim>][density_lower_slack<dim>] = DoFTools::none;
          coupling[density<dim>][density_lower_slack_multiplier<dim>] = DoFTools::none;
          coupling[density<dim>][density_upper_slack<dim>] = DoFTools::none;
          coupling[density<dim>][density_upper_slack_multiplier<dim>] = DoFTools::none;
          coupling[density_lower_slack<dim>][density<dim>] = DoFTools::none;
          coupling[density_lower_slack_multiplier<dim>][density<dim>] = DoFTools::none;
          coupling[density_upper_slack<dim>][density<dim>] = DoFTools::none;
          coupling[density_upper_slack_multiplier<dim>][density<dim>] = DoFTools::none;
        
          /* Coupling for displacement */

          for (unsigned int i = 0; i < dim; ++i) {
            for (unsigned int k = 0; k < dim; ++k) {
              coupling[displacement<dim> + i][displacement<dim> + k] = DoFTools::none;
            }
            coupling[displacement<dim> + i][unfiltered_density<dim>] = DoFTools::none;
            coupling[unfiltered_density<dim>][displacement<dim> + i] = DoFTools::none;

            for (unsigned int k = 0; k < dim; ++k) {
              coupling[displacement<dim> + i][displacement_multiplier<dim> + k] = DoFTools::always;
              coupling[displacement_multiplier<dim>+ k][displacement<dim> + i] = DoFTools::always;
            }

            coupling[displacement<dim> + i][unfiltered_density_multiplier<dim>] = DoFTools::none;
            coupling[displacement<dim> + i][density_lower_slack<dim>] = DoFTools::none;
            coupling[displacement<dim> + i][density_lower_slack_multiplier<dim>] = DoFTools::none;
            coupling[displacement<dim> + i][density_upper_slack<dim>] = DoFTools::none;
            coupling[displacement<dim> + i][density_upper_slack_multiplier<dim>] = DoFTools::none;

            coupling[unfiltered_density_multiplier<dim>][displacement<dim> + i] = DoFTools::none;
            coupling[density_lower_slack<dim>][displacement<dim> + i] = DoFTools::none;
            coupling[density_lower_slack_multiplier<dim>][displacement<dim> + i] = DoFTools::none;
            coupling[density_upper_slack<dim>][displacement<dim> + i] = DoFTools::none;
            coupling[density_upper_slack_multiplier<dim>][displacement<dim> + i] = DoFTools::none;

          }

          /* coupling for unfiltered density */
          coupling[unfiltered_density<dim>][unfiltered_density<dim>]= DoFTools::none;
          for (unsigned int i = 0; i < dim; ++i) {
            coupling[unfiltered_density<dim>][displacement_multiplier<dim> + i] = DoFTools::none;
            coupling[displacement_multiplier<dim> + i][unfiltered_density<dim>] = DoFTools::none;
          }

          coupling[unfiltered_density<dim>][3 + 2 * dim] = DoFTools::none;
          coupling[3 + 2 * dim][unfiltered_density<dim>] = DoFTools::none;
          coupling[unfiltered_density<dim>][4 + 2 * dim] = DoFTools::none;
          coupling[4 + 2 * dim][unfiltered_density<dim>] = DoFTools::none;
          coupling[unfiltered_density<dim>][5 + 2 * dim] = DoFTools::always;
          coupling[5 + 2 * dim][unfiltered_density<dim>] = DoFTools::always;
          coupling[unfiltered_density<dim>][6 + 2 * dim] = DoFTools::always;
          coupling[6 + 2 * dim][unfiltered_density<dim>] = DoFTools::always;

          /* Coupling for equality multipliers */
          for (unsigned int i = 0; i < dim; ++i)
            {
              for (unsigned int k = 0; k < dim; ++k)
                {
                  coupling[displacement_multiplier<dim> +i][displacement_multiplier<dim> + k] = DoFTools::none;
                }
              coupling[displacement_multiplier<dim> +i][unfiltered_density_multiplier<dim>] = DoFTools::none;
              coupling[unfiltered_density_multiplier<dim>][displacement_multiplier<dim> +i] = DoFTools::none;

              coupling[displacement_multiplier<dim> +i][density_lower_slack<dim>] = DoFTools::none;
              coupling[displacement_multiplier<dim> +i][density_lower_slack_multiplier<dim>] = DoFTools::none;
              coupling[displacement_multiplier<dim> +i][density_upper_slack<dim>] = DoFTools::none;
              coupling[displacement_multiplier<dim> +i][density_upper_slack_multiplier<dim>] = DoFTools::none;

              coupling[density_lower_slack<dim>][displacement_multiplier<dim> +i] = DoFTools::none;
              coupling[density_lower_slack_multiplier<dim>][displacement_multiplier<dim> +i] = DoFTools::none;
              coupling[density_upper_slack<dim>][displacement_multiplier<dim> +i] = DoFTools::none;
              coupling[density_upper_slack_multiplier<dim>][displacement_multiplier<dim> +i] = DoFTools::none;
            }

          /* Coupling for slack variables */
          coupling[density_lower_slack<dim>][density_lower_slack<dim>] = DoFTools::always;
          coupling[density_lower_slack<dim>][density_lower_slack_multiplier<dim>] = DoFTools::none;
          coupling[density_lower_slack<dim>][density_upper_slack<dim>] = DoFTools::always;
          coupling[density_lower_slack<dim>][density_upper_slack_multiplier<dim>] = DoFTools::none;
          coupling[density_lower_slack_multiplier<dim>][density_lower_slack<dim>] = DoFTools::none;
          coupling[density_upper_slack<dim>][density_lower_slack<dim>] = DoFTools::always;
          coupling[density_upper_slack_multiplier<dim>][density_lower_slack<dim>] = DoFTools::none;
        
          coupling[density_lower_slack_multiplier<dim>][density_lower_slack_multiplier<dim>] = DoFTools::always;
          coupling[density_lower_slack_multiplier<dim>][density_upper_slack<dim>] = DoFTools::none;
          coupling[density_lower_slack_multiplier<dim>][density_upper_slack_multiplier<dim>] = DoFTools::always;
          coupling[density_upper_slack<dim>][density_lower_slack_multiplier<dim>] = DoFTools::none;
          coupling[density_upper_slack_multiplier<dim>][density_lower_slack_multiplier<dim>] = DoFTools::always;

          coupling[density_upper_slack<dim>][density_upper_slack<dim>] = DoFTools::none;
          coupling[density_upper_slack<dim>][density_upper_slack_multiplier<dim>] = DoFTools::none;
          coupling[density_upper_slack_multiplier<dim>][density_upper_slack<dim>] = DoFTools::none;

          coupling[density_upper_slack_multiplier<dim>][density_upper_slack_multiplier<dim>] = DoFTools::none;
        }
        
        // Before we can create the sparsity pattern, we also have to
        // set up constraints. Since this program does not adaptively
        // refine the mesh, the only constraint we have is one that
        // couples all density variables to enforce the volume
        // constraint. This will ultimately lead to a dense sub-block
        // of the matrix, but there is little we can do about that.
        const FEValuesExtractors::Scalar densities(SolutionComponents::density<dim>);
        const ComponentMask density_mask = fe.component_mask(densities);
        const IndexSet density_dofs = DoFTools::extract_dofs(dof_handler,
                                                       density_mask);

        const unsigned int last_density_dof = density_dofs.nth_index_in_set(density_dofs.n_elements() - 1);
        constraints.clear();
        constraints.add_line(last_density_dof);
        for (unsigned int i = 1; i < density_dofs.n_elements(); ++i) 
            constraints.add_entry(last_density_dof,
                                  density_dofs.nth_index_in_set(i - 1), -1);
        constraints.set_inhomogeneity (last_density_dof, 0);

        constraints.close();

        // We can now finally create the sparsity pattern for the
        // matrix, taking into account which variables couple with
        // which other variables, and the constraints we have on the
        // density.
        DoFTools::make_sparsity_pattern(dof_handler, coupling, dsp, constraints);

        // The only part of the matrix we have not dealt with is the
        // filter matrix and its transpose. These are non-local
        // (integral) operators for which deal.II does not currently
        // have functions. What we will ultimately need to do is go
        // over all cells and couple the unfiltered density on this
        // cell to all filtered densities of neighboring cells that
        // are less than a threshold distance away, and the other way
        // around; for the moment, we are only concerned with building
        // the sparsity pattern that would correspond to this kind of
        // matrix, so we perform the equivalent loop and where later
        // on we would write into an entry of the matrix, we now
        // simply add an entry to the sparsity matrix:
        for (const auto &cell : dof_handler.active_cell_iterators())
          {
            const unsigned int i = cell->active_cell_index();
            
            std::set<unsigned int> neighbor_ids;
            std::set<typename Triangulation<dim>::cell_iterator> cells_to_check;

            neighbor_ids.insert(i);
            cells_to_check.insert(cell);
            
            unsigned int n_neighbors = 1;
            while (true) {
                std::set<typename Triangulation<dim>::cell_iterator> cells_to_check_temp;
                for (auto check_cell : cells_to_check) {
                    for (unsigned int n = 0;
                         n < GeometryInfo<dim>::faces_per_cell; ++n) {
                        if (!(check_cell->face(n)->at_boundary())) {
                            const double distance = cell->center().distance(
                                    check_cell->neighbor(n)->center());
                            if ((distance < filter_r) &&
                                !(neighbor_ids.count(check_cell->neighbor(n)->active_cell_index()))) {
                                cells_to_check_temp.insert(check_cell->neighbor(n));
                                neighbor_ids.insert(check_cell->neighbor(n)->active_cell_index());
                            }
                        }
                    }
                }

                if (neighbor_ids.size() == n_neighbors)
                    break;
                else {
                    cells_to_check = cells_to_check_temp;
                    n_neighbors = neighbor_ids.size();
                }
            }

            for (const auto j : neighbor_ids) {
                dsp.block(SolutionBlocks::unfiltered_density, SolutionBlocks::unfiltered_density_multiplier).add(i, j);
                dsp.block(SolutionBlocks::unfiltered_density_multiplier, SolutionBlocks::unfiltered_density).add(i, j);
            }
        }

        // Having so generated the "dynamic" sparsity pattern, we can
        // finally copy it to the structure that is used to associate
        // matrices with a sparsity pattern. Because the sparsity
        // pattern is large and complex, we also output it into a file
        // of its own for visualization purposes -- in other words,
        // for "visual debugging".
        sparsity_pattern.copy_from(dsp);

        std::ofstream out("sparsity.plt");
        sparsity_pattern.print_gnuplot(out);

        system_matrix.reinit(sparsity_pattern);


        // What is left is to correctly size the various vectors and
        // their blocks, as well as setting initial guesses for some
        // of the components of the (nonlinear) solution vector. We
        // here use the symbolic component names for individual blocks
        // of the solution vector and, for brevity, use the same trick
        // with `using namespace` as above:
        linear_solution.reinit(block_sizes);
        nonlinear_solution.reinit(block_sizes);
        system_rhs.reinit(block_sizes);

        {
          using namespace SolutionBlocks;
          
          for (unsigned int k = 0; k < n_u; ++k) {
            nonlinear_solution.block(displacement)[k] = 0;
            nonlinear_solution.block(displacement_multiplier)[k] = 0;
          }
          for (unsigned int k = 0; k < n_p; ++k) {
            nonlinear_solution.block(density)[k] = density_ratio;
            nonlinear_solution.block(unfiltered_density)[k] = density_ratio;
            nonlinear_solution.block(unfiltered_density_multiplier)[k] = density_ratio;
            nonlinear_solution.block(density_lower_slack)[k] = density_ratio;
            nonlinear_solution.block(density_lower_slack_multiplier)[k] = 50;
            nonlinear_solution.block(density_upper_slack)[k] = 1 - density_ratio;
            nonlinear_solution.block(density_upper_slack_multiplier)[k] = 50;
          }
        }
    }


  // @sect3{Creating the filter matrix}

    // Next up, a function that is used once at the beginning of the
    // program: It creates a matrix $H$ so that the filtered density
    // vector equals $H$ times the unfiltered density.  The creation
    // of this matrix is non-trivial, and it is used in every
    // iteration, and so rather than reforming it as we do with the
    // Newton matrix, it is made only once and stored separately.
  //
  // The way this matrix is computed follows the outline used above
  // already to form its sparsity pattern. We repeat this process here
  // for the sparsity pattern of this separately formed matrix, and
  // then actually build the matrix itself. You may want to check the
  // definition of this matrix in the introduction to this program.
    template<int dim>
    void
    SANDTopOpt<dim>::setup_filter_matrix() {

        //The sparsity pattern of the filter has already been determined
        // and implemented in the setup_system() function. We copy the
        // structure from the appropriate block and use it again here.

        filter_sparsity_pattern.copy_from(sparsity_pattern.block(SolutionBlocks::unfiltered_density,SolutionBlocks::unfiltered_density_multiplier));
        filter_matrix.reinit(filter_sparsity_pattern);

        // Having so built the sparsity pattern, now we re-do all of
        // these loops to actually compute the necessary values of the
        // matrix entries:
        for (const auto &cell : dof_handler.active_cell_iterators()) {
            const unsigned int i = cell->active_cell_index();

            std::set<unsigned int> neighbor_ids;
            std::set<typename Triangulation<dim>::cell_iterator> cells_to_check;

            neighbor_ids.insert(i);
            cells_to_check.insert(cell);
            
            unsigned int n_neighbors = 1;
            filter_matrix.add(i, i, filter_r);
            while (true) {
                std::set<typename Triangulation<dim>::cell_iterator> cells_to_check_temp;
                for (auto check_cell : cells_to_check) {
                    for (unsigned int n = 0; n < GeometryInfo<dim>::faces_per_cell; ++n) {
                        if (!(check_cell->face(n)->at_boundary())) {
                            const double distance = cell->center().distance(
                                    check_cell->neighbor(n)->center());
                            if ((distance < filter_r) && !(neighbor_ids.count(
                                    check_cell->neighbor(n)->active_cell_index()))) {
                                cells_to_check_temp.insert(
                                        check_cell->neighbor(n));
                                neighbor_ids.insert(
                                        check_cell->neighbor(n)->active_cell_index());

                                filter_matrix.add(i, check_cell->neighbor(n)->active_cell_index(),
                                                  filter_r - distance);
                            }
                        }
                    }
                }

                if (neighbor_ids.size() == n_neighbors)
                    break;
                else {
                    cells_to_check = cells_to_check_temp;
                    n_neighbors = neighbor_ids.size();
                }
            }
        }

        // The final step is to normalize the matrix so that for each
        // row, the sum of entries equals one.
        for (unsigned int i=0; i<filter_matrix.m(); ++i) {
            double denominator = 0;
            for (SparseMatrix<double>::iterator iter = filter_matrix.begin(i);
                 iter != filter_matrix.end(i); iter++)
              denominator = denominator + iter->value();
            for (SparseMatrix<double>::iterator iter = filter_matrix.begin(i);
                 iter != filter_matrix.end(i); iter++)
              iter->value() = iter->value() / denominator;
        }
    }


  // @sect3{Assembling the Newton matrix}

  // Whereas the previous function built a matrix that is the same as
  // long as the mesh does not change (which we don't do anyway in
  // this program), the next function builds the matrix to be solved
  // in each iteration. This is where the magic happens. The components
  // of the system of linear equations describing Newtons' method for
  // finding the solution of the KKT conditions are implemented here.
  //
  // The top of the function is as in most of these functions and just
  // sets up all sorts of variables necessary for the actual assembly,
  // including a whole bunch of extractors. The entire set up should
  // look familiar, though somewhat lengthier, if you've previously
  // looked at step-22.
    template<int dim>
    void
    SANDTopOpt<dim>::assemble_system(const double barrier_size) {
        TimerOutput::Scope t(timer, "assembly");
        
        system_matrix = 0;
        system_rhs = 0;

        const FEValuesExtractors::Scalar densities(SolutionComponents::density<dim>);
        const FEValuesExtractors::Vector displacements(SolutionComponents::displacement<dim>);
        const FEValuesExtractors::Scalar unfiltered_densities(SolutionComponents::unfiltered_density<dim>);
        const FEValuesExtractors::Vector displacement_multipliers(SolutionComponents::displacement_multiplier<dim>);
        const FEValuesExtractors::Scalar unfiltered_density_multipliers(SolutionComponents::unfiltered_density_multiplier<dim>);
        const FEValuesExtractors::Scalar density_lower_slacks(SolutionComponents::density_lower_slack<dim>);
        const FEValuesExtractors::Scalar density_lower_slack_multipliers(SolutionComponents::density_lower_slack_multiplier<dim>);
        const FEValuesExtractors::Scalar density_upper_slacks(SolutionComponents::density_upper_slack<dim>);
        const FEValuesExtractors::Scalar density_upper_slack_multipliers(SolutionComponents::density_upper_slack_multiplier<dim>);

        QGauss<dim> quadrature_formula(fe.degree + 1);
        QGauss<dim - 1> face_quadrature_formula(fe.degree + 1);
        FEValues<dim> fe_values(fe, quadrature_formula,
                                update_values | update_gradients | update_quadrature_points
                                | update_JxW_values);
        FEFaceValues<dim> fe_face_values(fe, face_quadrature_formula,
                                         update_values | update_quadrature_points | update_normal_vectors
                                         | update_JxW_values);

        const unsigned int dofs_per_cell = fe.dofs_per_cell;
        const unsigned int n_q_points = quadrature_formula.size();
        const unsigned int n_face_q_points = face_quadrature_formula.size();

        FullMatrix<double> cell_matrix(dofs_per_cell, dofs_per_cell);
        Vector<double> cell_rhs(dofs_per_cell);

        std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

        std::vector<double> lambda_values(n_q_points);
        std::vector<double> mu_values(n_q_points);
        const Functions::ConstantFunction<dim> lambda(1.);
        const Functions::ConstantFunction<dim> mu(1.);
        std::vector<Tensor<1, dim>> rhs_values(n_q_points);

        // At this point, we apply the filter to the unfiltered
        // density, and apply the adjoint (transpose) operation to the
        // unfiltered density multiplier, both to the current best
        // guess for the nonlinear solution. We use this later to tell
        // us how far off our filtered density is from the filter
        // applied to the unfiltered density. That is because while at
        // the solution of the nonlinear problem, we have
        // $\rho=H\sigma$, but at intermediate iterations, we in
        // general have $\rho^k\neq H\sigma^k$ and the "residual"
        // $\rho^k-H\sigma^k$ will then appear as the right hand side
        // of one of the Newton update equations that we compute
        // below.
        BlockVector<double> filtered_unfiltered_density_solution = nonlinear_solution;
        BlockVector<double> filter_adjoint_unfiltered_density_multiplier_solution = nonlinear_solution;
        
        filter_matrix.vmult(filtered_unfiltered_density_solution.block(SolutionBlocks::unfiltered_density),
                            nonlinear_solution.block(SolutionBlocks::unfiltered_density));
        filter_matrix.Tvmult(filter_adjoint_unfiltered_density_multiplier_solution.block(SolutionBlocks::unfiltered_density_multiplier),
                             nonlinear_solution.block(SolutionBlocks::unfiltered_density_multiplier));


        std::vector<double> old_density_values(n_q_points);
        std::vector<Tensor<1, dim>> old_displacement_values(n_q_points);
        std::vector<double> old_displacement_divs(n_q_points);
        std::vector<SymmetricTensor<2, dim>> old_displacement_symmgrads(
          n_q_points);
        std::vector<Tensor<1, dim>> old_displacement_multiplier_values(
          n_q_points);
        std::vector<double> old_displacement_multiplier_divs(n_q_points);
        std::vector<SymmetricTensor<2, dim>> old_displacement_multiplier_symmgrads(
          n_q_points);
        std::vector<double> old_lower_slack_multiplier_values(n_q_points);
        std::vector<double> old_upper_slack_multiplier_values(n_q_points);
        std::vector<double> old_lower_slack_values(n_q_points);
        std::vector<double> old_upper_slack_values(n_q_points);
        std::vector<double> old_unfiltered_density_values(n_q_points);
        std::vector<double> old_unfiltered_density_multiplier_values(n_q_points);
        std::vector<double> filtered_unfiltered_density_values(n_q_points);
        std::vector<double> filter_adjoint_unfiltered_density_multiplier_values(n_q_points);


        for (const auto &cell : dof_handler.active_cell_iterators()) {
            cell_matrix = 0;
            cell_rhs = 0;

            cell->get_dof_indices(local_dof_indices);

            fe_values.reinit(cell);

            lambda.value_list(fe_values.get_quadrature_points(), lambda_values);
            mu.value_list(fe_values.get_quadrature_points(), mu_values);

            fe_values[densities].get_function_values(nonlinear_solution,
                                                     old_density_values);
            fe_values[displacements].get_function_values(nonlinear_solution,
                                                         old_displacement_values);
            fe_values[displacements].get_function_divergences(nonlinear_solution,
                                                              old_displacement_divs);
            fe_values[displacements].get_function_symmetric_gradients(
                    nonlinear_solution, old_displacement_symmgrads);
            fe_values[displacement_multipliers].get_function_values(
                    nonlinear_solution, old_displacement_multiplier_values);
            fe_values[displacement_multipliers].get_function_divergences(
                    nonlinear_solution, old_displacement_multiplier_divs);
            fe_values[displacement_multipliers].get_function_symmetric_gradients(
                    nonlinear_solution, old_displacement_multiplier_symmgrads);
            fe_values[density_lower_slacks].get_function_values(
                    nonlinear_solution, old_lower_slack_values);
            fe_values[density_lower_slack_multipliers].get_function_values(
                    nonlinear_solution, old_lower_slack_multiplier_values);
            fe_values[density_upper_slacks].get_function_values(
                    nonlinear_solution, old_upper_slack_values);
            fe_values[density_upper_slack_multipliers].get_function_values(
                    nonlinear_solution, old_upper_slack_multiplier_values);
            fe_values[unfiltered_densities].get_function_values(
                    nonlinear_solution, old_unfiltered_density_values);
            fe_values[unfiltered_density_multipliers].get_function_values(
                    nonlinear_solution, old_unfiltered_density_multiplier_values);
            fe_values[unfiltered_densities].get_function_values(
                    filtered_unfiltered_density_solution, filtered_unfiltered_density_values);
            fe_values[unfiltered_density_multipliers].get_function_values(
                    filter_adjoint_unfiltered_density_multiplier_solution,
                    filter_adjoint_unfiltered_density_multiplier_values);

            for (unsigned int q_point = 0; q_point < n_q_points; ++q_point) {

                for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                    const SymmetricTensor<2, dim> displacement_phi_i_symmgrad =
                            fe_values[displacements].symmetric_gradient(i, q_point);
                    const double displacement_phi_i_div =
                            fe_values[displacements].divergence(i, q_point);

                    const SymmetricTensor<2, dim> displacement_multiplier_phi_i_symmgrad =
                            fe_values[displacement_multipliers].symmetric_gradient(i,
                                                                                   q_point);
                    const double displacement_multiplier_phi_i_div =
                            fe_values[displacement_multipliers].divergence(i,
                                                                           q_point);


                    const double density_phi_i = fe_values[densities].value(i,
                                                                            q_point);
                    const double unfiltered_density_phi_i = fe_values[unfiltered_densities].value(i,
                                                                                                  q_point);
                    const double unfiltered_density_multiplier_phi_i = fe_values[unfiltered_density_multipliers].value(
                            i, q_point);

                    const double lower_slack_multiplier_phi_i =
                            fe_values[density_lower_slack_multipliers].value(i,
                                                                             q_point);

                    const double lower_slack_phi_i =
                            fe_values[density_lower_slacks].value(i, q_point);

                    const double upper_slack_phi_i =
                            fe_values[density_upper_slacks].value(i, q_point);

                    const double upper_slack_multiplier_phi_i =
                            fe_values[density_upper_slack_multipliers].value(i,
                                                                             q_point);


                    for (unsigned int j = 0; j < dofs_per_cell; ++j) {
                        const SymmetricTensor<2, dim> displacement_phi_j_symmgrad =
                                fe_values[displacements].symmetric_gradient(j,
                                                                            q_point);
                        const double displacement_phi_j_div =
                                fe_values[displacements].divergence(j, q_point);

                        const SymmetricTensor<2, dim> displacement_multiplier_phi_j_symmgrad =
                                fe_values[displacement_multipliers].symmetric_gradient(
                                        j, q_point);
                        const double displacement_multiplier_phi_j_div =
                                fe_values[displacement_multipliers].divergence(j,
                                                                               q_point);

                        const double density_phi_j = fe_values[densities].value(
                                j, q_point);

                        const double unfiltered_density_phi_j = fe_values[unfiltered_densities].value(j,
                                                                                                      q_point);
                        const double unfiltered_density_multiplier_phi_j = fe_values[unfiltered_density_multipliers].value(
                                j, q_point);


                        const double lower_slack_phi_j =
                                fe_values[density_lower_slacks].value(j, q_point);

                        const double upper_slack_phi_j =
                                fe_values[density_upper_slacks].value(j, q_point);

                        const double lower_slack_multiplier_phi_j =
                                fe_values[density_lower_slack_multipliers].value(j,
                                                                                 q_point);

                        const double upper_slack_multiplier_phi_j =
                                fe_values[density_upper_slack_multipliers].value(j,
                                                                                 q_point);

                        // This is where the actual work starts. In
                        // the following, we will build all of the
                        // terms of the matrix -- they are numerous
                        // and not entirely self-explanatory, also
                        // depending on the previous solutions and its
                        // derivatives (which we have already
                        // evaluated above and put into the variables
                        // called `old_*`). To understand what each of
                        // these terms corresponds to, you will want
                        // to look at the explicit form of these terms
                        // in the introduction above.
                        //
                        // The right hand sides of the equations being
                        // driven to 0 give all the KKT conditions
                        // for finding a local minimum -- the descriptions of what
                        // each individual equation are given with the computations
                        // of the right hand side.
                        
                        /* Equation 0 */
                        cell_matrix(i, j) +=
                                fe_values.JxW(q_point) *
                                (

                                        -density_phi_i * unfiltered_density_multiplier_phi_j

                                        + density_penalty_exponent * (density_penalty_exponent
                                                                      - 1)
                                          * std::pow(
                                                old_density_values[q_point],
                                                density_penalty_exponent - 2)
                                          * density_phi_i
                                          * density_phi_j
                                          * (old_displacement_multiplier_divs[q_point] * old_displacement_divs[q_point]
                                             * lambda_values[q_point]
                                             + 2 * mu_values[q_point]
                                               * (old_displacement_symmgrads[q_point] *
                                                  old_displacement_multiplier_symmgrads[q_point]))

                                        + density_penalty_exponent * std::pow(
                                                old_density_values[q_point],
                                                density_penalty_exponent - 1)
                                          * density_phi_i
                                          * (displacement_multiplier_phi_j_div * old_displacement_divs[q_point]
                                             * lambda_values[q_point]
                                             + 2 * mu_values[q_point]
                                               *
                                               (old_displacement_symmgrads[q_point] *
                                                displacement_multiplier_phi_j_symmgrad))

                                        + density_penalty_exponent * std::pow(
                                                old_density_values[q_point],
                                                density_penalty_exponent - 1)
                                          * density_phi_i
                                          * (displacement_phi_j_div * old_displacement_multiplier_divs[q_point]
                                             * lambda_values[q_point]
                                             + 2 * mu_values[q_point]
                                               * (old_displacement_multiplier_symmgrads[q_point] *
                                                  displacement_phi_j_symmgrad)));
                        
                        /* Equation 1 */
                        cell_matrix(i, j) +=
                                fe_values.JxW(q_point) * (
                                        density_penalty_exponent * std::pow(
                                                old_density_values[q_point],
                                                density_penalty_exponent - 1)
                                        * density_phi_j
                                        * (old_displacement_multiplier_divs[q_point] * displacement_phi_i_div
                                           * lambda_values[q_point]
                                           + 2 * mu_values[q_point]
                                             * (old_displacement_multiplier_symmgrads[q_point] *
                                                displacement_phi_i_symmgrad))

                                        + std::pow(old_density_values[q_point],
                                                   density_penalty_exponent)
                                          * (displacement_multiplier_phi_j_div * displacement_phi_i_div
                                             * lambda_values[q_point]
                                             + 2 * mu_values[q_point]
                                               * (displacement_multiplier_phi_j_symmgrad * displacement_phi_i_symmgrad))

                                );

                        /* Equation 2, which has to do with the filter and which is calculated elsewhere. */
                        cell_matrix(i, j) +=
                                fe_values.JxW(q_point) * (
                                        -1 * unfiltered_density_phi_i * lower_slack_multiplier_phi_j
                                        + unfiltered_density_phi_i * upper_slack_multiplier_phi_j);


                        /* Equation 3: Primal feasibility */
                        cell_matrix(i, j) +=
                                fe_values.JxW(q_point) * (

                                        density_penalty_exponent * std::pow(
                                                old_density_values[q_point],
                                                density_penalty_exponent - 1)
                                        * density_phi_j
                                        * (old_displacement_divs[q_point] * displacement_multiplier_phi_i_div
                                           * lambda_values[q_point]
                                           + 2 * mu_values[q_point]
                                             * (old_displacement_symmgrads[q_point] *
                                                displacement_multiplier_phi_i_symmgrad))

                                        + std::pow(old_density_values[q_point],
                                                   density_penalty_exponent)
                                          * (displacement_phi_j_div * displacement_multiplier_phi_i_div
                                             * lambda_values[q_point]
                                             + 2 * mu_values[q_point]
                                               *
                                               (displacement_phi_j_symmgrad * displacement_multiplier_phi_i_symmgrad)));

                        /* Equation 4: Primal feasibility */
                        cell_matrix(i, j) +=
                                -1 * fe_values.JxW(q_point) * lower_slack_multiplier_phi_i *
                                (unfiltered_density_phi_j - lower_slack_phi_j);

                        /* Equation 5: Primal feasibility */
                        cell_matrix(i, j) +=
                                -1 * fe_values.JxW(q_point) * upper_slack_multiplier_phi_i * (
                                        -1 * unfiltered_density_phi_j - upper_slack_phi_j);

                        /* Equation 6: Primal feasibility - the part with the filter is added later */
                        cell_matrix(i, j) +=
                                -1 * fe_values.JxW(q_point) * unfiltered_density_multiplier_phi_i * (
                                        density_phi_j);

                        /* Equation 7: Complementary slackness */
                        cell_matrix(i, j) += fe_values.JxW(q_point)
                                             * (lower_slack_phi_i * lower_slack_multiplier_phi_j

                                                + lower_slack_phi_i * lower_slack_phi_j *
                                                  old_lower_slack_multiplier_values[q_point] /
                                                  old_lower_slack_values[q_point]);

                        /* Equation 8: Complementary slackness */
                        cell_matrix(i, j) += fe_values.JxW(q_point)
                                             * (upper_slack_phi_i * upper_slack_multiplier_phi_j


                                                + upper_slack_phi_i * upper_slack_phi_j
                                                  * old_upper_slack_multiplier_values[q_point] /
                                                  old_upper_slack_values[q_point]);

                    }

                    // Having accumulated all of the terms that belong
                    // into the Newton matrix, we now also have to
                    // compute the terms for the right hand side
                    // (i.e., the negative residual):

                    /* Equation 0: This equation, along with
                       equations 1 and 2, are the variational
                       derivatives of the Lagrangian with respect to
                       the decision variables - the density,
                       displacement, and unfiltered density.  */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    density_penalty_exponent *
                                    std::pow(old_density_values[q_point], density_penalty_exponent - 1) * density_phi_i
                                    * (old_displacement_multiplier_divs[q_point] * old_displacement_divs[q_point]
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (old_displacement_symmgrads[q_point]
                                                                   * old_displacement_multiplier_symmgrads[q_point]))
                                    - density_phi_i * old_unfiltered_density_multiplier_values[q_point]);

                    /* Equation 1; boundary terms will be added further down below. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    std::pow(old_density_values[q_point], density_penalty_exponent)
                                    * (old_displacement_multiplier_divs[q_point] * displacement_phi_i_div
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (old_displacement_multiplier_symmgrads[q_point]
                                                                   * displacement_phi_i_symmgrad))
                            );

                    /* Equation 2 */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    unfiltered_density_phi_i *
                                    filter_adjoint_unfiltered_density_multiplier_values[q_point]
                                    + unfiltered_density_phi_i * old_upper_slack_multiplier_values[q_point]
                                    + -1 * unfiltered_density_phi_i * old_lower_slack_multiplier_values[q_point]
                            );




                    /* Equation 3; boundary term will again be dealt
                       with below. This equation being driven to 0
                       ensures that the elasticity equation is met as
                       a constraint. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    std::pow(old_density_values[q_point], density_penalty_exponent)
                                    * (old_displacement_divs[q_point] * displacement_multiplier_phi_i_div
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (displacement_multiplier_phi_i_symmgrad
                                                                   * old_displacement_symmgrads[q_point]))
                            );

                    /* Equation 4: This equation sets the lower slack
                       variable equal to the unfiltered density,
                       giving a minimum density of 0. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) *
                            (lower_slack_multiplier_phi_i
                             * (old_unfiltered_density_values[q_point] - old_lower_slack_values[q_point])
                            );

                    /* Equation 5: This equation sets the upper slack
                       variable equal to one minus the unfiltered
                       density. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) * (
                                    upper_slack_multiplier_phi_i * (1 - old_unfiltered_density_values[q_point]
                                                                    - old_upper_slack_values[q_point]));

                    /* Equation 6: This is the difference between the
                       density and the filter applied to the unfiltered
                       density.  This being driven to 0 by the Newton
                       steps ensures that the filter is applied
                       correctly. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) * (
                                    unfiltered_density_multiplier_phi_i
                                    * (old_density_values[q_point] - filtered_unfiltered_density_values[q_point])
                            );

                    /* Equation 7: This along with equation 8 give the
                       requirement that $s*z = \alpha$ for the barrier
                       size alpha, and gives complementary slackness
                       from KKT conditions when $\alpha$ goes to 0. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (lower_slack_phi_i
                                                           * (old_lower_slack_multiplier_values[q_point] -
                                                              barrier_size / old_lower_slack_values[q_point])
                            );

                    /* Equation 8 */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (upper_slack_phi_i
                                                           * (old_upper_slack_multiplier_values[q_point] -
                                                              barrier_size / old_upper_slack_values[q_point]));
                }
            }


            // We still have to also deal with terms that stem from
            // boundary forces (tractions). These appear in the
            // equations for the displacement as well as for the
            // Lagrange multiplier for the displacement equation:
            for (const auto &face : cell->face_iterators()) {
                if (face->at_boundary() && face->boundary_id() == BoundaryIds::down_force) {
                    fe_face_values.reinit(cell, face);

                    for (unsigned int face_q_point = 0;
                         face_q_point < n_face_q_points; ++face_q_point) {
                        for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                          Tensor<1, dim> traction;
                          traction[1] = -1;

                            cell_rhs(i) += -1
                                           * traction
                                           * fe_face_values[displacements].value(i,
                                                                                 face_q_point)
                                           * fe_face_values.JxW(face_q_point);

                            cell_rhs(i) += traction
                                           * fe_face_values[displacement_multipliers].value(
                                             i, face_q_point)
                                           * fe_face_values.JxW(face_q_point);
                        }
                    }
                }
            }


            // Now that we have everything assembled, all we have to
            // do is deal with the effect of (Dirichlet) boundary
            // conditions and other constraints. We incorporate the
            // former locally with just the contributions from the
            // current cell, and then let the AffineConstraint class
            // deal with the latter while copying contributions from
            // the current cell into the global linear system:
            MatrixTools::local_apply_boundary_values(boundary_values, local_dof_indices,
                                                     cell_matrix, cell_rhs, true);

            constraints.distribute_local_to_global(
                    cell_matrix, cell_rhs, local_dof_indices, system_matrix, system_rhs);


        }


        // Here we use the filter matrix we have already
        // constructed. We only need to integrate this filter applied
        // to test functions, which are piecewise constant, and so the
        // integration becomes a simple multiplication by the measure
        // of the cell.  Iterating over the pre-made filter matrix
        // allows us to use the information about which cells are in
        // or out of the filter without repeatedly checking neighbor
        // cells again.
        for (const auto &cell : dof_handler.active_cell_iterators()) {
            const unsigned int i = cell->active_cell_index();
            for (typename SparseMatrix<double>::iterator iter = filter_matrix.begin(
                   i); iter != filter_matrix.end(i); ++iter) {
                const unsigned int j = iter->column();
                const double value = iter->value() * cell->measure();

                system_matrix.block(SolutionBlocks::unfiltered_density_multiplier, SolutionBlocks::unfiltered_density).add(i, j, value);
                system_matrix.block(SolutionBlocks::unfiltered_density, SolutionBlocks::unfiltered_density_multiplier).add(j, i, value);
            }
        }
    }


  // @sect3{Solving the Newton linear system}

  
    // We will need to solve a linear system in each iteration. We use
    // a direct solver, for now -- this is clearly not an efficient
    // choice for a matrix that has so many non-zeroes, and it will
    // not scale to anything interesting. For "real" applications, we
    // will need an iterative solver but the complexity of the system
    // means that an iterative solver algorithm will take a good deal
    // of work. Because this is not the focus of the current program,
    // we simply stick with the direct solver we have here -- the
    // function follows the same structure as used in step-29.
    template<int dim>
    void
    SANDTopOpt<dim>::solve() {

        linear_solution = 0;
        TimerOutput::Scope t(timer, "solver");

        SparseDirectUMFPACK A_direct;
        A_direct.initialize(system_matrix);
        A_direct.vmult(linear_solution, system_rhs);

        constraints.distribute(linear_solution);
    }


  // @sect3{Details of the optimization algorithm}

  // The next several functions deal with specific parts of the
  // optimization algorithm, most notably with deciding whether the
  // direction computed by solving the linearized (Newton) system is
  // viable and, if so, how far we want to go in this direction.
  
    // We start with a function that does a binary search to figure
    // out the maximum step that meets the dual feasibility -- that
    // is, how far can we go so that $s>0$ and $z>0$. The function
    // returns a pair of values, one each for the $s$ and $z$ slack
    // variables.
    template<int dim>
    std::pair<double,double>
    SANDTopOpt<dim>::calculate_max_step_size(const BlockVector<double> &state, const BlockVector<double> &step, const double barrier_size) const {

        double fraction_to_boundary;
        const double min_fraction_to_boundary = .8;
        const double max_fraction_to_boundary = .99999;

        if (min_fraction_to_boundary < 1 - barrier_size)
        {
            if (1 - barrier_size < max_fraction_to_boundary)
                fraction_to_boundary = 1-barrier_size;
            else
                fraction_to_boundary = max_fraction_to_boundary;
        }
        else
            fraction_to_boundary = min_fraction_to_boundary;

        double step_size_s_low = 0;
        double step_size_z_low = 0;
        double step_size_s_high = 1;
        double step_size_z_high = 1;
        double step_size_s, step_size_z;

        for (unsigned int k = 0; k < 50; ++k) {
            step_size_s = (step_size_s_low + step_size_s_high) / 2;
            step_size_z = (step_size_z_low + step_size_z_high) / 2;

            const BlockVector<double> state_test_s =
                    (fraction_to_boundary * state) + (step_size_s * step);

            const BlockVector<double> state_test_z =
                    (fraction_to_boundary * state) + (step_size_z * step);

            const bool accept_s = (state_test_s.block(SolutionBlocks::density_lower_slack).is_non_negative())
                                  && (state_test_s.block(SolutionBlocks::density_upper_slack).is_non_negative());
            const bool accept_z = (state_test_z.block(SolutionBlocks::density_lower_slack_multiplier).is_non_negative())
                                  && (state_test_z.block(SolutionBlocks::density_upper_slack_multiplier).is_non_negative());

            if (accept_s)
                step_size_s_low = step_size_s;
            else
                step_size_s_high = step_size_s;
            
            if (accept_z)
                step_size_z_low = step_size_z;
            else
                step_size_z_high = step_size_z;
        }

        return {step_size_s_low, step_size_z_low};
    }

  // The next function computes a right hand side vector linearized
  // around a "test solution vector" that we can use to look at the
  // magnitude of the KKT conditions.  This is then used for testing
  // the convergence before shrinking barrier size, as well as in the
  // calculation of the $l_1$ merit.
  //
  // The function is lengthy and complicated, but it is really just a
  // copy of the right hand side part of what the `assemble_system()`
  // function above did.
    template<int dim>
    BlockVector<double>
    SANDTopOpt<dim>::calculate_test_rhs(const BlockVector<double> &test_solution, const double barrier_size) const {
        BlockVector<double> test_rhs;
        test_rhs.reinit (system_rhs); /* a zero vector with size and blocking of system_rhs */

        const FEValuesExtractors::Scalar densities(SolutionComponents::density<dim>);
        const FEValuesExtractors::Vector displacements(SolutionComponents::displacement<dim>);
        const FEValuesExtractors::Scalar unfiltered_densities(SolutionComponents::unfiltered_density<dim>);
        const FEValuesExtractors::Vector displacement_multipliers(SolutionComponents::displacement_multiplier<dim>);
        const FEValuesExtractors::Scalar unfiltered_density_multipliers(SolutionComponents::unfiltered_density_multiplier<dim>);
        const FEValuesExtractors::Scalar density_lower_slacks(SolutionComponents::density_lower_slack<dim>);
        const FEValuesExtractors::Scalar density_lower_slack_multipliers(SolutionComponents::density_lower_slack_multiplier<dim>);
        const FEValuesExtractors::Scalar density_upper_slacks(SolutionComponents::density_upper_slack<dim>);
        const FEValuesExtractors::Scalar density_upper_slack_multipliers(SolutionComponents::density_upper_slack_multiplier<dim>);

        const QGauss<dim> quadrature_formula(fe.degree + 1);
        const QGauss<dim - 1> face_quadrature_formula(fe.degree + 1);
        FEValues<dim> fe_values(fe, quadrature_formula,
                                update_values | update_gradients | update_quadrature_points
                                | update_JxW_values);
        FEFaceValues<dim> fe_face_values(fe, face_quadrature_formula,
                                         update_values | update_quadrature_points | update_normal_vectors
                                         | update_JxW_values);

        const unsigned int dofs_per_cell = fe.dofs_per_cell;
        const unsigned int n_q_points = quadrature_formula.size();
        const unsigned int n_face_q_points = face_quadrature_formula.size();

        Vector<double> cell_rhs(dofs_per_cell);
        FullMatrix<double> dummy_cell_matrix(dofs_per_cell,dofs_per_cell);

        std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

        std::vector<double> lambda_values(n_q_points);
        std::vector<double> mu_values(n_q_points);

        const Functions::ConstantFunction<dim> lambda(1.), mu(1.);
        std::vector<Tensor<1, dim>> rhs_values(n_q_points);


        BlockVector<double> filtered_unfiltered_density_solution = test_solution;
        BlockVector<double> filter_adjoint_unfiltered_density_multiplier_solution = test_solution;
        filtered_unfiltered_density_solution.block(SolutionBlocks::unfiltered_density) = 0;
        filter_adjoint_unfiltered_density_multiplier_solution.block(SolutionBlocks::unfiltered_density_multiplier) = 0;

        filter_matrix.vmult(filtered_unfiltered_density_solution.block(SolutionBlocks::unfiltered_density), test_solution.block(SolutionBlocks::unfiltered_density));
        filter_matrix.Tvmult(filter_adjoint_unfiltered_density_multiplier_solution.block(SolutionBlocks::unfiltered_density_multiplier),
                             test_solution.block(SolutionBlocks::unfiltered_density_multiplier));


        std::vector<double> old_density_values(n_q_points);
        std::vector<Tensor<1, dim>> old_displacement_values(n_q_points);
        std::vector<double> old_displacement_divs(n_q_points);
        std::vector<SymmetricTensor<2, dim>> old_displacement_symmgrads(
          n_q_points);
        std::vector<Tensor<1, dim>> old_displacement_multiplier_values(
          n_q_points);
        std::vector<double> old_displacement_multiplier_divs(n_q_points);
        std::vector<SymmetricTensor<2, dim>> old_displacement_multiplier_symmgrads(
          n_q_points);
        std::vector<double> old_lower_slack_multiplier_values(n_q_points);
        std::vector<double> old_upper_slack_multiplier_values(n_q_points);
        std::vector<double> old_lower_slack_values(n_q_points);
        std::vector<double> old_upper_slack_values(n_q_points);
        std::vector<double> old_unfiltered_density_values(n_q_points);
        std::vector<double> old_unfiltered_density_multiplier_values(n_q_points);
        std::vector<double> filtered_unfiltered_density_values(n_q_points);
        std::vector<double> filter_adjoint_unfiltered_density_multiplier_values(n_q_points);

        for (const auto &cell : dof_handler.active_cell_iterators()) {
            cell_rhs = 0;

            cell->get_dof_indices(local_dof_indices);

            fe_values.reinit(cell);

            lambda.value_list(fe_values.get_quadrature_points(), lambda_values);
            mu.value_list(fe_values.get_quadrature_points(), mu_values);

            fe_values[densities].get_function_values(test_solution,
                                                     old_density_values);
            fe_values[displacements].get_function_values(test_solution,
                                                         old_displacement_values);
            fe_values[displacements].get_function_divergences(test_solution,
                                                              old_displacement_divs);
            fe_values[displacements].get_function_symmetric_gradients(
                    test_solution, old_displacement_symmgrads);
            fe_values[displacement_multipliers].get_function_values(
                    test_solution, old_displacement_multiplier_values);
            fe_values[displacement_multipliers].get_function_divergences(
                    test_solution, old_displacement_multiplier_divs);
            fe_values[displacement_multipliers].get_function_symmetric_gradients(
                    test_solution, old_displacement_multiplier_symmgrads);
            fe_values[density_lower_slacks].get_function_values(
                    test_solution, old_lower_slack_values);
            fe_values[density_lower_slack_multipliers].get_function_values(
                    test_solution, old_lower_slack_multiplier_values);
            fe_values[density_upper_slacks].get_function_values(
                    test_solution, old_upper_slack_values);
            fe_values[density_upper_slack_multipliers].get_function_values(
                    test_solution, old_upper_slack_multiplier_values);
            fe_values[unfiltered_densities].get_function_values(
                    test_solution, old_unfiltered_density_values);
            fe_values[unfiltered_density_multipliers].get_function_values(
                    test_solution, old_unfiltered_density_multiplier_values);
            fe_values[unfiltered_densities].get_function_values(
                    filtered_unfiltered_density_solution, filtered_unfiltered_density_values);
            fe_values[unfiltered_density_multipliers].get_function_values(
                    filter_adjoint_unfiltered_density_multiplier_solution,
                    filter_adjoint_unfiltered_density_multiplier_values);

            for (unsigned int q_point = 0; q_point < n_q_points; ++q_point) {

                for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                    const SymmetricTensor<2, dim> displacement_phi_i_symmgrad =
                            fe_values[displacements].symmetric_gradient(i, q_point);
                    const double displacement_phi_i_div =
                            fe_values[displacements].divergence(i, q_point);

                    const SymmetricTensor<2, dim> displacement_multiplier_phi_i_symmgrad =
                            fe_values[displacement_multipliers].symmetric_gradient(i,
                                                                                   q_point);
                    const double displacement_multiplier_phi_i_div =
                            fe_values[displacement_multipliers].divergence(i,
                                                                           q_point);


                    const double density_phi_i = fe_values[densities].value(i,
                                                                            q_point);
                    const double unfiltered_density_phi_i = fe_values[unfiltered_densities].value(i,
                                                                                                  q_point);
                    const double unfiltered_density_multiplier_phi_i = fe_values[unfiltered_density_multipliers].value(
                            i, q_point);

                    const double lower_slack_multiplier_phi_i =
                            fe_values[density_lower_slack_multipliers].value(i,
                                                                             q_point);

                    const double lower_slack_phi_i =
                            fe_values[density_lower_slacks].value(i, q_point);

                    const double upper_slack_phi_i =
                            fe_values[density_upper_slacks].value(i, q_point);

                    const double upper_slack_multiplier_phi_i =
                            fe_values[density_upper_slack_multipliers].value(i,
                                                                             q_point);

                    /* Equation 0: This equation, along with equations
                       1 and 2, are the variational derivatives of the
                       lagrangian with respect to the decision
                       variables - the density, displacement, and
                       unfiltered density. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    density_penalty_exponent *
                                    std::pow(old_density_values[q_point], density_penalty_exponent - 1) * density_phi_i
                                    * (old_displacement_multiplier_divs[q_point] * old_displacement_divs[q_point]
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (old_displacement_symmgrads[q_point]
                                                                   * old_displacement_multiplier_symmgrads[q_point]))
                                    - density_phi_i * old_unfiltered_density_multiplier_values[q_point]);

                    /* Equation 1; the boundary terms will be added further down below. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    std::pow(old_density_values[q_point], density_penalty_exponent)
                                    * (old_displacement_multiplier_divs[q_point] * displacement_phi_i_div
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (old_displacement_multiplier_symmgrads[q_point]
                                                                   * displacement_phi_i_symmgrad))
                            );

                    /* Equation 2 */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    unfiltered_density_phi_i *
                                    filter_adjoint_unfiltered_density_multiplier_values[q_point]
                                    + unfiltered_density_phi_i * old_upper_slack_multiplier_values[q_point]
                                    + -1 * unfiltered_density_phi_i * old_lower_slack_multiplier_values[q_point]
                            );




                    /* Equation 3; boundary term will again be dealt
                       with below. This equation being driven to 0
                       ensures that the elasticity equation is met as
                       a constraint. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (
                                    std::pow(old_density_values[q_point], density_penalty_exponent)
                                    * (old_displacement_divs[q_point] * displacement_multiplier_phi_i_div
                                       * lambda_values[q_point]
                                       + 2 * mu_values[q_point] * (displacement_multiplier_phi_i_symmgrad
                                                                   * old_displacement_symmgrads[q_point]))
                            );

                    /* Equation 4: This equation sets the lower slack
                       variable equal to the unfiltered density,
                       giving a minimum density of 0. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) *
                            (lower_slack_multiplier_phi_i
                             * (old_unfiltered_density_values[q_point] - old_lower_slack_values[q_point])
                            );

                    /* Equation 5: This equation sets the upper slack
                       variable equal to one minus the unfiltered
                       density. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) * (
                                    upper_slack_multiplier_phi_i * (1 - old_unfiltered_density_values[q_point]
                                       - old_upper_slack_values[q_point]));

                    /* Equation 6: This is the difference between the
                       density and the filter applied to the
                       unfiltered density.  This being driven to 0 by
                       the newton steps ensures that the filter is
                       applied correctly. */
                    cell_rhs(i) +=
                            fe_values.JxW(q_point) * (
                                    unfiltered_density_multiplier_phi_i
                                    * (old_density_values[q_point] - filtered_unfiltered_density_values[q_point])
                            );

                    /* Equation 7: This along with equation 8 give the
                       requirement that $s*z = \alpha$ for the barrier
                       size alpha, and gives complementary slackness
                       from KKT conditions when $\alpha$ goes to 0. */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (lower_slack_phi_i
                                                           * (old_lower_slack_multiplier_values[q_point] -
                                                              barrier_size / old_lower_slack_values[q_point])
                            );

                    /* Equation 8 */
                    cell_rhs(i) +=
                            -1 * fe_values.JxW(q_point) * (upper_slack_phi_i
                                                           * (old_upper_slack_multiplier_values[q_point] -
                                                              barrier_size / old_upper_slack_values[q_point]));
                }


            }

            for (const auto &face : cell->face_iterators()) {
                if (face->at_boundary() && face->boundary_id() == BoundaryIds::down_force) {
                    fe_face_values.reinit(cell, face);

                    for (unsigned int face_q_point = 0;
                         face_q_point < n_face_q_points; ++face_q_point) {
                        for (unsigned int i = 0; i < dofs_per_cell; ++i) {
                          Tensor<1, dim> traction;
                          traction[1] = -1;

                            cell_rhs(i) += -1
                                           * traction
                                           * fe_face_values[displacements].value(i,
                                                                                 face_q_point)
                                           * fe_face_values.JxW(face_q_point);

                            cell_rhs(i) += traction
                                           * fe_face_values[displacement_multipliers].value(
                                    i, face_q_point)
                                           * fe_face_values.JxW(face_q_point);
                        }
                    }
                }
            }

            MatrixTools::local_apply_boundary_values(boundary_values, local_dof_indices,
                                                     dummy_cell_matrix, cell_rhs, true);

            constraints.distribute_local_to_global(
                    cell_rhs, local_dof_indices, test_rhs);
        }
        
        return test_rhs;
    }


  // The algorithm we use herein uses a "watchdog" strategy to
  // determine where and how far to go from the current iterate.  We
  // base the watchdog strategy on an exact $l_1$ merit function. This
  // function calculates the exact $l_1$ merit of a given, putative,
  // next iterate.
  //
  // The merit function consists of the sum of the objective function
  // (which is simply an integral of external forces (on the boundary
  // of the domain) times the displacement values of a test solution
  // (typically, the current solution plus some multiple of the Newton
  // update), and the $l_1$ norms of the Lagrange multiplier
  // components of residual vectors. The following code computes these
  // parts in turn:
    template<int dim>
    double
    SANDTopOpt<dim>::calculate_exact_merit(const BlockVector<double> &test_solution, const double barrier_size)
    {
       TimerOutput::Scope t(timer, "merit function");

       // Start with computing the objective function:
       double objective_function_merit = 0;
       {
            const FEValuesExtractors::Vector displacements(SolutionComponents::displacement<dim>);
            const QGauss<dim> quadrature_formula(fe.degree + 1);
            const QGauss<dim - 1> face_quadrature_formula(fe.degree + 1);
            FEValues<dim> fe_values(fe, quadrature_formula,
                                    update_values | update_gradients | update_quadrature_points
                                    | update_JxW_values);
            FEFaceValues<dim> fe_face_values(fe, face_quadrature_formula,
                                             update_values | update_quadrature_points | update_normal_vectors
                                             | update_JxW_values);

            const unsigned int n_face_q_points = face_quadrature_formula.size();

            std::vector<Tensor<1, dim>> displacement_face_values(n_face_q_points);

            for (const auto &cell : dof_handler.active_cell_iterators())
              {
                for (const auto &face : cell->face_iterators()) {
                    if (face->at_boundary() && face->boundary_id()== BoundaryIds::down_force)
                    {
                        fe_face_values.reinit(cell, face);
                        fe_face_values[displacements].get_function_values(test_solution,
                                                                          displacement_face_values);
                        for (unsigned int face_q_point = 0;
                             face_q_point < n_face_q_points; ++face_q_point) {
                          Tensor<1, dim> traction;
                          traction[1] = -1;

                            objective_function_merit +=
                                    traction
                                    * displacement_face_values[face_q_point]
                                    * fe_face_values.JxW(face_q_point);
                        }
                    }
                }
            }
        }

       // Then compute the residual and take the $l_1$ norms of the
       // components that correspond to Lagrange mulipliers. We add
       // those to the objective function computed above, and return
       // the sum at the bottom:
        const BlockVector<double> test_rhs = calculate_test_rhs(test_solution, barrier_size);

        const double elasticity_constraint_merit = penalty_multiplier * test_rhs.block(SolutionBlocks::displacement_multiplier).l1_norm();
        const double filter_constraint_merit = penalty_multiplier * test_rhs.block(SolutionBlocks::unfiltered_density_multiplier).l1_norm();
        const double lower_slack_merit = penalty_multiplier * test_rhs.block(SolutionBlocks::density_lower_slack_multiplier).l1_norm();
        const double upper_slack_merit = penalty_multiplier * test_rhs.block(SolutionBlocks::density_upper_slack_multiplier).l1_norm();

        const double total_merit = objective_function_merit
                                   + elasticity_constraint_merit
                                   + filter_constraint_merit
                                   + lower_slack_merit + upper_slack_merit;
        return total_merit;
    }


  
    // This updates the penalty multiplier in the merit function, and then returns the largest scaled feasible step
    // Uses the "calculate_max_step_sizes" function to find the largest feasible step - s>0 and z>0

    template<int dim>
    BlockVector<double>
    SANDTopOpt<dim>::find_max_step(const BlockVector<double> &state,const double barrier_size)
    {
        nonlinear_solution = state;
        assemble_system(barrier_size);
        solve();
        const BlockVector<double> step = linear_solution;

        //Going to update penalty_multiplier in here too. Taken from 18.36 in Nocedal Wright

        double test_penalty_multiplier;
        double hess_part = 0;
        double grad_part = 0;
        double constraint_norm = 0;

        const std::vector<unsigned int> decision_variables = {SolutionBlocks::density, SolutionBlocks::displacement, SolutionBlocks::unfiltered_density};
        for(const unsigned int decision_variable_i : decision_variables)
        {
            for(const unsigned int decision_variable_j : decision_variables)
            {
                Vector<double> temp_vector(step.block(decision_variable_i).size());
                system_matrix.block(decision_variable_i,
                                    decision_variable_j).vmult(temp_vector,
                                                               step.block(decision_variable_j));
                hess_part += step.block(decision_variable_i) * temp_vector;
            }
            grad_part -= system_rhs.block(decision_variable_i)*step.block(decision_variable_i);
        }

        const std::vector<unsigned int> equality_constraints = {SolutionBlocks::displacement_multiplier,
                                                                SolutionBlocks::unfiltered_density_multiplier,
                                                                SolutionBlocks::density_lower_slack_multiplier,
                                                                SolutionBlocks::density_upper_slack_multiplier};
        for(unsigned int i : equality_constraints)
        {
            constraint_norm =   constraint_norm + system_rhs.block(i).linfty_norm();
        }

        if (hess_part > 0)
            test_penalty_multiplier = (grad_part + .5 * hess_part)/(.05 * constraint_norm);
        else
            test_penalty_multiplier = (grad_part)/(.05 * constraint_norm);
        
        if (test_penalty_multiplier > penalty_multiplier)
        {
            penalty_multiplier = test_penalty_multiplier;
            std::cout << "penalty multiplier updated to " << penalty_multiplier << std::endl;
        }

        const auto max_step_sizes= calculate_max_step_size(state,step,barrier_size);
        const double step_size_s = max_step_sizes.first;
        const double step_size_z = max_step_sizes.second;
        BlockVector<double> max_step(9);

        max_step.block(SolutionBlocks::density) = step_size_s * step.block(SolutionBlocks::density);
        max_step.block(SolutionBlocks::displacement) = step_size_s * step.block(SolutionBlocks::displacement);
        max_step.block(SolutionBlocks::unfiltered_density) = step_size_s * step.block(SolutionBlocks::unfiltered_density);
        max_step.block(SolutionBlocks::displacement_multiplier) = step_size_z * step.block(SolutionBlocks::displacement_multiplier);
        max_step.block(SolutionBlocks::unfiltered_density_multiplier) = step_size_z * step.block(SolutionBlocks::unfiltered_density_multiplier);
        max_step.block(SolutionBlocks::density_lower_slack) = step_size_s * step.block(SolutionBlocks::density_lower_slack);
        max_step.block(SolutionBlocks::density_lower_slack_multiplier) = step_size_z * step.block(SolutionBlocks::density_lower_slack_multiplier);
        max_step.block(SolutionBlocks::density_upper_slack) = step_size_s * step.block(SolutionBlocks::density_upper_slack);
        max_step.block(SolutionBlocks::density_upper_slack_multiplier) = step_size_z * step.block(SolutionBlocks::density_upper_slack_multiplier);

        return max_step;
    }

    // This is my back-stepping algorithm for a line search - keeps shrinking step size until it finds a step where the merit is decreased.

    template<int dim>
    BlockVector<double>
    SANDTopOpt<dim>::take_scaled_step(const BlockVector<double> &state,const BlockVector<double> &max_step,const double descent_requirement, const double barrier_size)
    {
        double step_size = 1;
            for(unsigned int k = 0; k<10; ++k)
            {
                const double merit_derivative = (calculate_exact_merit(state + .0001 * max_step,barrier_size) - calculate_exact_merit(state,barrier_size))/.0001;
                if(calculate_exact_merit(state + step_size * max_step,barrier_size) <calculate_exact_merit(state,barrier_size) + step_size * descent_requirement * merit_derivative )
                {
                    break;
                }
                else
                {
                    step_size = step_size/2;
                }
            }
        return state + (step_size * max_step);

    }



    // Checks to see if the KKT conditions are sufficiently met to lower barrier size.
    template<int dim>
    bool
    SANDTopOpt<dim>::check_convergence(const BlockVector<double> &state,  const double barrier_size)
    {
               const double convergence_condition = 1e-2;
               const BlockVector<double> test_rhs = calculate_test_rhs(state,barrier_size);
               std::cout << "current rhs norm is " << test_rhs.linfty_norm() << std::endl;
               if (test_rhs.l1_norm()<convergence_condition * barrier_size)
               {
                   return true;
               }
               else
               {
                   return false;
               }
    }


  // @sect3{Postprocessing the solution}
  
    // The first of the postprocessing functions outputs information
    // in a VTU file for visualization. It looks long, but it's really
    // just the same as what was done in step-22, for example, just
    // with (a lot) more solution variables:
    template<int dim>
    void
    SANDTopOpt<dim>::output_results(const unsigned int j) const {
        std::vector<std::string> solution_names(1, "density");
        std::vector<DataComponentInterpretation::DataComponentInterpretation> data_component_interpretation(
                1, DataComponentInterpretation::component_is_scalar);
        for (unsigned int i = 0; i < dim; ++i) {
            solution_names.emplace_back("displacement");
            data_component_interpretation.push_back(
                    DataComponentInterpretation::component_is_part_of_vector);
        }
        solution_names.emplace_back("unfiltered_density");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);
        for (unsigned int i = 0; i < dim; ++i) {
            solution_names.emplace_back("displacement_multiplier");
            data_component_interpretation.push_back(
                    DataComponentInterpretation::component_is_part_of_vector);
        }
        solution_names.emplace_back("unfiltered_density_multiplier");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);
        solution_names.emplace_back("low_slack");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);
        solution_names.emplace_back("low_slack_multiplier");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);
        solution_names.emplace_back("high_slack");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);
        solution_names.emplace_back("high_slack_multiplier");
        data_component_interpretation.push_back(
                DataComponentInterpretation::component_is_scalar);

        DataOut<dim> data_out;
        data_out.attach_dof_handler(dof_handler);
        data_out.add_data_vector(nonlinear_solution, solution_names,
                                 DataOut<dim>::type_dof_data, data_component_interpretation);
        data_out.build_patches();

        std::ofstream output("solution" + std::to_string(j) + ".vtu");
        data_out.write_vtu(output);
    }


    // The second of these functions outputs the solution as an `.stl`
    // file for 3d
    // printing. [STL](https://en.wikipedia.org/wiki/STL_(file_format))
    // files are made up of triangles and normal vectors, and we will
    // use it to show all of those cells with a density value larger
    // than zero by first extruding the mesh from a $z$ value of zero
    // to $z=0.25$, and then generating two triangles for each face of
    // the cells with a sufficiently large density value. The triangle
    // nodes must go counter-clockwise when looking from the outside,
    // and the normal vectors must be unit vectors pointing outwards,
    // which requires a few checks.
    template<int dim>
    void
    SANDTopOpt<dim>::write_as_stl()
    {
      static_assert(dim==2, "This function is not implemented for anything "
                    "other than the 2d case.");
      
    std::ofstream stlfile;
    stlfile.open ("bridge.stl");
    
    stlfile << "solid bridge\n" << std::scientific;
    double height = .25;
    
         for (const auto cell : dof_handler.active_cell_iterators())
         {

           if (nonlinear_solution.block(SolutionBlocks::density)[cell->active_cell_index()]>0.5)
             {
               // We have now found a cell with a density value larger
               // than zero. Let us start by writing out the bottom
               // and top faces. Owing to the ordering issue mentioned
               // above, we have to make sure that we understand
               // whether a cell has a right- or left-handed
               // coordinate system. We do this by interrogating the
               // directions of the two edges starting at vertex 0 and
               // whether they form a right-handed coordinate system.
               const Tensor<1,dim> edge_directions[2] = {cell->vertex(1)-cell->vertex(0),
                                                         cell->vertex(2)-cell->vertex(0) };
               const Tensor<2,dim> edge_tensor({{edge_directions[0][0], edge_directions[0][1]},
                     {edge_directions[2][0], edge_directions[2][1] }});
               const bool is_right_handed_cell = (determinant(edge_tensor) > 0);
               
                 if (is_right_handed_cell)
                 {
                   /* Write one side at z = 0. */
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << -1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(0)[0] << " " << cell->vertex(0)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << -1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(3)[0] << " " << cell->vertex(3)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";

                   /* Write one side at z = height. */
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << 1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(0)[0] << " " << cell->vertex(0)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << height << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << 1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(3)[0] << " " << cell->vertex(3)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << height << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                 }
                 else /* The cell has a left-handed set up */
                 {
                   /* Write one side at z = 0. */
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << -1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(0)[0] << " " << cell->vertex(0)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << -1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(3)[0] << " " << cell->vertex(3)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << 0.000000e+00 << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";

                   /* Write one side at z = height. */
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << 1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(0)[0] << " " << cell->vertex(0)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << height << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                     stlfile << "   facet normal " <<0.000000e+00 <<" " << 0.000000e+00 << " " << 1.000000e+00 << "\n";
                     stlfile << "      outer loop\n";
                     stlfile << "         vertex " << cell->vertex(1)[0] << " " << cell->vertex(1)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(2)[0] << " " << cell->vertex(2)[1] << " " << height << "\n";
                     stlfile << "         vertex " << cell->vertex(3)[0] << " " << cell->vertex(3)[1] << " " << height << "\n";
                     stlfile << "      endloop\n";
                     stlfile << "   endfacet\n";
                 }

                 // Next we need to deal with the four faces of the
                 // cell, extended into the $z$ direction. However, we
                 // only need to write these faces if either the face
                 // is on the domain boundary, or if it is the
                 // interface between a cell with density greater than
                 // 0.5, and a cell with a density less than 0.5.
                 for (unsigned int face_number = 0;
                      face_number < GeometryInfo<dim>::faces_per_cell; ++face_number)
                     {
                       const typename DoFHandler<dim>::face_iterator face = cell->face(face_number);
                       
                         if ((face->at_boundary())
                             ||
                             (!face->at_boundary()
                              &&
                              (nonlinear_solution.block(0)[cell->neighbor(face_number)->active_cell_index()]<0.5)))
                         {
                             const Tensor<1,dim> normal_vector
                                     = (face->center() - cell->center());
                             const double normal_norm = normal_vector.norm();
                             if ((face->vertex(0)[0] - face->vertex(0)[0])  *  (face->vertex(1)[1] - face->vertex(0)[1])  *  0.000000e+00
                                +(face->vertex(0)[1] - face->vertex(0)[1])  *  (0 - 0)                                                                                *  normal_vector[0]
                                +(height - 0)                                                                          *  (face->vertex(1)[0] - face->vertex(0)[0])        *  normal_vector[1]
                                -(face->vertex(0)[0] - face->vertex(0)[0])  *  (0 - 0)                                                                                *  normal_vector[1]
                                -(face->vertex(0)[1] - face->vertex(0)[1])  *  (face->vertex(1)[0] - face->vertex(0)[0]) *  normal_vector[0]
                                -(height - 0)                                                                     *  (face->vertex(1)[1] - face->vertex(0)[1])        *  0 >0)
                             {
                                 stlfile << "   facet normal " <<normal_vector[0]/normal_norm <<" " << normal_vector[1]/normal_norm << " " << 0.000000e+00 << "\n";
                                 stlfile << "      outer loop\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << height << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "      endloop\n";
                                 stlfile << "   endfacet\n";
                                 stlfile << "   facet normal " <<normal_vector[0]/normal_norm <<" " << normal_vector[1]/normal_norm << " " << 0.000000e+00 << "\n";
                                 stlfile << "      outer loop\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << height << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << height << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "      endloop\n";
                                 stlfile << "   endfacet\n";
                             }
                             else
                             {
                                 stlfile << "   facet normal " <<normal_vector[0]/normal_norm <<" " << normal_vector[1]/normal_norm << " " << 0.000000e+00 << "\n";
                                 stlfile << "      outer loop\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << height << "\n";
                                 stlfile << "      endloop\n";
                                 stlfile << "   endfacet\n";
                                 stlfile << "   facet normal " <<normal_vector[0]/normal_norm <<" " << normal_vector[1]/normal_norm << " " << 0.000000e+00 << "\n";
                                 stlfile << "      outer loop\n";
                                 stlfile << "         vertex " << face->vertex(0)[0] << " " << face->vertex(0)[1] << " " << height << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << 0.000000e+00 << "\n";
                                 stlfile << "         vertex " << face->vertex(1)[0] << " " << face->vertex(1)[1] << " " << height << "\n";
                                 stlfile << "      endloop\n";
                                 stlfile << "   endfacet\n";

                             }
                     }
                 }
             }
         }
        stlfile << "endsolid bridge";
    }

  

  // @sect3{The run() function driving the overall algorithm}

    // Contains watchdog algorithm
    template<int dim>
    void
    SANDTopOpt<dim>::run() {
        {
          TimerOutput::Scope t(timer, "setup");
          
          create_triangulation();

          dof_handler.distribute_dofs(fe);
          DoFRenumbering::component_wise(dof_handler);

          setup_boundary_values();
          setup_block_system();
          setup_filter_matrix();
        }
        
        double barrier_size = 25;
        const double min_barrier_size = .0005;
        
        const unsigned int max_uphill_steps = 8;
        unsigned int iteration_number = 0;
        const double descent_requirement = .0001;
        //while barrier value above minimal value and total iterations under some value
        BlockVector<double> current_state = nonlinear_solution;
        BlockVector<double> current_step;
        

        while((barrier_size > .0005 || !check_convergence(current_state, barrier_size)) && iteration_number < 10000)
        {
            bool converged = false;
            //while not converged
            while(!converged && iteration_number < 10000)
            {
                bool found_step = false;
                //save current state as watchdog state

                const BlockVector<double> watchdog_state = current_state;
                BlockVector<double> watchdog_step;
                double goal_merit;
                //for 1-8 steps - this is the number of steps away we will let it go uphill before demanding downhill
                for(unsigned int k = 0; k<max_uphill_steps; ++k)
                {
                    //compute step from current state  - function from kktSystem
                    current_step = find_max_step(current_state, barrier_size);
                    // save the first of these as the watchdog step
                    if(k==0)
                    {
                        watchdog_step = current_step;
                    }
                    //apply full step to current state
                    current_state=current_state+current_step;
                    //if merit of current state is less than goal
                    double current_merit = calculate_exact_merit(current_state, barrier_size);
                    std::cout << "current merit is: " <<current_merit << "  and  ";
                    double merit_derivative = ((calculate_exact_merit(watchdog_state+.0001*watchdog_step,barrier_size) - calculate_exact_merit(watchdog_state,barrier_size ))/.0001);
                    goal_merit = calculate_exact_merit(watchdog_state,barrier_size) + descent_requirement * merit_derivative;
                    std::cout << "goal merit is "<<goal_merit <<std::endl;
                    if(current_merit < goal_merit)
                    {
                        //Accept current state
                        // iterate number of steps by number of steps taken in this process
                        iteration_number = iteration_number + k + 1;
                        //found step = true
                        found_step = true;
                        std::cout << "found workable step after " << k+1 << " iterations"<<std::endl;
                        //break for loop
                        break;
                        //end if
                    }
                    //end for
                }
                //if found step = false
                if (!found_step)
                {
                    //Compute step from current state
                    current_step = find_max_step(current_state,barrier_size);
                    //find step length so that merit of stretch state - sized step from current length - is less than merit of (current state + descent requirement * linear derivative of merit of current state in direction of current step)
                    //update stretch state with found step length
                    const BlockVector<double> stretch_state = take_scaled_step(current_state, current_step, descent_requirement, barrier_size);
                    //if current merit is less than watchdog merit, or if stretch merit is less than earlier goal merit
                    if(calculate_exact_merit(current_state,barrier_size) < calculate_exact_merit(watchdog_state,barrier_size) || calculate_exact_merit(stretch_state,barrier_size) < goal_merit)
                    {
                        std::cout << "Taking scaled step from end of watchdog" << std::endl;
                        current_state = stretch_state;
                        iteration_number = iteration_number + max_uphill_steps + 1;
                    }
                    else
                    {
                        std::cout << "Taking scaled step from beginning of watchdog" << std::endl;
                        //if merit of stretch state is bigger than watchdog merit
                        if (calculate_exact_merit(stretch_state,barrier_size) > calculate_exact_merit(watchdog_state,barrier_size))
                        {
                            //find step length from watchdog state that meets descent requirement
                            current_state = take_scaled_step(watchdog_state, watchdog_step, descent_requirement, barrier_size);
                            //update iteration count
                            iteration_number = iteration_number +  max_uphill_steps + 1;
                        }
                        else
                        {
                            //calculate direction from stretch state
                            const BlockVector<double> stretch_step = find_max_step(stretch_state,barrier_size);
                            //find step length from stretch state that meets descent requirement
                            current_state = take_scaled_step(stretch_state, stretch_step, descent_requirement,barrier_size);
                            //update iteration count
                            iteration_number = iteration_number + max_uphill_steps + 2;
                        }
                    }
                }
                //output current state
                output_results(iteration_number);
                //check convergence
                converged = check_convergence(current_state, barrier_size);
                //end while
            }
            const double barrier_size_multiplier = .8;
            const double barrier_size_exponent = 1.2;

            if (barrier_size * barrier_size_multiplier < std::pow(barrier_size, barrier_size_exponent))
            {
                if (barrier_size * barrier_size_multiplier < min_barrier_size)
                {
                    barrier_size = min_barrier_size;
                }
                else
                {
                    barrier_size = barrier_size * barrier_size_multiplier;
                }
            }
            else
            {
                if (std::pow(barrier_size, barrier_size_exponent) < min_barrier_size)
                {
                    barrier_size = min_barrier_size;
                }
                else
                {
                    barrier_size = std::pow(barrier_size, barrier_size_exponent);
                }
            }



//            barrier_size = barrier_size * barrier_size_multiplier;
            std::cout << "barrier size reduced to " << barrier_size << " on iteration number " << iteration_number << std::endl;
//
//            penalty_multiplier = 1;
            //end while
        }

        write_as_stl();
        timer.print_summary ();
    }

} // namespace SAND



// @sect3{The main function}

// The remainder of the code, the `main()` function, is as usual:
int
main() {
    try {
        SAND::SANDTopOpt<2> elastic_problem_2d;
        elastic_problem_2d.run();
    }
    catch (std::exception &exc) {
        std::cerr << std::endl << std::endl
                  << "----------------------------------------------------" << std::endl;
        std::cerr << "Exception on processing: " << std::endl << exc.what()
                  << std::endl << "Aborting!" << std::endl
                  << "----------------------------------------------------" << std::endl;

        return 1;
    }
    catch (...) {
        std::cerr << std::endl << std::endl
                  << "----------------------------------------------------" << std::endl;
        std::cerr << "Unknown exception!" << std::endl << "Aborting!" << std::endl
                  << "----------------------------------------------------" << std::endl;
        return 1;
    }

    return 0;
}
