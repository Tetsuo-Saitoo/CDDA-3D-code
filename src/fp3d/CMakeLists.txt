# FP3D: bridge + boot helpers are compiled into the game library so CDDA's input/draw hooks can reach them.
target_sources(cataclysm-tiles-common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/boot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/snapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/id_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chargen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/held.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/shot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gun_faults.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bullet_paths.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/item_hands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/worldgen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_range.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cooking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cheats.cpp)

# Block B1: headless smoke test. Links the same object library as the game, opens no window.
add_executable(fp3d-headless headless_smoke.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-headless PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-headless PUBLIC SDL_MAIN_HANDLED)

# Block B2: game loop on a worker thread, driven through the bridge by a scripted client.
add_executable(fp3d-sim-test sim_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-sim-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-sim-test PUBLIC SDL_MAIN_HANDLED)

# Block B3: snapshots checked against CDDA's map, revisions, creature uids, map shift.
add_executable(fp3d-snapshot-test snapshot_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-snapshot-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-snapshot-test PUBLIC SDL_MAIN_HANDLED)

# Block B4: events emitted for things that happen next to the player, perception filter, overflow rule.
add_executable(fp3d-events-test events_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-events-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-events-test PUBLIC SDL_MAIN_HANDLED)

# Block B6: id tables against CDDA's factories, stable session indices, every snapshot id resolvable.
add_executable(fp3d-ids-test ids_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-ids-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-ids-test PUBLIC SDL_MAIN_HANDLED)

# FP3D character creation: catalog, applying choices, refusing bad ones.
add_executable(fp3d-chargen-test chargen_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-chargen-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-chargen-test PUBLIC SDL_MAIN_HANDLED)

# K7 test range: zones built and counted, range operations and their refusals, frozen AI, turn time per zone.
add_executable(fp3d-range-test range_test.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-range-test PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-range-test PUBLIC SDL_MAIN_HANDLED)

# The Godot GDExtension (repo root bridge/), built when it and bridge/godot-cpp are there. See docs/BUILD.md.
set(FP3D_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/../bridge" CACHE PATH "Folder of the Godot bridge (GDExtension)")
if(EXISTS "${FP3D_BRIDGE_DIR}/CMakeLists.txt" AND EXISTS "${FP3D_BRIDGE_DIR}/godot-cpp/CMakeLists.txt")
    add_subdirectory("${FP3D_BRIDGE_DIR}" "${CMAKE_BINARY_DIR}/fp3d-bridge")
endif()
