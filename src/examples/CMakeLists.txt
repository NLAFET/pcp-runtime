set(BLAS_INCLUDE "" CACHE PATH "BLAS library include directory")
set(BLAS_LIBRARY "" CACHE STRING "BLAS link library")
set(LAPACK_INCLUDE "" CACHE PATH "LAPACK library include directory")
set(LAPACK_LIBRARY "" CACHE STRING "LAPACK link library")

include_directories(${BLAS_INCLUDE})
include_directories(${LAPACK_INCLUDE})

add_subdirectory(dtrsm)
add_subdirectory(dpotrf)
