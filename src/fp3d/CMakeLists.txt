# FP3D block B1: headless smoke test. Links the same object library as the game, opens no window.
add_executable(fp3d-headless headless_smoke.cpp ${MESSAGES_CPP})
target_link_libraries(fp3d-headless PRIVATE cataclysm-tiles-common)
target_compile_definitions(fp3d-headless PUBLIC SDL_MAIN_HANDLED)
