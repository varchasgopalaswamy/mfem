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

#include "../config/config.hpp"
#include "nonlininteg.hpp"
#include "fespace.hpp"
#include "libceed/ceed.hpp"
#include "fem.hpp"

#include "restriction.hpp"
#include "gridfunc.hpp"
#include "fespace.hpp"
#include "../general/forall.hpp"

namespace mfem
{

ElementRestriction::ElementRestriction(const FiniteElementSpace &f,
                                       ElementDofOrdering e_ordering)
   : fes(f),
     ne(fes.GetNE()),
     vdim(fes.GetVDim()),
     byvdim(fes.GetOrdering() == Ordering::byVDIM),
     ndofs(fes.GetNDofs()),
     dof(ne > 0 ? fes.GetFE(0)->GetDof() : 0),
     nedofs(ne*dof),
     offsets(ndofs+1),
     indices(ne*dof),
     gatherMap(ne*dof)
{
   // Assuming all finite elements are the same.
   height = vdim*ne*dof;
   width = fes.GetVSize();
   const bool dof_reorder = (e_ordering == ElementDofOrdering::LEXICOGRAPHIC);
   const int *dof_map = NULL;
   if (dof_reorder && ne > 0)
   {
      for (int e = 0; e < ne; ++e)
      {
         const FiniteElement *fe = fes.GetFE(e);
         const TensorBasisElement* el =
            dynamic_cast<const TensorBasisElement*>(fe);
         if (el) { continue; }
         mfem_error("Finite element not suitable for lexicographic ordering");
      }
      const FiniteElement *fe = fes.GetFE(0);
      const TensorBasisElement* el =
         dynamic_cast<const TensorBasisElement*>(fe);
      const Array<int> &fe_dof_map = el->GetDofMap();
      MFEM_VERIFY(fe_dof_map.Size() > 0, "invalid dof map");
      dof_map = fe_dof_map.GetData();
   }
   const Table& e2dTable = fes.GetElementToDofTable();
   const int* elementMap = e2dTable.GetJ();
   // We will be keeping a count of how many local nodes point to its global dof
   for (int i = 0; i <= ndofs; ++i)
   {
      offsets[i] = 0;
   }
   for (int e = 0; e < ne; ++e)
   {
      for (int d = 0; d < dof; ++d)
      {
         const int sgid = elementMap[dof*e + d];  // signed
         const int gid = (sgid >= 0) ? sgid : -1 - sgid;
         ++offsets[gid + 1];
      }
   }
   // Aggregate to find offsets for each global dof
   for (int i = 1; i <= ndofs; ++i)
   {
      offsets[i] += offsets[i - 1];
   }
   // For each global dof, fill in all local nodes that point to it
   for (int e = 0; e < ne; ++e)
   {
      for (int d = 0; d < dof; ++d)
      {
         const int sdid = dof_reorder ? dof_map[d] : 0;  // signed
         const int did = (!dof_reorder)?d:(sdid >= 0 ? sdid : -1-sdid);
         const int sgid = elementMap[dof*e + did];  // signed
         const int gid = (sgid >= 0) ? sgid : -1-sgid;
         const int lid = dof*e + d;
         const bool plus = (sgid >= 0 && sdid >= 0) || (sgid < 0 && sdid < 0);
         gatherMap[lid] = plus ? gid : -1-gid;
         indices[offsets[gid]++] = plus ? lid : -1-lid;
      }
   }
   // We shifted the offsets vector by 1 by using it as a counter.
   // Now we shift it back.
   for (int i = ndofs; i > 0; --i)
   {
      offsets[i] = offsets[i - 1];
   }
   offsets[0] = 0;
}

void ElementRestriction::Mult(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
   auto d_y = Reshape(y.Write(), nd, vd, ne);
   auto d_gatherMap = gatherMap.Read();
   MFEM_FORALL(i, dof*ne,
   {
      const int gid = d_gatherMap[i];
      const bool plus = gid >= 0;
      const int j = plus ? gid : -1-gid;
      for (int c = 0; c < vd; ++c)
      {
         const double dofValue = d_x(t?c:j, t?j:c);
         d_y(i % nd, c, i / nd) = plus ? dofValue : -dofValue;
      }
   });
}

void ElementRestriction::MultUnsigned(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
   auto d_y = Reshape(y.Write(), nd, vd, ne);
   auto d_gatherMap = gatherMap.Read();

   MFEM_FORALL(i, dof*ne,
   {
      const int gid = d_gatherMap[i];
      const int j = gid >= 0 ? gid : -1-gid;
      for (int c = 0; c < vd; ++c)
      {
         d_y(i % nd, c, i / nd) = d_x(t?c:j, t?j:c);
      }
   });
}

void ElementRestriction::MultTranspose(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_offsets = offsets.Read();
   auto d_indices = indices.Read();
   auto d_x = Reshape(x.Read(), nd, vd, ne);
   auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
   MFEM_FORALL(i, ndofs,
   {
      const int offset = d_offsets[i];
      const int nextOffset = d_offsets[i + 1];
      for (int c = 0; c < vd; ++c)
      {
         double dofValue = 0;
         for (int j = offset; j < nextOffset; ++j)
         {
            const int idx_j = (d_indices[j] >= 0) ? d_indices[j] : -1 - d_indices[j];
            dofValue += (d_indices[j] >= 0) ? d_x(idx_j % nd, c,
            idx_j / nd) : -d_x(idx_j % nd, c, idx_j / nd);
         }
         d_y(t?c:i,t?i:c) = dofValue;
      }
   });
}

void ElementRestriction::MultTransposeUnsigned(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_offsets = offsets.Read();
   auto d_indices = indices.Read();
   auto d_x = Reshape(x.Read(), nd, vd, ne);
   auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
   MFEM_FORALL(i, ndofs,
   {
      const int offset = d_offsets[i];
      const int nextOffset = d_offsets[i + 1];
      for (int c = 0; c < vd; ++c)
      {
         double dofValue = 0;
         for (int j = offset; j < nextOffset; ++j)
         {
            const int idx_j = (d_indices[j] >= 0) ? d_indices[j] : -1 - d_indices[j];
            dofValue += d_x(idx_j % nd, c, idx_j / nd);
         }
         d_y(t?c:i,t?i:c) = dofValue;
      }
   });
}

void ElementRestriction::BooleanMask(Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;

   Array<char> processed(vd * ndofs);
   processed = 0;

   auto d_offsets = offsets.HostRead();
   auto d_indices = indices.HostRead();
   auto d_x = Reshape(processed.HostReadWrite(), t?vd:ndofs, t?ndofs:vd);
   auto d_y = Reshape(y.HostWrite(), nd, vd, ne);
   for (int i = 0; i < ndofs; ++i)
   {
      const int offset = d_offsets[i];
      const int nextOffset = d_offsets[i+1];
      for (int c = 0; c < vd; ++c)
      {
         for (int j = offset; j < nextOffset; ++j)
         {
            const int idx_j = d_indices[j];
            if (d_x(t?c:i,t?i:c))
            {
               d_y(idx_j % nd, c, idx_j / nd) = 0.0;
            }
            else
            {
               d_y(idx_j % nd, c, idx_j / nd) = 1.0;
               d_x(t?c:i,t?i:c) = 1;
            }
         }
      }
   }
}

void ElementRestriction::FillSparseMatrix(const Vector &mat_ea,
                                          SparseMatrix &mat) const
{
   mat.GetMemoryI().New(mat.Height()+1, mat.GetMemoryI().GetMemoryType());
   const int nnz = FillI(mat);
   mat.GetMemoryJ().New(nnz, mat.GetMemoryJ().GetMemoryType());
   mat.GetMemoryData().New(nnz, mat.GetMemoryData().GetMemoryType());
   FillJAndData(mat_ea, mat);
}

template <int MaxNbNbr>
static MFEM_HOST_DEVICE int GetMinElt(const int *my_elts, const int nbElts,
                                      const int *nbr_elts, const int nbrNbElts)
{
   // Building the intersection
   int inter[MaxNbNbr];
   int cpt = 0;
   for (int i = 0; i < nbElts; i++)
   {
      const int e_i = my_elts[i];
      for (int j = 0; j < nbrNbElts; j++)
      {
         if (e_i==nbr_elts[j])
         {
            inter[cpt] = e_i;
            cpt++;
         }
      }
   }
   // Finding the minimum
   int min = inter[0];
   for (int i = 1; i < cpt; i++)
   {
      if (inter[i] < min)
      {
         min = inter[i];
      }
   }
   return min;
}

/** Returns the index where a non-zero entry should be added and increment the
    number of non-zeros for the row i_L. */
static MFEM_HOST_DEVICE int GetAndIncrementNnzIndex(const int i_L, int* I)
{
   int ind = AtomicAdd(I[i_L],1);
   return ind;
}

int ElementRestriction::FillI(SparseMatrix &mat) const
{
   static constexpr int Max = MaxNbNbr;
   const int all_dofs = ndofs;
   const int vd = vdim;
   const int elt_dofs = dof;
   auto I = mat.ReadWriteI();
   auto d_offsets = offsets.Read();
   auto d_indices = indices.Read();
   auto d_gatherMap = gatherMap.Read();
   MFEM_FORALL(i_L, vd*all_dofs+1,
   {
      I[i_L] = 0;
   });
   MFEM_FORALL(e, ne,
   {
      for (int i = 0; i < elt_dofs; i++)
      {
         int i_elts[Max];
         const int i_E = e*elt_dofs + i;
         const int i_L = d_gatherMap[i_E];
         const int i_offset = d_offsets[i_L];
         const int i_nextOffset = d_offsets[i_L+1];
         const int i_nbElts = i_nextOffset - i_offset;
         for (int e_i = 0; e_i < i_nbElts; ++e_i)
         {
            const int i_E = d_indices[i_offset+e_i];
            i_elts[e_i] = i_E/elt_dofs;
         }
         for (int j = 0; j < elt_dofs; j++)
         {
            const int j_E = e*elt_dofs + j;
            const int j_L = d_gatherMap[j_E];
            const int j_offset = d_offsets[j_L];
            const int j_nextOffset = d_offsets[j_L+1];
            const int j_nbElts = j_nextOffset - j_offset;
            if (i_nbElts == 1 || j_nbElts == 1) // no assembly required
            {
               GetAndIncrementNnzIndex(i_L, I);
            }
            else // assembly required
            {
               int j_elts[Max];
               for (int e_j = 0; e_j < j_nbElts; ++e_j)
               {
                  const int j_E = d_indices[j_offset+e_j];
                  const int elt = j_E/elt_dofs;
                  j_elts[e_j] = elt;
               }
               int min_e = GetMinElt<Max>(i_elts, i_nbElts, j_elts, j_nbElts);
               if (e == min_e) // add the nnz only once
               {
                  GetAndIncrementNnzIndex(i_L, I);
               }
            }
         }
      }
   });
   // We need to sum the entries of I, we do it on CPU as it is very sequential.
   auto h_I = mat.HostReadWriteI();
   const int nTdofs = vd*all_dofs;
   int sum = 0;
   for (int i = 0; i < nTdofs; i++)
   {
      const int nnz = h_I[i];
      h_I[i] = sum;
      sum+=nnz;
   }
   h_I[nTdofs] = sum;
   // We return the number of nnz
   return h_I[nTdofs];
}

void ElementRestriction::FillJAndData(const Vector &ea_data,
                                      SparseMatrix &mat) const
{
   static constexpr int Max = MaxNbNbr;
   const int all_dofs = ndofs;
   const int vd = vdim;
   const int elt_dofs = dof;
   auto I = mat.ReadWriteI();
   auto J = mat.WriteJ();
   auto Data = mat.WriteData();
   auto d_offsets = offsets.Read();
   auto d_indices = indices.Read();
   auto d_gatherMap = gatherMap.Read();
   auto mat_ea = Reshape(ea_data.Read(), elt_dofs, elt_dofs, ne);
   MFEM_FORALL(e, ne,
   {
      for (int i = 0; i < elt_dofs; i++)
      {
         int i_elts[Max];
         int i_B[Max];
         const int i_E = e*elt_dofs + i;
         const int i_L = d_gatherMap[i_E];
         const int i_offset = d_offsets[i_L];
         const int i_nextOffset = d_offsets[i_L+1];
         const int i_nbElts = i_nextOffset - i_offset;
         for (int e_i = 0; e_i < i_nbElts; ++e_i)
         {
            const int i_E = d_indices[i_offset+e_i];
            i_elts[e_i] = i_E/elt_dofs;
            i_B[e_i]    = i_E%elt_dofs;
         }
         for (int j = 0; j < elt_dofs; j++)
         {
            const int j_E = e*elt_dofs + j;
            const int j_L = d_gatherMap[j_E];
            const int j_offset = d_offsets[j_L];
            const int j_nextOffset = d_offsets[j_L+1];
            const int j_nbElts = j_nextOffset - j_offset;
            if (i_nbElts == 1 || j_nbElts == 1) // no assembly required
            {
               const int nnz = GetAndIncrementNnzIndex(i_L, I);
               J[nnz] = j_L;
               Data[nnz] = mat_ea(j,i,e);
            }
            else // assembly required
            {
               int j_elts[Max];
               int j_B[Max];
               for (int e_j = 0; e_j < j_nbElts; ++e_j)
               {
                  const int j_E = d_indices[j_offset+e_j];
                  const int elt = j_E/elt_dofs;
                  j_elts[e_j] = elt;
                  j_B[e_j]    = j_E%elt_dofs;
               }
               int min_e = GetMinElt<Max>(i_elts, i_nbElts, j_elts, j_nbElts);
               if (e == min_e) // add the nnz only once
               {
                  double val = 0.0;
                  for (int i = 0; i < i_nbElts; i++)
                  {
                     const int e_i = i_elts[i];
                     const int i_Bloc = i_B[i];
                     for (int j = 0; j < j_nbElts; j++)
                     {
                        const int e_j = j_elts[j];
                        const int j_Bloc = j_B[j];
                        if (e_i == e_j)
                        {
                           val += mat_ea(j_Bloc, i_Bloc, e_i);
                        }
                     }
                  }
                  const int nnz = GetAndIncrementNnzIndex(i_L, I);
                  J[nnz] = j_L;
                  Data[nnz] = val;
               }
            }
         }
      }
   });
   // We need to shift again the entries of I, we do it on CPU as it is very
   // sequential.
   auto h_I = mat.HostReadWriteI();
   const int size = vd*all_dofs;
   for (int i = 0; i < size; i++)
   {
      h_I[size-i] = h_I[size-(i+1)];
   }
   h_I[0] = 0;
}

L2ElementRestriction::L2ElementRestriction(const FiniteElementSpace &fes)
   : ne(fes.GetNE()),
     vdim(fes.GetVDim()),
     byvdim(fes.GetOrdering() == Ordering::byVDIM),
     ndof(ne > 0 ? fes.GetFE(0)->GetDof() : 0),
     ndofs(fes.GetNDofs())
{
   height = vdim*ne*ndof;
   width = vdim*ne*ndof;
}

void L2ElementRestriction::Mult(const Vector &x, Vector &y) const
{
   const int nd = ndof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
   auto d_y = Reshape(y.Write(), nd, vd, ne);
   MFEM_FORALL(i, ndofs,
   {
      const int idx = i;
      const int dof = idx % nd;
      const int e = idx / nd;
      for (int c = 0; c < vd; ++c)
      {
         d_y(dof, c, e) = d_x(t?c:idx, t?idx:c);
      }
   });
}

void L2ElementRestriction::MultTranspose(const Vector &x, Vector &y) const
{
   const int nd = ndof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_x = Reshape(x.Read(), nd, vd, ne);
   auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
   MFEM_FORALL(i, ndofs,
   {
      const int idx = i;
      const int dof = idx % nd;
      const int e = idx / nd;
      for (int c = 0; c < vd; ++c)
      {
         d_y(t?c:idx,t?idx:c) = d_x(dof, c, e);
      }
   });
}

void L2ElementRestriction::FillI(SparseMatrix &mat) const
{
   const int elem_dofs = ndof;
   const int vd = vdim;
   auto I = mat.WriteI();
   MFEM_FORALL(dof, ne*elem_dofs*vd,
   {
      I[dof] = elem_dofs;
   });
}

static MFEM_HOST_DEVICE int AddNnz(const int iE, int *I, const int dofs)
{
   int val = AtomicAdd(I[iE],dofs);
   return val;
}

void L2ElementRestriction::FillJAndData(const Vector &ea_data,
                                        SparseMatrix &mat) const
{
   const int elem_dofs = ndof;
   const int vd = vdim;
   auto I = mat.ReadWriteI();
   auto J = mat.WriteJ();
   auto Data = mat.WriteData();
   auto mat_ea = Reshape(ea_data.Read(), elem_dofs, elem_dofs, ne);
   MFEM_FORALL(iE, ne*elem_dofs*vd,
   {
      const int offset = AddNnz(iE,I,elem_dofs);
      const int e = iE/elem_dofs;
      const int i = iE%elem_dofs;
      for (int j = 0; j < elem_dofs; j++)
      {
         J[offset+j] = e*elem_dofs+j;
         Data[offset+j] = mat_ea(j,i,e);
      }
   });
}

int inline Get_dof_from_ijk(int i,int j,int k,int dof1d)
{
   return i + dof1d*j + dof1d*dof1d*k;
}

/*
            const int face_dof = facemapnorself[p*dof1d+0];
            const int did = face_dof;
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + p ;
*/

// Return the face degrees of freedom returned in Lexicographic order.
void GetNormalDFaceDofStencil(const int dim, const int face_id,
                 const int dof1d, Array<int> &facemapnor)
{
   int end = dof1d-1;
   switch (dim)
   {
      case 1:
         MFEM_ABORT("GetNormalDFaceDofStencil not implemented for 1D!");         
      case 2:
         switch (face_id)
         {
            // i is WEST to EAST
            // j is SOUTH to NORTH
            // dof = i + j*dof1d
            case 0: // SOUTH, j = 0
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemapnor[dof1d*i+s] = Get_dof_from_ijk(i,s,0,dof1d);
                     //i + s*dof1d;
                  }
               }
               break;
            case 1: // EAST, i = dof1d-1
               for (int j = 0; j < dof1d; ++j)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemapnor[dof1d*j+s] = Get_dof_from_ijk(end-s,j,0,dof1d);//end-s + j*dof1d;
                  }
               }
               break;
            case 2: // NORTH, j = dof1d-1
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemapnor[dof1d*i+s] = Get_dof_from_ijk(i,end-s,0,dof1d);//(end-s)*dof1d + i;
                  }
               }
               break;
            case 3: // WEST, i = 0
               for (int j = 0; j < dof1d; ++j)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemapnor[dof1d*j+s] = Get_dof_from_ijk(s,j,0,dof1d);//s + j*dof1d;
                  }
               }
               break;
            default:
               MFEM_ABORT("Invalid face_id");
         }
         break;
      case 3:
         switch (face_id)
         {
            // dof = i + j*dof1d + k*dof1d*dof1d 
            case 0: // BOTTOM
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,j,s,dof1d);//i + j*dof1d + s*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 1: // SOUTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,s,j,dof1d);//i + s*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 2: // EAST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(end-s,i,j,dof1d);//end-s + i*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 3: // NORTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,end-s,j,dof1d);//(end-s)*dof1d + i + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 4: // WEST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(s,i,j,dof1d);//s + i*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 5: // TOP
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemapnor[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,j,end-s,dof1d);//(end-s)*dof1d*dof1d + i + j*dof1d;
                     }
                  }
               }
               break;
            default: 
               MFEM_ABORT("Invalid face_id");
         }
         break;
