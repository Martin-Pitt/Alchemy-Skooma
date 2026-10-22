# -*- cmake -*-
#
# The world census: the records of every physics shape the viewer can see,
# their clusters, features and lattice. Nothing here knows what a viewer
# object, a region or a frame is, which is what makes it testable without one.

add_library(skcensus STATIC)

target_sources(
  skcensus
  PRIVATE
    skcensuscluster.cpp
    skcensusdelta.cpp
    skcensusfeatures.cpp
    skcensuslattice.cpp
    skcensusrecord.cpp
)

target_sources(
  skcensus
  PUBLIC
    FILE_SET HEADERS
      FILES
        skcensuscluster.h
        skcensusdelta.h
        skcensusfeatures.h
        skcensusland.h
        skcensuslattice.h
        skcensusprofile.h
        skcensusrecord.h
        skcensustypes.h
)

target_link_libraries(skcensus PRIVATE al::flags PUBLIC llmath llcommon)

target_precompile_headers(skcensus REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test_binary(
    skcensus_tests
    PROJECT skcensus
    SOURCES
      tests/skcensuscluster_test.cpp
      tests/skcensusclusterhash_test.cpp
      tests/skcensusfeatures_test.cpp
      tests/skcensusrecordsignature_test.cpp
    BENCH_SOURCES tests/skcensusfeatures_bench.cpp
    LIBRARIES skcensus llcommon
  )
endif()
