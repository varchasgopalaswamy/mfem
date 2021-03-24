
// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "../general/forall.hpp"
#include "bilininteg.hpp"
#include "gridfunc.hpp"
#include "restriction.hpp"

/* 
   Integrator for the DG form:

    - < {(Q grad(u)).n}, [v] > + sigma < [u], {(Q grad(v)).n} >  + kappa < {h^{-1} Q} [u], [v] >

*/

namespace mfem
{

// PA DG DGDiffusion Integrator
static void PADGDiffusionSetup2D(const int Q1D,
                             const int D1D,
                             const int NF,
                             const Array<double> &weights,
                             const Array<double> &g,
                             const Array<double> &b,
                             const Vector &jac,
                             const Vector &det_jac,
                             const Vector &nor,
                             const Vector &Q,
                             const Vector &rho,
                             const Vector &vel,
                             const Vector &face_2_elem_volumes,
                             const double sigma,
                             const double kappa,
                             Vector &op1,
                             Vector &op2,
                             Vector &op3)
{
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto B = Reshape(b.Read(), Q1D, D1D);
   const int VDIM = 2; // why ? 
   auto detJ = Reshape(det_jac.Read(), Q1D, NF); // assumes conforming mesh
   auto norm = Reshape(nor.Read(), Q1D, VDIM, NF);
   auto f2ev = Reshape(face_2_elem_volumes.Read(), 2, NF);

   // Input
   const bool const_Q = Q.Size() == 1;
   auto Qreshaped =
      const_Q ? Reshape(Q.Read(), 1,1) : Reshape(Q.Read(), Q1D,NF);
   auto wgts = weights.Read();

   // Output
   auto op_data_ptr1 = Reshape(op1.Write(), Q1D, 2, 2, NF);
   auto op_data_ptr2 = Reshape(op2.Write(), Q1D, 2, NF);
   auto op_data_ptr3 = Reshape(op3.Write(), Q1D, 2, NF);

   MFEM_FORALL(f, NF, // can be optimized with Q1D thread for NF blocks
   {
      for (int q = 0; q < Q1D; ++q)
      {
         const double normx = norm(q,0,f);
         const double normy = norm(q,1,f);
         //const double normz = norm(q,0,f) + norm(q,1,f); //todo: fix for 
         const double mag_norm = sqrt(normx*normx + normy*normy); //todo: fix for 
         const double Q = const_Q ? Qreshaped(0,0) : Qreshaped(q,f);
         const double w = wgts[q]*Q*detJ(q,f);
         // Need to correct the scaling of w to account for d/dn, etc..
         double w_o_detJ = w/detJ(q,f);
         if( f2ev(1,f) == -1.0 )
         {
            // Boundary face
            // data for 1st term    - < {(Q grad(u)).n}, [v] >
            op_data_ptr1(q,0,0,f) = w_o_detJ;
            op_data_ptr1(q,1,0,f) = -w_o_detJ;
            op_data_ptr1(q,0,1,f) = w_o_detJ;
            op_data_ptr1(q,1,1,f) = -w_o_detJ;
            // data for 2nd term    + sigma < [u], {(Q grad(v)).n} > 
            op_data_ptr2(q,0,f) =   w_o_detJ*sigma;
            op_data_ptr2(q,1,f) =   0.0*w_o_detJ*sigma;
            // data for 3rd term    + kappa < {h^{-1} Q} [u], [v] >
            const double h0 = detJ(q,f)/mag_norm;
            const double h1 = detJ(q,f)/mag_norm;
            op_data_ptr3(q,0,f) =   w*kappa/h0;
            op_data_ptr3(q,1,f) =  -w*kappa/h0;
         }
         else
         {
            // Interior face
            // data for 1st term    - < {(Q grad(u)).n}, [v] >
            op_data_ptr1(q,0,0,f) = w_o_detJ/2.0;
            op_data_ptr1(q,1,0,f) = -w_o_detJ/2.0;
            op_data_ptr1(q,0,1,f) = w_o_detJ/2.0;
            op_data_ptr1(q,1,1,f) = -w_o_detJ/2.0;;
            // data for 2nd term    + sigma < [u], {(Q grad(v)).n} > 
            op_data_ptr2(q,0,f) =   w_o_detJ*sigma/2.0;
            op_data_ptr2(q,1,f) =   w_o_detJ*sigma/2.0;
            // data for 3rd term    + kappa < {h^{-1} Q} [u], [v] >
            const double h0 = detJ(q,f)/mag_norm;
            const double h1 = detJ(q,f)/mag_norm;
            op_data_ptr3(q,0,f) =   -w*kappa*(1.0/h0+1.0/h1)/2.0;
            op_data_ptr3(q,1,f) =   w*kappa*(1.0/h0+1.0/h1)/2.0;
         }

      }
   });
}

static void PADGDiffusionSetup3D(const int Q1D,
                             const int D1D,
                             const int NF,
                             const Array<double> &weights,
                             const Array<double> &g,
                             const Array<double> &b,
                             const Vector &jac,
                             const Vector &det_jac,
                             const Vector &nor,
                             const Vector &Q,
                             const Vector &rho,
                             const Vector &vel,
                             const Vector &face_2_elem_volumes,
                             const double sigma,
                             const double kappa,
                             Vector &op1,
                             Vector &op2,
                             Vector &op3)
{
   mfem_error("not yet implemented.");
}

static void PADGDiffusionSetup(const int dim,
                           const int D1D,
                           const int Q1D,
                           const int NF,
                           const Array<double> &weights,
                           const Array<double> &g,
                           const Array<double> &b,
                           const Vector &jac,
                           const Vector &det_jac,
                           const Vector &nor,
                           const Vector &Q,
                           const Vector &rho,
                           const Vector &u,
                           const Vector &face_2_elem_volumes,
                           const double sigma,
                           const double kappa,
                           Vector &op1,
                           Vector &op2,
                           Vector &op3)
{

   if (dim == 1) { MFEM_ABORT("dim==1 not supported in PADGDiffusionSetup"); }
   else if (dim == 2)
   {
      PADGDiffusionSetup2D(Q1D, D1D, NF, weights, g, b, jac, det_jac, nor, Q, rho, u, face_2_elem_volumes, sigma, kappa, op1, op2, op3);
   }
   else if (dim == 3)
   {
      PADGDiffusionSetup3D(Q1D, D1D, NF, weights, g, b, jac, det_jac, nor, Q, rho, u, face_2_elem_volumes, sigma, kappa, op1, op2, op3);
   }
   else
   {
      MFEM_ABORT("dim > 3 not supported in PADGDiffusionSetup");     
   }
}

void DGDiffusionIntegrator::SetupPA(const FiniteElementSpace &fes, FaceType type)
{
   nf = fes.GetNFbyType(type);
   if (nf==0) { return; }
   // Assumes tensor-product elements
   Mesh *mesh = fes.GetMesh();
   const FiniteElement &el =
      *fes.GetTraceElement(0, fes.GetMesh()->GetFaceBaseGeometry(0));
   FaceElementTransformations &T =
      *fes.GetMesh()->GetFaceElementTransformations(0);
   const IntegrationRule *ir = IntRule?
                               IntRule:
                               &GetRule(el.GetGeomType(), el.GetOrder(), T);
   const int nq = ir->GetNPoints();
   dim = mesh->Dimension();
   facegeom = mesh->GetFaceGeometricFactors(
                  *ir,
                  FaceGeometricFactors::DETERMINANTS |
                  FaceGeometricFactors::NORMALS, type);
   maps = &el.GetDofToQuad(*ir, DofToQuad::TENSOR);
   dofs1D = maps->ndof;
   quad1D = maps->nqpt;

   // Grad Rule   
   const IntegrationRule *ir_grad = &GetRuleGrad(el.GetGeomType(), el.GetOrder());
   maps_grad = &el.GetDofToQuad(*ir_grad, DofToQuad::TENSOR);

   auto Gf = Reshape(maps->G.Read(), dofs1D, dofs1D);
   auto G = Reshape(maps_grad->G.Read(), dofs1D, dofs1D);
   auto Bf = Reshape(maps->B.Read(), dofs1D, dofs1D);
   auto B = Reshape(maps_grad->B.Read(), dofs1D, dofs1D);
   
   // are these the right things for our data?
   coeff_data_1.SetSize( 4 * nq * nf, Device::GetMemoryType());
   coeff_data_2.SetSize( 2 * nq * nf, Device::GetMemoryType());
   coeff_data_3.SetSize( 2 * nq * nf, Device::GetMemoryType());

   face_2_elem_volumes.SetSize( 2 * nf, Device::GetMemoryType());

   // Get element sizes on either side of each face
   auto f2ev = Reshape(face_2_elem_volumes.ReadWrite(), 2, nf);

   int f_ind = 0;
   // Loop over all faces
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      int e0,e1;
      int inf0, inf1;
      // Get the two elements associated with the current face
      fes.GetMesh()->GetFaceElements(f, &e0, &e1);
      fes.GetMesh()->GetFaceInfos(f, &inf0, &inf1);
      //int face_id = inf0 / 64; //I don't know what 64 is all about 
      // Act if type matches the kind of face f is
      bool int_type_match = (type==FaceType::Interior && (e1>=0 || (e1<0 && inf1>=0)));
      bool bdy_type_match = (type==FaceType::Boundary && e1<0 && inf1<0);
      if ( int_type_match )
      {
         mesh->GetFaceElements(f, &e0, &e1);
         f2ev(0,f_ind) = mesh->GetElementVolume(e0);
         f2ev(1,f_ind) = mesh->GetElementVolume(e1);
         f_ind++;
      }
      else if ( bdy_type_match )
      {
         mesh->GetFaceElements(f, &e0, &e1);
         f2ev(0,f_ind) = mesh->GetElementVolume(e0);
         f2ev(1,f_ind) = -1.0; // Not a real element
         f_ind++;
      } 
   }