#ifdef MFEM_DEBUG
         for(int k = 0 ; k < dof1d*dof1d*dof1d ; k++ )
         {
            MFEM_VERIFY( (facemapnor[k] >= dof1d*dof1d*dof1d) or (facemapnor[k] < 0 ) ,
            "Invalid facemapnor values.");
         }
#endif
   }

/*
   //std::cout << "%{ " << std::endl; 
   //std::cout << "faceid for nor " << " " << face_id << std::endl;
   for (int i = 0; i < dof1d; ++i)
   {
      for (int s = 0; s < dof1d; ++s)
      {
         //std::cout << i << " " << s << " " << facemapnor[i*dof1d+s] << std::endl;
      }
   }
   //std::cout << "%} " << std::endl; 
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
*/

}

/*
*/

int GetGid(const int ipid, 
           const int k, 
           const int dof1d,
           const int elemid,
           const int elem_dofs,
           Array<int> &facemap,
           const int* elementMap)
{
   const int face_dof = facemap[ipid*dof1d+k];
   const int gid = elementMap[elemid*elem_dofs + face_dof];
   return gid;
}

int GetLid(const int ipid, 
           const int k, 
           const int dof1d,
           const int dof,
           const int face_id,
           Array<int> &facemap,
           const int d)
{
   const int face_dof = facemap[ipid*dof1d+k];
   const int lid = dof*face_id + dof1d*d + k;
   //const int lid = dof*face_dof + ipid;
   return lid;
}               

int GetLid(const int ipid, 
           const int k, 
           const int dof1d,
           const int dof,
           const int face_id,
           Array<int> &facemap)
{
   // combine with other getlid
   const int face_dof = facemap[ipid*dof1d+k];
   const int lid = dof*face_id + ipid;
   //const int lid = dof*face_dof + ipid;
   return lid;
}     

// Return the face degrees of freedom returned in Lexicographic order.
void GetTangentDFaceDofStencil(const int dim,
                               const int face_id,
                               const int dof1d, 
                               Array<int> &facemaptan1,
                               Array<int> &facemaptan2)
{
   int end = dof1d-1;
   int face_offset = 2*dof1d*face_id;
   switch (dim)
   {
      case 1:
         MFEM_ABORT("GetTangentDFaceDofStencil not implemented for 1D!");         
      case 2:
         switch (face_id)
         {
            // i is WEST to EAST
            // j is SOUTH to NORTH
            // dof = i + j*dof1d
            case 0: // SOUTH, j = 0
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemaptan1[dof1d*i+s] = Get_dof_from_ijk(s,0,0,dof1d);
                     //i + s*dof1d;
                  }
               }
               break;
            case 1: // EAST, i = dof1d-1
               for (int j = 0; j < dof1d; ++j)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemaptan1[dof1d*j+s] = Get_dof_from_ijk(end,s,0,dof1d);
                  }
               }
               break;
            case 2: // NORTH, j = dof1d-1
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemaptan1[dof1d*i+s] = Get_dof_from_ijk(s,end,0,dof1d);
                  }
               }
               break;
            case 3: // WEST, i = 0
               for (int j = 0; j < dof1d; ++j)
               {
                  for (int s = 0; s < dof1d; ++s)
                  {
                     facemaptan1[dof1d*j+s] = Get_dof_from_ijk(0,s,0,dof1d);
                  }
               }
               break;
            default:
               MFEM_ABORT("Invalid face_id");
         }
         break;
      /*
      case 3:
         switch (face_id)
         {
            // dof = i + j*dof1d + k*dof1d*dof1d 
            case 0: // BOTTOM
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,j,s,dof1d);//i + j*dof1d + s*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 1: // SOUTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,s,j,dof1d);//i + s*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 2: // EAST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(end-s,i,j,dof1d);//end-s + i*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 3: // NORTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,end-s,j,dof1d);//(end-s)*dof1d + i + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 4: // WEST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(s,i,j,dof1d);//s + i*dof1d + j*dof1d*dof1d;
                     }
                  }
               }
               break;
            case 5: // TOP
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     for (int s = 0; s < dof1d; ++s)
                     {
                        facemaptan[s+i*dof1d+j*dof1d*dof1d] = Get_dof_from_ijk(i,j,end-s,dof1d);//(end-s)*dof1d*dof1d + i + j*dof1d;
                     }
                  }
               }
               break;
            default: 
               MFEM_ABORT("Invalid face_id");
         }
         break;
         */
#ifdef MFEM_DEBUG
         for(int k = 0 ; k < dof1d*dof1d*dof1d ; k++ )
         {
            MFEM_VERIFY( (facemaptan1[k] >= dof1d*dof1d*dof1d) or (facemaptan1[k] < 0 ) ,
            "Invalid facemaptan values.");
            MFEM_VERIFY( (facemaptan2[k] >= dof1d*dof1d*dof1d) or (facemaptan2[k] < 0 ) ,
            "Invalid facemaptan values.");
         }
#endif
   }
}

// Generates faceMap, which maps face indices to element indices
// based on Lexicographic ordering
void GetFaceDofs(const int dim, const int face_id,
                 const int dof1d, Array<int> &faceMap)
{
   switch (dim)
   {
      case 1:
         switch (face_id)
         {
            case 0: // WEST
               faceMap[0] = 0;
               break;
            case 1: // EAST
               faceMap[0] = dof1d-1;
               break;
         }
         break;
      case 2:
         switch (face_id)
         {
            case 0: // SOUTH
               for (int i = 0; i < dof1d; ++i)
               {
                  faceMap[i] = i;
               }
               break;
            case 1: // EAST
               for (int i = 0; i < dof1d; ++i)
               {
                  faceMap[i] = dof1d-1 + i*dof1d;
               }
               break;
            case 2: // NORTH
               for (int i = 0; i < dof1d; ++i)
               {
                  faceMap[i] = (dof1d-1)*dof1d + i;
               }
               break;
            case 3: // WEST
               for (int i = 0; i < dof1d; ++i)
               {
                  faceMap[i] = i*dof1d;
               }
               break;
         }
         break;
      case 3:
         switch (face_id)
         {
            case 0: // BOTTOM
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = i + j*dof1d;
                  }
               }
               break;
            case 1: // SOUTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = i + j*dof1d*dof1d;
                  }
               }
               break;
            case 2: // EAST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = dof1d-1 + i*dof1d + j*dof1d*dof1d;
                  }
               }
               break;
            case 3: // NORTH
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = (dof1d-1)*dof1d + i + j*dof1d*dof1d;
                  }
               }
               break;
            case 4: // WEST
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = i*dof1d + j*dof1d*dof1d;
                  }
               }
               break;
            case 5: // TOP
               for (int i = 0; i < dof1d; ++i)
               {
                  for (int j = 0; j < dof1d; ++j)
                  {
                     faceMap[i+j*dof1d] = (dof1d-1)*dof1d*dof1d + i + j*dof1d;
                  }
               }
               break;
         }
         break;
   }
}

