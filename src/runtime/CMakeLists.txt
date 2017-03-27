add_library(pcp STATIC
  pcp.c
  spin-barrier.c)

target_link_libraries(pcp
  hwloc
  pthread)
