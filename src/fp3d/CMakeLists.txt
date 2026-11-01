# FP3D: bridge + boot helpers are compiled into the game library so CDDA's input/draw hooks can reach them.
target_sources(cataclysm-tiles-common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/boot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/snapshot.cpp)

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