H1FaceRestriction::H1FaceRestriction(const FiniteElementSpace &fes,
                                     const ElementDofOrdering e_ordering,
                                     const FaceType type)
   : fes(fes),
     nf(fes.GetNFbyType(type)),
     vdim(fes.GetVDim()),
     byvdim(fes.GetOrdering() == Ordering::byVDIM),
     ndofs(fes.GetNDofs()),
     dof(nf > 0 ? fes.GetFaceElement(0)->GetDof() : 0),
     nfdofs(nf*dof),
     scatter_indices(nf*dof),
     offsets(ndofs+1),
     gather_indices(nf*dof)
{
   if (nf==0) { return; }
   // If fespace == H1
   const FiniteElement *fe = fes.GetFE(0);
   const TensorBasisElement *tfe = dynamic_cast<const TensorBasisElement*>(fe);
   MFEM_VERIFY(tfe != NULL &&
               (tfe->GetBasisType()==BasisType::GaussLobatto ||
                tfe->GetBasisType()==BasisType::Positive),
               "Only Gauss-Lobatto and Bernstein basis are supported in "
               "H1FaceRestriction.");
   MFEM_VERIFY(fes.GetMesh()->Conforming(),
               "Non-conforming meshes not yet supported with partial assembly.");
   // Assuming all finite elements are using Gauss-Lobatto.
   height = vdim*nf*dof;
   width = fes.GetVSize();
   const bool dof_reorder = (e_ordering == ElementDofOrdering::LEXICOGRAPHIC);
   if (dof_reorder && nf > 0)
   {
      for (int f = 0; f < fes.GetNF(); ++f)
      {
         const FiniteElement *fe = fes.GetFaceElement(f);
         const TensorBasisElement* el =
            dynamic_cast<const TensorBasisElement*>(fe);
         if (el) { continue; }
         mfem_error("Finite element not suitable for lexicographic ordering");
      }
      const FiniteElement *fe = fes.GetFaceElement(0);
      const TensorBasisElement* el =
         dynamic_cast<const TensorBasisElement*>(fe);
      const Array<int> &fe_dof_map = el->GetDofMap();
      MFEM_VERIFY(fe_dof_map.Size() > 0, "invalid dof map");
   }
   const TensorBasisElement* el =
      dynamic_cast<const TensorBasisElement*>(fe);
   const int *dof_map = el->GetDofMap().GetData();
   const Table& e2dTable = fes.GetElementToDofTable();
   const int* elementMap = e2dTable.GetJ();
   Array<int> faceMap(dof);
   int e1, e2;
   int inf1, inf2;
   int face_id;
   int orientation;
   const int dof1d = fes.GetFE(0)->GetOrder()+1;
   const int elem_dofs = fes.GetFE(0)->GetDof();
   const int dim = fes.GetMesh()->SpaceDimension();
   // Computation of scatter_indices
   int f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      orientation = inf1 % 64;
      face_id = inf1 / 64;
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         // Assumes Gauss-Lobatto basis
         if (dof_reorder)
         {
            if (orientation != 0)
            {
               mfem_error("FaceRestriction used on degenerated mesh.");
            }
            GetFaceDofs(dim, face_id, dof1d, faceMap); // Only for hex
         }
         else
         {
            mfem_error("FaceRestriction not yet implemented for this type of "
                       "element.");
            // TODO Something with GetFaceDofs?
         }
         for (int d = 0; d < dof; ++d)
         {
            const int face_dof = faceMap[d];
            const int did = (!dof_reorder)?face_dof:dof_map[face_dof];
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + d;
            scatter_indices[lid] = gid;
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   // Computation of gather_indices
   for (int i = 0; i <= ndofs; ++i)
   {
      offsets[i] = 0;
   }
   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      orientation = inf1 % 64;
      face_id = inf1 / 64;
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         GetFaceDofs(dim, face_id, dof1d, faceMap);
         for (int d = 0; d < dof; ++d)
         {
            const int face_dof = faceMap[d];
            const int did = (!dof_reorder)?face_dof:dof_map[face_dof];
            const int gid = elementMap[e1*elem_dofs + did];
            ++offsets[gid + 1];
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = 1; i <= ndofs; ++i)
   {
      offsets[i] += offsets[i - 1];
   }
   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      orientation = inf1 % 64;
      face_id = inf1 / 64;
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         GetFaceDofs(dim, face_id, dof1d, faceMap);
         for (int d = 0; d < dof; ++d)
         {
            const int face_dof = faceMap[d];
            const int did = (!dof_reorder)?face_dof:dof_map[face_dof];
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + d;
            gather_indices[offsets[gid]++] = lid;
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = ndofs; i > 0; --i)
   {
      offsets[i] = offsets[i - 1];
   }
   offsets[0] = 0;
}

void H1FaceRestriction::Mult(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_indices = scatter_indices.Read();
   auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
   auto d_y = Reshape(y.Write(), nd, vd, nf);
   MFEM_FORALL(i, nfdofs,
   {
      const int idx = d_indices[i];
      const int dof = i % nd;
      const int face = i / nd;
      for (int c = 0; c < vd; ++c)
      {
         d_y(dof, c, face) = d_x(t?c:idx, t?idx:c);
      }
   });
}

void H1FaceRestriction::MultTranspose(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   auto d_offsets = offsets.Read();
   auto d_indices = gather_indices.Read();
   auto d_x = Reshape(x.Read(), nd, vd, nf);
   auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
   MFEM_FORALL(i, ndofs,
   {
      const int offset = d_offsets[i];
      const int nextOffset = d_offsets[i + 1];
      for (int c = 0; c < vd; ++c)
      {
         double dofValue = 0;
         for (int j = offset; j < nextOffset; ++j)
         {
            const int idx_j = d_indices[j];
            dofValue +=  d_x(idx_j % nd, c, idx_j / nd);
         }
         d_y(t?c:i,t?i:c) += dofValue;
      }
   });
}

static int ToLexOrdering2D(const int face_id, const int size1d, const int i)
{
   if (face_id==2 || face_id==3)
   {
      return size1d-1-i;
   }
   else
   {
      return i;
   }
}

static int PermuteFace2D(const int face_id1, const int face_id2,
                         const int orientation,
                         const int size1d, const int index)
{
   int new_index;
   // Convert from lex ordering
   if (face_id1==2 || face_id1==3)
   {
      new_index = size1d-1-index;
   }
   else
   {
      new_index = index;
   }
   // Permute based on face orientations
   if (orientation==1)
   {
      new_index = size1d-1-new_index;
   }
   return ToLexOrdering2D(face_id2, size1d, new_index);
}

static int PermuteDFaceDNorm2D(const int face_id1, const int face_id2,
                                 const int orientation,
                                 const int size1d, const int index)
{
   int new_index;
   // Convert from lex ordering
   if (face_id1==2 || face_id1==3)
   {
      new_index = size1d-1-index;
   }
   else
   {
      new_index = index;
   }
   // Permute based on face orientations
   if (orientation==1)
   {
      new_index = size1d-1-new_index;
   }
   return ToLexOrdering2D(face_id2, size1d, new_index);
}

static int ToLexOrdering3D(const int face_id, const int size1d, const int i,
                           const int j)
{
   if (face_id==2 || face_id==1 || face_id==5)
   {
      return i + j*size1d;
   }
   else if (face_id==3 || face_id==4)
   {
      return (size1d-1-i) + j*size1d;
   }
   else // face_id==0
   {
      return i + (size1d-1-j)*size1d;
   }
}

static int PermuteFace3D(const int face_id1, const int face_id2,
                         const int orientation,
                         const int size1d, const int index)
{
   int i=0, j=0, new_i=0, new_j=0;
   i = index%size1d;
   j = index/size1d;
   // Convert from lex ordering
   if (face_id1==3 || face_id1==4)
   {
      i = size1d-1-i;
   }
   else if (face_id1==0)
   {
      j = size1d-1-j;
   }
   // Permute based on face orientations
   switch (orientation)
   {
      case 0:
         new_i = i;
         new_j = j;
         break;
      case 1:
         new_i = j;
         new_j = i;
         break;
      case 2:
         new_i = j;
         new_j = (size1d-1-i);
         break;
      case 3:
         new_i = (size1d-1-i);
         new_j = j;
         break;
      case 4:
         new_i = (size1d-1-i);
         new_j = (size1d-1-j);
         break;
      case 5:
         new_i = (size1d-1-j);
         new_j = (size1d-1-i);
         break;
      case 6:
         new_i = (size1d-1-j);
         new_j = i;
         break;
      case 7:
         new_i = i;
         new_j = (size1d-1-j);
         break;
   }
   return ToLexOrdering3D(face_id2, size1d, new_i, new_j);
}

// Permute dofs or quads on a face for e2 to match with the ordering of e1
int PermuteFaceL2(const int dim, const int face_id1,
                  const int face_id2, const int orientation,
                  const int size1d, const int index)
{
   switch (dim)
   {
      case 1:
         return 0;
      case 2:
         return PermuteFace2D(face_id1, face_id2, orientation, size1d, index);
      case 3:
         return PermuteFace3D(face_id1, face_id2, orientation, size1d, index);
      default:
         mfem_error("Unsupported dimension.");
         return 0;
   }
}

// Permute dofs or quads on a face for e2 to match with the ordering of e1
int PermuteDFaceDNormL2(const int dim, const int face_id1,
                  const int face_id2, const int orientation,
                  const int size1d, const int index)
{
   switch (dim)
   {
      case 1:
         return 0;
      case 2:
         return PermuteDFaceDNorm2D(face_id1, face_id2, orientation, size1d, index);
    //  case 3:
      //x   return PermuteDFaceDNorm3D(face_id1, face_id2, orientation, size1d, index);
      default:
         mfem_error("Unsupported dimension.");
         return 0;
   }
}

L2FaceRestriction::L2FaceRestriction(const FiniteElementSpace &fes,
                                     const FaceType type,
                                     const L2FaceValues m)
   : fes(fes),
     nf(fes.GetNFbyType(type)),
     ne(fes.GetNE()),
     vdim(fes.GetVDim()),
     byvdim(fes.GetOrdering() == Ordering::byVDIM),
     ndofs(fes.GetNDofs()),
     dof(nf > 0 ?
         fes.GetTraceElement(0, fes.GetMesh()->GetFaceBaseGeometry(0))->GetDof()
         : 0),
     elemDofs(fes.GetFE(0)->GetDof()),
     m(m),
     nfdofs(nf*dof),
     scatter_indices1(nf*dof),
     scatter_indices2(m==L2FaceValues::DoubleValued?nf*dof:0),
     offsets(ndofs+1),
     gather_indices((m==L2FaceValues::DoubleValued? 2 : 1)*nf*dof)
{
}

L2FaceRestriction::L2FaceRestriction(const FiniteElementSpace &fes,
                                     const ElementDofOrdering e_ordering,
                                     const FaceType type,
                                     const L2FaceValues m)
   : L2FaceRestriction(fes, type, m)
{

   // If fespace == L2
   const FiniteElement *fe = fes.GetFE(0);
   const TensorBasisElement *tfe = dynamic_cast<const TensorBasisElement*>(fe);
   MFEM_VERIFY(tfe != NULL &&
               (tfe->GetBasisType()==BasisType::GaussLobatto ||
                tfe->GetBasisType()==BasisType::Positive),
               "Only Gauss-Lobatto and Bernstein basis are supported in "
               "L2FaceRestriction.");
   MFEM_VERIFY(fes.GetMesh()->Conforming(),
               "Non-conforming meshes not yet supported with partial assembly.");
   if (nf==0) { return; }
   height = (m==L2FaceValues::DoubleValued? 2 : 1)*vdim*nf*dof;
   width = fes.GetVSize();
   const bool dof_reorder = (e_ordering == ElementDofOrdering::LEXICOGRAPHIC);
   if (!dof_reorder)
   {
      mfem_error("Non-Tensor L2FaceRestriction not yet implemented.");
   }
   if (dof_reorder && nf > 0)
   {
      for (int f = 0; f < fes.GetNF(); ++f)
      {
         const FiniteElement *fe =
            fes.GetTraceElement(f, fes.GetMesh()->GetFaceBaseGeometry(f));
         const TensorBasisElement* el =
            dynamic_cast<const TensorBasisElement*>(fe);
         if (el) { continue; }
         mfem_error("Finite element not suitable for lexicographic ordering");
      }
   }
   const Table& e2dTable = fes.GetElementToDofTable();
   const int* elementMap = e2dTable.GetJ();
   Array<int> faceMap1(dof), faceMap2(dof);
   int e1, e2;
   int inf1, inf2;
   int face_id1, face_id2;
   int orientation;
   const int dof1d = fes.GetFE(0)->GetOrder()+1;
   const int elem_dofs = fes.GetFE(0)->GetDof();
   const int dim = fes.GetMesh()->SpaceDimension();
   // Computation of scatter indices
   int f_ind=0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if (dof_reorder)
      {
         orientation = inf1 % 64; // why 64?
         face_id1 = inf1 / 64;
         GetFaceDofs(dim, face_id1, dof1d, faceMap1); // Only for hex
         orientation = inf2 % 64;
         face_id2 = inf2 / 64;
         GetFaceDofs(dim, face_id2, dof1d, faceMap2); // Only for hex
      }
      else
      {
         mfem_error("FaceRestriction not yet implemented for this type of "
                    "element.");
         // TODO Something with GetFaceDofs?
      }
      if ((type==FaceType::Interior && e2>=0) ||
          (type==FaceType::Boundary && e2<0))
      {
         for (int d = 0; d < dof; ++d)
         {
            const int face_dof = faceMap1[d];
            const int did = face_dof;
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + d;
            scatter_indices1[lid] = gid;
         }
         if (m==L2FaceValues::DoubleValued)
         {
            for (int d = 0; d < dof; ++d)
            {
               if (type==FaceType::Interior && e2>=0) // interior face
               {
                  const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                               orientation, dof1d, d);
                  const int face_dof = faceMap2[pd];
                  const int did = face_dof;
                  const int gid = elementMap[e2*elem_dofs + did];
                  const int lid = dof*f_ind + d;
                  scatter_indices2[lid] = gid;
               }
               else if (type==FaceType::Boundary && e2<0) // true boundary face
               {
                  const int lid = dof*f_ind + d;
                  scatter_indices2[lid] = -1;
               }
            }
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   // Computation of gather_indices
   for (int i = 0; i <= ndofs; ++i)
   {
      offsets[i] = 0;
   }
   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         orientation = inf1 % 64;
         face_id1 = inf1 / 64;
         GetFaceDofs(dim, face_id1, dof1d, faceMap1);
         orientation = inf2 % 64;
         face_id2 = inf2 / 64;
         GetFaceDofs(dim, face_id2, dof1d, faceMap2);

         for (int d = 0; d < dof; ++d)
         {
            const int did = faceMap1[d];
            const int gid = elementMap[e1*elem_dofs + did];
            ++offsets[gid + 1];
         }
         if (m==L2FaceValues::DoubleValued)
         {
            for (int d = 0; d < dof; ++d)
            {
               if (type==FaceType::Interior && e2>=0) // interior face
               {
                  const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                               orientation, dof1d, d);
                  const int did = faceMap2[pd];
                  const int gid = elementMap[e2*elem_dofs + did];
                  ++offsets[gid + 1];
               }
            }
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = 1; i <= ndofs; ++i)
   {
      offsets[i] += offsets[i - 1];
   }
   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         orientation = inf1 % 64;
         face_id1 = inf1 / 64;
         GetFaceDofs(dim, face_id1, dof1d, faceMap1);
         orientation = inf2 % 64;
         face_id2 = inf2 / 64;
         GetFaceDofs(dim, face_id2, dof1d, faceMap2);
         for (int d = 0; d < dof; ++d)
         {
            const int did = faceMap1[d];
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + d;
            // We don't shift lid to express that it's e1 of f
            gather_indices[offsets[gid]++] = lid;
         }
         if (m==L2FaceValues::DoubleValued)
         {
            for (int d = 0; d < dof; ++d)
            {
               if (type==FaceType::Interior && e2>=0) // interior face
               {
                  const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                               orientation, dof1d, d);
                  const int did = faceMap2[pd];
                  const int gid = elementMap[e2*elem_dofs + did];
                  const int lid = dof*f_ind + d;
                  // We shift lid to express that it's e2 of f
                  gather_indices[offsets[gid]++] = nfdofs + lid;
               }
            }
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = ndofs; i > 0; --i)
   {
      offsets[i] = offsets[i - 1];
   }
   offsets[0] = 0;
}

