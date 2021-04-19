//
// Compile with: make maxwellp
//
//               mpirun -np 4 ./maxwellp -o 2 -f 8.0 -sr 3 -m ../../data/inline-quad.mesh
//

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "ParDST/ParDST.hpp"
#include "common/PML.hpp"
#include "common/complex_coeff.hpp"

using namespace std;
using namespace mfem;
  
void source_re(const Vector &x, Vector & f);
void source_im(const Vector &x, Vector & f);
void exact_re(const Vector & x, Vector & E);
void exact_im(const Vector & x, Vector & E);
void maxwell_solution(const Vector & x, double E[], double curl2E[]);
double wavespeed(const Vector &x);
void Mwavespeed(const Vector & x, DenseMatrix & M);
double curlcoeff(const Vector &x);
void Mcurlcoeff(const Vector & x, DenseMatrix & M);

void ess_data_func(const Vector & x, Vector & E);


double mu = 1.0;
double epsilon = 1.0;
double omega;
int dim;
double length = 1.0;
double sigma_ = 0.0;

Array2D<double> comp_bdr;
Array2D<double> domain_bdr;
bool exact_known = false;

int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);
   const char *mesh_file = "../../data/inline-quad.mesh";
   int order = 1;
   // number of serial refinements
   int ser_ref_levels = 1;
   // number of parallel refinements
   int par_ref_levels = 2;
   double freq = 5.0;
   int bc_type = 1;
   bool herm_conv = true;
   bool visualization = 1;
   int nd=2;
   int nx=2;
   int ny=2;
   int nz=2;
   bool mat_masscoeff = false;
   bool mat_curlcoeff = false;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   args.AddOption(&nd, "-nd", "--dim","Problem space dimension");
   args.AddOption(&nx, "-nx", "--nx","Number of subdomains in x direction");
   args.AddOption(&ny, "-ny", "--ny","Number of subdomains in y direction");
   args.AddOption(&nz, "-nz", "--nz","Number of subdomains in z direction");               
   args.AddOption(&ser_ref_levels, "-sr", "--ser_ref_levels",
                  "Number of Serial Refinements.");
   args.AddOption(&par_ref_levels, "-pr", "--par_ref_levels",
                  "Number of Parallel Refinements."); 
   args.AddOption(&mu, "-mu", "--permeability",
                  "Permeability of free space (or 1/(spring constant)).");
   args.AddOption(&epsilon, "-eps", "--permittivity",
                  "Permittivity of free space (or mass constant).");
   args.AddOption(&mat_masscoeff, "-mat_masscoeff", "--mass-matrix-coeff", "-no-mat_masscoeff",
                  "--no-mass-matrix-coeff",
                  "Mass Matrix/scalar matrix coefficient");    
   args.AddOption(&mat_curlcoeff, "-mat_curlcoeff", "--curl-matrix-coeff", "-no-mat_curlcoeff",
                  "--no-curl-matrix-coeff",
                  "Curl Matrix/scalar matrix coefficient");                                          
   args.AddOption(&sigma_, "-sigma", "--damping-coef",
                  "Damping coefficient (or sigma).");          
   args.AddOption(&bc_type, "-bct", "--bc-type",
                  "BC type - 0:Neumann, 1: Dirichlet");                        
   args.AddOption(&freq, "-f", "--frequency",
                  "Frequency (in Hz).");
   args.AddOption(&herm_conv, "-herm", "--hermitian", "-no-herm",
                  "--no-hermitian", "Use convention for Hermitian operators.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.Parse();

   // check if the inputs are correct
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }
   // Angular frequency
   omega = 2.0 * M_PI * freq;

   Mesh *mesh;

   int nel = 1;
   if (nd == 2)
   {
      mesh = new Mesh(nel, nel, Element::QUADRILATERAL, true, length, length, false);
   }
   else
   {
      mesh = new Mesh(nel, nel, nel, Element::HEXAHEDRON, true, length, length, length,false);
   }

   dim = mesh->Dimension();
   // dim for coefficient of the curl term
   // (scalar in 2D since curl(E) is scalar)
   int cdim = (dim == 2) ? 1 : dim; 

   // 4. Refine the mesh to increase the resolution.
   for (int l = 0; l < ser_ref_levels; l++)
   {
      mesh->UniformRefinement();
   }

   // 4. Define a parallel mesh by a partitioning of the serial mesh.
   // ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   int nprocs;
   int nprocsx;
   int nprocsy;
   int nprocsz;
   if (dim == 2)
   {
      nprocs = sqrt(num_procs); 
      nprocsx = nprocs;
      nprocsy = nprocs;
      nprocsz = 1;
   }    
   else
   {
      nprocs = cbrt(num_procs); 
      nprocsx = nprocs;
      nprocsy = nprocs;
      nprocsz = nprocs;
   }
   int nxyz[3] = {nprocsx,nprocsy,nprocsz};
   int * part = mesh->CartesianPartitioning(nxyz);
   // ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD,*mesh,part);
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD,*mesh);
   delete [] part;

   
   delete mesh;

   for (int l = 0; l < par_ref_levels; l++)
   {
      pmesh->UniformRefinement();
   }

   //    char vishost[] = "localhost";
   // int visport = 19916;
   // socketstream mesh_sock1(vishost, visport);
   // mesh_sock1.precision(8);
   // mesh_sock1 << "parallel " << num_procs << " " << myid << "\n"
   //            << "mesh\n"
   //            << *pmesh << "window_title 'Global mesh'" << flush;

   double hl = GetUniformMeshElementSize(pmesh);
   int nrlayers = 4;
   Array2D<double> lengths(dim,2);
   lengths = hl*nrlayers;
   // lengths[0][1] = 0.0;
   // lengths[1][1] = 0.0;
   // lengths[1][0] = 0.0;
   // lengths[0][0] = 0.0;
   if (exact_known) lengths = 0.0;
   CartesianPML pml(pmesh,lengths);
   pml.SetAttributes(pmesh);
   pml.SetOmega(omega);
   comp_bdr.SetSize(dim,2);
   comp_bdr = pml.GetCompDomainBdr(); 

   // 6. Define a finite element space on the mesh. Here we use the Nedelec
   //    finite elements of the specified order.
   FiniteElementCollection *fec = new ND_FECollection(order, dim);
   ParFiniteElementSpace *fespace = new ParFiniteElementSpace(pmesh, fec);
   HYPRE_Int size = fespace->GlobalTrueVSize();

   if (myid == 0)
   {
      cout << "Number of finite element unknowns: " << size << endl;
   }

   // 7. Determine the list of true essential boundary dofs. In this example,
   //    the boundary conditions are defined based on the specific mesh and the
   //    problem type.
   Array<int> ess_tdof_list;
   if (pmesh->bdr_attributes.Size())
   {
      Array<int> ess_bdr(pmesh->bdr_attributes.Max());
      ess_bdr = bc_type;
      fespace->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   }

   Array<int> attr;
   Array<int> attrPML;
   if (pmesh->attributes.Size())
   {
      attr.SetSize(pmesh->attributes.Max());
      attrPML.SetSize(pmesh->attributes.Max());
      attr = 0;   attr[0] = 1;
      attrPML = 0;
      if (pmesh->attributes.Max() > 1)
      {
         attrPML[1] = 1;
      }
   }

   // 8. Setup Complex Operator convention
   ComplexOperator::Convention conv =
      herm_conv ? ComplexOperator::HERMITIAN : ComplexOperator::BLOCK_SYMMETRIC;

   // 9. Set up the linear form b(.) which corresponds to the right-hand side of
   //    the FEM linear system.
   VectorFunctionCoefficient f_re(dim, source_re);
   VectorFunctionCoefficient f_im(dim, source_re);
   ParComplexLinearForm b(fespace, conv);
   b.AddDomainIntegrator(new VectorFEDomainLFIntegrator(f_re),
                         new VectorFEDomainLFIntegrator(f_im));
   b.Vector::operator=(0.0);
   b.Assemble();

   // 10. Define the solution vector x as a complex finite element grid function
   //     corresponding to fespace.
   ParComplexGridFunction x(fespace);
   x = 0.0;
   VectorFunctionCoefficient E_re(dim,exact_re);
   VectorFunctionCoefficient E_im(dim,exact_re);
   if (exact_known)
   {
      x.ProjectCoefficient(E_re,E_re);
   }
   // 11. Set up the sesquilinear form a(.,.)
   //
   //       1/mu (1/det(J) J^T J Curl E, Curl F)
   //        - omega^2 * epsilon (det(J) * (J^T J)^-1 * E, F)
   //
   ConstantCoefficient muinv(1.0/mu);
   ConstantCoefficient omeg(-pow(omega, 2) * epsilon);
   ConstantCoefficient lossCoef(-omega * sigma_);
   RestrictedCoefficient restr_loss(lossCoef,attr);
   
   ComplexCoefficient * ws = nullptr;
   ProductComplexCoefficient * wsomeg = nullptr;
   RestrictedComplexCoefficient * restr_wsomeg = nullptr;

   MatrixComplexCoefficient * Mws = nullptr;
   ScalarMatrixProductComplexCoefficient * Mwsomeg = nullptr;
   MatrixRestrictedComplexCoefficient * restr_Mwsomeg = nullptr; 
   if (mat_masscoeff)
   {
      Mws = new MatrixComplexCoefficient(
                  new MatrixFunctionCoefficient(dim,Mwavespeed),nullptr,true,false);
      Mwsomeg = new ScalarMatrixProductComplexCoefficient(&omeg,Mws);
      restr_Mwsomeg = new MatrixRestrictedComplexCoefficient(Mwsomeg,attr);
   }
   else
   {
      ws = new ComplexCoefficient(new FunctionCoefficient(wavespeed),nullptr, true,false);
      wsomeg = new ProductComplexCoefficient(&omeg,ws);
      restr_wsomeg = new RestrictedComplexCoefficient(wsomeg,attr);
   }
   // Coefficient for the curl term 
   ComplexCoefficient * alpha = nullptr;
   ProductComplexCoefficient * amu = nullptr;
   RestrictedComplexCoefficient * restr_amu = nullptr;
   MatrixComplexCoefficient * Alpha = nullptr;
   ScalarMatrixProductComplexCoefficient * Amu = nullptr;
   MatrixRestrictedComplexCoefficient * restr_Amu = nullptr; 
   if (mat_curlcoeff)
   {
      Alpha = new MatrixComplexCoefficient(
            new MatrixFunctionCoefficient(cdim,Mcurlcoeff),nullptr,true,false);
      Amu = new ScalarMatrixProductComplexCoefficient(&muinv,Alpha);
      restr_Amu = new MatrixRestrictedComplexCoefficient(Amu,attr);
   }
   else
   {
      alpha = new ComplexCoefficient(
            new FunctionCoefficient(curlcoeff),nullptr, true,false);
      amu = new ProductComplexCoefficient(&muinv,alpha);
      restr_amu = new RestrictedComplexCoefficient(amu,attr);
   }
   
   // Integrators inside the computational domain (excluding the PML region)
   ParSesquilinearForm a(fespace, conv);
   if (mat_curlcoeff)
   {
     a.AddDomainIntegrator(new CurlCurlIntegrator(*restr_Amu->real()),NULL);
   }
   else
   {
     a.AddDomainIntegrator(new CurlCurlIntegrator(*restr_amu->real()),NULL);
   }
   if (mat_masscoeff)
   {
      a.AddDomainIntegrator(new VectorFEMassIntegrator(restr_Mwsomeg->real()),NULL);
   }
   else
   {
      a.AddDomainIntegrator(new VectorFEMassIntegrator(restr_wsomeg->real()),NULL);
   }
   a.AddDomainIntegrator(NULL, new VectorFEMassIntegrator(lossCoef));                         

   MatrixComplexCoefficient * pml_c1 = new MatrixComplexCoefficient(
      new PmlMatrixCoefficient(cdim,detJ_inv_JT_J_Re, &pml), 
      new PmlMatrixCoefficient(cdim,detJ_inv_JT_J_Im, &pml),true,true);

   MatrixComplexCoefficient * c1 = nullptr;

   if (mat_curlcoeff)
   {
      c1 = new MatrixMatrixProductComplexCoefficient(Amu,pml_c1);
   }
   else
   {
      c1 = new ScalarMatrixProductComplexCoefficient(amu,pml_c1);
   }

   MatrixRestrictedComplexCoefficient rest_c1(c1,attrPML);

   MatrixComplexCoefficient * pml_c2 = new MatrixComplexCoefficient(
      new PmlMatrixCoefficient(dim, detJ_JT_J_inv_Re,&pml), 
      new PmlMatrixCoefficient(dim, detJ_JT_J_inv_Im,&pml), true, true);

   MatrixComplexCoefficient * c2 = nullptr;
   if (mat_masscoeff)
   {
      c2 = new MatrixMatrixProductComplexCoefficient(Mwsomeg,pml_c2);
   }
   else
   {
      c2 = new ScalarMatrixProductComplexCoefficient(wsomeg,pml_c2);
   }

   MatrixRestrictedComplexCoefficient rest_c2(c2,attrPML);

   // Integrators inside the PML region
   a.AddDomainIntegrator(new CurlCurlIntegrator(*rest_c1.real()),
                         new CurlCurlIntegrator(*rest_c1.imag()));                         
   a.AddDomainIntegrator(new VectorFEMassIntegrator(*rest_c2.real()),
                         new VectorFEMassIntegrator(*rest_c2.imag()));

   a.Assemble(0);

   OperatorHandle Ah;
   Vector B, X;
   a.FormLinearSystem(ess_tdof_list, x, b, Ah, X, B);

   ComplexSparseMatrix * Ac = Ah.As<ComplexSparseMatrix>();
   StopWatch chrono;


   // chrono.Clear();
   // chrono.Start();

   ParDST::BCType bct = (bc_type == 1)? ParDST::BCType::DIRICHLET : ParDST::BCType::NEUMANN;
   ParDST * S = nullptr;
   

   S = new ParDST(&a,lengths, omega, nrlayers, 
                  (mat_curlcoeff) ? nullptr : alpha->real(), 
                  (mat_masscoeff) ? nullptr : ws->real(),
                  (mat_curlcoeff) ? Alpha->real() : nullptr,
                  (mat_masscoeff) ? Mws->real() : nullptr,
                  nx, ny, nz, bct, &lossCoef);
   
   // S = new ParDST(&a,lengths, omega, nrlayers, 
   //                (mat_curlcoeff) ? nullptr : alpha, 
   //                (mat_masscoeff) ? nullptr : ws,
   //                (mat_curlcoeff) ? Alpha : nullptr,
   //                (mat_masscoeff) ? Mws : nullptr,
   //                nx, ny, nz, bct, &lossCoef);                  
   // chrono.Stop();
   // double t1 = chrono.RealTime();

   // chrono.Clear();
   // chrono.Start();
   // X = 0.0;
	GMRESSolver gmres(MPI_COMM_WORLD);
	// gmres.iterative_mode = true;
   gmres.SetPreconditioner(*S);
	gmres.SetOperator(*Ac);
	gmres.SetRelTol(1e-8);
	gmres.SetMaxIter(20);
	gmres.SetPrintLevel(1);
	gmres.Mult(B, X);
   delete S;
   // chrono.Stop();
   // double t2 = chrono.RealTime();

   // MPI_Barrier(MPI_COMM_WORLD);


   // cout << " myid: " << myid 
   //       << ", setup time: " << t1
   //       << ", solution time: " << t2 << endl; 

   // ComplexMUMPSSolver mumps;
   // mumps.SetOperator(*Ah);
   // mumps.Mult(B,X);

   // // {
   // //    HypreParMatrix *A = Ah.As<ComplexHypreParMatrix>()->GetSystemMatrix();
   // //    SuperLURowLocMatrix SA(*A);
   // //    SuperLUSolver superlu(MPI_COMM_WORLD);
   // //    superlu.SetPrintStatistics(false);
   // //    superlu.SetSymmetricPattern(false);
   // //    superlu.SetColumnPermutation(superlu::PARMETIS);
   // //    superlu.SetOperator(SA);
   // //    superlu.Mult(B, X);
   // //    delete A;
   // // }

   a.RecoverFEMSolution(X, b, x);


   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      string keys;
      if (dim ==2 )
      {
         keys = "keys mrRljcUUuuu\n";
      }
      else
      {
         keys = "keys mc\n";
      }
      // socketstream mesh_sock(vishost, visport);
      // mesh_sock.precision(8);
      // mesh_sock << "parallel " << num_procs << " " << myid << "\n"
      //             << "mesh\n" << *pmesh  << flush;
      socketstream sol_sock_re(vishost, visport);
      sol_sock_re.precision(8);
      sol_sock_re << "parallel " << num_procs << " " << myid << "\n"
                  << "solution\n" << *pmesh << x.real() << keys 
                  << "window_title 'E: Real Part' " << flush;                     

      socketstream sol_sock_im(vishost, visport);
      sol_sock_im.precision(8);
      sol_sock_im << "parallel " << num_procs << " " << myid << "\n"
                  << "solution\n" << *pmesh << x.imag() << keys 
                  << "window_title 'E: Imag Part' " << flush;   


      {
         ParGridFunction x_t(fespace);
         x_t = x.real();

         socketstream sol_sock(vishost, visport);
         sol_sock.precision(8);
         sol_sock << "parallel " << num_procs << " " << myid << "\n"
                  << "solution\n" << *pmesh << x_t << keys << "autoscale off\n"
                  << "window_title 'Harmonic Solution (t = 0.0 T)'"
                  << "pause\n" << flush;

         if (myid == 0)
         {
            cout << "GLVis visualization paused."
                 << " Press space (in the GLVis window) to resume it.\n";
         }

         int num_frames = 32;
         int i = 0;
         while (sol_sock)
         {
            double t = (double)(i % num_frames) / num_frames;
            ostringstream oss;
            oss << "Harmonic Solution (t = " << t << " T)";

            add(cos(2.0*M_PI*t), x.real(), sin(2.0*M_PI*t), x.imag(), x_t);
            sol_sock << "parallel " << num_procs << " " << myid << "\n";
            sol_sock << "solution\n" << *pmesh << x_t
                     << "window_title '" << oss.str() << "'" << flush;
            i++;
         }
      }
   }




   // // 18. Free the used memory.
   delete c2;
   delete pml_c2;
   delete c1;      
   delete pml_c1;
   if (mat_curlcoeff)
   {
      delete restr_Amu;
      delete Alpha;
      delete Amu; 
   }
   else
   {
      delete restr_amu;
      delete alpha;
      delete amu;
   }

   if (mat_masscoeff)
   {
      delete restr_Mwsomeg;
      delete Mwsomeg;
      delete Mws;
   }
   else
   {
      delete restr_wsomeg;
      delete wsomeg;
      delete ws;
   }
   // delete c2;
   delete fespace;
   delete fec;
   delete pmesh;
   MPI_Finalize();
   return 0;
}