   MFEM_VERIFY(f_ind==nf, "Incorrect number of faces.");
   // convert Q to a vector
   Vector Qcoeff;
   if (Q==nullptr)
   {
      // Default value
      Qcoeff.SetSize(1);
      Qcoeff(0) = 1.0;
   }
   else if (ConstantCoefficient *c_Q = dynamic_cast<ConstantCoefficient*>(Q))
   {
      mfem_error("not yet implemented.");
      // Constant Coefficient
      Qcoeff.SetSize(1);
      Qcoeff(0) = c_Q->constant;
   }
   else if (QuadratureFunctionCoefficient* c_Q =
               dynamic_cast<QuadratureFunctionCoefficient*>(Q))
   {
      mfem_error("not yet implemented.");
   }
   else
   {
      mfem_error("not yet implemented.");
      //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
      exit(1);
      // ???????
      /*
      r.SetSize(nq * nf);
      auto C_vel = Reshape(vel.HostRead(), dim, nq, nf);
      auto n = Reshape(facegeom->normal.HostRead(), nq, dim, nf);
      auto C = Reshape(r.HostWrite(), nq, nf);
      int f_ind = 0;
      for (int f = 0; f < fes.GetNF(); ++f)
      {
         int e1, e2;
         int inf1, inf2;
         fes.GetMesh()->GetFaceElements(f, &e1, &e2);
         fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
         int face_id = inf1 / 64;
         if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
             (type==FaceType::Boundary && e2<0 && inf2<0) )
         {
            FaceElementTransformations &T =
               *fes.GetMesh()->GetFaceElementTransformations(f);
            for (int q = 0; q < nq; ++q)
            {
               // Convert to lexicographic ordering
               int iq = ToLexOrdering(dim, face_id, quad1D, q);

               T.SetAllIntPoints(&ir->IntPoint(q));
               const IntegrationPoint &eip1 = T.GetElement1IntPoint();
               const IntegrationPoint &eip2 = T.GetElement2IntPoint();
               double r;

               if (inf2 < 0)
               {
                  r = rho->Eval(*T.Elem1, eip1);
               }
               else
               {
                  double udotn = 0.0;
                  for (int d=0; d<dim; ++d)
                  {
                     udotn += C_vel(d,iq,f_ind)*n(iq,d,f_ind);
                  }
                  if (udotn >= 0.0) { r = rho->Eval(*T.Elem2, eip2); }
                  else { r = rho->Eval(*T.Elem1, eip1); }
               }
               C(iq,f_ind) = r;
            }
            f_ind++;
         }
      }
      MFEM_VERIFY(f_ind==nf, "Incorrect number of faces.");
      */
   }





