cmake_minimum_required(VERSION 3.25)

project(MetasequoiaImeLinux VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(vendor/MetasequoiaImeEngine engine)

add_library(MetasequoiaImeLinuxCore STATIC src/InputController.cpp)
target_link_libraries(MetasequoiaImeLinuxCore PUBLIC MetasequoiaIme::Engine)
target_include_directories(MetasequoiaImeLinuxCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(MetasequoiaImeLinuxCore PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
    add_executable(MetasequoiaImeLinuxTests tests/InputSessionTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxTests PRIVATE MetasequoiaIme::Engine)
    target_compile_options(MetasequoiaImeLinuxTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME input_session COMMAND MetasequoiaImeLinuxTests)

    add_executable(MetasequoiaImeLinuxControllerTests tests/InputControllerTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxControllerTests PRIVATE MetasequoiaImeLinuxCore)
    target_compile_options(MetasequoiaImeLinuxControllerTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME input_controller COMMAND MetasequoiaImeLinuxControllerTests)
endif()

option(METASEQUOIA_IME_BUILD_IBUS "Build the IBus engine" ON)
set(METASEQUOIA_IME_DICTIONARY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/MetasequoiaImeDict/out/msime.db" CACHE FILEPATH "Path to msime.db")

if(METASEQUOIA_IME_BUILD_IBUS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IBUS REQUIRED IMPORTED_TARGET ibus-1.0)

    add_executable(metasequoia-ime-ibus src/IBusEngine.cpp)
    target_link_libraries(metasequoia-ime-ibus PRIVATE MetasequoiaImeLinuxCore PkgConfig::IBUS)
    target_compile_options(metasequoia-ime-ibus PRIVATE -Wall -Wextra -Wpedantic -Werror)

    configure_file(resources/metasequoiaime.xml.in ${CMAKE_CURRENT_BINARY_DIR}/metasequoiaime.xml @ONLY)
    install(TARGETS metasequoia-ime-ibus RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoiaime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component)

    if(EXISTS "${METASEQUOIA_IME_DICTIONARY}")
        install(FILES "${METASEQUOIA_IME_DICTIONARY}" DESTINATION ${CMAKE_INSTALL_DATADIR}/metasequoiaime)
    else()
        message(WARNING "Dictionary not found at ${METASEQUOIA_IME_DICTIONARY}; the engine will need msime.db in the data directory at runtime.")
    endif()
endif()