void L2FaceRestriction::Mult(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;

   if (m==L2FaceValues::DoubleValued)
   {
      auto d_indices1 = scatter_indices1.Read();
      auto d_indices2 = scatter_indices2.Read();
      auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
      auto d_y = Reshape(y.Write(), nd, vd, 2, nf);
      MFEM_FORALL(i, nfdofs,
      {
         const int dof = i % nd;
         const int face = i / nd;
         const int idx1 = d_indices1[i];
         for (int c = 0; c < vd; ++c)
         {
            d_y(dof, c, 0, face) = d_x(t?c:idx1, t?idx1:c);
         }
         const int idx2 = d_indices2[i];
         for (int c = 0; c < vd; ++c)
         {
            d_y(dof, c, 1, face) = idx2==-1 ? 0.0 : d_x(t?c:idx2, t?idx2:c);
         }
      });
   }
   else
   {
      auto d_indices1 = scatter_indices1.Read();
      auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
      auto d_y = Reshape(y.Write(), nd, vd, nf);
      MFEM_FORALL(i, nfdofs,
      {
         const int dof = i % nd;
         const int face = i / nd;
         const int idx1 = d_indices1[i];
         for (int c = 0; c < vd; ++c)
         {
            d_y(dof, c, face) = d_x(t?c:idx1, t?idx1:c);
         }
      });
   }
}

void L2FaceRestriction::MultTranspose(const Vector& x, Vector& y) const
{
   // Assumes all elements have the same number of dofs
   const int nd = dof;
   const int vd = vdim;
   const bool t = byvdim;
   const int dofs = nfdofs;
   auto d_offsets = offsets.Read();
   auto d_indices = gather_indices.Read();

   if (m == L2FaceValues::DoubleValued)
   {
      auto d_x = Reshape(x.Read(), nd, vd, 2, nf);
      auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
      MFEM_FORALL(i, ndofs,
      {
         const int offset = d_offsets[i];
         const int nextOffset = d_offsets[i + 1];
         for (int c = 0; c < vd; ++c)
         {
            double dofValue = 0;
            for (int j = offset; j < nextOffset; ++j)
            {
               int idx_j = d_indices[j];
               bool isE1 = idx_j < dofs;
               idx_j = isE1 ? idx_j : idx_j - dofs;
               dofValue +=  isE1 ?
               d_x(idx_j % nd, c, 0, idx_j / nd)
               :d_x(idx_j % nd, c, 1, idx_j / nd);
            }
            d_y(t?c:i,t?i:c) += dofValue;
         }
      });
   }
   else
   {
      auto d_x = Reshape(x.Read(), nd, vd, nf);
      auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
      MFEM_FORALL(i, ndofs,
      {
         const int offset = d_offsets[i];
         const int nextOffset = d_offsets[i + 1];
         for (int c = 0; c < vd; ++c)
         {
            double dofValue = 0;
            for (int j = offset; j < nextOffset; ++j)
            {
               int idx_j = d_indices[j];
               dofValue +=  d_x(idx_j % nd, c, idx_j / nd);
            }
            d_y(t?c:i,t?i:c) += dofValue;
         }
      });
   }
}

void L2FaceRestriction::FillI(SparseMatrix &mat,
                              SparseMatrix &face_mat) const
{
   const int face_dofs = dof;
   auto d_indices1 = scatter_indices1.Read();
   auto d_indices2 = scatter_indices2.Read();
   auto I = mat.ReadWriteI();
   MFEM_FORALL(fdof, nf*face_dofs,
   {
      const int iE1 = d_indices1[fdof];
      const int iE2 = d_indices2[fdof];
      AddNnz(iE1,I,face_dofs);
      AddNnz(iE2,I,face_dofs);
   });
}

void L2FaceRestriction::FillJAndData(const Vector &ea_data,
                                     SparseMatrix &mat,
                                     SparseMatrix &face_mat) const
{
   const int face_dofs = dof;
   auto d_indices1 = scatter_indices1.Read();
   auto d_indices2 = scatter_indices2.Read();
   auto I = mat.ReadWriteI();
   auto mat_fea = Reshape(ea_data.Read(), face_dofs, face_dofs, 2, nf);
   auto J = mat.WriteJ();
   auto Data = mat.WriteData();
   MFEM_FORALL(fdof, nf*face_dofs,
   {
      const int f  = fdof/face_dofs;
      const int iF = fdof%face_dofs;
      const int iE1 = d_indices1[f*face_dofs+iF];
      const int iE2 = d_indices2[f*face_dofs+iF];
      const int offset1 = AddNnz(iE1,I,face_dofs);
      const int offset2 = AddNnz(iE2,I,face_dofs);
      for (int jF = 0; jF < face_dofs; jF++)
      {
         const int jE1 = d_indices1[f*face_dofs+jF];
         const int jE2 = d_indices2[f*face_dofs+jF];
         J[offset2+jF] = jE1;
         J[offset1+jF] = jE2;
         Data[offset2+jF] = mat_fea(jF,iF,0,f);
         Data[offset1+jF] = mat_fea(jF,iF,1,f);
      }
   });
}

void L2FaceRestriction::AddFaceMatricesToElementMatrices(Vector &fea_data,
                                                         Vector &ea_data) const
{
   const int face_dofs = dof;
   const int elem_dofs = elemDofs;
   const int NE = ne;
   if (m==L2FaceValues::DoubleValued)
   {
      auto d_indices1 = scatter_indices1.Read();
      auto d_indices2 = scatter_indices2.Read();
      auto mat_fea = Reshape(fea_data.Read(), face_dofs, face_dofs, 2, nf);
      auto mat_ea = Reshape(ea_data.ReadWrite(), elem_dofs, elem_dofs, ne);
      MFEM_FORALL(f, nf,
      {
         const int e1 = d_indices1[f*face_dofs]/elem_dofs;
         const int e2 = d_indices2[f*face_dofs]/elem_dofs;
         for (int j = 0; j < face_dofs; j++)
         {
            const int jB1 = d_indices1[f*face_dofs+j]%elem_dofs;
            for (int i = 0; i < face_dofs; i++)
            {
               const int iB1 = d_indices1[f*face_dofs+i]%elem_dofs;
               AtomicAdd(mat_ea(iB1,jB1,e1), mat_fea(i,j,0,f));
            }
         }
         if (e2 < NE)
         {
            for (int j = 0; j < face_dofs; j++)
            {
               const int jB2 = d_indices2[f*face_dofs+j]%elem_dofs;
               for (int i = 0; i < face_dofs; i++)
               {
                  const int iB2 = d_indices2[f*face_dofs+i]%elem_dofs;
                  AtomicAdd(mat_ea(iB2,jB2,e2), mat_fea(i,j,1,f));
               }
            }
         }
      });
   }
   else
   {
      auto d_indices = scatter_indices1.Read();
      auto mat_fea = Reshape(fea_data.Read(), face_dofs, face_dofs, nf);
      auto mat_ea = Reshape(ea_data.ReadWrite(), elem_dofs, elem_dofs, ne);
      MFEM_FORALL(f, nf,
      {
         const int e = d_indices[f*face_dofs]/elem_dofs;
         for (int j = 0; j < face_dofs; j++)
         {
            const int jE = d_indices[f*face_dofs+j]%elem_dofs;
            for (int i = 0; i < face_dofs; i++)
            {
               const int iE = d_indices[f*face_dofs+i]%elem_dofs;
               AtomicAdd(mat_ea(iE,jE,e), mat_fea(i,j,f));
            }
         }
      });
   }
}

int ToLexOrdering(const int dim, const int face_id, const int size1d,
                  const int index)
{
   switch (dim)
   {
      case 1:
         return 0;
      case 2:
         return ToLexOrdering2D(face_id, size1d, index);
      case 3:
         return ToLexOrdering3D(face_id, size1d, index%size1d, index/size1d);
      default:
         mfem_error("Unsupported dimension.");
         return 0;
   }
}

L2FaceNormalDRestriction::L2FaceNormalDRestriction(const FiniteElementSpace &fes,
                                     const FaceType type,
                                     const L2FaceValues m)
   : fes(fes),
     nf(fes.GetNFbyType(type)),
     ne(fes.GetNE()),
     vdim(fes.GetVDim()),
     byvdim(fes.GetOrdering() == Ordering::byVDIM),
     ndofs(fes.GetNDofs()),
     dof1d(fes.GetFE(0)->GetOrder()+1),
     dof(nf > 0 ?
         fes.GetTraceElement(0, fes.GetMesh()->GetFaceBaseGeometry(0))->GetDof()
         : 0),
     elemDofs(fes.GetFE(0)->GetDof()),
     m(m),
     nfdofs(nf*dof*dof1d),
     scatter_indices1(nf*dof*dof1d),
     scatter_indices_tan1(nf*dof*dof1d),
     scatter_indices_tan2(nf*dof*dof1d),
     scatter_indices2(m==L2FaceValues::DoubleValued?nf*dof*dof1d:0),
     scatter_indices2_tan1(m==L2FaceValues::DoubleValued?nf*dof*dof1d:0),
     scatter_indices2_tan2(m==L2FaceValues::DoubleValued?nf*dof*dof1d:0),
     offsets_nor(ndofs+1),
     offsets_tan1(ndofs+1),
     offsets_tan2(ndofs+1),
     gather_indices_nor((m==L2FaceValues::DoubleValued? 2 : 1)*nf*dof*dof1d),
     gather_indices_tan1((m==L2FaceValues::DoubleValued? 2 : 1)*nf*dof*dof1d),
     gather_indices_tan2((m==L2FaceValues::DoubleValued? 2 : 1)*nf*dof*dof1d)
{
}

