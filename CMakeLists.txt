cmake_minimum_required(VERSION 3.22)
project(TeacherScheduler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------
# Find dependencies
# ------------------------------------------------
find_package(ortools REQUIRED)  

# nlohmann_json - try find_package first, then FetchContent
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)
endif()

# Drogon - try find_package first
find_package(Drogon QUIET)
if(NOT Drogon_FOUND)
    message(FATAL_ERROR "Drogon not found. Please install Drogon or ensure it's in CMAKE_PREFIX_PATH")
endif()

# ------------------------------------------------
# Source files
# ------------------------------------------------
add_executable(teacher_scheduler
    src/main.cpp
    src/controller/TeacherSchedulerController.cpp
    src/controller/TestCaseController.cpp
    src/controller/CPSatSchedulerController.cpp
    src/scheduler/phase_common.cpp
    src/scheduler/phase1.cpp
    src/scheduler/phase2.cpp
    src/scheduler/phase3.cpp
    src/scheduler/phase3_invariants.cpp
    src/scheduler/phase3_index.cpp
    src/scheduler/phase3_move_builder.cpp
    src/scheduler/phase3_penalty_state.cpp
    src/scheduler/phase3_move_evaluator.cpp
    src/scheduler/phase3_move_committer.cpp
    src/scheduler/phase3_sa.cpp
    src/scheduler/phase3_repair.cpp
    src/scheduler/CPSatSolver.cpp
    src/scheduler/SchedulerService.cpp
)

# ------------------------------------------------
# Compile-time flags
# ------------------------------------------------
# AddressSanitizer: Enable with -DUSE_ASAN=ON
# Example: cmake -DUSE_ASAN=ON ..
option(USE_ASAN "Enable AddressSanitizer" OFF)

if(USE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    target_compile_options(teacher_scheduler PRIVATE -fsanitize=address -g -fno-omit-frame-pointer)
    target_link_options(teacher_scheduler PRIVATE -fsanitize=address)
endif()

# Phase 3 refactor guard: Enable with -DPHASE3_REFACTOR_GUARD
# Example: target_compile_definitions(teacher_scheduler PRIVATE PHASE3_REFACTOR_GUARD)
# When enabled, uses refactored Phase 3 implementation; otherwise uses original implementation

# ------------------------------------------------
# Include + link
# ------------------------------------------------
target_include_directories(teacher_scheduler PRIVATE
    /usr/local/include
)

target_link_directories(teacher_scheduler PRIVATE
    /usr/local/lib
)

target_link_libraries(teacher_scheduler PRIVATE
    ortools::ortools
    nlohmann_json::nlohmann_json
    Drogon::Drogon
)

# Link filesystem library if needed (for std::filesystem)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(teacher_scheduler PRIVATE stdc++fs)
endif()

# ------------------------------------------------
# Output
# ------------------------------------------------
set_target_properties(teacher_scheduler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build/bin
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
