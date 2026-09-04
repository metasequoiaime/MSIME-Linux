cmake_minimum_required(VERSION 3.25)

project(MetasequoiaImeLinux VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

find_package(Boost REQUIRED COMPONENTS json)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(vendor/MetasequoiaImeEngine engine)

add_library(MetasequoiaImeLinuxCore STATIC src/InputController.cpp src/TextTransform.cpp)
target_link_libraries(MetasequoiaImeLinuxCore PUBLIC MetasequoiaIme::Engine)
target_include_directories(MetasequoiaImeLinuxCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(MetasequoiaImeLinuxCore PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(MetasequoiaImeLinuxOnline STATIC
            src/online/AiSuggestionProvider.cpp
            src/online/CurlHttpTransport.cpp
            src/online/GoogleCloudProvider.cpp
            src/online/OnlineCandidateService.cpp
            src/online/TranslationProvider.cpp)
target_link_libraries(MetasequoiaImeLinuxOnline
                      PUBLIC MetasequoiaImeLinuxCore
                      PRIVATE Boost::json CURL::libcurl Threads::Threads)
target_include_directories(MetasequoiaImeLinuxOnline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(MetasequoiaImeLinuxOnline PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(metasequoia-ime-dictionary-replay src/DictionaryReplay.cpp)
target_link_libraries(metasequoia-ime-dictionary-replay PRIVATE MetasequoiaIme::Engine)
target_compile_options(metasequoia-ime-dictionary-replay PRIVATE -Wall -Wextra -Wpedantic -Werror)
install(TARGETS metasequoia-ime-dictionary-replay RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

if(BUILD_TESTING)
    add_executable(MetasequoiaImeLinuxTextTransformTests tests/TextTransformTests.cpp src/TextTransform.cpp)
    target_include_directories(MetasequoiaImeLinuxTextTransformTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(MetasequoiaImeLinuxTextTransformTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME text_transform COMMAND MetasequoiaImeLinuxTextTransformTests)

    add_executable(MetasequoiaImeLinuxTests tests/InputSessionTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxTests PRIVATE MetasequoiaIme::Engine)
    target_compile_options(MetasequoiaImeLinuxTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME input_session COMMAND MetasequoiaImeLinuxTests)

    add_executable(MetasequoiaImeLinuxControllerTests tests/InputControllerTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxControllerTests PRIVATE MetasequoiaImeLinuxCore)
    target_compile_options(MetasequoiaImeLinuxControllerTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME input_controller COMMAND MetasequoiaImeLinuxControllerTests)

    add_executable(MetasequoiaImeLinuxOnlineCandidateTests tests/OnlineCandidateServiceTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxOnlineCandidateTests PRIVATE MetasequoiaImeLinuxOnline)
    target_compile_options(MetasequoiaImeLinuxOnlineCandidateTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME online_candidate_service COMMAND MetasequoiaImeLinuxOnlineCandidateTests)

    add_executable(MetasequoiaImeLinuxTranslationTests tests/TranslationProviderTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxTranslationTests PRIVATE MetasequoiaImeLinuxOnline)
    target_compile_options(MetasequoiaImeLinuxTranslationTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME translation_provider COMMAND MetasequoiaImeLinuxTranslationTests)
endif()

option(METASEQUOIA_IME_BUILD_IBUS "Build the IBus engine" ON)
set(METASEQUOIA_IME_DICTIONARY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/MetasequoiaImeDict/out/msime.db" CACHE FILEPATH "Path to msime.db")
set(METASEQUOIA_IME_HELPCODE_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/MetasequoiaImeHelpCode/helpcodes"
    CACHE PATH "Path to the Metasequoia helpcode data directory")

if(METASEQUOIA_IME_BUILD_IBUS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IBUS REQUIRED IMPORTED_TARGET ibus-1.0)
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

    add_library(MetasequoiaImeLinuxSettings STATIC src/SecretStore.cpp src/SettingsStore.cpp)
    target_link_libraries(MetasequoiaImeLinuxSettings
                          PUBLIC MetasequoiaImeLinuxCore PkgConfig::GLIB PkgConfig::LIBSECRET)
    target_compile_options(MetasequoiaImeLinuxSettings PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_library(MetasequoiaImeLinuxIBusKeyMapper STATIC src/IBusKeyMapper.cpp)
    target_link_libraries(MetasequoiaImeLinuxIBusKeyMapper PUBLIC MetasequoiaImeLinuxCore PkgConfig::IBUS)
    target_compile_options(MetasequoiaImeLinuxIBusKeyMapper PRIVATE -Wall -Wextra -Wpedantic -Werror)

    if(BUILD_TESTING)
        add_executable(MetasequoiaImeLinuxIBusKeyMapperTests tests/IBusKeyMapperTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxIBusKeyMapperTests PRIVATE MetasequoiaImeLinuxIBusKeyMapper)
        target_compile_options(MetasequoiaImeLinuxIBusKeyMapperTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME ibus_key_mapper COMMAND MetasequoiaImeLinuxIBusKeyMapperTests)

        add_executable(MetasequoiaImeLinuxSettingsTests tests/SettingsStoreTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxSettingsTests PRIVATE MetasequoiaImeLinuxSettings)
        target_compile_options(MetasequoiaImeLinuxSettingsTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME settings_store COMMAND MetasequoiaImeLinuxSettingsTests)

        add_executable(MetasequoiaImeLinuxSecretStoreTests tests/SecretStoreTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxSecretStoreTests PRIVATE MetasequoiaImeLinuxSettings)
        target_compile_options(MetasequoiaImeLinuxSecretStoreTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME secret_store
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/SecretStoreIntegration.sh
                              $<TARGET_FILE:MetasequoiaImeLinuxSecretStoreTests>)
        set_tests_properties(secret_store PROPERTIES TIMEOUT 15)

        add_test(NAME ibus_smoke
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/IBusSmoke.sh ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(ibus_smoke PROPERTIES TIMEOUT 15)
    endif()

    add_executable(metasequoia-ime-ibus src/IBusEngine.cpp)
    target_link_libraries(metasequoia-ime-ibus PRIVATE MetasequoiaImeLinuxIBusKeyMapper MetasequoiaImeLinuxSettings
                                                       MetasequoiaImeLinuxOnline)
    target_compile_options(metasequoia-ime-ibus PRIVATE -Wall -Wextra -Wpedantic -Werror)

    configure_file(resources/metasequoiaime.xml.in ${CMAKE_CURRENT_BINARY_DIR}/metasequoiaime.xml @ONLY)
    install(TARGETS metasequoia-ime-ibus RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoiaime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component)

    if(EXISTS "${METASEQUOIA_IME_DICTIONARY}")
        install(FILES "${METASEQUOIA_IME_DICTIONARY}" DESTINATION ${CMAKE_INSTALL_DATADIR}/metasequoiaime)
    else()
        message(WARNING "Dictionary not found at ${METASEQUOIA_IME_DICTIONARY}; the engine will need msime.db in the data directory at runtime.")
    endif()
    if(EXISTS "${METASEQUOIA_IME_HELPCODE_DIRECTORY}")
        install(DIRECTORY "${METASEQUOIA_IME_HELPCODE_DIRECTORY}/"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/metasequoiaime/helpcodes
                FILES_MATCHING PATTERN "*.txt")
    else()
        message(WARNING "Helpcodes not found at ${METASEQUOIA_IME_HELPCODE_DIRECTORY}; helpcode input will be unavailable at runtime.")
    endif()
endif()
