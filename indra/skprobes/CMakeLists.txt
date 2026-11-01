# -*- cmake -*-
#
# Reflection-probe placement over the rooms: the parts each space is cut into,
# their spheres and boxes, the budget and the cache of placed parts. A
# consumer of the rooms layer, kept out of it so that rooms never calls it.

add_library(skprobes STATIC)

target_sources(skprobes PRIVATE skroomsprobecache.cpp skroomsprobes.cpp)

target_sources(skprobes PUBLIC FILE_SET HEADERS FILES skroomsprobecache.h skroomsprobes.h)

target_link_libraries(skprobes PRIVATE al::flags PUBLIC skrooms)

target_precompile_headers(skprobes REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test_binary(
    skprobes_tests
    PROJECT skprobes
    SOURCES
      tests/skroomsprobecache_test.cpp
      tests/skroomsprobeinvariants_test.cpp
      tests/skroomsprobes_test.cpp
      tests/skroomsprobesasync_test.cpp
      tests/skroomsprobeseval_test.cpp
    LIBRARIES skprobes llcommon
  )
endif()