   // 
   /*        
   if (VectorConstantCoefficient *c_u = dynamic_cast<VectorConstantCoefficient*>
                                        (u))
   {
      vel = c_u->GetVec();
   }
   else if (VectorQuadratureFunctionCoefficient* c_u =
               dynamic_cast<VectorQuadratureFunctionCoefficient*>(u))
   {
      // Assumed to be in lexicographical ordering
      const QuadratureFunction &qFun = c_u->GetQuadFunction();
      MFEM_VERIFY(qFun.Size() == dim * nq * nf,
                  "Incompatible QuadratureFunction dimension \n");

      MFEM_VERIFY(ir == &qFun.GetSpace()->GetElementIntRule(0),
                  "IntegrationRule used within integrator and in"
                  " QuadratureFunction appear to be different");
      qFun.Read();
      vel.MakeRef(const_cast<QuadratureFunction &>(qFun),0);
   }
   else
   {
      vel.SetSize(dim * nq * nf);
      auto C = Reshape(vel.HostWrite(), dim, nq, nf);
      Vector Vq(dim);
      int f_ind = 0;
      for (int f = 0; f < fes.GetNF(); ++f)
      {
         int e1, e2;
         int inf1, inf2;
         fes.GetMesh()->GetFaceElements(f, &e1, &e2);
         fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
         int face_id = inf1 / 64;
         if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
             (type==FaceType::Boundary && e2<0 && inf2<0) )
         {
            FaceElementTransformations &T =
               *fes.GetMesh()->GetFaceElementTransformations(f);
            for (int q = 0; q < nq; ++q)
            {
               // Convert to lexicographic ordering
               int iq = ToLexOrdering(dim, face_id, quad1D, q);
               T.SetAllIntPoints(&ir->IntPoint(q));
               const IntegrationPoint &eip1 = T.GetElement1IntPoint();
               u->Eval(Vq, *T.Elem1, eip1);
               for (int i = 0; i < dim; ++i)
               {
                  C(i,iq,f_ind) = Vq(i);
               }
            }
            f_ind++;
         }
      }
      MFEM_VERIFY(f_ind==nf, "Incorrect number of faces.");
   }
   */
   PADGDiffusionSetup(dim, dofs1D, quad1D, nf, ir->GetWeights(), 
                        maps->G, maps->B,
                        facegeom->J, 
                        facegeom->detJ, 
                        facegeom->normal,
                        Qcoeff, r, vel,
                        face_2_elem_volumes,
                        sigma, kappa,
                        coeff_data_1,coeff_data_2,coeff_data_3);
}

