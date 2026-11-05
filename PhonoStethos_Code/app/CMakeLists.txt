cmake_minimum_required(VERSION 3.16)
project(phonostethos_server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PHONO_BUILD_SERVER "Build the Raspberry Pi hardware server" ON)

find_package(Threads REQUIRED)
include(CTest)
if(BUILD_TESTING)
    add_executable(band-pass-filter-test tests/band_pass_filter_test.cpp)
    target_include_directories(band-pass-filter-test PRIVATE include)
    target_compile_options(band-pass-filter-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME band-pass-filter-test COMMAND band-pass-filter-test)

    add_executable(protocol-test tests/protocol_test.cpp)
    target_include_directories(protocol-test PRIVATE include)
    target_compile_options(protocol-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME protocol-test COMMAND protocol-test)

    add_executable(architecture-test tests/architecture_test.cpp)
    target_include_directories(architecture-test PRIVATE include)
    target_compile_options(architecture-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(architecture-test PRIVATE Threads::Threads)
    add_test(NAME architecture-test COMMAND architecture-test)

    add_executable(pthread-sync-test tests/pthread_sync_test.cpp)
    target_include_directories(pthread-sync-test PRIVATE include)
    target_link_libraries(pthread-sync-test PRIVATE Threads::Threads)
    add_test(NAME pthread-sync-test COMMAND pthread-sync-test)
endif()

if(PHONO_BUILD_SERVER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

    add_executable(phonostethos-server
        src/AudioEngine.cpp
        src/DspProcessor.cpp
        src/FileIoManager.cpp
        src/LedDriver.cpp
        src/NetworkManager.cpp
        src/main.cpp
        src/SystemController.cpp
    )
    target_include_directories(phonostethos-server PRIVATE include)
    target_link_libraries(phonostethos-server PRIVATE
        Threads::Threads
        PkgConfig::ALSA
        PkgConfig::FFTW3F
        PkgConfig::SNDFILE
        PkgConfig::UDEV
    )
    target_compile_definitions(phonostethos-server PRIVATE _GNU_SOURCE)
    target_compile_options(phonostethos-server PRIVATE -Wall -Wextra -Wpedantic)
    if(BUILD_TESTING)
        add_executable(runtime-test tests/runtime_test.cpp
            src/DspProcessor.cpp src/FileIoManager.cpp src/LedDriver.cpp)
        target_include_directories(runtime-test PRIVATE include)
        target_link_libraries(runtime-test PRIVATE Threads::Threads
            PkgConfig::FFTW3F PkgConfig::SNDFILE)
        add_test(NAME runtime-test COMMAND runtime-test)
    endif()
    install(TARGETS phonostethos-server RUNTIME DESTINATION bin)
endif()

# Tests use assert(); keep assertions active even in a Release build.
if(BUILD_TESTING)
    foreach(test_target band-pass-filter-test protocol-test architecture-test pthread-sync-test)
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endforeach()
    if(TARGET runtime-test)
        target_compile_options(runtime-test PRIVATE -UNDEBUG)
    endif()
endif()