void source_re(const Vector &x, Vector &f)
{
   f = 0.0;
   if (exact_known)
   {
      double E[3], curl2E[3];
      maxwell_solution(x, E, curl2E);
      // curl ( curl E) +/- omega^2 E = f
      double coeff = -omega * omega;
      f(0) = curl2E[0] + coeff * E[0];
      f(1) = curl2E[1] + coeff * E[1];
      if (dim == 2)
      {
         if (x.Size() == 3) {f(2)=0.0;}
      }
      else
      {
         f(2) = curl2E[2] + coeff * E[2];
      }
   }
   else
   {
      int nrsources = (dim == 2) ? 4 : 8;
      Vector x0(nrsources);
      Vector y0(nrsources);
      Vector z0(nrsources);
      // x0(0) = 0.25; y0(0) = 0.25; z0(0) = 0.25;
      x0(0) = 0.5; y0(0) = 0.5; z0(0) = 0.25;
      x0(1) = 0.75; y0(1) = 0.25; z0(1) = 0.25;
      x0(2) = 0.25; y0(2) = 0.75; z0(2) = 0.25;
      x0(3) = 0.75; y0(3) = 0.75; z0(3) = 0.25;
      if (dim == 3)
      {
         x0(4) = 0.25; y0(4) = 0.25; z0(4) = 0.75;
         x0(5) = 0.75; y0(5) = 0.25; z0(5) = 0.75;
         x0(6) = 0.25; y0(6) = 0.75; z0(6) = 0.75;
         x0(7) = 0.75; y0(7) = 0.75; z0(7) = 0.75;
      }
  
      double n = 4.0*omega/M_PI;
      double coeff = 16.0*omega*omega/M_PI/M_PI/M_PI;

      // for (int i = 0; i<nrsources; i++)
      for (int i = 0; i<1; i++)
      {
         double beta = pow(x0(i)-x(0),2) + pow(y0(i)-x(1),2);
         if (dim == 3) { beta += pow(z0(i)-x(2),2); }
         double alpha = -pow(n,2) * beta;
         f[0] += coeff*exp(alpha);
      }

      bool in_pml = false;
      for (int i = 0; i<dim; i++)
      {
         if (x(i)<=comp_bdr(i,0) || x(i)>=comp_bdr(i,1))
         {
            in_pml = true;
            break;
         }
      }
      if (in_pml) f = 0.0;
   }
}

