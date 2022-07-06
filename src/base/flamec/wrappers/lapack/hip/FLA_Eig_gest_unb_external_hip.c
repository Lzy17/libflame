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
#include "rocsolver.h"





FLA_Error FLA_Eig_gest_il_unb_ext_hip( rocblas_handle handle, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  return FLA_Eig_gest_unb_external_hip( handle, FLA_INVERSE, FLA_LOWER_TRIANGULAR, A, A_hip, B, B_hip );
}

FLA_Error FLA_Eig_gest_iu_unb_ext_hip( rocblas_handle handle, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  return FLA_Eig_gest_unb_external_hip( handle, FLA_INVERSE, FLA_UPPER_TRIANGULAR, A, A_hip, B, B_hip );
}

FLA_Error FLA_Eig_gest_nl_unb_ext_hip( rocblas_handle handle, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  return FLA_Eig_gest_unb_external_hip( handle, FLA_NO_INVERSE, FLA_LOWER_TRIANGULAR, A, A_hip, B, B_hip );
}

FLA_Error FLA_Eig_gest_nu_unb_ext_hip( rocblas_handle handle, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  return FLA_Eig_gest_unb_external_hip( handle, FLA_NO_INVERSE, FLA_UPPER_TRIANGULAR, A, A_hip, B, B_hip );
}




FLA_Error FLA_Eig_gest_unb_external_hip( rocblas_handle handle, FLA_Inv inv, FLA_Uplo uplo, FLA_Obj A, void* A_hip, FLA_Obj B, void* B_hip )
{
  //FLA_Error    r_val = FLA_SUCCESS;
  rocblas_eform  itype;
  //int          info;

  FLA_Datatype datatype;
  int          n_A, ld_A;
  int          ld_B;

  if ( FLA_Check_error_level() == FLA_FULL_ERROR_CHECKING )
    FLA_Eig_gest_check( inv, uplo, A, B );

//  if ( FLA_Obj_has_zero_dim( A ) ) return FLA_SUCCESS;
  if ( inv == FLA_INVERSE )
    itype  = rocblas_eform_ax;
  else
    itype  = rocblas_eform_abx;

  datatype = FLA_Obj_datatype( A );

  n_A      = FLA_Obj_width( A );
  ld_A     = FLA_Obj_length( A );
  ld_B     = FLA_Obj_length( B );

  rocblas_fill blas_uplo = FLA_Param_map_flame_to_rocblas_uplo( uplo );
  //rocblas_int* info;
  //hipMalloc( (void**) &info, sizeof( rocblas_int) );



  switch( datatype ){

  case FLA_FLOAT:
  {

    rocsolver_ssygs2( handle, itype,
                blas_uplo,
                n_A,
                ( float * ) A_hip, ld_A,
                ( float * ) B_hip, ld_B);

    break;
  }

  case FLA_DOUBLE:
  {

    rocsolver_dsygs2( handle, itype,
                blas_uplo,
                n_A,
                ( double * ) A_hip, ld_A,
                ( double * ) B_hip, ld_B);

    break;
  }

  case FLA_COMPLEX:
  {

    rocsolver_chegs2( handle, itype,
                blas_uplo,
                n_A,
                ( rocblas_float_complex * ) A_hip, ld_A,
                ( rocblas_float_complex * ) B_hip, ld_B);

    break;
  }

  case FLA_DOUBLE_COMPLEX:
  {

    rocsolver_zhegs2( handle, itype,
                blas_uplo,
                n_A,
                ( rocblas_double_complex * ) A_hip, ld_A,
                ( rocblas_double_complex * ) B_hip, ld_B);

    break;
  }

  }

  //hipFree( info )
  return FLA_SUCCESS;
}


#endif


