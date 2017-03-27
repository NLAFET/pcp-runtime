add_executable(test-dpotrf.x
  main.c
  driver.c
  random_matrix.c
  task-chol-seq.c
  task-chol-par.c
  task-trsm-seq.c
  task-trsm-par.c
  task-gemm-seq.c
  task-gemm-par.c
  task-syrk-seq.c
  task-syrk-par.c)

target_link_libraries(test-dpotrf.x
  pcp
  ${BLAS_LIBRARY}
  ${LAPACK_LIBRARY}
  m)
