#include "mfem.hpp"
#include <fstream>
#include <iostream>

#include "stokes.hpp"

// example runs
// mpirun -np 4 ./stokes -m ./ball2D.msh -petscopts ./stokes_fieldsplit
// mpirun -np 4 ./stokes -m ./ball2D.msh -petscopts ./stokes_fieldsplit_01

double bpenal(const mfem::Vector &x)
{
    double nx=(x[0]-1.5)*(x[0]-1.5)+(x[1]-0.5)*(x[1]-0.5);
    if(std::sqrt(nx)<0.1){ return 1e6;}
    return 0.0;
}

double inlet_vel(const mfem::Vector &x)
{
    double d=(x[1]-1.4);
    if(fabs(d)>0.2){return 0.0;}
    return 1.5*(1-d*d/(0.2*0.2));
}


double charfunc(const mfem::Vector &x)
{
    double nx=(x[0]-1.5)*(x[0]-1.5)+(x[1]-0.5)*(x[1]-0.5);
    if(x.Size()==3){
        nx=nx+(x[2]-0.5)*(x[2]-0.5);
    }

    nx=std::sqrt(nx);
    double r=0.25;

    double rez=1.0;
    if(nx<=1.5*r){
        double a=-26.0/27.0;;
        double b=62.0/27.0;
        double c=-5.0/6.0;
        nx=nx/r;
        rez=a*nx*nx*nx+b*nx*nx+c*nx;
        if(rez<0.0){rez=0.0;}
    }
    return rez;
}

