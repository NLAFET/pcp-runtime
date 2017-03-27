add_executable(test-dtrsm.x
  main.c
  driver.c
  task-update-seq.c
  task-update-par.c
  task-solve-seq.c
  task-solve-par.c)

target_link_libraries(test-dtrsm.x
  pcp
  ${BLAS_LIBRARY}
  m)
