cmake_minimum_required(VERSION 3.25)

project(MetasequoiaImeLinux VERSION 0.2.0 LANGUAGES CXX) # x-release-please-version

include(CTest)
include(GNUInstallDirs)

# The release workflow stamps the tag here so generated packages carry the
# published version instead of the in-tree default.
set(METASEQUOIA_IME_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into generated packages")

find_package(Boost REQUIRED COMPONENTS json)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/MetasequoiaImeEngine/CMakeLists.txt")
    message(FATAL_ERROR "The engine submodule is missing. Run 'git submodule update --init --recursive', "
                        "or 'scripts/bootstrap_ci_dependencies.sh' where git transport is unavailable.")
endif()

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

add_library(MetasequoiaImeLinuxVoice STATIC src/VoiceInput.cpp)
target_link_libraries(MetasequoiaImeLinuxVoice PUBLIC MetasequoiaImeLinuxOnline PRIVATE Boost::json)
target_include_directories(MetasequoiaImeLinuxVoice PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(MetasequoiaImeLinuxVoice PRIVATE -Wall -Wextra -Wpedantic -Werror)

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

    add_executable(MetasequoiaImeLinuxVoiceInputTests tests/VoiceInputTests.cpp)
    target_link_libraries(MetasequoiaImeLinuxVoiceInputTests PRIVATE MetasequoiaImeLinuxVoice)
    target_include_directories(MetasequoiaImeLinuxVoiceInputTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(MetasequoiaImeLinuxVoiceInputTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME voice_input COMMAND MetasequoiaImeLinuxVoiceInputTests)

endif()

option(METASEQUOIA_IME_BUILD_IBUS "Build the IBus engine" ON)
option(METASEQUOIA_IME_BUILD_SETTINGS_UI "Build the GTK settings application" ON)
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

    add_library(MetasequoiaImeLinuxSettingsUiModel STATIC src/SettingsUiModel.cpp)
    target_link_libraries(MetasequoiaImeLinuxSettingsUiModel PUBLIC MetasequoiaImeLinuxSettings)
    target_include_directories(MetasequoiaImeLinuxSettingsUiModel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(MetasequoiaImeLinuxSettingsUiModel PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_library(MetasequoiaImeLinuxDesktopTools STATIC src/ClipboardHistory.cpp src/HandwritingRecognizer.cpp)
    target_link_libraries(MetasequoiaImeLinuxDesktopTools PUBLIC MetasequoiaImeLinuxSettings PRIVATE Boost::json)
    target_include_directories(MetasequoiaImeLinuxDesktopTools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(MetasequoiaImeLinuxDesktopTools PRIVATE -Wall -Wextra -Wpedantic -Werror)

    if(METASEQUOIA_IME_BUILD_SETTINGS_UI)
        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        add_executable(metasequoia-ime-voice src/VoiceInputCli.cpp)
        target_link_libraries(metasequoia-ime-voice PRIVATE MetasequoiaImeLinuxVoice MetasequoiaImeLinuxSettings)
        target_compile_options(metasequoia-ime-voice PRIVATE -Wall -Wextra -Wpedantic -Werror)
        install(TARGETS metasequoia-ime-voice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        configure_file(resources/metasequoia-ime-voice.desktop.in
                       ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-voice.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-voice.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        if(BUILD_TESTING)
            add_test(NAME voice_cli_check COMMAND metasequoia-ime-voice --check)
        endif()
        add_executable(metasequoia-ime-settings src/SettingsUi.cpp)
        target_link_libraries(metasequoia-ime-settings PRIVATE MetasequoiaImeLinuxSettingsUiModel PkgConfig::GTK3)
        target_compile_options(metasequoia-ime-settings PRIVATE -Wall -Wextra -Wpedantic -Werror)
        install(TARGETS metasequoia-ime-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        configure_file(resources/metasequoia-ime-settings.desktop.in
                       ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-settings.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-settings.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        if(BUILD_TESTING)
            add_test(NAME settings_ui_check COMMAND metasequoia-ime-settings --check)
        endif()

        add_executable(metasequoia-ime-tools src/DesktopToolsUi.cpp)
        target_link_libraries(metasequoia-ime-tools PRIVATE MetasequoiaImeLinuxDesktopTools PkgConfig::GTK3)
        target_compile_options(metasequoia-ime-tools PRIVATE -Wall -Wextra -Wpedantic -Werror)
        install(TARGETS metasequoia-ime-tools RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        configure_file(resources/metasequoia-ime-tools.desktop.in
                       ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-tools.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-tools.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        if(BUILD_TESTING)
            add_test(NAME desktop_tools_check COMMAND metasequoia-ime-tools --check)
        endif()

        add_executable(metasequoia-ime-toolbar src/FloatingToolbarUi.cpp)
        target_link_libraries(metasequoia-ime-toolbar PRIVATE MetasequoiaImeLinuxSettings PkgConfig::GTK3)
        target_compile_options(metasequoia-ime-toolbar PRIVATE -Wall -Wextra -Wpedantic -Werror)
        install(TARGETS metasequoia-ime-toolbar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        configure_file(resources/metasequoia-ime-toolbar.desktop.in
                       ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-toolbar.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/metasequoia-ime-toolbar.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        if(BUILD_TESTING)
            add_test(NAME floating_toolbar_check COMMAND metasequoia-ime-toolbar --check)
        endif()
    endif()

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

        add_executable(MetasequoiaImeLinuxSettingsUiModelTests tests/SettingsUiModelTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxSettingsUiModelTests PRIVATE MetasequoiaImeLinuxSettingsUiModel)
        target_include_directories(MetasequoiaImeLinuxSettingsUiModelTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(MetasequoiaImeLinuxSettingsUiModelTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME settings_ui_model COMMAND MetasequoiaImeLinuxSettingsUiModelTests)

        add_executable(MetasequoiaImeLinuxClipboardHistoryTests tests/ClipboardHistoryTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxClipboardHistoryTests PRIVATE MetasequoiaImeLinuxDesktopTools)
        target_include_directories(MetasequoiaImeLinuxClipboardHistoryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(MetasequoiaImeLinuxClipboardHistoryTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME clipboard_history COMMAND MetasequoiaImeLinuxClipboardHistoryTests)

        add_executable(MetasequoiaImeLinuxHandwritingRecognizerTests tests/HandwritingRecognizerTests.cpp)
        target_link_libraries(MetasequoiaImeLinuxHandwritingRecognizerTests PRIVATE MetasequoiaImeLinuxDesktopTools)
        target_include_directories(MetasequoiaImeLinuxHandwritingRecognizerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(MetasequoiaImeLinuxHandwritingRecognizerTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME handwriting_recognizer COMMAND MetasequoiaImeLinuxHandwritingRecognizerTests)

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

# Every binary here statically links the GPLv3 engine, so the packages carry the
# same terms. Ship the license text with them and declare it in the metadata.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/metasequoia-ime-linux)

# CPack gives downstreams a reproducible baseline package. Debian and RPM
# generators consume the same install manifest; TGZ remains available on
# minimal build hosts that do not ship native package tooling.
set(CPACK_PACKAGE_NAME "metasequoia-ime-linux")
set(CPACK_PACKAGE_VENDOR "Metasequoia IME contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Metasequoia IME Linux IBus frontend and desktop tools")
set(CPACK_PACKAGE_CONTACT "Metasequoia IME contributors <suzukaze.haduki@gmail.com>")
set(CPACK_PACKAGE_VERSION ${METASEQUOIA_IME_PACKAGE_VERSION})
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/metasequoiaime/MSIME-Linux")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_GENERATOR "TGZ;DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ibus")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "tesseract-ocr, tesseract-ocr-chi-sim")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Input Methods")
include(CPack)

if(BUILD_TESTING)
    add_test(NAME packaging_tgz
             COMMAND ${CMAKE_CPACK_COMMAND} --config ${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake -G TGZ)

    # The DEB generator is the one downstreams actually consume, so gate it too
    # wherever dpkg is present. Hosts without dpkg keep the TGZ-only coverage.
    find_program(METASEQUOIA_IME_DPKG_DEB dpkg-deb)
    if(METASEQUOIA_IME_DPKG_DEB)
        add_test(NAME packaging_deb
                 COMMAND ${CMAKE_CPACK_COMMAND} --config ${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake -G DEB)
        # dpkg-shlibdeps walks every installed binary, so this needs more than
        # the short default the unit tests run under.
        set_tests_properties(packaging_deb PROPERTIES TIMEOUT 300)
    endif()
endif()
