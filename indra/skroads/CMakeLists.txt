# -*- cmake -*-
#
# The road model: the road read from its pieces (parts, segments, joins, the walk and the lane line), with the road
# kits it measures from their own prims and the sign library it matches pieces against. What it knows of the viewer it
# asks a host (skroadmodel.h's skroad::host): skroadhost.cpp in the viewer, a frozen case in the road bench, which is
# what makes it testable without one.

add_library(skroads STATIC)

target_sources(skroads PRIVATE skroadkits.cpp skroadmodel.cpp skroadsigns.cpp)

target_sources(skroads PUBLIC FILE_SET HEADERS FILES skroadkits.h skroadmodel.h skroadsigns.h)

target_link_libraries(skroads PRIVATE al::flags ll::simdjson PUBLIC skair skcensus llmath llcommon)

target_precompile_headers(skroads REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  # The road geometry bench: frozen cases (AL_ROAD_BENCH_DIR) read by the model as the viewer reads the world.
  add_library(skroads_test_host OBJECT tests/skroadbenchcase.cpp tests/skroadbenchgeometry.cpp)
  target_link_libraries(skroads_test_host PRIVATE al::flags skroads)
  al_add_test_binary(
    skroads_tests
    PROJECT skroads
    SOURCES tests/skroadbench_test.cpp tests/skroadkits_test.cpp
    LIBRARIES skroads llcommon
    SUPPORT skroads_test_host
    DEFINES "SK_ROADKITS_TEST_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests\""
  )
endif()
