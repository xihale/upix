cmake_minimum_required(VERSION 3.21)
project(upix-desktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Widgets Svg)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(upix
    src/main.cpp
    src/theme.cpp
    src/parser.cpp
    src/desk.cpp
    src/qrprovider.cpp
    src/icons.cpp
    src/qrcodegen.cpp
)

target_include_directories(upix PRIVATE src)

if(CMAKE_BUILD_TYPE STREQUAL "" OR NOT CMAKE_BUILD_TYPE)
    set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
else()
    set(FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()
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

target_link_libraries(upix PRIVATE Qt6::Quick)

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
endif()