L2FaceNormalDRestriction::L2FaceNormalDRestriction(const FiniteElementSpace &fes,
                                     const ElementDofOrdering e_ordering,
                                     const FaceType type,
                                     const IntegrationRule* ir,
                                     const L2FaceValues m)
   : L2FaceNormalDRestriction(fes, type, m)
{
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
      //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   //std::cout << "dof1d " << dof1d << std::endl;
   //std::cout << "dof " << dof << std::endl;

   // If fespace == L2
   const FiniteElement *fe = fes.GetFE(0);
   const TensorBasisElement *tfe = dynamic_cast<const TensorBasisElement*>(fe);

   MFEM_VERIFY(tfe != NULL, 
               "Element type incompatible with partial assembly. ");
   MFEM_VERIFY(tfe->GetBasisType()==BasisType::GaussLobatto ||
               tfe->GetBasisType()==BasisType::Positive,
               "Only Gauss-Lobatto and Bernstein basis are supported in "
               "L2FaceNormalDRestriction.");
   MFEM_VERIFY(fes.GetMesh()->Conforming(),
               "Non-conforming meshes not yet supported with partial assembly.");
   if (nf==0) { return; }
   // Operator parameters
   height = (m==L2FaceValues::DoubleValued? 2 : 1)*vdim*nfdofs*2;
   width = fes.GetVSize();

   const bool dof_reorder = (e_ordering == ElementDofOrdering::LEXICOGRAPHIC);
   if (!dof_reorder)
   {
      mfem_error("Non-Tensor L2FaceRestriction not yet implemented.");
   }
   if (dof_reorder && nf > 0)
   {
      for (int f = 0; f < fes.GetNF(); ++f)
      {
         const FiniteElement *fe =
            fes.GetTraceElement(f, fes.GetMesh()->GetFaceBaseGeometry(f));
         const TensorBasisElement* el =
            dynamic_cast<const TensorBasisElement*>(fe);
         if (el) { continue; }
         mfem_error("Finite element not suitable for lexicographic ordering");
      }
   }
   const Table& e2dTable = fes.GetElementToDofTable();
   const int* elementMap = e2dTable.GetJ();
   Array<int> facemapnorself(dof*dof1d), facemapnorother(dof*dof1d);
   Array<int> facemaptan1self(dof*dof1d), facemaptan2self(dof*dof1d);
   Array<int> facemaptan1other(dof*dof1d), facemaptan2other(dof*dof1d);
   int e1, e2;
   int inf1, inf2;
   int face_id1, face_id2;
   int orientation1;
   int orientation2;
   const int elem_dofs = fes.GetFE(0)->GetDof();
   const int dim = fes.GetMesh()->SpaceDimension();

   Vector bf;
   Vector gf;
   // Initialize face restriction operators
   bf.SetSize(dof1d); 
   gf.SetSize(dof1d);
   // Needs to be dof1d* dofs_per_face *nf
   Bf.SetSize( dof1d*dof1d*nf*2*3, Device::GetMemoryType());
   Gf.SetSize( dof1d*dof1d*nf*2*3, Device::GetMemoryType());
   bf = 0.;
   gf = 0.;
   Bf = 0.;
   Gf = 0.;
   IntegrationPoint zero;
   double zeropt[1] = {0};
   zero.Set(zeropt,1);
   auto u_face    = Reshape(Bf.Write(), dof1d, dof, nf, 2);
   auto dudn_face = Reshape(Gf.Write(), dof1d, dof, nf, 2, 3);

   //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   //std::cout << "i p B G" << std::endl;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

   // Computation of scatter indices
   int f_ind=0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if (dof_reorder)
      {

         Array<int> V, E, Eo;
         fes.GetMesh()->GetElementVertices(e1, V);
         fes.GetMesh()->GetElementEdges(e1, E, Eo);
         //std::cout << "%{ " << std::endl;
         V.Print();
         E.Print();
         Eo.Print();
         //std::cout << "%} " << std::endl;

         orientation1 = inf1 % 64; // why 64?
         face_id1 = inf1 / 64;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetNormalDFaceDofStencil(dim, face_id1, dof1d, facemapnorself);
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         orientation2 = inf2 % 64;
         face_id2 = inf2 / 64;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetNormalDFaceDofStencil(dim, face_id2, dof1d, facemapnorother);
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetTangentDFaceDofStencil(dim, face_id1, dof1d, facemaptan1self, facemaptan2self); 
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetTangentDFaceDofStencil(dim, face_id2, dof1d, facemaptan1other, facemaptan2other); 
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

         // dont forget 3d piece 
         
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         orientation1 = Eo[face_id1];
         //orientation2 = Eo[face_id2];
         //std::cout << "%{ " << std::endl;
         //std::cout << " face_id1 " << face_id1 << std::endl;
         //std::cout << " orientation1 " << orientation1 << std::endl;
         for( int k = 0 ; k < dof1d ; k++ )
         {
            //std::cout <<  facemapnorself[k] << "   " << facemaptan1self[k] << "  " << facemaptan2self[k] << std::endl; 
         }
         //std::cout << "%} " << std::endl;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;


      }
      else
      {
         mfem_error("FaceRestriction not yet implemented for this type of "
                    "element.");
      }

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
      // port int_type_match 
      if ((type==FaceType::Interior && e2>=0) ||
          (type==FaceType::Boundary && e2<0))
      {

         FaceElementTransformations &Trans0 =
         *fes.GetMesh()->GetFaceElementTransformations(f);

         const FiniteElement &el1 =
         *fes.GetTraceElement(e1, fes.GetMesh()->GetFaceBaseGeometry(f));
         const FiniteElement &el2 =
         *fes.GetTraceElement(e2, fes.GetMesh()->GetFaceBaseGeometry(f));

         const FiniteElement &elf1 = *fes.GetFE(e1);
         const FiniteElement &elf2 = *fes.GetFE(e2);

         // may need bf, gf for elf2?
         elf1.Calc1DShape(zero, bf, gf);
         gf *= -1.0;

         if (ir == NULL)
         {
            // a simple choice for the integration order; is this OK?
            int order;
            if (type==FaceType::Interior && e2>=0)
            {
               order = 2*std::max(el1.GetOrder(), el2.GetOrder());
            }
            else
            {
               order = 2*el1.GetOrder();
            }
            ir = &IntRules.Get(Trans0.GetGeometryType(), order);
         }
         //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

         IntegrationRule ir_glob;
         QuadratureFunctions1D quad;

         quad.GaussLobatto(1+el1.GetOrder(),&ir_glob);
         IntegrationRule ir_glob2(ir_glob,ir_glob);
         Vector realpt;
         realpt.SetSize(2);
         int NP2 = ir_glob2.GetNPoints();

         for(int p = 0; p < NP2 ; p++ )
         {
            IntegrationPoint &ip2p = ir_glob2.IntPoint(p); 
            fes.GetMesh()->GetElementTransformation(e1)->Transform(ip2p,realpt);
            //std::cout << "% ips " << p << " " << ip2p.x << " " << ip2p.y 
                      //<< " realps " << realpt(0) << " " << realpt(1) << std::endl;
         }

         Vector locR;
         // Initialize face restriction operators
         locR.SetSize(dof1d);

         Vector locR2;
         // Initialize face restriction operators
         locR2.SetSize(dof1d);

         int NP = ir_glob.GetNPoints();
         int NP1d = ir_glob.GetNPoints();
         //std::cout << "% el1.GetOrder() = " << el1.GetOrder() << std::endl;
         //std::cout << "% NP = " << NP << std::endl;
         //std::cout << "% NP1d = " << NP1d << std::endl;

         // Loop over integration points on the the face
         for (int p = 0; p < NP; p++) 
         {
            // For each of these, we need an inner 1D loop
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

            Vector locRn;
            locRn.SetSize(2);
            locRn = 0.0;

            Vector locRt1;
            locRt1.SetSize(2);
            locRt1 = 0.0;

            Vector locRn_2;
            locRn_2.SetSize(2);
            locRn_2 = 0.0;

            Vector locRt1_2;
            locRt1_2.SetSize(2);
            locRt1_2 = 0.0;

            //Vector locRt2;
            //locRt2.SetSize(2);


      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

            IntegrationPoint &pb = ir_glob2.IntPoint(facemapnorself[p*dof1d+0]); 
            IntegrationPoint &pb2 = ir_glob2.IntPoint(facemapnorother[p*dof1d+0]); 

            // my use of getElement1Transformation seemed to give the wrong pointmatrix
            //Trans0.GetElement1Transformation().Transform(pb,locR);

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
            fes.GetMesh()->GetElementTransformation(e1)->Transform(pb,locR);

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
            if (type==FaceType::Interior && e2>=0)
            {
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
      //std::cout << "% e2 " << e2 << std::endl;
               fes.GetMesh()->GetElementTransformation(e2)->Transform(pb2,locR2);
            }

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

            // combine these into one loop
            for (int l = 0; l < NP1d; l++)
            {
               const int pn  = facemapnorself[p*dof1d+l];
               const int pt1 = facemaptan1self[p*dof1d+l];

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               IntegrationPoint &ip_nor = ir_glob2.IntPoint(pn); 
               IntegrationPoint &ip_tan1 = ir_glob2.IntPoint(pt1); 

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;




      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               Vector this_locRn;
               this_locRn.SetSize(2);
               this_locRn = 0.0;

               Vector this_locRt1;
               this_locRt1.SetSize(2);
               this_locRt1 = 0.0;


      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               fes.GetMesh()->GetElementTransformation(e1)->Transform(ip_nor , this_locRn );
               fes.GetMesh()->GetElementTransformation(e1)->Transform(ip_tan1, this_locRt1);
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               int lid_t1shape_pt = GetLid(p, l, dof1d, dof, f_ind, facemaptan1self);
               int lid_nshape_pt = GetLid(p, l, dof1d, dof, f_ind, facemapnorself);

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               this_locRn *= gf(l);
               locRn += this_locRn;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               this_locRt1 *= gf(l);
               locRt1 += this_locRt1;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

               if (type==FaceType::Interior && e2>=0)
               {
                  const int pn_2  = facemapnorother[p*dof1d+l];
                  const int pt1_2 = facemaptan1other[p*dof1d+l];

                  IntegrationPoint &ip_nor_2 = ir_glob2.IntPoint(pn_2); 
                  IntegrationPoint &ip_tan1_2 = ir_glob2.IntPoint(pt1_2); 

                  Vector this_locRn_2;
                  this_locRn_2.SetSize(2);
                  this_locRn_2 = 0.0;

                  Vector this_locRt1_2;
                  this_locRt1_2.SetSize(2);
                  this_locRt1_2 = 0.0;
                  
                  fes.GetMesh()->GetElementTransformation(e2)->Transform(ip_nor_2 , this_locRn_2 );
                  fes.GetMesh()->GetElementTransformation(e2)->Transform(ip_tan1_2, this_locRt1_2);

                  int lid_t1shape_pt_2 = GetLid(p, l, dof1d, dof, f_ind, facemaptan1other);
                  int lid_nshape_pt_2 = GetLid(p, l, dof1d, dof, f_ind, facemapnorother);

                  this_locRn_2 *= gf(l);
                  locRn_2 += this_locRn_2;
                  this_locRt1_2 *= gf(l);
                  locRt1_2 += this_locRt1_2;
               }


               //const int pt2 = facemaptan2self[p*dof1d+l];
               //IntegrationPoint &ip_tan2 = ir_glob2.IntPoint(pt2); 


/*
               // Need to figure out how this will work in 3D
               IntegrationPoint &ip3 = ir_glob.IntPoint(l);                

               //std::cout << "%{ " << std::endl; 
               //std::cout << " l = " << l << std::endl;
               //std::cout << " ip3(:," << int(lid)+1 << ") = [" << ip3.x << " , " << ip3.y << "];" << std::endl;
               //std::cout << " this_locRt1(:," << int(lid)+1 << ") = [" << this_locRt1(0) << " , " << this_locRt1(1)<< "];" << std::endl;
               //std::cout << "%} " << std::endl; 
*/

               //lid_t2shape_pt = GetLid(p, l, dof1d, dof, f_ind, facemaptan2self);

               //gid_nshape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemapnorself, elementMap);
               //gid_t1shape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemaptan1self, elementMap);
               //gid_t2shape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemaptan2self, elementMap);      
            }

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

            locRn /= locRn.Norml2();
            locRt1 /= locRt1.Norml2();


            if (type==FaceType::Interior && e2>=0)
            {
               locRn_2 /= locRn_2.Norml2();
               locRt1_2 /= locRt1_2.Norml2();
            }

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
            //std::cout << "%                      tangent vector " << std::endl;
/*
            for (int l = 0; l < NP1d; l++)
            {
               //const int pt2 = facemaptan2self[p*dof1d+l];
               //IntegrationPoint &ip_tan2 = ir_glob2.IntPoint(pt2); 
*/
/*
               // Need to figure out how this will work in 3D
               IntegrationPoint &ip3 = ir_glob.IntPoint(l);                

               //std::cout << "%{ " << std::endl; 
               //std::cout << " l = " << l << std::endl;
               //std::cout << " ip3(:," << int(lid)+1 << ") = [" << ip3.x << " , " << ip3.y << "];" << std::endl;
               //std::cout << " this_locRt1(:," << int(lid)+1 << ") = [" << this_locRt1(0) << " , " << this_locRt1(1)<< "];" << std::endl;
               //std::cout << "%} " << std::endl; 
*/

               //lid_t2shape_pt = GetLid(p, l, dof1d, dof, f_ind, facemaptan2self);

               //gid_nshape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemapnorself, elementMap);
               //gid_t1shape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemaptan1self, elementMap);
               //gid_t2shape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemaptan2self, elementMap);      


//            }
            
            const int lid = GetLid(p, 0, dof1d, dof, f_ind, facemapnorself);
            const int gid = GetGid(p, 0, dof1d, e1, elem_dofs, facemapnorself, elementMap);

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;


            const int lid2 = (type==FaceType::Interior && e2>=0)? GetLid(p, 0, dof1d, dof, f_ind, facemapnorother):0;
            const int gid2 = (type==FaceType::Interior && e2>=0)? GetGid(p, 0, dof1d, e2, elem_dofs, facemapnorother, elementMap):0;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

            //std::cout << "%physical ip for restrictions  info = " << inf1  << std::endl;
            //std::cout << "% p = " <<  p << ";" << std::endl;
            //std::cout << "% e1 = " <<  e1 << ";" << std::endl;
            //std::cout << "% e2 = " <<  e2 << ";" << std::endl;
            //std::cout << "% lid = " <<  lid << ";" << std::endl;
            //std::cout << "% face_no = " << f << ";" << std::endl;
            //std::cout << "% f_ind = " <<  f_ind << ";" << std::endl;
            //std::cout << "% p*dof1d+0 = " <<  p*dof1d+0 << ";" << std::endl;
            //std::cout << "% facemapnorself[p*dof1d+0] = " <<  facemapnorself[p*dof1d+0] << ";" << std::endl;

            //std::cout << "pipror(:," << int(lid)+1 << ") = [" <<  orientation1 << "];" << std::endl;
            //std::cout << "pipror(:," << int(lid)+1 << ") = [" <<  orientation1 << "];" << std::endl;
            //std::cout << "pipr2xy(:," << int(lid)+1 << ") = [" <<  pb.x << " , " << pb.y << "];" << std::endl;
            //std::cout << "pipRt1(:," << int(lid)+1 << ") = [" <<  locRt1(0) << " , " << locRt1(1) << "];" << std::endl;
            //std::cout << "pipRn(:," << int(lid)+1 << ") = [" <<  locRn(0) << " , " << locRn(1) << "];" << std::endl;
            //std::cout << "piploc(:," << int(lid)+1 << ") = [" <<  locR(0) << " , " << locR(1) << "];" << std::endl;

            //if(int(lid)+1 == 2 )
            //  exit(1);


/*
            }
            /*
            const int face_dof = facemapnorself[p*dof1d+0];
            const int did = face_dof;
            const int gid = elementMap[e1*elem_dofs + did];
            const int lid = dof*f_ind + p ;
            */
            /*

            int s = 1;
            int Ns = 0;
            int pflip = p;

            Vector locR;
            locR.SetSize(2);

            if( face_id1 == 0)
            {
               
            }else if( face_id1 == 1){

            }else if( face_id1 == 2){
               pflip = NP - 1 - pflip;
            }else if( face_id1 == 3){

            }
            IntegrationPoint &ip2 = ir_glob.IntPoint(pflip); 
            // This is messy

            //std::cout << "% face_id1 " << face_id1 << std::endl;
            if( face_id1 == 0)
            {
               ip2.x = 0.0;
               ip2.y = 1.0;
            }else if( face_id1 == 1){
               ip2.x = 0.0;
               ip2.y = 1.0;
            }else if( face_id1 == 2){
               ip2.x = 0.0;
               ip2.y = 1.0;
            }else if( face_id1 == 3){
               ip2.x = 0.0;
               ip2.y = 1.0;
            }
            // Physical location of ip2
            Trans0.Transform(ip2,locR);
            */

/// need to look into getting tangent from the element transformation
// can I just create an orthogonal coordinate system in reference space?
/*


         Trans.Loc2.Transform(ip, eip2);
         test_fe2.CalcVShape(eip2, shape2);
         Trans.Loc2.Transf.SetIntPoint(&ip);
         CalcOrtho(Trans.Loc2.Transf.Jacobian(), normal);
*/

            // Gf for ip2

/*
            //std::cout << "p = " << p << std::endl;
            //std::cout << "face_dof = " << face_dof << std::endl;
            //std::cout << "gid = " << gid << std::endl;
            //std::cout << "lid = " << lid << std::endl;
            */

//          Vector vert_coord;
//          el1.GetVertices(vert_coord);

            //Array<int> vertices;
            //el1.GetElementVertices(0, vertices);
            
            //std::cout << "vertices = " << vertices[0] << " " << vertices[1] << std::endl;

/*
            IntegrationPoint ip2;
            double sr = 1.0/sqrt(3.0);

            ip2.x = (ip.x + sr/2.0 - 1.0/2.0)/sr ;
            ip2.x = (orientation1==1)? ip2.x : (1.0-ip2.x) ;

            Trans0.SetAllIntPoints(&ip2);
            Vector shape;
            shape.SetSize(2);
            */
/*

            ip2.x = (ip.x + sr/2.0 - 1.0/2.0)/sr ;
            ip2.x = (orientation1==1)? ip2.x : (1.0-ip2.x) ;
            */
            //std::cout << "ipw(" << int(lid)+1 << ") = " <<  pb.weight << ";" << std::endl;
            //std::cout << "trw(" << int(lid)+1 << ") = " <<  Trans0.Elem1->Weight() << ";" << std::endl;

            const FaceGeometricFactors *geom = fes.GetMesh()->GetFaceGeometricFactors(
                           ir_glob,
                           FaceGeometricFactors::DETERMINANTS |
                           FaceGeometricFactors::NORMALS, type);

            auto truenor = Reshape(geom->normal.Read(), NP, dim, nf);
            Vector facenorm; // change this to use gf*pts
            facenorm.SetSize(2);
            facenorm(0) = truenor(p,0,f_ind);
            facenorm(1) = truenor(p,1,f_ind);
            
            Vector norm; 
            norm.SetSize(2);
            Vector refnorm; 
            refnorm.SetSize(2);
            refnorm(0) = 0;
            refnorm(1) = 1;

            CalcOrtho(Trans0.Jacobian(), norm);
            //detJ.Mult(refnorm,norm);
            //std::cout << "%{" << std::endl;
            //std::cout << "facenorm" << std::endl;
            facenorm.Print(std::cout,2);

            //std::cout << "detJ" << std::endl;
            //detJ.Print(std::cout,4);
           // //std::cout << "normal" << std::endl;
            //Vector normp;
            //Vector norm;
            //normp = geom->normal;
            /*
            norm.SetSize(2);
            norm(0) = 
            norm(1) = normp(p,1,f_ind);
            norm.Print(std::cout,1);
            */
                        /*
            Vector tangent; // change this to use gf*pts
            //std::cout << "tangent" << std::endl;
            tangent.SetSize(2);
            tangent(0) = norm(1);
            tangent(1) = -norm(0);
            tangent.Print(std::cout,2);
*/

/*
            //std::cout << "%}" << std::endl;

            //std::cout << "truenormal(" << int(lid)+1 << ",:) = [" << facenorm(0) << "," << facenorm(1) << "];" << std::endl;
            //std::cout << "refnormal(" << int(lid)+1 << ",:) = [" << locRn(0) << "," << locRn(1) << "];" << std::endl;
            //std::cout << "reftangent(" << int(lid)+1 << ",:) = [" << locRt1(0) << "," << locRt1(1) << "];" << std::endl;

            //std::cout << "detj(" << int(lid)+1 << ") = " << geom->detJ(lid) << ";" << std::endl;
            */
            //std::cout << "detnor(" << int(lid)+1 << ") = " <<  norm(lid) << ";" << std::endl;


 //          //std::cout << "detJ(" << int(lid)+1 << ") = " <<  geom->detJ(lid) << ";" << std::endl;

            //std::cout << std::endl;
  // auto detJ = Reshape(det_jac.Read(), Q1D, NF); // assumes conforming mesh

//ipw./trw


            /*
            double scaling = geom->detJ(lid)/Trans0.Elem1->Weight();
            double correct = pb.weight/Trans0.Elem1->Weight();
            scaling /= correct;
            */
            double scaling = 1.0/geom->detJ(lid);///pb.weight;
            
            double term = locRn(0)*locRt1(1) - locRn(1)*locRt1(0);
            double coeffn = scaling*(locRt1(1)*facenorm(0)-locRt1(0)*facenorm(1))/term;
            double coefft1 = scaling*(locRn(0)*facenorm(1)-locRn(1)*facenorm(0))/term;
            double coefft2 = scaling*0; // need to fix this for 3D 
            /*
            //std::cout << "% scaling = " << scaling << std::endl;
            //std::cout << "% term = " << term  << std::endl;
            //std::cout << "% coefft1 = " << coefft1 << "    " << locRn(1)*facenorm(0) << "  " << locRn(0)*facenorm(1) << std::endl;
            //std::cout << "% coeffn = " << coeffn << std::endl;
*/
/*
            Vector check;
            check.SetSize(2);
            check(0) = coeffn*locRn(0) + coefft1*locRt1(0);
            check(1) = coeffn*locRn(1) + coefft1*locRt1(1);
            check /= check.Norml2();
            //std::cout << "check(" << int(lid)+1 << ",:) = [" << check(0) << "," << check(1) << "];" << std::endl;
*/
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
            for( int i = 0 ; i < dof1d ; i++ )
            {
               u_face(i,p,f_ind,0) = bf(i);
               dudn_face(i,p,f_ind,0,0) = gf(i)*coeffn;
               dudn_face(i,p,f_ind,0,1) = gf(i)*coefft1;
               if(dim==3)
               {
                  //dudn_face(i,p,f_ind,0,2) = gf(i)*coefft2;
               }

               if(type==FaceType::Interior && e2>=0)
               {
                  double scaling = -1.0/geom->detJ(lid);///pb.weight;
                  
                  double term = locRn_2(0)*locRt1_2(1) - locRn_2(1)*locRt1_2(0);
                  double coeffn = scaling*(locRt1_2(1)*facenorm(0)-locRt1_2(0)*facenorm(1))/term;
                  double coefft1 = scaling*(locRn_2(0)*facenorm(1)-locRn_2(1)*facenorm(0))/term;
                  double coefft2 = scaling*0; // need to fix this for 3D                   
                  u_face(i,p,f_ind,1) = bf(i);
                  dudn_face(i,p,f_ind,1,0) = gf(i)*coeffn;
                  dudn_face(i,p,f_ind,1,1) = gf(i)*coefft1;
                  if(dim==3)
                  {
                     //dudn_face(i,p,f_ind,1,2) = -gf(i)*coefft2;
                  }
               }
//               //std::cout << i << " " << p  << " " << u_face(i,p,f_ind,1) << " " <<  dudn_face(i,p,f_ind,1) << std::endl;

            }

/*
         const int k = i % dof1d;
         const int fdof = (i/dof1d) % dof;
         const int face = i / (dof1d*dof);
         const int idx1 = d_indices1[i];

         for (int c = 0; c < vd; ++c)
         {  
            d_y( fdof , c, 0, face, 0) += d_x(t?c:idx1, t?idx1:c)*u_face(k,fdof,face,0);
            d_y( fdof , c, 0, face, 1) += d_x(t?c:idx1, t?idx1:c)*dudn_face(k,fdof,face,0);
*/
         
            //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

/*
            if (type==FaceType::Interior && e2>=0)
            {
               elf2.Calc1DShape(zero, bf, gf);
               gf *= -1.0;
               for( int i = 0 ; i < dof1d ; i++ )
               {
                  u_face(i,p,f_ind,1) = bf(i);
                  dudn_face(i,p,f_ind,1,0) = gf(i)*scaling;
                  dudn_face(i,p,f_ind,1,1) = gf(i)*scaling;
                  if(dim==3)
                  {
                     dudn_face(i,p,f_ind,1,2) = gf(i)*scaling;
                  }
                  //std::cout << i << " " << p  << " " << u_face(i,p,f_ind,1) << " " <<  dudn_face(i,p,f_ind,1) << std::endl;
               }
            }
*/


         }
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;


         // Compute task-local scatter id for each face dof
         for (int d = 0; d < dof; ++d)
         {
            for (int k = 0; k < dof1d; ++k)
            {

               int gid = GetGid(d, k, dof1d, e1, elem_dofs, facemapnorself, elementMap);
               int lid = GetLid(d, k, dof1d, dof1d*dof, f_ind, facemapnorself, d);

               //const int face_dof = facemapnorself[d*dof1d+k];
               //const int did = face_dof;
               //const int gid = elementMap[e1*elem_dofs + did];
               //const int lid = dof*dof1d*f_ind + dof1d*d + k;
               scatter_indices1[lid] = gid;

               gid = GetGid(d, k, dof1d, e2, elem_dofs, facemapnorother, elementMap);
               lid = GetLid(d, k, dof1d, dof1d*dof, f_ind, facemapnorother, d);
               scatter_indices2[lid] = gid;

/*
               int lid_nshape_pt = GetLid(d, l, dof1d, dof, f_ind, facemapnorself);
               int lid_t1shape_pt = GetLid(p, l, dof1d, dof, f_ind, facemaptan1self);
               //lid_t2shape_pt = GetLid(p, l, dof1d, dof, f_ind, facemaptan2self);

               //gid_nshape_pt = GetGid(p, l, dof1d, e1, elem_dofs, facemapnorself, elementMap);
               //gid_t1shape_pt = GetGid
               */

               {
                  const int gid = GetGid(d, k, dof1d, e1, elem_dofs, facemaptan1self, elementMap);
                  const int lid = GetLid(d, k, dof1d, dof1d*dof, f_ind, facemaptan1self, d);
/*
                  const int face_dof = facemaptan1self[d*dof1d+k];
                  const int did = face_dof;
                  const int gid = elementMap[e1*elem_dofs + did];
                  const int lid = dof*dof1d*f_ind + dof1d*d + k;
                  */
                  scatter_indices_tan1[lid] = gid;
               }

               if( dim==3 )
               {
                  /*
                  const int face_dof = facemaptan2self[d*dof1d+k];
                  const int did = face_dof;
                  const int gid = elementMap[e1*elem_dofs + did];
                  const int lid = dof*dof1d*f_ind + dof1d*d + k;
                  scatter_indices_tan2[lid] = gid;
                  */
               }

               // todo - guard this with (m==L2FaceValues::DoubleValued
               // For double-values face dofs, compute second scatter index
               if (type==FaceType::Interior && e2>=0) // interior face
               {
                  /*
                  const int pd = PermuteDFaceDNormL2(dim, face_id1, face_id2,
                                             orientation, dof1d, d);
                                             */

                                             //should this be or1 or or2 ?
                  const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                             orientation1, dof1d, d);

/*
std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
std::cout << "facemapnorother.Size() = " << facemapnorother.Size() << std::endl;
std::cout << "pd = " << pd << std::endl;
std::cout << "dof = " << dof << std::endl;
std::cout << "k = " << k << std::endl;
std::cout << "pd*dof1d+k = " << pd*dof1d+k << std::endl;
std::cout << "pd*dof1d+dof1d-1-k = " << pd*dof1d+dof1d-1-k << std::endl;
*/

                  const int gid = GetGid(d, (orientation2==1)? k:dof1d-1-k, dof1d, e2, elem_dofs, facemaptan1self, elementMap);
                  const int lid = GetLid(d, (orientation2==1)? k:dof1d-1-k, dof1d, dof1d*dof, f_ind, facemaptan1self, d);
                 // const int face_dof = (orientation1==1)? facemapnorother[pd*dof1d+k]:facemapnorother[pd*dof1d+dof1d-1-k];
                  //const int did = face_dof;
                  //const int gid = elementMap[e2*elem_dofs + did];
                  //const int lid = dof1d*dof*f_ind + dof1d*d + k;
                  scatter_indices2_tan1[lid] = gid;

                  {
                     /*
                     const int face_dof = (orientation1==1)? facemaptan1other[pd*dof1d+k]:facemaptan1other[pd*dof1d+dof1d-1-k];
                     const int did = face_dof;
                     const int gid = elementMap[e2*elem_dofs + did];
                     const int lid = dof1d*dof*f_ind + dof1d*d + k;
                     */
                     const int gid = GetGid(d, (orientation2==1)? k:dof1d-1-k, dof1d, e2, elem_dofs, facemaptan1other, elementMap);
                     const int lid = GetLid(d, (orientation2==1)? k:dof1d-1-k, dof1d, dof1d*dof, f_ind, facemaptan1other, d);

                     scatter_indices2_tan2[lid] = gid;
                  }

                  //needs tangential components
               }
               else if (type==FaceType::Boundary && e2<0) // true boundary face
               {
                  const int lid = dof1d*dof*f_ind + dof1d*d + k;
                  scatter_indices2[lid] = -1;
               }
            }
         }
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

//std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         if (m==L2FaceValues::DoubleValued)
         {
            // Moved above. Can revert later

            /*
            // For double-values face dofs, compute second scatter index
            for (int d = 0; d < dof; ++d)
            {
               if (type==FaceType::Interior && e2>=0) // interior face
               {
                  for (int k = 0; k < dof1d; ++k)
                  {
                     const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                                orientation, dof1d, d);
                     const int face_dof = facemapnorother[pd*dof1d+k];
                     const int did = face_dof;
                     const int gid = elementMap[e2*elem_dofs + did];
                     const int lid = dof1d*dof*f_ind + dof1d*d + k;
                     scatter_indices2[lid] = gid;
                  }
               }
               else if (type==FaceType::Boundary && e2<0) // true boundary face
               {
                  for (int k = 0; k < dof1d; ++k)
                  {
                     const int lid = dof1d*dof*f_ind + dof1d*d + k;
                     scatter_indices2[lid] = -1;
                  }
               }
            }
            */
         }
         f_ind++;
      }
   }
   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   // Computation of gather_indices_*
   for (int i = 0; i <= ndofs; ++i)
   {
      offsets_nor[i] = 0;
      offsets_tan1[i] = 0;
      offsets_tan2[i] = 0;
   }
   //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

         orientation1 = inf1 % 64;
         face_id1 = inf1 / 64;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetNormalDFaceDofStencil(dim, face_id1, dof1d, facemapnorself);
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         orientation2 = inf2 % 64;
         face_id2 = inf2 / 64;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
         GetNormalDFaceDofStencil(dim, face_id2, dof1d, facemapnorother);

      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

         for (int d = 0; d < dof; ++d)
         {
            for (int k = 0; k < dof1d; ++k)
            {
               // dof should be 1d dof, need to check this for 3D
            //std::cout << "size(facemapnorself) = " << dof*dof1d << std::endl;
            //std::cout << "d = " << d << std::endl;
            //std::cout << "k = " << k << std::endl;
            //std::cout << "dof1d = " << dof1d << std::endl;
            //std::cout << "d*dof1d+k = " << d*dof1d+k << std::endl;
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
             //  int did = facemapnorself[d*dof1d+k];
            //std::cout << "did = " << did << std::endl;
            //std::cout << "e1*elem_dofs + did = " << e1*elem_dofs + did << std::endl;
//               int gid = elementMap[e1*elem_dofs + did];
               int gid = GetGid(d, k, dof1d, e1, elem_dofs, facemapnorself, elementMap);                     

            //std::cout << "gid = " << gid << std::endl;

               MFEM_VERIFY(gid+1 < ndofs+1, " something wrong ");

                     ++offsets_nor[gid + 1];

                     //did = facemaptan1self[d*dof1d+k];
                     //gid = elementMap[e1*elem_dofs + did];
                     gid = GetGid(d, k, dof1d, e1, elem_dofs, facemaptan1self, elementMap);                     

               MFEM_VERIFY(gid+1 < ndofs+1, " something wrong ");

               ++offsets_tan1[gid + 1];
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
               //did = facemaptan2self[d*dof1d+k];
               // facemaptan2self is not currently allocated correctly
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
     //std::cout << "% " << did << std::endl;
              // gid = elementMap[e1*elem_dofs + did];
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
              // MFEM_VERIFY(gid+1 < ndofs+1, " something wrong ");
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
                //     ++offsets_tan2[gid + 1];
      std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
            }
         }
         if (m==L2FaceValues::DoubleValued)
         {
            for (int d = 0; d < dof; ++d)
               for (int k = 0; k < dof1d; ++k)
               {
                  if (type==FaceType::Interior && e2>=0) // interior face
                  {
                     /*
                     const int pd2 = PermuteDFaceDNormL2(dim, face_id1, face_id2,
                                                orientation, dof1d, d);
                                                */
                     const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                                orientation1, dof1d, d);
                     /*
                     if( pd != pd2 )
                     {
                        //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
                        //std::cout << "pd = " << pd;
                        //std::cout << "pd2 = " << pd2;
                     }
                     */

                     // correct or1 or or2?
                     //int did  = orientation1==1?facemapnorother[pd*dof1d+k]:facemapnorother[pd*dof1d+dof1d-1-k];
                     //int gid = elementMap[e2*elem_dofs + did];
                     int gid = GetGid(pd, orientation1==1?k:dof1d-1-k, dof1d, e2, elem_dofs, facemapnorother, elementMap);                     
                     ++offsets_nor[gid + 1];

                     //did  = orientation1==1?facemaptan1other[pd*dof1d+k]:facemaptan1other[pd*dof1d+dof1d-1-k];
                     //gid = elementMap[e2*elem_dofs + did];
                     gid = GetGid(pd, orientation1==1?k:dof1d-1-k, dof1d, e2, elem_dofs, facemaptan1other, elementMap);
                     ++offsets_tan1[gid + 1];
                     //did  = orientation1==1?facemaptan2other[pd*dof1d+k]:facemaptan2other[pd*dof1d+dof1d-1-k];
                     //gid = elementMap[e2*elem_dofs + did];
                     //++offsets_tan2[gid + 1];                     
                  }
               }
         }
         f_ind++;
      }
   }

   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = 1; i <= ndofs; ++i)
   {
      offsets_nor[i] += offsets_nor[i - 1];
      offsets_tan1[i] += offsets_tan1[i - 1];
      offsets_tan2[i] += offsets_tan2[i - 1];
   }

