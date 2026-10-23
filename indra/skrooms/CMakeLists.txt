# -*- cmake -*-
#
# Rooms and spaces over the air cells: each gap's enclosure, the spaces and
# portals cut from the floor cells, the room graph, wind, floors under a point
# and reflection-probe placement. Nothing here knows what a viewer object, a
# region or a frame is, which is what makes it testable without one.

add_library(skrooms STATIC)

target_sources(
  skrooms
  PRIVATE
    skroomscensus.cpp
    skroomsenclosure.cpp
    skroomsexport.cpp
    skroomsflights.cpp
    skroomsfloors.cpp
    skroomsgraph.cpp
    skroomsprobecache.cpp
    skroomsprobes.cpp
    skroomsspaceproducts.cpp
    skroomsspaces.cpp
    skroomsstairalign.cpp
    skroomswind.cpp
)

target_sources(
  skrooms
  PUBLIC
    FILE_SET HEADERS
      FILES
        skroomscensus.h
        skroomsenclosure.h
        skroomsexport.h
        skroomsflights.h
        skroomsfloors.h
        skroomsgraph.h
        skroomsprobecache.h
        skroomsprobes.h
        skroomsschedule.h
        skroomsspaceproducts.h
        skroomsspaces.h
        skroomsstairalign.h
        skroomswind.h
)

target_link_libraries(skrooms PRIVATE al::flags PUBLIC skair)

target_precompile_headers(skrooms REUSE_FROM llprecompiled)

if(AL_BUILD_TESTS)
  al_add_test_binary(
    skrooms_tests
    PROJECT skrooms
    SOURCES
      tests/skrooms_test.cpp
      tests/skroomsairdebug_test.cpp
      tests/skroomsairunknown_test.cpp
      tests/skroomscensus_test.cpp
      tests/skroomsasync_test.cpp
      tests/skroomsenclosure_test.cpp
      tests/skroomsenclosurerepair_test.cpp
      tests/skroomsflights_test.cpp
      tests/skroomsfloors_test.cpp
      tests/skroomsgraph_test.cpp
      tests/skroomsprobecache_test.cpp
      tests/skroomsprobeinvariants_test.cpp
      tests/skroomsprobes_test.cpp
      tests/skroomsprobeseval_test.cpp
      tests/skroomsschedule_test.cpp
      tests/skroomsspaceproducts_test.cpp
      tests/skroomsstairalign_test.cpp
      tests/skroomswind_test.cpp
    BENCH_SOURCES tests/skroomsspacesdraw_bench.cpp
    LIBRARIES skrooms llcommon
  )
endif()
