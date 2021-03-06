// The libMesh Finite Element Library.
// Copyright (C) 2002-2017 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA



#include "libmesh/libmesh_common.h"

#ifdef LIBMESH_HAVE_PETSC


// C++ includes

// Local Includes
#include "libmesh/nonlinear_implicit_system.h"
#include "libmesh/petsc_nonlinear_solver.h"
#include "libmesh/petsc_linear_solver.h"
#include "libmesh/petsc_vector.h"
#include "libmesh/petsc_matrix.h"
#include "libmesh/dof_map.h"
#include "libmesh/preconditioner.h"
#include "libmesh/solver_configuration.h"

/* DMlibMesh include. */
#include "libmesh/petscdmlibmesh.h"

namespace libMesh
{

//--------------------------------------------------------------------
// Functions with C linkage to pass to PETSc.  PETSc will call these
// methods as needed.
//
// Since they must have C linkage they have no knowledge of a namespace.
// Give them an obscure name to avoid namespace pollution.
extern "C"
{
  //-------------------------------------------------------------------
  // this function is called by PETSc at the end of each nonlinear step
  PetscErrorCode
  __libmesh_petsc_snes_monitor (SNES, PetscInt its, PetscReal fnorm, void *)
  {
    //PetscErrorCode ierr=0;

    //if (its > 0)
    libMesh::out << "  NL step "
                 << std::setw(2) << its
                 << std::scientific
                 << ", |residual|_2 = " << fnorm
                 << std::endl;

    //return ierr;
    return 0;
  }



  //---------------------------------------------------------------
  // this function is called by PETSc to evaluate the residual at X
  PetscErrorCode
  __libmesh_petsc_snes_residual (SNES snes, Vec x, Vec r, void * ctx)
  {
    LOG_SCOPE("residual()", "PetscNonlinearSolver");

    PetscErrorCode ierr=0;

    libmesh_assert(x);
    libmesh_assert(r);
    libmesh_assert(ctx);

    // No way to safety-check this cast, since we got a void *...
    PetscNonlinearSolver<Number> * solver =
      static_cast<PetscNonlinearSolver<Number> *> (ctx);

    // Get the current iteration number from the snes object,
    // store it in the PetscNonlinearSolver object for possible use
    // by the user's residual function.
    {
      PetscInt n_iterations = 0;
      ierr = SNESGetIterationNumber(snes, &n_iterations);
      CHKERRABORT(solver->comm().get(),ierr);
      solver->_current_nonlinear_iteration_number = cast_int<unsigned>(n_iterations);
    }

    NonlinearImplicitSystem & sys = solver->system();

    PetscVector<Number> & X_sys = *cast_ptr<PetscVector<Number> *>(sys.solution.get());
    PetscVector<Number> X_global(x, sys.comm()), R(r, sys.comm());

    // Use the system's update() to get a good local version of the
    // parallel solution.  This operation does not modify the incoming
    // "x" vector, it only localizes information from "x" into
    // sys.current_local_solution.
    X_global.swap(X_sys);
    sys.update();
    X_global.swap(X_sys);

    // Enforce constraints (if any) exactly on the
    // current_local_solution.  This is the solution vector that is
    // actually used in the computation of the residual below, and is
    // not locked by debug-enabled PETSc the way that "x" is.
    sys.get_dof_map().enforce_constraints_exactly(sys, sys.current_local_solution.get());

    if (solver->_zero_out_residual)
      R.zero();

    //-----------------------------------------------------------------------------
    // if the user has provided both function pointers and objects only the pointer
    // will be used, so catch that as an error
    if (solver->residual && solver->residual_object)
      libmesh_error_msg("ERROR: cannot specify both a function and object to compute the Residual!");

    if (solver->matvec && solver->residual_and_jacobian_object)
      libmesh_error_msg("ERROR: cannot specify both a function and object to compute the combined Residual & Jacobian!");

    if (solver->residual != libmesh_nullptr)
      solver->residual(*sys.current_local_solution.get(), R, sys);

    else if (solver->residual_object != libmesh_nullptr)
      solver->residual_object->residual(*sys.current_local_solution.get(), R, sys);

    else if (solver->matvec != libmesh_nullptr)
      solver->matvec (*sys.current_local_solution.get(), &R, libmesh_nullptr, sys);

    else if (solver->residual_and_jacobian_object != libmesh_nullptr)
      solver->residual_and_jacobian_object->residual_and_jacobian (*sys.current_local_solution.get(), &R, libmesh_nullptr, sys);

    else
      libmesh_error_msg("Error! Unable to compute residual and/or Jacobian!");

    R.close();

    return ierr;
  }



  //---------------------------------------------------------------
  // this function is called by PETSc to evaluate the Jacobian at X
  PetscErrorCode
  __libmesh_petsc_snes_jacobian(
#if PETSC_RELEASE_LESS_THAN(3,5,0)
                                SNES snes, Vec x, Mat * jac, Mat * pc, MatStructure * msflag, void * ctx
#else
                                SNES snes, Vec x, Mat jac, Mat pc, void * ctx
#endif
                                )
  {
    LOG_SCOPE("jacobian()", "PetscNonlinearSolver");

    PetscErrorCode ierr=0;

    libmesh_assert(ctx);

    // No way to safety-check this cast, since we got a void *...
    PetscNonlinearSolver<Number> * solver =
      static_cast<PetscNonlinearSolver<Number> *> (ctx);

    // Get the current iteration number from the snes object,
    // store it in the PetscNonlinearSolver object for possible use
    // by the user's Jacobian function.
    {
      PetscInt n_iterations = 0;
      ierr = SNESGetIterationNumber(snes, &n_iterations);
      CHKERRABORT(solver->comm().get(),ierr);
      solver->_current_nonlinear_iteration_number = cast_int<unsigned>(n_iterations);
    }

    NonlinearImplicitSystem & sys = solver->system();
#if PETSC_RELEASE_LESS_THAN(3,5,0)
    PetscMatrix<Number> PC(*pc, sys.comm());
    PetscMatrix<Number> Jac(*jac, sys.comm());
#else
    PetscMatrix<Number> PC(pc, sys.comm());
    PetscMatrix<Number> Jac(jac, sys.comm());
#endif
    PetscVector<Number> & X_sys = *cast_ptr<PetscVector<Number> *>(sys.solution.get());
    PetscVector<Number> X_global(x, sys.comm());

    // Set the dof maps
    PC.attach_dof_map(sys.get_dof_map());
    Jac.attach_dof_map(sys.get_dof_map());

    // Use the systems update() to get a good local version of the parallel solution
    X_global.swap(X_sys);
    sys.update();
    X_global.swap(X_sys);

    // Enforce constraints (if any) exactly on the
    // current_local_solution.  This is the solution vector that is
    // actually used in the computation of the residual below, and is
    // not locked by debug-enabled PETSc the way that "x" is.
    sys.get_dof_map().enforce_constraints_exactly(sys, sys.current_local_solution.get());

    if (solver->_zero_out_jacobian)
      PC.zero();

    //-----------------------------------------------------------------------------
    // if the user has provided both function pointers and objects only the pointer
    // will be used, so catch that as an error
    if (solver->jacobian && solver->jacobian_object)
      libmesh_error_msg("ERROR: cannot specify both a function and object to compute the Jacobian!");

    if (solver->matvec && solver->residual_and_jacobian_object)
      libmesh_error_msg("ERROR: cannot specify both a function and object to compute the combined Residual & Jacobian!");

    if (solver->jacobian != libmesh_nullptr)
      solver->jacobian(*sys.current_local_solution.get(), PC, sys);

    else if (solver->jacobian_object != libmesh_nullptr)
      solver->jacobian_object->jacobian(*sys.current_local_solution.get(), PC, sys);

    else if (solver->matvec != libmesh_nullptr)
      solver->matvec(*sys.current_local_solution.get(), libmesh_nullptr, &PC, sys);

    else if (solver->residual_and_jacobian_object != libmesh_nullptr)
      solver->residual_and_jacobian_object->residual_and_jacobian (*sys.current_local_solution.get(), libmesh_nullptr, &PC, sys);

    else
      libmesh_error_msg("Error! Unable to compute residual and/or Jacobian!");

    PC.close();
    Jac.close();
#if PETSC_RELEASE_LESS_THAN(3,5,0)
    *msflag = SAME_NONZERO_PATTERN;
#endif

    return ierr;
  }

  // This function gets called by PETSc after the SNES linesearch is
  // complete.  We use it to exactly enforce any constraints on the
  // solution which may have drifted during the linear solve.  In the
  // PETSc nomenclature:
  // * "x" is the old solution vector,
  // * "y" is the search direction (Newton step) vector,
  // * "w" is the candidate solution vector, and
  // the user is responsible for setting changed_y and changed_w
  // appropriately, depending on whether or not the search
  // direction or solution vector was changed, respectively.
  PetscErrorCode __libmesh_petsc_snes_postcheck(
#if PETSC_VERSION_LESS_THAN(3,3,0)
                                                SNES, Vec x, Vec y, Vec w, void * context, PetscBool * changed_y, PetscBool * changed_w
#else
                                                SNESLineSearch, Vec x, Vec y, Vec w, PetscBool * changed_y, PetscBool * changed_w, void * context
#endif
                                                )
  {
    LOG_SCOPE("postcheck()", "PetscNonlinearSolver");

    PetscErrorCode ierr = 0;

    // PETSc almost certainly initializes these to false already, but
    // it doesn't hurt to be explicit.
    *changed_w = PETSC_FALSE;
    *changed_y = PETSC_FALSE;

    libmesh_assert(context);

    // Cast the context to a NonlinearSolver object.
    PetscNonlinearSolver<Number> * solver =
      static_cast<PetscNonlinearSolver<Number> *> (context);

    // If the user has provided both postcheck function pointer and
    // object, this is ambiguous, so throw an error.
    if (solver->postcheck && solver->postcheck_object)
      libmesh_error_msg("ERROR: cannot specify both a function and object for performing the solve postcheck!");

    // It's also possible that we don't need to do anything at all, in
    // that case return early...
    NonlinearImplicitSystem & sys = solver->system();
    DofMap & dof_map = sys.get_dof_map();

    if (!dof_map.n_constrained_dofs() && !solver->postcheck && !solver->postcheck_object)
      return ierr;

    // We definitely need to wrap at least "w"
    PetscVector<Number> petsc_w(w, sys.comm());

    // The user sets these flags in his/her postcheck function to
    // indicate whether they changed something.
    bool
      changed_search_direction = false,
      changed_new_soln = false;

    if (solver->postcheck || solver->postcheck_object)
      {
        PetscVector<Number> petsc_x(x, sys.comm());
        PetscVector<Number> petsc_y(y, sys.comm());

        if (solver->postcheck)
          solver->postcheck(petsc_x,
                            petsc_y,
                            petsc_w,
                            changed_search_direction,
                            changed_new_soln,
                            sys);

        else if (solver->postcheck_object)
          solver->postcheck_object->postcheck(petsc_x,
                                              petsc_y,
                                              petsc_w,
                                              changed_search_direction,
                                              changed_new_soln,
                                              sys);
      }

    // Record whether the user changed the solution or the search direction.
    if (changed_search_direction)
      *changed_y = PETSC_TRUE;

    if (changed_new_soln)
      *changed_w = PETSC_TRUE;

    if (dof_map.n_constrained_dofs())
      {
        PetscVector<Number> & system_soln = *cast_ptr<PetscVector<Number> *>(sys.solution.get());

        // ... and swap it in before enforcing the constraints.
        petsc_w.swap(system_soln);

        dof_map.enforce_constraints_exactly(sys);

        // If we have constraints, we'll assume that we did change the
        // solution w (hopefully slightly).  Enforcing constraints
        // does not change the search direction, y, but the user may
        // have, so we leave it alone.
        *changed_w = PETSC_TRUE;

        // Swap back
        petsc_w.swap(system_soln);
      }

    return ierr;
  }

} // end extern "C"



//---------------------------------------------------------------------
// PetscNonlinearSolver<> methods
template <typename T>
PetscNonlinearSolver<T>::PetscNonlinearSolver (sys_type & system_in) :
  NonlinearSolver<T>(system_in),
  _reason(SNES_CONVERGED_ITERATING/*==0*/), // Arbitrary initial value...
  _n_linear_iterations(0),
  _current_nonlinear_iteration_number(0),
  _zero_out_residual(true),
  _zero_out_jacobian(true),
  _default_monitor(true)
{
}



template <typename T>
PetscNonlinearSolver<T>::~PetscNonlinearSolver ()
{
  this->clear ();
}



template <typename T>
void PetscNonlinearSolver<T>::clear ()
{
  if (this->initialized())
    {
      this->_is_initialized = false;

      PetscErrorCode ierr=0;

      ierr = LibMeshSNESDestroy(&_snes);
      LIBMESH_CHKERR(ierr);

      // Reset the nonlinear iteration counter.  This information is only relevant
      // *during* the solve().  After the solve is completed it should return to
      // the default value of 0.
      _current_nonlinear_iteration_number = 0;
    }
}



template <typename T>
void PetscNonlinearSolver<T>::init (const char * name)
{
  // Initialize the data structures if not done so already.
  if (!this->initialized())
    {
      this->_is_initialized = true;

      PetscErrorCode ierr=0;

      ierr = SNESCreate(this->comm().get(),&_snes);
      LIBMESH_CHKERR(ierr);

      if (name)
        {
          ierr = SNESSetOptionsPrefix(_snes, name);
          LIBMESH_CHKERR(ierr);
        }

#if !PETSC_RELEASE_LESS_THAN(3,3,0)
      // Attaching a DM to SNES.
      DM dm;
      ierr = DMCreate(this->comm().get(), &dm);LIBMESH_CHKERR(ierr);
      ierr = DMSetType(dm,DMLIBMESH);LIBMESH_CHKERR(ierr);
      ierr = DMlibMeshSetSystem(dm,this->system());LIBMESH_CHKERR(ierr);
      if (name)
        {
          ierr = DMSetOptionsPrefix(dm,name);    LIBMESH_CHKERR(ierr);
        }
      ierr = DMSetFromOptions(dm);               LIBMESH_CHKERR(ierr);
      ierr = DMSetUp(dm);                        LIBMESH_CHKERR(ierr);
      ierr = SNESSetDM(this->_snes, dm);         LIBMESH_CHKERR(ierr);
      // SNES now owns the reference to dm.
      ierr = DMDestroy(&dm);                     LIBMESH_CHKERR(ierr);

#endif

      if (_default_monitor)
        {
          ierr = SNESMonitorSet (_snes, __libmesh_petsc_snes_monitor,
                                 this, PETSC_NULL);
          LIBMESH_CHKERR(ierr);
        }

      // If the SolverConfiguration object is provided, use it to set
      // options during solver initialization.
      if (this->_solver_configuration)
        {
          this->_solver_configuration->set_options_during_init();
        }

#if PETSC_VERSION_LESS_THAN(3,1,0)
      // Cannot call SNESSetOptions before SNESSetFunction when using
      // any matrix free options with PETSc 3.1.0+
      ierr = SNESSetFromOptions(_snes);
      LIBMESH_CHKERR(ierr);
#endif

      if (this->_preconditioner)
        {
          KSP ksp;
          ierr = SNESGetKSP (_snes, &ksp);
          LIBMESH_CHKERR(ierr);
          PC pc;
          ierr = KSPGetPC(ksp,&pc);
          LIBMESH_CHKERR(ierr);

          this->_preconditioner->init();

          PCSetType(pc, PCSHELL);
          PCShellSetContext(pc,(void *)this->_preconditioner);

          //Re-Use the shell functions from petsc_linear_solver
          PCShellSetSetUp(pc,__libmesh_petsc_preconditioner_setup);
          PCShellSetApply(pc,__libmesh_petsc_preconditioner_apply);
        }
    }


  // Tell PETSc about our linesearch "post-check" function, but only
  // if the user has provided one.  There seem to be extra,
  // unnecessary residual calculations if a postcheck function is
  // attached for no reason.
  if (this->postcheck || this->postcheck_object)
    {
#if PETSC_VERSION_LESS_THAN(3,3,0)
      PetscErrorCode ierr = SNESLineSearchSetPostCheck(_snes, __libmesh_petsc_snes_postcheck, this);
      LIBMESH_CHKERR(ierr);

#else
      // Pick the right version of the function name
#if PETSC_VERSION_LESS_THAN(3,4,0)
#  define SNESGETLINESEARCH SNESGetSNESLineSearch
#else
#  define SNESGETLINESEARCH SNESGetLineSearch
#endif

      SNESLineSearch linesearch;
      PetscErrorCode ierr = SNESGETLINESEARCH(_snes, &linesearch);
      LIBMESH_CHKERR(ierr);

      ierr = SNESLineSearchSetPostCheck(linesearch, __libmesh_petsc_snes_postcheck, this);
      LIBMESH_CHKERR(ierr);
#endif
    }
}

#if !PETSC_VERSION_LESS_THAN(3,3,0)
template <typename T>
void
PetscNonlinearSolver<T>::build_mat_null_space(NonlinearImplicitSystem::ComputeVectorSubspace * computeSubspaceObject,
                                              void (*computeSubspace)(std::vector<NumericVector<Number> *> &, sys_type &),
                                              MatNullSpace * msp)
{
  PetscErrorCode ierr;
  std::vector<NumericVector<Number> *> sp;
  if (computeSubspaceObject)
    (*computeSubspaceObject)(sp, this->system());
  else
    (*computeSubspace)(sp, this->system());

  *msp = PETSC_NULL;
  if (sp.size())
    {
      Vec * modes;
      PetscScalar * dots;
      PetscInt nmodes = cast_int<PetscInt>(sp.size());

#if PETSC_RELEASE_LESS_THAN(3,5,0)
      ierr = PetscMalloc2(nmodes,Vec,&modes,nmodes,PetscScalar,&dots);
#else
      ierr = PetscMalloc2(nmodes,&modes,nmodes,&dots);
#endif
      LIBMESH_CHKERR(ierr);

      for (PetscInt i=0; i<nmodes; ++i)
        {
          PetscVector<T> * pv = cast_ptr<PetscVector<T> *>(sp[i]);
          Vec v = pv->vec();

          ierr = VecDuplicate(v, modes+i);
          LIBMESH_CHKERR(ierr);

          ierr = VecCopy(v,modes[i]);
          LIBMESH_CHKERR(ierr);
        }

      // Normalize.
      ierr = VecNormalize(modes[0],PETSC_NULL);
      LIBMESH_CHKERR(ierr);

      for (PetscInt i=1; i<nmodes; i++)
        {
          // Orthonormalize vec[i] against vec[0:i-1]
          ierr = VecMDot(modes[i],i,modes,dots);
          LIBMESH_CHKERR(ierr);

          for (PetscInt j=0; j<i; j++)
            dots[j] *= -1.;

          ierr = VecMAXPY(modes[i],i,dots,modes);
          LIBMESH_CHKERR(ierr);

          ierr = VecNormalize(modes[i],PETSC_NULL);
          LIBMESH_CHKERR(ierr);
        }

      ierr = MatNullSpaceCreate(this->comm().get(), PETSC_FALSE, nmodes, modes, msp);
      LIBMESH_CHKERR(ierr);

      for (PetscInt i=0; i<nmodes; ++i)
        {
          ierr = VecDestroy(modes+i);
          LIBMESH_CHKERR(ierr);
        }

      ierr = PetscFree2(modes,dots);
      LIBMESH_CHKERR(ierr);
    }
}
#endif

template <typename T>
std::pair<unsigned int, Real>
PetscNonlinearSolver<T>::solve (SparseMatrix<T> &  jac_in,  // System Jacobian Matrix
                                NumericVector<T> & x_in,    // Solution vector
                                NumericVector<T> & r_in,    // Residual vector
                                const double,              // Stopping tolerance
                                const unsigned int)
{
  LOG_SCOPE("solve()", "PetscNonlinearSolver");
  this->init ();

  // Make sure the data passed in are really of Petsc types
  PetscMatrix<T> * jac = cast_ptr<PetscMatrix<T> *>(&jac_in);
  PetscVector<T> * x   = cast_ptr<PetscVector<T> *>(&x_in);
  PetscVector<T> * r   = cast_ptr<PetscVector<T> *>(&r_in);

  PetscErrorCode ierr=0;
  PetscInt n_iterations =0;
  // Should actually be a PetscReal, but I don't know which version of PETSc first introduced PetscReal
  Real final_residual_norm=0.;

  ierr = SNESSetFunction (_snes, r->vec(), __libmesh_petsc_snes_residual, this);
  LIBMESH_CHKERR(ierr);

  // Only set the jacobian function if we've been provided with something to call.
  // This allows a user to set their own jacobian function if they want to
  if (this->jacobian || this->jacobian_object || this->residual_and_jacobian_object)
    {
      ierr = SNESSetJacobian (_snes, jac->mat(), jac->mat(), __libmesh_petsc_snes_jacobian, this);
      LIBMESH_CHKERR(ierr);
    }
#if !PETSC_VERSION_LESS_THAN(3,3,0)
  // Only set the nullspace if we have a way of computing it and the result is non-empty.
  if (this->nullspace || this->nullspace_object)
    {
      MatNullSpace msp;
      this->build_mat_null_space(this->nullspace_object, this->nullspace, &msp);
      if (msp)
        {
          ierr = MatSetNullSpace(jac->mat(), msp);
          LIBMESH_CHKERR(ierr);

          ierr = MatNullSpaceDestroy(&msp);
          LIBMESH_CHKERR(ierr);
        }
    }

  // Only set the transpose nullspace if we have a way of computing it and the result is non-empty.
  if (this->transpose_nullspace || this->transpose_nullspace_object)
    {
#if PETSC_VERSION_LESS_THAN(3,6,0)
      libmesh_warning("MatSetTransposeNullSpace is only supported for PETSc >= 3.6, transpose nullspace will be ignored.");
#else
      MatNullSpace msp = PETSC_NULL;
      this->build_mat_null_space(this->transpose_nullspace_object, this->transpose_nullspace, &msp);
      if (msp)
        {
          ierr = MatSetTransposeNullSpace(jac->mat(), msp);
          LIBMESH_CHKERR(ierr);

          ierr = MatNullSpaceDestroy(&msp);
          LIBMESH_CHKERR(ierr);
        }
#endif
    }

  // Only set the nearnullspace if we have a way of computing it and the result is non-empty.
  if (this->nearnullspace || this->nearnullspace_object)
    {
      MatNullSpace msp = PETSC_NULL;
      this->build_mat_null_space(this->nearnullspace_object, this->nearnullspace, &msp);

      if (msp)
        {
          ierr = MatSetNearNullSpace(jac->mat(), msp);
          LIBMESH_CHKERR(ierr);

          ierr = MatNullSpaceDestroy(&msp);
          LIBMESH_CHKERR(ierr);
        }
    }
#endif
  // Have the Krylov subspace method use our good initial guess rather than 0
  KSP ksp;
  ierr = SNESGetKSP (_snes, &ksp);
  LIBMESH_CHKERR(ierr);

  // Set the tolerances for the iterative solver.  Use the user-supplied
  // tolerance for the relative residual & leave the others at default values
  ierr = KSPSetTolerances (ksp, this->initial_linear_tolerance, PETSC_DEFAULT,
                           PETSC_DEFAULT, this->max_linear_iterations);
  LIBMESH_CHKERR(ierr);

  // Set the tolerances for the non-linear solver.
  ierr = SNESSetTolerances(_snes, this->absolute_residual_tolerance, this->relative_residual_tolerance,
                           this->relative_step_tolerance, this->max_nonlinear_iterations, this->max_function_evaluations);
  LIBMESH_CHKERR(ierr);

  //Pull in command-line options
#if PETSC_VERSION_LESS_THAN(3,7,0)
  KSPSetFromOptions(ksp);
#endif
  SNESSetFromOptions(_snes);

  if (this->user_presolve)
    this->user_presolve(this->system());

  //Set the preconditioning matrix
  if (this->_preconditioner)
    {
      this->_preconditioner->set_matrix(jac_in);
      this->_preconditioner->init();
    }

  // If the SolverConfiguration object is provided, use it to override
  // solver options.
  if (this->_solver_configuration)
    {
      this->_solver_configuration->configure_solver();
    }

  ierr = SNESSolve (_snes, PETSC_NULL, x->vec());
  LIBMESH_CHKERR(ierr);

  ierr = SNESGetIterationNumber(_snes,&n_iterations);
  LIBMESH_CHKERR(ierr);

  ierr = SNESGetLinearSolveIterations(_snes, &_n_linear_iterations);
  LIBMESH_CHKERR(ierr);

  // Enforce constraints exactly now that the solve is done.  We have
  // been enforcing them on the current_local_solution during the
  // solve, but now need to be sure they are enforced on the parallel
  // solution vector as well.
  this->system().get_dof_map().enforce_constraints_exactly(this->system());

  // SNESGetFunction has been around forever and should work on all
  // versions of PETSc.  This is also now the recommended approach
  // according to the documentation for the PETSc 3.5.1 release:
  // http://www.mcs.anl.gov/petsc/documentation/changes/35.html
  Vec f;
  ierr = SNESGetFunction(_snes, &f, 0, 0);
  LIBMESH_CHKERR(ierr);
  ierr = VecNorm(f, NORM_2, &final_residual_norm);
  LIBMESH_CHKERR(ierr);

  // Get and store the reason for convergence
  SNESGetConvergedReason(_snes, &_reason);

  //Based on Petsc 2.3.3 documentation all diverged reasons are negative
  this->converged = (_reason >= 0);

  this->clear();

  // return the # of its. and the final residual norm.
  return std::make_pair(n_iterations, final_residual_norm);
}



template <typename T>
void PetscNonlinearSolver<T>::print_converged_reason()
{

  libMesh::out << "Nonlinear solver convergence/divergence reason: "
               << SNESConvergedReasons[this->get_converged_reason()] << std::endl;
}



template <typename T>
SNESConvergedReason PetscNonlinearSolver<T>::get_converged_reason()
{
  PetscErrorCode ierr=0;

  if (this->initialized())
    {
      ierr = SNESGetConvergedReason(_snes, &_reason);
      LIBMESH_CHKERR(ierr);
    }

  return _reason;
}

template <typename T>
int PetscNonlinearSolver<T>::get_total_linear_iterations()
{
  return _n_linear_iterations;
}


//------------------------------------------------------------------
// Explicit instantiations
template class PetscNonlinearSolver<Number>;

} // namespace libMesh



#endif // #ifdef LIBMESH_HAVE_PETSC