void DGDiffusionIntegrator::AssemblePAInteriorFaces(const FiniteElementSpace& fes)
{
   SetupPA(fes, FaceType::Interior);
}

void DGDiffusionIntegrator::AssemblePABoundaryFaces(const FiniteElementSpace& fes)
{
   
   SetupPA(fes, FaceType::Boundary);
}

// PA DGDiffusion Apply 2D kernel for Gauss-Lobatto/Bernstein
template<int T_D1D = 0, int T_Q1D = 0> static
void PADGDiffusionApply2D(const int NF,
                      const Array<double> &b,
                      const Array<double> &bt,
                      const Array<double> &g,
                      const Array<double> &gt,
                      const Vector &_op1,
                      const Vector &_op2,
                      const Vector &_op3,
                      const Vector &_x,
                      Vector &_y,
                      const int d1d = 0,
                      const int q1d = 0)
{
   // vdim is confusing as it seems to be used differently based on the context
   const int VDIM = 1;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;

   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   // the following variables are evaluated at compile time
   constexpr int max_D1D = T_D1D ? T_D1D : MAX_D1D;
   constexpr int max_Q1D = T_Q1D ? T_Q1D : MAX_Q1D;

   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto G = Reshape(g.Read(), Q1D, D1D);
   auto Gt = Reshape(gt.Read(), D1D, Q1D);
   auto op1 = Reshape(_op1.Read(), Q1D, 2, 2, NF);
   auto op2 = Reshape(_op2.Read(), Q1D, 2, NF);
   auto op3 = Reshape(_op3.Read(), Q1D, 2, NF);
   auto x = Reshape(_x.Read(), D1D, D1D, VDIM, 2, NF);
   auto y = Reshape(_y.ReadWrite(), D1D, D1D, VDIM, 2, NF);

   // Loop over all faces
   MFEM_FORALL(f, NF,
   {
      // 1. Evaluation of solution and normal derivative on the faces
      double u0[max_D1D][VDIM] = {0};
      double u1[max_D1D][VDIM] = {0};
      double Gu0[max_D1D][VDIM] = {0};
      double Gu1[max_D1D][VDIM] = {0};
      for (int d = 0; d < D1D; d++)
      {
         for (int c = 0; c < VDIM; c++)
         {
            // Evaluate u on the face from each side
            u0[d][c] = x(0,d,c,0,f);
            u1[d][c] = x(0,d,c,1,f);
            for (int q = 0; q < D1D; q++)
            {  
               // Evaluate du/dn on the face from each side
               // Uses a stencil inside 
               Gu0[d][c] += g*x(q,d,c,0,f);
               Gu1[d][c] += g*x(q,d,c,1,f);
            }
         }
      }
   
      // 2. Contraction with basis evaluation Bu = B:u, and Gu = G:u    
      double Bu0[max_Q1D][VDIM] = {0};
      double Bu1[max_Q1D][VDIM] = {0};      

      for (int q = 0; q < Q1D; ++q)
      {
         for (int d = 0; d < D1D; ++d)
         {
            const double b = B(q,d);
            for (int c = 0; c < VDIM; c++)
            {
               Bu0[q][c] += b*u0[d][c];
               Bu1[q][c] += b*u1[d][c];
               BGu0[q][c] += b*Gu0[d][c];
               BGu1[q][c] += b*Gu1[d][c];
            }
         }
      }

      // 3. Form numerical fluxes
      double D1[max_Q1D][VDIM] = {0};
      double D0[max_Q1D][VDIM] = {0};
      double D1jumpu[max_Q1D][VDIM] = {0};
      double D0jumpu[max_Q1D][VDIM] = {0};
      for (int q = 0; q < Q1D; ++q)
      {
         for (int c = 0; c < VDIM; c++)
         {
            const double jump_u = Bu0[q][c] - Bu1[q][c];
            // numerical fluxes
            D1[q][c] = op1(q,1,0,f)*Gu0[q][c] 
                        + op1(q,1,1,f)*Gu1[q][c]
                        + op3(q,0,f)*jump_u; 
            D0[q][c] = op1(q,0,0,f)*Gu0[q][c] 
                        + op1(q,0,1,f)*Gu1[q][c] 
                        + op3(q,1,f)*jump_u; 
            D1jumpu[q][c] = op2(q,1,f)*jump_u;
            D0jumpu[q][c] = op2(q,0,f)*jump_u;
         }
      }

      // 4. Contraction with B^T evaluation B^T:(G*D*B:u) and B^T:(D*B:Gu)   
      double BD1[max_D1D][VDIM] = {0};
      double BD0[max_D1D][VDIM] = {0};
      for (int d = 0; d < D1D; ++d)
      {
         for (int q = 0; q < Q1D; ++q)
         {
            const double b = Bt(d,q);
            const double g = Gt(d,0);
            // this needs a negative based on the normal
            for (int c = 0; c < VDIM; c++)
            {
               BD0[d][c] += b*D0[q][c] + b*g*D0[q][c];
               BD1[d][c] += b*D1[q][c] + b*g*D1[q][c];
            }
         }
         for (int c = 0; c < VDIM; c++)
         {
            y(d,c,0,f) +=  BD0[d][c];
            y(d,c,1,f) +=  BD1[d][c];
         }
      }

      // done with the loop over all faces
   });
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
}

