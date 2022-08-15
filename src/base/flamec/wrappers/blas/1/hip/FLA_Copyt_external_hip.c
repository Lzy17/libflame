/*

    Copyright (C) 2014, The University of Texas at Austin
    Copyright (C) 2022, Advanced Micro Devices, Inc.

    This file is part of libflame and is available under the 3-Clause
    BSD license, which can be found in the LICENSE file at the top-level
    directory, or at http://opensource.org/licenses/BSD-3-Clause

*/

#include "FLAME.h"

#ifdef FLA_ENABLE_HIP

#include "hip/hip_runtime_api.h"
#include "rocblas.h"

FLA_Error FLA_Copyt_external_hip( rocblas_handle handle, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  printf("ENTER COPYT!!!!!!!!!!!!!!!!!\n");
  if ( FLA_Check_error_level() == FLA_FULL_ERROR_CHECKING ) 
    FLA_Copy_check( A, B );

  if ( FLA_Obj_has_zero_dim( A ) ) return FLA_SUCCESS;

  FLA_Datatype dt_A;
  FLA_Datatype dt_B;

  dt_A     = FLA_Obj_datatype( A );
  dt_B     = FLA_Obj_datatype( B );

  dim_t elem_size = FLA_Obj_elem_size( B );
  dim_t ldim_A    = FLA_Obj_col_stride( A );

  dim_t m_B       = FLA_Obj_length( B );
  dim_t n_B       = FLA_Obj_width( B );
  dim_t ldim_B    = FLA_Obj_col_stride( B );

  size_t dpitch   = elem_size * ldim_B;
  size_t spitch   = elem_size * ldim_A;
  size_t width    = elem_size * m_B;
  size_t height   = n_B;

  void* A_mat = NULL;
  void* B_mat = NULL;
  if ( FLASH_Queue_get_malloc_managed_enabled_hip( ) )
  {
    A_mat = FLA_Obj_buffer_at_view( A );
    B_mat = FLA_Obj_buffer_at_view( B );
  }
  else
  {
    A_mat = A_hip;
    B_mat = B_hip;
  }

  //hipStream_t stream;
  //rocblas_get_stream( handle, &stream );
  //hipStream_t stream = (hipStream_t) 0;
  // rocblas_get_stream( handle, &stream );
  // hipMemcpy2D assumes row-major layout
  hipError_t err = 0;
  if ( dt_A == FLA_FLOAT ){

    err = hip_fcopyt( handle, A_mat, B_mat, width, height, dpitch, spitch);
  
  }
  else if ( dt_A == FLA_DOUBLE ){

    err = hip_dcopyt( handle, A_mat, B_mat, width, height, dpitch, spitch);
  
  }
  else{

    err = 3;
    fprintf(stderr, "type not supported yet\n");

  }


  if ( err != hipSuccess )
  {
    fprintf( stderr, "Failure to memcpy D2D in HIP: %d\n", err);
    return FLA_FAILURE;
  }

  return FLA_SUCCESS;
}

#endif
