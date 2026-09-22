# -*- cmake -*-
#
# The navmesh proper: the data model the viewer fills in, and the algorithms
# over it. Nothing here knows what a viewer object, a region or a frame is,
# which is what makes it testable without one.

add_library(sknavmesh STATIC)

target_sources(sknavmesh PRIVATE sknavbuild.cpp sknavcluster.cpp sknavwalls.cpp)

target_sources(
  sknavmesh
  PUBLIC FILE_SET HEADERS FILES sknavbuild.h sknavcluster.h sknavtypes.h sknavwalls.h
)

target_link_libraries(sknavmesh PRIVATE al::flags PUBLIC llmath llcommon ll::recast)

target_precompile_headers(sknavmesh REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test(sknavcluster PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavbuild PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavwalls PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
endif()