// PA DGDiffusion Apply 3D kernel for Gauss-Lobatto/Bernstein
template<int T_D1D = 0, int T_Q1D = 0> static
void PADGDiffusionApply3D(const int NF,
                      const Array<double> &b,
                      const Array<double> &bt,
                      const Array<double> &g,
                      const Array<double> &gt,
                      const Vector &_op1,
                      const Vector &_op2,
                      const Vector &_op3,
                      const Vector &_x,
                      Vector &_y,
                      const int d1d = 0,
                      const int q1d = 0)
{
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   std::cout << "TODO: Correct this for DG diffusion" << std::endl;
   exit(1);
   /*
   const int VDIM = 1;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto Bt = Reshape(bt.Read(), D1D, Q1D);
   auto op1 = Reshape(_op1.Read(), Q1D, Q1D, 2, 2, NF);
   auto op2 = Reshape(_op2.Read(), Q1D, Q1D, 2, 2, NF);
   auto op3 = Reshape(_op3.Read(), Q1D, Q1D, 2, 2, NF);
   auto x = Reshape(_x.Read(), D1D, D1D, VDIM, 2, NF);
   auto y = Reshape(_y.ReadWrite(), D1D, D1D, VDIM, 2, NF);

   // Loop over all faces
   MFEM_FORALL(f, NF,
   {
      const int VDIM = 1;
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : MAX_Q1D;
      double u0[max_D1D][max_D1D][VDIM];
      double u1[max_D1D][max_D1D][VDIM];
      for (int d1 = 0; d1 < D1D; d1++)
      {
         for (int d2 = 0; d2 < D1D; d2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               u0[d1][d2][c] = x(d1,d2,c,0,f);
               u1[d1][d2][c] = x(d1,d2,c,1,f);
            }
         }
      }
      double Bu0[max_Q1D][max_D1D][VDIM];
      double Bu1[max_Q1D][max_D1D][VDIM];
      for (int q = 0; q < Q1D; ++q)
      {
         for (int d2 = 0; d2 < D1D; d2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               Bu0[q][d2][c] = 0.0;
               Bu1[q][d2][c] = 0.0;
            }
            for (int d1 = 0; d1 < D1D; ++d1)
            {
               const double b = B(q,d1);
               for (int c = 0; c < VDIM; c++)
               {
                  Bu0[q][d2][c] += b*u0[d1][d2][c];
                  Bu1[q][d2][c] += b*u1[d1][d2][c];
               }
            }
         }
      }
      double BBu0[max_Q1D][max_Q1D][VDIM];
      double BBu1[max_Q1D][max_Q1D][VDIM];
      for (int q1 = 0; q1 < Q1D; ++q1)
      {
         for (int q2 = 0; q2 < Q1D; q2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               BBu0[q1][q2][c] = 0.0;
               BBu1[q1][q2][c] = 0.0;
            }
            for (int d2 = 0; d2 < D1D; ++d2)
            {
               const double b = B(q2,d2);
               for (int c = 0; c < VDIM; c++)
               {
                  BBu0[q1][q2][c] += b*Bu0[q1][d2][c];
                  BBu1[q1][q2][c] += b*Bu1[q1][d2][c];
               }
            }
         }
      }
      double DBBu[max_Q1D][max_Q1D][VDIM];
      for (int q1 = 0; q1 < Q1D; ++q1)
      {
         for (int q2 = 0; q2 < Q1D; q2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               DBBu[q1][q2][c] = op(q1,q2,0,0,f)*BBu0[q1][q2][c] +
                                 op(q1,q2,1,0,f)*BBu1[q1][q2][c];
            }
         }
      }
      double BDBBu[max_Q1D][max_D1D][VDIM];
      for (int q1 = 0; q1 < Q1D; ++q1)
      {
         for (int d2 = 0; d2 < D1D; d2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               BDBBu[q1][d2][c] = 0.0;
            }
            for (int q2 = 0; q2 < Q1D; ++q2)
            {
               const double b = Bt(d2,q2);
               for (int c = 0; c < VDIM; c++)
               {
                  BDBBu[q1][d2][c] += b*DBBu[q1][q2][c];
               }
            }
         }
      }
      double BBDBBu[max_D1D][max_D1D][VDIM];
      for (int d1 = 0; d1 < D1D; ++d1)
      {
         for (int d2 = 0; d2 < D1D; d2++)
         {
            for (int c = 0; c < VDIM; c++)
            {
               BBDBBu[d1][d2][c] = 0.0;
            }
            for (int q1 = 0; q1 < Q1D; ++q1)
            {
               const double b = Bt(d1,q1);
               for (int c = 0; c < VDIM; c++)
               {
                  BBDBBu[d1][d2][c] += b*BDBBu[q1][d2][c];
               }
            }
            for (int c = 0; c < VDIM; c++)
            {
               y(d1,d2,c,0,f) +=  BBDBBu[d1][d2][c];
               y(d1,d2,c,1,f) += -BBDBBu[d1][d2][c];
            }
         }
      }
   });
   */
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
}

