/*******************************************************************************
 *
 * triqs_arpack
 *
 * Copyright (C) 2016 I. Krivenko
 *
 * triqs_arpack is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * triqs_arpack is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * triqs_arpack. If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#pragma once

namespace triqs { namespace arrays { namespace arpack {

using namespace triqs::arrays;

/******************
 * Complex matrix A
 ******************/
template<> class arpack_worker<Complex> {

 int N;                    // Matrix size
 const char* which;        // WHICH parameter
 int nev;                  // Number of eigenvalues
 int tol;                  // Relative tolerance for Ritz value convergence
 vector<dcomplex> resid;   // Residual vector
 int ncv;                  // Number of Lanczos vectors to be generated
 matrix<dcomplex> v;       // Matrix with Arnoldi basis vectors
 matrix<dcomplex> z;       // Matrix with Ritz vectors
 vector<dcomplex> d;       // Ritz values (real and imaginary parts)
 int iparam[11];           // Various input/output parameters
 int ipntr[14];            // Pointer to mark the starting locations in the workd and workl
 vector<dcomplex> workd;   // Working space
 int info = 0;             // !=0 to use resid, 0 otherwise
 int rvec;                 // RVEC parameter of dseupd
 char howmny;              // HOWMNY parameter of dseupd
 vector<int> select;       // SELECT parameter of dseupd

public:

 struct params_t {
  // Number of eigenvalues (Ritz values) to compute
  unsigned int n_eigenvalues;
  // Which of the Ritz values to compute
  enum {LargestMagnitude, SmallestMagnitude,
        LargestReal, SmallestReal, LargestImag, SmallestImag} eigenvalues_select;

  // Expert option: number of Lanczos vectors to be generated
  // default: min(2*n_eigenvalues+1, N)
  int ncv = -1;

  // Compute Ritz/Schur vectors?
  // None: do not compute anything;
  // Ritz: compute eigenvectors of A;
  // Schur: compute orthogonal basis vectors of the n_eigenvalues-dimensional subspace.
  enum {None, Ritz, Schur} compute_vectors;

  // Use a randomly generated initial residual vector
  bool random_residual_vector = true;
  // Initial residual vector
  vector<dcomplex> init_residual_vector = {};

  // Eigenvalue shift
  dcomplex sigma = 0;

  // relative tolerance for Ritz value convergence
  double tolerance = 0;
  // Maximum number of Arnoldi update iterations
  unsigned int max_iter = INT_MAX;

  params_t(unsigned int n_eigenvalues,
           decltype(eigenvalues_select) ev_select,
           decltype(compute_vectors) compute_evec) :
   n_eigenvalues(n_eigenvalues),
   eigenvalues_select(ev_select),
   compute_vectors(compute_evec)
  {}
 };

 arpack_worker(unsigned int N) :
  N(N), resid(N), workd(3*N), v(0,0,FORTRAN_LAYOUT), z(0,0,FORTRAN_LAYOUT) {
  iparam[3] = 1;
 }

 // Prepare values of input parameters
 void prepare(params_t const& params) {

  // Check n_eigenvalues
  nev = params.n_eigenvalues;
  unsigned int nev_min = 1;
  unsigned int nev_max = N-2;

  if(nev < nev_min || nev > nev_max)
   TRIQS_RUNTIME_ERROR << "arpack_worker: n_eigenvalues must be within [" << nev_min
                       << ";" << nev_max << "]";

  // Character codes for eigenvalues_select
  static const std::array<const char*,6> wh = {"LM","SM","LR","SR","LI","SI"};
  which = wh[int(params.eigenvalues_select)];

  // Check ncv
  ncv = params.ncv;
  if(ncv == -1) ncv = std::min(2*int(params.n_eigenvalues)+2, N);
  else if(ncv <= params.n_eigenvalues+1 || ncv > N)
   TRIQS_RUNTIME_ERROR << "arpack_worker: ncv must be within ]" << params.n_eigenvalues+1
                       << ";" << N << "]";
  v.resize(N,ncv);

  // Eigenvectors
  rvec = (params.compute_vectors != params_t::None);
  howmny = params.compute_vectors == params_t::Schur ? 'P' : 'A';
  select.resize(ncv);
  if(rvec && params.compute_vectors == params_t::Ritz)
   z.resize(N,nev+1);
  else
   z.resize(1,nev+1);

  // Tolerance
  tol = std::max(.0, params.tolerance);
  if(params.tolerance < 0) {
   std::cerr << "arpack_worker: negative tolerance " << params.tolerance
             << " is interpreted as machine epsilon." << std::endl;
  }

  // Initial residual vector
  if(params.random_residual_vector) {
   info = 0;
  } else {
   info = 1;
   if(params.init_residual_vector.size() != N)
    TRIQS_RUNTIME_ERROR << "arpack_worker: initial residual vector of a wrong size "
                        << params.init_residual_vector.size()
                        << " (must be " << N << ")";
    resid = params.init_residual_vector;
  }

  iparam[2] = int(params.max_iter); // Max number of iterations
  if(iparam[2] <= 0)
   TRIQS_RUNTIME_ERROR << "arpack_worker: maximum number of Arnoldi update iterations must be positive";
 }

 struct trivial_shifts_f {
  void operator()(vector_view<dcomplex> shifts_re, vector_view<dcomplex> shifts_im){}
 };

 /**********************************
 * Solve the standard eigenproblem
 * A*x = \lambda*x
 **********************************/

 // a: callable taking 4 arguments
 // a(vector_view<dcomplex> from, int from_n, vector_view<dcomplex> to, int to_n)
 // 'a' is expected to act on 'from' and write the result to 'to': to = A*from
 // 'from' is also indirectly available as this->workspace_vector(from_n)
 // 'to' is also indirectly available as this->workspace_vector(to_n)
 //
 // shifts_f: callable taking two arguments
 // shifts_f(vector_view<dcomplex> shifts_re, vector_view<dcomplex> shifts_im)
 // 'shifts_f' is expected to place real and imaginary parts of the shifts for implicit restart
 // into 'shifts_re' and 'shifts_im' respectively.
 template<typename A, typename ShiftsF = trivial_shifts_f>
 void operator()(A a, params_t const& params, ShiftsF shifts_f = {}) {

  prepare(params);

  iparam[0] = (std::is_same<ShiftsF,trivial_shifts_f>::value ? 1 : 0);
  iparam[6] = 1; // Mode 1, standard eigenproblem

  vector<dcomplex> workl(3*ncv*ncv + 5*ncv);
  vector<double> rwork(ncv);

  rci_flag ido = Init;
  do {
   f77::aupd((int&)ido, "I", N, which,
             nev, tol, resid.data_start(), ncv,
             v.data_start(), first_dim(v),
             iparam, ipntr, workd.data_start(),
             workl.data_start(), workl.size(),
             rwork.data_start(), info);
   switch(ido) {
    case ApplyOpInit:
    case ApplyOp: {
      int from_n = ipntr[0]-1;
      int to_n = ipntr[1]-1;
      a(workd(range(from_n,from_n+N)),from_n/N,workd(range(to_n,to_n+N)),to_n/N);
     }
     break;
    case Shifts: {
      int np = iparam[7];
      int shifts_re_n = ipntr[13]-1;
      int shifts_im_n = ipntr[13]-1+np;
      shifts_f(workl(range(shifts_re_n,shifts_re_n+np)), workl(range(shifts_im_n,shifts_im_n+np)));
     }
     break;
    case Done: break;
    default: TRIQS_RUNTIME_ERROR << "arpack_worker: reverse communication interface error";
   }
  } while(ido != Done);

  switch(info) {
   case 0: break;
   case 1: throw(maxiter_reached(iparam[2]));
   case 3: throw(ncv_insufficient(ncv));
   default: TRIQS_RUNTIME_ERROR << "arpack_worker: znaupd failed with error code " << info;
  }

  d.resize(nev+1);
  vector<dcomplex> workev(2*ncv);

  f77::eupd(rvec, &howmny, select.data_start(),
            d.data_start(), z.data_start(), first_dim(z),
            params.sigma, workev.data_start(),
            "I", N, which, nev, tol, resid.data_start(), ncv,
            v.data_start(), first_dim(v),
            iparam, ipntr, workd.data_start(),
            workl.data_start(), workl.size(),
            rwork.data_start(), info);

  if(info) TRIQS_RUNTIME_ERROR << "arpack_worker: zneupd failed with error code " << info;
 }

 /***********************************
 * Solve the generalized eigenproblem
 * A*x = \lambda*M*x
 ************************************/

 enum Mode : int {Invert = 2,             // OP = inv[M]*A and B = M
                  ShiftAndInvert = 3 };   // OP = inv[A - sigma*M]*M and B = M

 // op: callable taking 5 arguments
 // op(vector_view<dcomplex> from, int from_n, vector_view<dcomplex> to, int to_n, bool Bx_available)
 // 'op' is expected to act on 'from' and write the result to 'to': to = OP*from
 // 'from' is also indirectly available as this->workspace_vector(from_n)
 // 'to' is also indirectly available as this->workspace_vector(to_n)
 // 'Bx_available' indicate whether B*x has already been computed and available as this->Bx_vector()
 //
 // b: callable taking 4 arguments
 // 'b' is expected to act on 'from' and write the result to 'to': to = B*from
 // 'from' is also indirectly available as this->workspace_vector(from_n)
 // 'to' is also indirectly available as this->workspace_vector(to_n)
 //
 // shifts_f: callable taking two arguments
 // shifts_f(vector_view<dcomplex> shifts_re, vector_view<dcomplex> shifts_im)
 // 'shifts_f' is expected to place real and imaginary parts of the shifts for implicit restart
 // into 'shifts_re' and 'shifts_im' respectively.
 template<typename OP, typename B, typename ShiftsF = trivial_shifts_f>
 void operator()(OP op, B b, Mode mode, params_t const& params, ShiftsF shifts_f = {}) {

  prepare(params);

  iparam[0] = (std::is_same<ShiftsF,trivial_shifts_f>::value ? 1 : 0);
  iparam[6] = mode; // Modes 2-3, generalized eigenproblem

  vector<dcomplex> workl(3*ncv*ncv + 5*ncv);
  vector<double> rwork(ncv);

  rci_flag ido = Init;
  do {
   f77::aupd((int&)ido, "G", N, which,
             nev, tol, resid.data_start(), ncv,
             v.data_start(), first_dim(v),
             iparam, ipntr, workd.data_start(),
             workl.data_start(), workl.size(),
             rwork.data_start(), info);
   switch(ido) {
    case ApplyOpInit: {
      int from_n = ipntr[0]-1;
      int to_n = ipntr[1]-1;
      op(workd(range(from_n,from_n+N)),from_n/N,workd(range(to_n,to_n+N)),to_n/N,false);
     }
     break;
    case ApplyOp: {
      int from_n = ipntr[0]-1;
      int to_n = ipntr[1]-1;
      // B*x is available via Bx_vector()
      op(workd(range(from_n,from_n+N)),from_n/N,workd(range(to_n,to_n+N)),to_n/N,true);
     }
     break;
    case ApplyB: {
      int from_n = ipntr[0]-1;
      int to_n = ipntr[1]-1;
      b(workd(range(from_n,from_n+N)),from_n/N,workd(range(to_n,to_n+N)),to_n/N);
     }
     break;
    case Shifts: {
      int np = iparam[7];
      int shifts_re_n = ipntr[13]-1;
      int shifts_im_n = ipntr[13]-1+np;
      shifts_f(workl(range(shifts_re_n,shifts_re_n+np)), workl(range(shifts_im_n,shifts_im_n+np)));
     }
     break;
    case Done: break;
    default: TRIQS_RUNTIME_ERROR << "arpack_worker: reverse communication interface error";
   }
  } while(ido != Done);

  switch(info) {
   case 0: break;
   case 1: throw(maxiter_reached(iparam[2]));
   case 3: throw(ncv_insufficient(ncv));
   default: TRIQS_RUNTIME_ERROR << "arpack_worker: znaupd failed with error code " << info;
  }

  d.resize(nev+1);
  vector<dcomplex> workev(2*ncv);

  f77::eupd(rvec, &howmny, select.data_start(),
            d.data_start(), z.data_start(), first_dim(z),
            params.sigma, workev.data_start(),
            "G", N, which, nev, tol, resid.data_start(), ncv,
            v.data_start(), first_dim(v),
            iparam, ipntr, workd.data_start(),
            workl.data_start(), workl.size(),
            rwork.data_start(), info);

  if(info) TRIQS_RUNTIME_ERROR << "arpack_worker: zneupd failed with error code " << info;
 }

 // Get view of a workspace vector
 vector_view<dcomplex> workspace_vector(int n) const {
  if(n < 0 || n > 2)
   TRIQS_RUNTIME_ERROR << "arpack_worker: valid indices of workspace vectors are 0, 1 and 2"
                       << " (got " << n << ")";
  return workd(range(n*N,(n+1)*N));
 }

 // Access eigenvalues
 vector_view<dcomplex> eigenvalues() const { return d(range(0,nev)); }

 // Access Ritz/Schur vectors
 matrix_view<dcomplex> eigenvectors() const { return z; }

 // Access residual vector
 vector_view<dcomplex> residual_vector() const { return resid; }

 // Previously computed vector B*x
 vector_view<dcomplex> Bx_vector() const {
  int n = ipntr[2]-1;
  return workd(range(n,n+N));
 }

 struct stats_t {
  // Number of Arnoldi update iterations
  unsigned int n_iter;
  // Number of "converged" Ritz values
  unsigned int n_converged;
  // Total number of OP*x operations
  unsigned int n_op_x_operations;
  // Total number of B*x operations
  unsigned int n_b_x_operations;
  // Total number of steps of re-orthogonalization
  unsigned int n_reorth_steps;
 };

 // Return computation statistics
 stats_t stats() const {
  stats_t s;
  s.n_iter = iparam[2];
  s.n_converged = iparam[4];
  s.n_op_x_operations = iparam[8];
  s.n_b_x_operations = iparam[9];
  s.n_reorth_steps = iparam[10];
  return s;
 }

};

}}}