/*
   for (int i = 1; i <= ndofs; ++i)
   {
      std::cout << "%offs " << offsets_nor[i] << " " << offsets_tan1[i] << std::endl;
   }
*/
      

   f_ind = 0;
   for (int f = 0; f < fes.GetNF(); ++f)
   {
      fes.GetMesh()->GetFaceElements(f, &e1, &e2);
      fes.GetMesh()->GetFaceInfos(f, &inf1, &inf2);
      if ((type==FaceType::Interior && (e2>=0 || (e2<0 && inf2>=0))) ||
          (type==FaceType::Boundary && e2<0 && inf2<0) )
      {
         orientation1 = inf1 % 64;
         face_id1 = inf1 / 64;
         GetNormalDFaceDofStencil(dim, face_id1, dof1d, facemapnorself);
//         //std::cout << "% face_id1  " << face_id1 << std::endl;
 //        //std::cout << "% facemapnorself[0]  " << facemapnorself[0] << std::endl;

         orientation2 = inf2 % 64;
         face_id2 = inf2 / 64;
         GetNormalDFaceDofStencil(dim, face_id2, dof1d, facemapnorother);

/*
         const FiniteElement &elf1 = *fes.GetFE(e1);
         const FiniteElement &elf2 = *fes.GetFE(e2);

         const IntegrationRule *ir = IntRule;
*/

   //std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
   //std::cout << "dof = " << dof << std::endl;

         for (int d = 0; d < dof; ++d)
            for (int k = 0; k < dof1d; ++k)
            {
               //int did = facemapnorself[d*dof1d+k];

//std::cout << "size(facemapnorself) = " << dof*dof1d << std::endl;
//std::cout << "d = " << d << std::endl;
//std::cout << "k = " << k << std::endl;
//std::cout << "dof = " << dof << std::endl;
//std::cout << "d*dof+k = " << d*dof1d+k << std::endl;
//std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

//std::cout << "e1 = " << e1 << std::endl;
//std::cout << "elem_dofs = " << elem_dofs << std::endl;
//std::cout << "did = " << did << std::endl;
//std::cout << "e1*elem_dofs + did = " << e1*elem_dofs + did << std::endl;

               int gid = GetGid(d, k, dof1d, e1, elem_dofs, facemapnorself, elementMap);
               int lid = GetLid(d, k, dof1d, dof1d*dof, f_ind, facemapnorself, d);
               //int gid = elementMap[e1*elem_dofs + did];
               //std::cout << "gid = " << gid << std::endl;
               //int lid = dof1d*dof*f_ind + dof1d*d + k;
               // We don't shift lid to express that it's e1 of f
               int offset = offsets_nor[gid];
               gather_indices_nor[offset] = lid;
               offsets_nor[gid]++;
               const int did = facemapnorself[d*dof+k];

/*
               std::cout << d << " " << k << " " <<  did << " " << gid << " " <<  lid << " " << offset << std::endl;
               if(  lid == 194 )
               {
                  std::cout << d << " " << k << " " <<  did << " " << gid << " " <<  lid  << " " << offset << " bug? " << std::endl;
                  //exit(1);
               }
               */


               //did = facemaptan1self[d*dof1d+k];
               //gid = elementMap[e1*elem_dofs + did];
               //lid = dof1d*dof*f_ind + dof1d*d + k;

               gid = GetGid(d, k, dof1d, e1, elem_dofs, facemaptan1self, elementMap);
               lid = GetLid(d, k, dof1d, dof1d*dof, f_ind, facemaptan1self, d);

               offset = offsets_tan1[gid];

               //std::cout <<  offset << std::endl;
               gather_indices_tan1[offset] = lid;
               offsets_tan1[gid]++;

            // fix this 
//               did = facemaptan2self[d*dof1d+k];
  //             gid = elementMap[e1*elem_dofs + did];
    //           lid = dof1d*dof*f_ind + dof1d*d + k;
      //         offset = offsets_tan2[gid];
        //       gather_indices_tan2[offset] = lid;
          //     offsets_tan2[gid]++;
            }

         if (m==L2FaceValues::DoubleValued)
         {
            for (int d = 0; d < dof; ++d)
               for (int k = 0; k < dof1d; ++k)
               {
                  if (type==FaceType::Interior && e2>=0) // interior face
                  {
                     
                     const int pd2 = PermuteDFaceDNormL2(dim, face_id1, face_id2,
                                                orientation2, dof1d, d);
                                                
                                                // correct or1 or or2? ?
                     const int pd = PermuteFaceL2(dim, face_id1, face_id2,
                                                orientation2, dof1d, d);

         
                     if( pd != pd2 )
                     {
                        std::cout << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;
                        std::cout << "pd = " << pd;
                        std::cout << "pd2 = " << pd2;
                        exit(1);
                     }                     

                     // correct orientation1 or 2 ? 
                     int gid = GetGid(pd, orientation2==1?k:dof1d-1-k, dof1d, e2, elem_dofs, facemapnorother, elementMap);
                     int lid = GetLid(pd, orientation2==1?k:dof1d-1-k, dof1d, dof1d*dof, f_ind, facemapnorother,d);
                     const int did = facemapnorother[d*dof+(orientation2==1?k:dof1d-1-k)];

                     //int did  = orientation1==1?facemapnorother[pd*dof1d+k]:facemapnorother[pd*dof1d+dof1d-1-k];
                     //int gid = elementMap[e2*elem_dofs + did];
                     //int lid = dof1d*dof*f_ind + dof1d*d + k;
                     // We shift lid to express that it's e2 of f
                     int offset = offsets_nor[gid];

/*
                     std::cout << d << " " << pd << " " << did << " " <<  gid << " " <<  2*nfdofs + lid<< " " << offset << std::endl;
                     if(  2*nfdofs + lid== 194 )
                     {
                        std::cout << d << " " << pd << " " <<  did << " " << gid << " " <<  2*nfdofs + lid<< " " << offset << " bug? " << std::endl;
                        //exit(1);
                     }
                     */


                     gather_indices_nor[offset] = 2*nfdofs + lid;
                     offsets_nor[gid]++;

/*
                     did  = orientation1==1?facemaptan1other[pd*dof1d+k]:facemaptan1other[pd*dof1d+dof1d-1-k];
                     gid = elementMap[e2*elem_dofs + did];
                     lid = dof1d*dof*f_ind + dof1d*d + k;
*/
                     gid = GetGid(pd, orientation1==1?k:dof1d-1-k, dof1d, e2, elem_dofs, facemaptan1other, elementMap);
                     lid = GetLid(pd, orientation1==1?k:dof1d-1-k, dof1d, dof1d*dof, f_ind, facemaptan1self,d);
                     // We shift lid to express that it's e2 of f
                     offset = offsets_tan1[gid];
                     gather_indices_tan1[offset] = 2*nfdofs + lid;
                     offsets_tan1[gid]++;

      /*
                     did  = orientation1==1?facemaptan2other[pd*dof1d+k]:facemaptan2other[pd*dof1d+dof1d-1-k];
                     gid = elementMap[e2*elem_dofs + did];
                     lid = dof1d*dof*f_ind + dof1d*d + k;
                     // We shift lid to express that it's e2 of f
                     offset = offsets_tan2[gid];
                     gather_indices_tan2[offset] = 2*nfdofs + lid;
                     offsets_tan2[gid]++;
                     */
                  }
               }
         }
         f_ind++;
      }
   }
   
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

   MFEM_VERIFY(f_ind==nf, "Unexpected number of faces.");
   for (int i = ndofs; i > 0; --i)
   {
      offsets_nor[i] = offsets_nor[i - 1];
      offsets_tan1[i] = offsets_tan1[i - 1];
      //offsets_tan2[i] = offsets_tan2[i - 1];
   }
   offsets_nor[0] = 0;
   offsets_tan1[0] = 0;
   //offsets_tan2[0] = 0;
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