int main(int argc, char *argv[])
{
   // Initialize MPI.
   int nprocs, myrank;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myrank);



   // Parse command-line options.
   const char *mesh_file = "../../data/star.mesh";
   int order = 1;
   bool static_cond = false;
   int ser_ref_levels = 1;
   int par_ref_levels = 1;
   double rel_tol = 1e-7;
   double abs_tol = 1e-15;
   int tot_iter = 100;
   int print_level = 1;
   bool visualization = false;

   const char *petscrc_file = "stokes_fieldsplit";

   mfem::OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&ser_ref_levels,
                  "-rs",
                  "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&par_ref_levels,
                  "-rp",
                  "--refine-parallel",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree) or -1 for"
                  " isoparametric space.");
   args.AddOption(&visualization,
                  "-vis",
                  "--visualization",
                  "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&static_cond, "-sc", "--static-condensation", "-no-sc",
                  "--no-static-condensation", "Enable static condensation.");
   args.AddOption(&rel_tol,
                  "-rel",
                  "--relative-tolerance",
                  "Relative tolerance for the Newton solve.");
   args.AddOption(&abs_tol,
                  "-abs",
                  "--absolute-tolerance",
                  "Absolute tolerance for the Newton solve.");
   args.AddOption(&tot_iter,
                  "-it",
                  "--linear-iterations",
                  "Maximum iterations for the linear solve.");
   args.AddOption(&petscrc_file, "-petscopts", "--petscopts",
                                 "PetscOptions file to use.");
   args.Parse();
   if (!args.Good())
   {
      if (myrank == 0)
      {
         args.PrintUsage(std::cout);
      }
      MPI_Finalize();
      return 1;
   }

   if (myrank == 0)
   {
      args.PrintOptions(std::cout);
   }

   mfem::MFEMInitializePetsc(NULL,NULL,petscrc_file,NULL);

   // Read the (serial) mesh from the given mesh file on all processors.  We
   // can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   // and volume meshes with the same code.
   mfem::Mesh mesh(mesh_file, 1, 1);
   int dim = mesh.Dimension();

   // Refine the serial mesh on all processors to increase the resolution. In
   // this example we do 'ref_levels' of uniform refinement. We choose
   // 'ref_levels' to be the largest number that gives a final mesh with no
   // more than 10,000 elements.
   {
      int ref_levels =
         (int)floor(log(100./mesh.GetNE())/log(2.)/dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh.UniformRefinement();
      }
   }

   // Define a parallel mesh by a partitioning of the serial mesh. Refine
   // this mesh further in parallel to increase the resolution. Once the
   // parallel mesh is defined, the serial mesh can be deleted.
   mfem::ParMesh pmesh(MPI_COMM_WORLD, mesh);
   mesh.Clear();
   /*
   {
      for (int l = 0; l < par_ref_levels; l++)
      {
         pmesh.UniformRefinement();
      }
   }*/

   // Define design space
   //mfem::FiniteElementCollection* dfec=new mfem::L2_FECollection(1,dim);
   mfem::FiniteElementCollection* dfec=new mfem::H1_FECollection(1,dim);
   mfem::ParFiniteElementSpace* dfes=new mfem::ParFiniteElementSpace(&pmesh,dfec);
   mfem::ParGridFunction design; design.SetSpace(dfes); design=0.0;
   mfem::Vector tdesign(dfes->GetTrueVSize());
   //project the characteristic function onto the design space
   {
        mfem::FunctionCoefficient fco(charfunc);
        design.ProjectCoefficient(fco);
   }
   design.GetTrueDofs(tdesign);

   mfem::StokesSolver* solver=new mfem::StokesSolver(&pmesh,2);

   solver->SetSolver(rel_tol,abs_tol,tot_iter,print_level);

   mfem::ConstantCoefficient viscosity(1);
   solver->SetViscosity(viscosity);

   mfem::FunctionCoefficient vin(inlet_vel);
   solver->AddVelocityBC(2,0,vin);
   //solver->AddVelocityBC(2,0,1.0);
   solver->AddVelocityBC(1,4,0.0);

   mfem::Vector vload(2); vload(0)=0.0; vload(1)=0.0; //vload(2)=0.0;
   mfem::VectorConstantCoefficient load(vload);
   solver->SetVolForces(load);

   mfem::FunctionCoefficient brink(bpenal);
   solver->SetBrinkmanPenal(brink);

   solver->SetDesignSpace(dfes);
   solver->SetDesignParameters(0.5,2.0,0.001,200*1000);
   solver->SetTargetDesignParameters(0.5,2.0,0.001,500*1000);
   solver->SetDesign(tdesign);
   //solver->SetSolver(1e-15,1e-12,1000,1);
   solver->SetSolver(1e-12,1e-12,100,1);

   //solver->AddVelocityBC(3,4,0.0);
   //solver->AddVelocityBC(1,4,0.0);

   //mfem::ConstantCoefficient bcvelx(2.0);
   //mfem::ConstantCoefficient bcvely(1.5);
   //solver->AddVelocityBC(1,0,bcvelx);
   //solver->AddVelocityBC(2,1,bcvely);

   solver->FSolve();

   mfem::Vector gradd(dfes->GetTrueVSize()); gradd=0.0;
   mfem::ParGridFunction model_error;
   mfem::ParGridFunction discr_error;

   //test QoI


   /*
   {
       mfem::PowerDissipationTGQoI pdqoi(solver);
       double qoi=pdqoi.Eval();
       if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

       double derr=pdqoi.DiscretizationError(discr_error);
       if(myrank==0){std::cout<<"Discr. error="<<derr<<std::endl;}

       //pdqoi.Grad(gradd);
       double merr=pdqoi.ModelError(model_error);
       if(myrank==0){std::cout<<"Model error="<<merr<<std::endl;}
   }*/

   /*
   {
       mfem::AveragePressureDropQoI pdqoi(solver);
       double qoi=pdqoi.Eval();
       if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

       double derr=pdqoi.DiscretizationError(discr_error);
       if(myrank==0){std::cout<<"Discr. error="<<derr<<std::endl;}

       //pdqoi.Grad(gradd);
       double merr=pdqoi.ModelError(model_error);
       if(myrank==0){std::cout<<"Model error="<<merr<<std::endl;}
   }
   */

   {
       mfem::VelocityIntQoI pdqoi(solver,3);
       double qoi=pdqoi.Eval();
       if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

       double derr=pdqoi.DiscretizationError(discr_error);
       if(myrank==0){std::cout<<"Discr. error="<<derr<<std::endl;}

       //pdqoi.Grad(gradd);
       double merr=pdqoi.ModelError(model_error);
       if(myrank==0){std::cout<<"Model error="<<merr<<std::endl;}
   }



   /*
   {
       mfem::PowerDissipationQoI pdqoi(solver);
       double qoi=pdqoi.Eval();

       pdqoi.Grad(gradd);
       double merr=pdqoi.ModelError(model_error);
       std::cout<<"merr="<<merr<<std::endl;

       mfem::AveragePressureDropQoI bdqoi(solver);
       double boi=bdqoi.Eval();
       std::cout<<"vol qoi="<<qoi<<" bdr qoi="<<boi<<std::endl;

       bdqoi.Grad(gradd);
       merr=bdqoi.ModelError(model_error);
       std::cout<<"merr="<<merr<<std::endl;

   }
   */

   //test velocity QoI
   /*
   {
       mfem::VelocityIntQoI* veqoi=new mfem::VelocityIntQoI(solver,3);
       double qoi=veqoi->Eval();
       if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}
       veqoi->Grad(gradd);
       delete veqoi;
   }
   */

   {
       mfem::ParGridFunction& veloc=solver->GetVelocity();
       mfem::ParGridFunction& press=solver->GetPressure();
       mfem::ParGridFunction& aveloc=solver->GetAVelocity();
       mfem::ParGridFunction& apress=solver->GetAPressure();

       mfem::ParGridFunction pdesign; pdesign.SetSpace(dfes);
       pdesign.ProjectCoefficient(*(solver->GetBrinkmanPenal()));

       mfem::ParGridFunction desgrad; desgrad.SetSpace(dfes);
       desgrad.SetFromTrueDofs(gradd);

       mfem::ParaViewDataCollection paraview_dc("Stokes", &pmesh);
       paraview_dc.SetPrefixPath("ParaView");
       paraview_dc.SetLevelsOfDetail(order);
       paraview_dc.SetDataFormat(mfem::VTKFormat::BINARY);
       paraview_dc.SetHighOrderOutput(true);
       paraview_dc.SetCycle(0);
       paraview_dc.SetTime(0.0);
       paraview_dc.RegisterField("velocity",&veloc);
       paraview_dc.RegisterField("pressure",&press);
       paraview_dc.RegisterField("idesign",&design);
       paraview_dc.RegisterField("pdesign",&pdesign);
       paraview_dc.RegisterField("grads",&desgrad);
       paraview_dc.RegisterField("aveloc",&aveloc);
       paraview_dc.RegisterField("apress",&apress);
       //paraview_dc.RegisterField("merr",&model_error);
       //paraview_dc.RegisterField("derr",&discr_error);
       paraview_dc.Save();
   }

   /*
   {
        mfem::BlockVector& sol=solver->GetSol();
        //mfem::VolumeQoI pdqoi(solver,0.5);
        mfem::AveragePressureDropQoI pdqoi(solver);
        mfem::Vector grad(dfes->GetTrueVSize());
        double qoi=pdqoi.Eval();
        if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

        pdqoi.Grad(gradd);

        mfem::Vector prtv;
        mfem::Vector tmpv;

        prtv.SetSize(gradd.Size());
        tmpv.SetSize(gradd.Size());

        prtv.Randomize();

        double nd=mfem::InnerProduct(pmesh.GetComm(),prtv,prtv);
        double td=mfem::InnerProduct(pmesh.GetComm(),prtv,gradd);

        td=td/nd;
        double lsc=1.0;
        double lqoi;

        for(int l=0;l<10;l++){
            lsc/=10.0;
            prtv/=10.0;
            add(prtv,tdesign,tmpv);
            solver->SetDesign(tmpv);
            solver->FSolve();
            lqoi=pdqoi.Eval();
            double ld=(lqoi-qoi)/lsc;
            if(myrank==0){
                std::cout << "dx=" << lsc <<" FD approximation=" << ld/nd
                          << " adjoint gradient=" << td
                          << " err=" << std::fabs(ld/nd-td) << std::endl;
            }
        }

   }
   */


   /*
   {
        mfem::BlockVector& sol=solver->GetSol();
        mfem::VelocityIntQoI pdqoi(solver,3);
        mfem::Vector grad(dfes->GetTrueVSize());
        double qoi=pdqoi.Eval();
        if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

        pdqoi.Grad(gradd);

        mfem::Vector prtv;
        mfem::Vector tmpv;

        prtv.SetSize(gradd.Size());
        tmpv.SetSize(gradd.Size());

        prtv.Randomize();

        double nd=mfem::InnerProduct(pmesh.GetComm(),prtv,prtv);
        double td=mfem::InnerProduct(pmesh.GetComm(),prtv,gradd);

        td=td/nd;
        double lsc=1.0;
        double lqoi;

        for(int l=0;l<10;l++){
            lsc/=10.0;
            prtv/=10.0;
            add(prtv,tdesign,tmpv);
            solver->SetDesign(tmpv);
            solver->FSolve();
            lqoi=pdqoi.Eval();
            double ld=(lqoi-qoi)/lsc;
            if(myrank==0){
                std::cout << "dx=" << lsc <<" FD approximation=" << ld/nd
                          << " adjoint gradient=" << td
                          << " err=" << std::fabs(ld/nd-td) << std::endl;
            }
        }

   }
   */


   /*
   {
           mfem::BlockVector& sol=solver->GetSol();
           mfem::PowerDissipationTGQoI pdqoi(solver);

           mfem::Vector grad(dfes->GetTrueVSize());

           double qoi=pdqoi.Eval();
           if(myrank==0){std::cout<<"QoI="<<qoi<<std::endl;}

           pdqoi.Grad(gradd);

           mfem::Vector prtv;
           mfem::Vector tmpv;

           prtv.SetSize(gradd.Size());
           tmpv.SetSize(gradd.Size());

           prtv.Randomize();

           double nd=mfem::InnerProduct(pmesh.GetComm(),prtv,prtv);
           double td=mfem::InnerProduct(pmesh.GetComm(),prtv,gradd);

           td=td/nd;
           double lsc=1.0;
           double lqoi;

           for(int l=0;l<10;l++){
               lsc/=10.0;
               prtv/=10.0;
               add(prtv,tdesign,tmpv);
               solver->SetDesign(tmpv);
               solver->FSolve();
               lqoi=pdqoi.Eval();
               double ld=(lqoi-qoi)/lsc;
               if(myrank==0){
                   std::cout << "dx=" << lsc <<" FD approximation=" << ld/nd
                             << " adjoint gradient=" << td
                             << " err=" << std::fabs(ld/nd-td) << std::endl;
               }
           }

   }
   */


   delete solver;
   delete dfes;
   delete dfec;

   mfem::MFEMFinalizePetsc();
   MPI_Finalize();
   return 0;
}