void source_im(const Vector &x, Vector &f)
{
   f = 0.0;
}

double wavespeed(const Vector &x)
{
   double ws;
   ws = 1.0;
   return ws;
}

void Mwavespeed(const Vector & x, DenseMatrix & M)
{
   M = 0.0;
   M(0,0) = 1.0;
   M(1,1) = 1.0;
   // M(2,2) = 4.0*x(0)-1.0;
   if (dim == 3) M(2,2) = 1.0;
}

double curlcoeff(const Vector &x)
{
   return 1.0;
}
void Mcurlcoeff(const Vector & x, DenseMatrix & M)
{
   if (dim == 2) 
   {
      M = 1.0; // in 2D this coefficient should be of dim = 1;
   }
   else
   {
      M = 0.0;
      M(0,0) = 1.0;
      M(1,1) = 1.0;
      M(2,2) = 1.0;
   }
}


void exact_re(const Vector & x, Vector & E)
{
   double curl2E[3];
   maxwell_solution(x, E, curl2E);
}
void exact_im(const Vector & x, Vector & E)
{
   // double curl2E[3];
   // maxwell_solution(x, E, curl2E);
   E = 0.0;
}
void maxwell_solution(const Vector & x, double E[], double curl2E[])
{
   // point source
   if (dim == 2)
   {
      // shift to avoid singularity
      double x0 = x(0) + 0.1;
      double x1 = x(1) + 0.1;
      //
      double r = sqrt(x0 * x0 + x1 * x1);

      E[0] = cos(omega * r);
      E[1] = 0.0;

      double r_x = x0 / r;
      double r_y = x1 / r;
      double r_xy = -(r_x / r) * r_y;
      double r_yx = r_xy;
      double r_yy = (1.0 / r) * (1.0 - r_y * r_y);

      curl2E[0] = omega * ((r_yy ) * sin(omega * r) + (omega * r_y * r_y) * cos(omega * r));
      curl2E[1] = -omega * (r_yx * sin(omega * r) + omega * r_y * r_x * cos(omega * r));
      curl2E[2] = 0.0;
   }
   else
   {
   // shift to avoid singularity
      double x0 = x(0) + 0.1;
      double x1 = x(1) + 0.1;
      double x2 = x(2) + 0.1;
      //
      double r = sqrt(x0 * x0 + x1 * x1 + x2 * x2);

      E[0] = cos(omega * r);
      E[1] = 0.0;
      E[2] = 0.0;

      double r_x = x0 / r;
      double r_y = x1 / r;
      double r_z = x2 / r;
      double r_xy = -(r_x / r) * r_y;
      double r_xz = -(r_x / r) * r_z;
      double r_yx = r_xy;
      double r_yy = (1.0 / r) * (1.0 - r_y * r_y);
      double r_zx = r_xz;
      double r_zz = (1.0 / r) * (1.0 - r_z * r_z);

      curl2E[0] = omega * ((r_yy + r_zz) * sin(omega * r) +
                           (omega * r_y * r_y + omega * r_z * r_z) * cos(omega * r));
      curl2E[1] = -omega * (r_yx * sin(omega * r) + omega * r_y * r_x * cos(omega * r));
      curl2E[2] = -omega * (r_zx * sin(omega * r) + omega * r_z * r_x * cos(omega * r));
   }
}


void ess_data_func(const Vector & x, Vector & E)
{
   E = 0.0;
   // if (x(0)==0.0) E[0] = sin(x(0)+x(1));
   if (x(1)==0.0) E[0] = sin(x(0)+x(1));


   bool in_pml = false;
   for (int i = 0; i<dim; i++)
   {
      if (x(i)<comp_bdr(i,0) || x(i)>comp_bdr(i,1))
      {
         in_pml = true;
         break;
      }
   }
   if (in_pml) E = 0.0;

}