/*
   for (int i = 0; i < 2*nf*dof*dof1d ; i++)
   {
      std::cout << "gi " << i << " = " <<  gather_indices_nor[i] << std::endl;
   }
*/

}

void L2FaceNormalDRestriction::Mult(const Vector& x, Vector& y) const
{
   /*
   //std::cout << " restrict x" << std::endl;
   x.Print(std::cout,1);
   //std::cout << " end restrict x" << std::endl;
*/
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

   // Assumes all elements have the same number of dofs, nd
   //const int dim = fes.GetMesh()->SpaceDimension();
   const int vd = vdim;
   // is x transposed?
   const bool t = byvdim;
   auto u_face    = Reshape(Bf.Read(), dof1d, dof, nf, 2);
   auto dudn_face = Reshape(Gf.Read(), dof1d, dof, nf, 2, 3);
   y = 0.0;

   if (m==L2FaceValues::DoubleValued)
   {
      auto d_indices1 = scatter_indices1.Read();
      auto d_indicestan1self = scatter_indices_tan1.Read();
      auto d_indices2 = scatter_indices2.Read();
      auto d_indicestan1other = scatter_indices2_tan1.Read();
      auto d_x = Reshape(x.Read(), t?vd:ndofs, t?ndofs:vd);
      auto d_y = Reshape(y.Write(), dof, vd, 2, nf, 2);

      // Loop over all face dofs
      MFEM_FORALL(i, dof1d*dof*vd*nf,
      {
         const int k = i % dof1d;
         const int fdof = (i/dof1d) % dof;
         const int face = i / (dof1d*dof);
         const int idx1 = d_indices1[i];
         const int idxt1 = d_indicestan1self[i];

         for (int c = 0; c < vd; ++c)
         {  
            d_y( fdof , c, 0, face, 0) += d_x(t?c:idx1, t?idx1:c)*u_face(k,fdof,face,0);
            d_y( fdof , c, 0, face, 1) += d_x(t?c:idx1, t?idx1:c)*dudn_face(k,fdof,face,0,0);
            d_y( fdof , c, 0, face, 1) += d_x(t?c:idxt1, t?idxt1:c)*dudn_face(k,fdof,face,0,1);

//            //std::cout << "u1(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 0, face , 0) << "; % bf = "  << u_face(k,fdof,face,0) << " x = " << d_x(t?c:idx1, t?idx1:c) << std::endl;
            //std::cout << "dudn(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 0, face , 1) << ";% gf = " << dudn_face(k,fdof,face,0,0) << " x = " << d_x(t?c:idx1, t?idx1:c) << std::endl;
//            //std::cout << "dudn1(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 0, face , 1) << ";% gfn = " << dudn_face(k,fdof,face,0,0) << " gft = " << dudn_face(k,fdof,face,0,1) << " x = " << d_x(t?c:idx1, t?idx1:c) << std::endl;
         }
         // other side 
         const int idx2 = d_indices2[i];
         const int idxt2 = d_indicestan1other[i];
         if( idx2==-1 )
         {
            for (int c = 0; c < vd; ++c)
            {
               d_y( fdof , c, 1, face, 0) = 0.0;
               d_y( fdof , c, 1, face, 1) = 0.0;
            }
         }
         else
         {
            for (int c = 0; c < vd; ++c)
            {
               d_y( fdof , c, 1, face, 0) += d_x(t?c:idx2, t?idx2:c)*u_face(k,fdof,face,1);
               d_y( fdof , c, 1, face, 1) += d_x(t?c:idx2, t?idx2:c)*dudn_face(k,fdof,face,1,0);
               d_y( fdof , c, 1, face, 1) += d_x(t?c:idxt2, t?idxt2:c)*dudn_face(k,fdof,face,1,1);

//               //std::cout << "u2(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 1, face , 0) << "; % bf = "  << u_face(k,fdof,face,1) << " x = " << d_x(t?c:idx2, t?idx2:c) << std::endl;
               //std::cout << "dudn(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 0, face , 1) << ";% gf = " << dudn_face(k,fdof,face,0,0) << " x = " << d_x(t?c:idx1, t?idx1:c) << std::endl;
//               //std::cout << "dudn2(" << int(i/dof1d)+1 << ") = " <<  d_y( fdof , c, 1, face , 1) << ";% gfn = " << dudn_face(k,fdof,face,1,0) << " gft = " << dudn_face(k,fdof,face,1,1) << " x = " << d_x(t?c:idx2, t?idx2:c) << std::endl;

/*
               d_y( fdof , c, 1, face, 0) += d_x(t?c:idx2, t?idx2:c)*u_face(k,fdof,face,1);
               d_y( fdof , c, 1, face, 1) += d_x(t?c:idx2, t?idx2:c)*dudn_face(k,fdof,face,1,0);
               */
            }
         }
      });

/*
         for( int face = 0 ; face < nf ; face++ )
            for( int fdof = 0 ; fdof < dof ; fdof++ )
            {
               int c = 0;
               //std::cout << "% fid " << fdof << " f " << face << " gu0 " << d_y( fdof , c, 0, face, 1) << " gu1 " << d_y( fdof , c, 1, face, 1) << std::endl;
            }
*/

   }
   else
   {
      mfem_error("not yet implemented.");
   }



/*
   //std::cout << " restrict y" << std::endl;
   y.Print(std::cout,1);
   //std::cout << " end restrict y" << std::endl;
*/

   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

}

