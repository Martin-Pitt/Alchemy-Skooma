# -*- cmake -*-
#
# The navmesh proper: the data model the viewer fills in, and the algorithms
# over it. Nothing here knows what a viewer object, a region or a frame is,
# which is what makes it testable without one.

add_library(sknavmesh STATIC)

target_sources(sknavmesh PRIVATE sknavair.cpp sknavannotate.cpp sknavbuild.cpp sknavcluster.cpp sknavfeatures.cpp sknavflights.cpp sknavwalls.cpp)

target_sources(
  sknavmesh
  PUBLIC FILE_SET HEADERS FILES sknavair.h sknavannotate.h sknavbuild.h sknavcluster.h sknavfeatures.h sknavflights.h sknavtypes.h sknavwalls.h
)

target_link_libraries(sknavmesh PRIVATE al::flags PUBLIC llmath llcommon ll::recast)

target_precompile_headers(sknavmesh REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test(sknavair PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavannotate PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavcluster PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavbuild PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavwalls PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavfeatures PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
  al_add_test(sknavflights PROJECT sknavmesh LIBRARIES sknavmesh llcommon)
endif()
