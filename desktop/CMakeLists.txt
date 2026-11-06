cmake_minimum_required(VERSION 3.21)
project(upix-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Svg)

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

set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_compile_definitions(upix PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
target_link_libraries(upix PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Svg)
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
qml/Grouping.js
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
    target_include_directories(tst_desk_jobs PRIVATE src)
    target_compile_definitions(tst_desk_jobs PRIVATE FIXTURE_DIR="${FIXTURE_DIR}")
    target_link_libraries(tst_desk_jobs PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Widgets Qt6::Test)
    add_test(NAME desk_jobs COMMAND tst_desk_jobs -platform offscreen)
    set_tests_properties(desk_jobs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
