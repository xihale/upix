cmake_minimum_required(VERSION 3.21)
project(upix-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Svg)
find_package(ZLIB REQUIRED)  # share network_utils::crc32

qt_standard_project_setup(REQUIRES 6.5)
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.8")
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(upix WIN32
    src/main.cpp
    src/theme.cpp
    src/machine.cpp
    src/desk.cpp
    src/qrprovider.cpp
    src/icons.cpp
    src/qrcodegen.cpp
)

target_include_directories(upix PRIVATE src)

# ---------------------------------------------------------------------------
# share/ linkage (docs/SPEC-frontends.md stage 2).
#
# UNIX: the full core (engine, batch pipeline, model management, daemon
# supervision, coordinator) — desktop is a first-class frontend with no CLI
# dependency; it supervises the daemon by re-execing itself (`--serve-daemon`).
# The core static lib + its package graph are built by xmake (single source of
# truth for ncnn/curl/crypto/codec deps), queried at configure time.
#
# WIN32: AF_UNIX unavailable — keep the stage-0 shape (thin serve-protocol
# client TUs + the QProcess CLI bridge in desk.cpp).
# ---------------------------------------------------------------------------
set(UPIX_SHARE_TUS
    ${CMAKE_CURRENT_SOURCE_DIR}/../share/core/distributed/protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../share/utils/stream_socket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../share/utils/unix_socket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../share/utils/network.cpp
)
set(UPIX_REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(UNIX)
    find_program(XMAKE_PROGRAM xmake REQUIRED)
    # Configure + build the core through xmake (incremental; a no-op when
    # fresh). Explicit -P: xmake's cwd resolution can pick a sibling clone.
    execute_process(
        COMMAND ${XMAKE_PROGRAM} -y -P ${UPIX_REPO} upix-core
        RESULT_VARIABLE _xmake_rc)
    if(NOT _xmake_rc EQUAL 0)
        message(FATAL_ERROR "xmake could not build upix-core (rc=${_xmake_rc})")
    endif()
    execute_process(
        COMMAND ${XMAKE_PROGRAM} show -P ${UPIX_REPO} -t upix-core --json
        OUTPUT_VARIABLE UPIX_CORE_JSON
        RESULT_VARIABLE _show_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _show_rc EQUAL 0)
        message(FATAL_ERROR "xmake show failed (rc=${_show_rc})")
    endif()
    string(JSON UPIX_CORE_LIB GET "${UPIX_CORE_JSON}" targetfile)
    # xmake reports the targetfile relative to the project dir; CMake needs
    # an absolute path or it becomes a -l name.
    if(NOT IS_ABSOLUTE "${UPIX_CORE_LIB}")
        set(UPIX_CORE_LIB "${UPIX_REPO}/${UPIX_CORE_LIB}")
    endif()
    get_filename_component(UPIX_CORE_LIB "${UPIX_CORE_LIB}" ABSOLUTE)

    # links/linkdirs/syslinks arrays → CMake lists.
    function(upix_json_list json key out)
        string(JSON len LENGTH "${json}" ${key})
        set(items "")
        math(EXPR last "${len} - 1")
        foreach(i RANGE 0 ${last})
            string(JSON item GET "${json}" ${key} ${i} value)
            list(APPEND items "${item}")
        endforeach()
        set(${out} "${items}" PARENT_SCOPE)
    endfunction()
    upix_json_list("${UPIX_CORE_JSON}" links UPIX_CORE_LINKS)
    upix_json_list("${UPIX_CORE_JSON}" linkdirs UPIX_CORE_LINK_DIRS)
    upix_json_list("${UPIX_CORE_JSON}" syslinks UPIX_CORE_SYSLINKS)

    # CLI --version parity feeds the desktop's own version chip.
    file(READ ${UPIX_REPO}/xmake.lua _root_lua)
    string(REGEX MATCH "set_version\\(\"([0-9.]+)\"" _ ${_root_lua})
    set(UPIX_VERSION ${CMAKE_MATCH_1})
endif()

if(UPIX_CORE_LIB)
    # Keep the core fresh on every build (xmake is incremental internally).
    add_custom_target(upix_core_lib
        COMMAND ${XMAKE_PROGRAM} -y -P ${UPIX_REPO} upix-core
        BYPRODUCTS ${UPIX_CORE_LIB}
        VERBATIM)
endif()

set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_compile_definitions(upix PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
target_include_directories(upix PRIVATE ${UPIX_REPO}/share)
if(UNIX)
    target_compile_definitions(upix PRIVATE UPIX_VERSION="${UPIX_VERSION}")
    add_dependencies(upix upix_core_lib)
    target_link_directories(upix PRIVATE ${UPIX_CORE_LINK_DIRS})
    target_link_libraries(upix PRIVATE ${UPIX_CORE_LIB} ${UPIX_CORE_LINKS} ${UPIX_CORE_SYSLINKS})
else()
    target_sources(upix PRIVATE ${UPIX_SHARE_TUS})
endif()
target_link_libraries(upix PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Svg ZLIB::ZLIB)
qt_add_qml_module(upix
URI UpixDesk
VERSION 1.0
NO_RESOURCE_TARGET_PATH
QML_FILES
qml/Main.qml
qml/Rail.qml
qml/Icon.qml
qml/Pill.qml
qml/Picker.qml
qml/Segmented.qml
qml/TrackSlider.qml
qml/Toggle.qml
qml/StepRow.qml
qml/Progress.qml
qml/CompareStage.qml
qml/Workbench.qml
qml/Inspector.qml
qml/ModelsPage.qml
qml/HostPage.qml
qml/PairingSheet.qml
)

# ---- Windows zip: upix.exe + upix-cli.exe side by side ----------------------
# The GUI finds the CLI via the sibling-exe rule (Desk::findCli), so the zip
# is the whole install: no PATH edits, no registry, no installer.
if(WIN32)
    install(TARGETS upix RUNTIME DESTINATION .)
    # Qt DLLs + QML modules via the Qt 6.5+ deploy script (windeployqt under
    # the hood), run at install time.
    qt_generate_deploy_qml_app_script(
        TARGET upix
        OUTPUT_SCRIPT upix_deploy_script
    )
    install(SCRIPT ${upix_deploy_script})
    set(UPIX_CLI_BIN "" CACHE FILEPATH "upix-cli.exe to bundle next to the GUI in the zip")
    if(UPIX_CLI_BIN)
        install(FILES ${UPIX_CLI_BIN} DESTINATION .)
    else()
        message(WARNING "UPIX_CLI_BIN not set: the zip ships without upix-cli "
                        "and the GUI will report 未找到 upix-cli until the CLI is "
                        "placed next to upix.exe")
    endif()
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()


# Regression tests. `ctest` in the build dir; the divider test drives the real
# CompareStage with real mouse events (the headless shots cannot see gestures).
option(UPIX_BUILD_TESTS "Build desktop regression tests" ON)
if(UPIX_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(tst_compare_drag
        tests/tst_compare_drag.cpp
        src/theme.cpp
    )
    target_include_directories(tst_compare_drag PRIVATE src)
    target_compile_definitions(tst_compare_drag PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
    target_link_libraries(tst_compare_drag PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Test)
    # The harness module compiles the REAL component sources (the same files
    # the app module lists above) — not copies — so the assertions pin shipped
    # behavior. They are re-listed because a QML module cannot import another
    # module that lives inside an executable target.
    qt_add_qml_module(tst_compare_drag
        URI TstCompare
        VERSION 1.0
        NO_RESOURCE_TARGET_PATH
        QML_FILES
        tests/tst_stage.qml
        qml/CompareStage.qml
        qml/Pill.qml
        qml/Icon.qml
    )
    add_dependencies(tst_compare_drag upix)
    # `offscreen` keeps it runnable on a headless CI box.
    add_test(NAME compare_drag COMMAND tst_compare_drag -platform offscreen)
    set_tests_properties(compare_drag PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(tst_desk_jobs
        tests/tst_desk_jobs.cpp
        src/desk.cpp
        src/machine.cpp
    )
    target_include_directories(tst_desk_jobs PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../share)
    target_compile_definitions(tst_desk_jobs PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
    if(UNIX)
        target_compile_definitions(tst_desk_jobs PRIVATE UPIX_VERSION="${UPIX_VERSION}")
        add_dependencies(tst_desk_jobs upix_core_lib)
        target_link_directories(tst_desk_jobs PRIVATE ${UPIX_CORE_LINK_DIRS})
        target_link_libraries(tst_desk_jobs PRIVATE
            ${UPIX_CORE_LIB} ${UPIX_CORE_LINKS} ${UPIX_CORE_SYSLINKS}
            Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test ZLIB::ZLIB Threads::Threads)
    else()
        target_sources(tst_desk_jobs PRIVATE ${UPIX_SHARE_TUS})
        target_link_libraries(tst_desk_jobs PRIVATE
            Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test ZLIB::ZLIB Threads::Threads)
    endif()
    add_test(NAME desk_jobs COMMAND tst_desk_jobs -platform offscreen)
    set_tests_properties(desk_jobs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