static void PADGDiffusionApply(const int dim,
                           const int D1D,
                           const int Q1D,
                           const int NF,
                           const Array<double> &B,
                           const Array<double> &Bt,
                           const Array<double> &G,
                           const Array<double> &Gt,
                           const Vector &_op1,
                           const Vector &_op2,
                           const Vector &_op3,   
                           const Vector &x,
                           Vector &y)
{
   if (dim == 2)
   {
      switch ((D1D << 4 ) | Q1D)
      {  
         /*
         case 0x22: return PADGDiffusionApply2D<2,2>(NF,B,Bt,op,x,y);
         case 0x33: return PADGDiffusionApply2D<3,3>(NF,B,Bt,op,x,y);
         case 0x44: return PADGDiffusionApply2D<4,4>(NF,B,Bt,op,x,y);
         case 0x55: return PADGDiffusionApply2D<5,5>(NF,B,Bt,op,x,y);
         case 0x66: return PADGDiffusionApply2D<6,6>(NF,B,Bt,op,x,y);
         case 0x77: return PADGDiffusionApply2D<7,7>(NF,B,Bt,op,x,y);
         case 0x88: return PADGDiffusionApply2D<8,8>(NF,B,Bt,op,x,y);
         case 0x99: return PADGDiffusionApply2D<9,9>(NF,B,Bt,op,x,y);
         */
         default:   return PADGDiffusionApply2D(NF,B,Bt,G,Gt,_op1,_op2,_op3,x,y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         /*
         case 0x23: return SmemPADGDiffusionApply3D<2,3,1>(NF,B,Bt,op,x,y);
         case 0x34: return SmemPADGDiffusionApply3D<3,4,2>(NF,B,Bt,op,x,y);
         case 0x45: return SmemPADGDiffusionApply3D<4,5,2>(NF,B,Bt,op,x,y);
         case 0x56: return SmemPADGDiffusionApply3D<5,6,1>(NF,B,Bt,op,x,y);
         case 0x67: return SmemPADGDiffusionApply3D<6,7,1>(NF,B,Bt,op,x,y);
         case 0x78: return SmemPADGDiffusionApply3D<7,8,1>(NF,B,Bt,op,x,y);
         case 0x89: return SmemPADGDiffusionApply3D<8,9,1>(NF,B,Bt,op,x,y);
         */
         default:   return PADGDiffusionApply3D(NF,B,Bt,G,Gt,_op1,_op2,_op3,x,y,D1D,Q1D);
      }
   }
   MFEM_ABORT("PADGDiffusionApply not implemented for dim.");
}

/*
static void PADGDiffusionApplyTranspose(const int dim,
                                    const int D1D,
                                    const int Q1D,
                                    const int NF,
                                    const Array<double> &B,
                                    const Array<double> &Bt,
                                    const Array<double> &G,
                                    const Array<double> &Gt,
                                    const Vector &op,
                                    const Vector &x,
                                    Vector &y)
{
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   std::cout << "TODO: Correct this for DG diffusion" << std::endl;
   exit(1);

   if (dim == 2)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         default: return PADGDiffusionApplyTranspose2D(NF,B,Bt,op,x,y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {
      switch ((D1D << 4 ) | Q1D)
      {

         default: return PADGDiffusionApplyTranspose3D(NF,B,Bt,op,x,y,D1D,Q1D);
      }
   }
   MFEM_ABORT("Unknown kernel.");
 
   exit(1);
}
*/

// PA DGDiffusionIntegrator Apply kernel
void DGDiffusionIntegrator::AddMultPA(const Vector &x, Vector &y) const
{
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   PADGDiffusionApply(dim, dofs1D, quad1D, nf,
                  maps->B, maps->Bt,
                  maps->G, maps->Gt,
                  coeff_data_1,coeff_data_2,coeff_data_3,
                  x, y);
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
}

void DGDiffusionIntegrator::AddMultTransposePA(const Vector &x, Vector &y) const
{
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   MFEM_ABORT("DGDiffusionIntegrator::AddMultTransposePA not yet implemented");
   /*
   PADGDiffusionApplyTranspose(dim, dofs1D, quad1D, nf,
                           maps->B, maps->Bt,
                           maps->G, maps->Gt,
                           coeff_data_1,coeff_data_2,coeff_data_3,
                           x, y);
                           */
   std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   exit(1);
}

} // namespace mfem