void L2FaceNormalDRestriction::MultTranspose(const Vector& x, Vector& y) const
{
   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;

/*
   std::cout << "multT x " << std::endl;
   x.Print(std::cout,1);
   std::cout << "end multT x " << std::endl;
*/

   auto u_face    = Reshape(Bf.Read(), dof1d, dof, nf, 2);
   auto dudn_face = Reshape(Gf.Read(), dof1d, dof, nf, 2, 3);

   // Assumes all elements have the same number of dofs
   const int vd = vdim;
   const bool t = byvdim;
   auto d_offsets_nor = offsets_nor.Read();
   auto d_offsets_tan1 = offsets_tan1.Read();
   auto d_offsets_tan2 = offsets_tan2.Read();
   auto d_indices_nor = gather_indices_nor.Read();
   auto d_indices_tan1 = gather_indices_tan1.Read();
   auto d_indices_tan2 = gather_indices_tan2.Read();
   //auto d_indices_tan2 = gather_indices_tan.Read();

   if (m == L2FaceValues::DoubleValued)
   {
      auto d_x = Reshape(x.Read(), dof, vd, 2, nf, 2);
      auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
      MFEM_FORALL(i, ndofs,
      {
         int offset = d_offsets_nor[i];
         int nextOffset = d_offsets_nor[i + 1];
         for (int c = 0; c < vd; ++c)
         {
            for (int j = offset; j < nextOffset; ++j)
            {
               int idx_j0 = d_indices_nor[j];
               bool isE1 = idx_j0 < nfdofs;
               // we use e1 e2 but then use D0 D1 in the PA kernel 
               int idx_j = isE1 ? idx_j0 : idx_j0 - 2*nfdofs;
               int s = idx_j % dof1d;
               int did = (idx_j/dof1d) % dof1d;
               int faceid = idx_j / (dof1d*dof);

/*
               double gamma = 1.0;//0.5; 

               double b = 0;
               if( s == 0)
                  b = 1;

               double g = 0;
               if( dof1d == 2)
               {
                  if(s==0)
                     g = 1.0;
                  if(s==1)
                     g = -1.0;
               }
               else if( dof1d == 3)
               {
                  if(s==0)
                     g = 3;
                  if(s==1)
                     g = -4;
                  if(s==2)
                     g = 1;
               }
               else
               {
                  if(s==0)
                     g = 11.0;
                  if(s==1)
                     g = -18.0;
                  if(s==2)
                     g = 9.0;
                  if(s==3)
                     g = -2.0;

//                  //std::cout << "error!   dof1d > 3 " << std::endl;
//                  exit(1);
               }
*/

/*
               std::cout << "% Mtrans " 
                     << " offset " << offset
                     << " nextOffset " << nextOffset
                     << " idx_j0 " << idx_j0
                     << " offset " << offset
                     << " offset " << offset
                     << " did " << did 
                     << " faceid " << faceid 
                     << " s " << s 
                     << " dx1 " << d_x( did, c, 0, faceid, 1)
                     << " dx2 " << d_x( did, c, 1, faceid, 1)
                     << " dudn1 " << dudn_face(s,did,faceid,0,0)*gamma
                     << " dudn2 " << dudn_face(s,did,faceid,1,0)*gamma
                     << " b " << b
                     << " g " << g
                     << std::endl;
*/

               if( isE1 )
               {

/*
                  if(  abs(dudn_face(s,did,faceid,0,0)*gamma - g ) > 1.0e-09 )
                  {
                     //std::cout << "error! s = " << s << std::endl;
                     //std::cout << "dudn_face(s,did,faceid,0,0)*gamma = " << dudn_face(s,did,faceid,0,0)*gamma << std::endl;
                     //std::cout << "g = " << g << std::endl;

                     for( int si = 0 ; si < dof1d ; si++ )
                     {
                        //std::cout << "s " << s << " dudn " << dudn_face(si,did,faceid,0,0) << std::endl;
                     }
                     //exit(1);
                  }
                  */

                  d_y(t?c:i,t?i:c) += d_x( did, c, 0, faceid, 0)*u_face(s,did,faceid,0);
                  d_y(t?c:i,t?i:c) += d_x( did, c, 0, faceid, 1)*dudn_face(s,did,faceid,0,0);
               }
               else
               {
                  d_y(t?c:i,t?i:c) += d_x( did, c, 1, faceid, 0)*u_face(s,did,faceid,1);
                  d_y(t?c:i,t?i:c) += d_x( did, c, 1, faceid, 1)*dudn_face(s,did,faceid,1,0);
               }
            }
         }

         offset = d_offsets_tan1[i];
         nextOffset = d_offsets_tan1[i + 1];
         for (int c = 0; c < vd; ++c)
         {
            for (int j = offset; j < nextOffset; ++j)
            {
               int idx_j0 = d_indices_tan1[j];
               bool isE1 = idx_j0 < nfdofs;
               // we use e1 e2 but then use D0 D1 in the PA kernel 
               int idx_j = isE1 ? idx_j0 : idx_j0 - 2*nfdofs;
               int s = idx_j % dof1d;
               int did = (idx_j/dof1d) % dof1d;
               int faceid = idx_j / (dof1d*dof);
               if( isE1 )
               {
                  if( abs(dudn_face(s,did,faceid,0,1)) > 1.0e-11 )
                  {
                     //std::cout << " nonzero dudnt = " << dudn_face(s,did,faceid,0,1) << std::endl;
                     //exit(1);
                  } 
                  d_y(t?c:i,t?i:c) += d_x( did, c, 0, faceid, 1)*dudn_face(s,did,faceid,0,1);
               }
               else
               {

                  if( abs(dudn_face(s,did,faceid,1,1)) > 1.0e-11 )
                  {
                     //std::cout << " nonzero dudnt = " << dudn_face(s,did,faceid,1,1) << std::endl;
                     //exit(1);
                  } 

                  d_y(t?c:i,t?i:c) += d_x( did, c, 1, faceid, 1)*dudn_face(s,did,faceid,1,1);
               }
            }
         }

         // need to do tan2 ... 


      });
   }
   else
   {
      mfem_error("not yet implemented.");
   }

   std::cout << "% " << __LINE__ << " in " << __FUNCTION__ << " in " << __FILE__ << std::endl;


/*

   // Assumes all elements have the same number of dofs
   const int nd = dof*dof; //this is for 2D, need to generalize for 3D
   const int vd = vdim;
   const bool t = byvdim;
   auto d_offsets = offsets_nor.Read();
   auto d_indices = gather_indices_nor.Read();

   //std::cout << "                         nfdofs " << nfdofs << std::endl;
   //std::cout << "                         ndofs " << ndofs << std::endl;
   //std::cout << "                         x.Size() " << x.Size() << std::endl;

   if (m == L2FaceValues::DoubleValued)
   {
      auto d_x = Reshape(x.Read(), dof, vd, 2, nf, 2);
      auto d_y = Reshape(y.Write(), t?vd:ndofs, t?ndofs:vd);
      MFEM_FORALL(i, ndofs,
      {
         const int offset = d_offsets[i];
         const int nextOffset = d_offsets[i + 1];
         for (int c = 0; c < vd; ++c)
         {
            //double dofValue = 0;
            for (int j = offset; j < nextOffset; ++j)
            {
               int idx_j0 = d_indices[j];
               bool isE1 = idx_j0 < nfdofs;
               // we use e1 e2 but then use D0 D1 in the PA kernel 
               int idx_j = isE1 ? idx_j0 : idx_j0 - 2*nfdofs;
               int s = idx_j % dof1d;
               int did = (idx_j/dof1d) % dof1d;
               int faceid = idx_j / nd;
               //std::cout << offset << " " << nextOffset << " " << i << " " << j << " " <<  idx_j0 << " " << isE1 << " " << idx_j << " " << 1 << " " << did << " " << faceid << std::endl ;
               MFEM_VERIFY( idx_j <= 2*nfdofs , " offset is too large? ");
               MFEM_VERIFY( idx_j >= 0 , " offset is negative? ");

               double b = 0;
               if( s == 0)
                  b = 1;

               double g = 0;
               if( dof1d == 2)
               {
                  if(s==0)
                     g = 1.0;
                  if(s==1)
                     g = -1.0;
               }
               else if( dof1d == 3)
               {
                  if(s==0)
                     g = 3;
                  if(s==1)
                     g = -4;
                  if(s==2)
                     g = 1;
               }
               else
               {

               }

               //std::cout << "% Mtrans " 
                     << " offset " << offset
                     << " nextOffset " << nextOffset
                     << " idx_j0 " << idx_j0
                     << " offset " << offset
                     << " offset " << offset
                     << " did " << did 
                     << " faceid " << faceid 
                     << " s " << s 
                     << " dx1 " << d_x( did, c, 0, faceid, 1)
                     << " dx2 " << d_x( did, c, 1, faceid, 1)
                     << " dudn1 " << g
                     << " dudn2 " << 0
                     << std::endl;

               double dofValue = isE1 ? 
               d_x( did, c, 0, faceid, 0 )
               :d_x( did, c, 1, faceid, 0 );
               d_y(t?c:i,t?i:c) += b*dofValue;

               double dofValueprime = isE1 ? 
               d_x( did, c, 0, faceid, 1 )
               :d_x( did, c, 1, faceid, 1 );
               d_y(t?c:i,t?i:c) += g*dofValueprime;

            }
         }
      });
   }
   else
   {
      mfem_error("not yet implemented.");
   }

*/


/*
   std::cout << "multT y " << std::endl;
   y.Print(std::cout,1);
   std::cout << "end multT y " << std::endl;
*/
}

}