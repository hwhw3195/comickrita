kis_add_test(VCurveDataTest.cpp
    TEST_NAME VCurveDataTest
    LINK_LIBRARIES kritavstroke kritaflake kritaimage kritatestsdk
    )

kis_add_test(VGeomCurveShapeTest.cpp
    TEST_NAME VGeomCurveShapeTest
    LINK_LIBRARIES kritavstroke kritaflake kritaimage kritatestsdk
    )

# the Amount of Opacity/Density Increase and Decrease, and the smallest radius a shape keeps
kis_add_test(VEditValuesTest.cpp
    TEST_NAME VEditValuesTest
    LINK_LIBRARIES kritavstroke kritaflake kritaimage kritatestsdk
    )

kis_add_test(VStrokePainterTest.cpp
    TEST_NAME VStrokePainterTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_painter_test
    )

kis_add_test(VStrokeShapeTest.cpp
    TEST_NAME VStrokeShapeTest
    LINK_LIBRARIES kritavstroke kritaflake kritaimage kritatestsdk
    )

kis_add_test(VStrokeRenderTest.cpp
    TEST_NAME VStrokeRenderTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_render_test
    )
# Apply Current Brush: a stroke drawn again with another preset keeps what was recorded
kis_add_test(VLinePresetTest.cpp
    TEST_NAME VLinePresetTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_line_preset_test
    )
kis_add_test(VStrokeRecorderTest.cpp
    TEST_NAME VStrokeRecorderTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_recorder_test
    )
kis_add_test(VStrokeParityTest.cpp ${CMAKE_SOURCE_DIR}/sdk/tests/stroke_testing_utils.cpp
    TEST_NAME VStrokeParityTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_parity_test
    )
kis_add_test(VStrokeLayerTest.cpp
    TEST_NAME VStrokeLayerTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_layer_test
    )
kis_add_test(VPointEditTest.cpp
    TEST_NAME VPointEditTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritadefaultpaintops_static
                   kritalibpaintop kritaflake kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_point_edit_test
    )
# loads the installed plugins, as the application does; links the shared
# vstroke library only to look into the shapes (the plugins load the same one)
kis_add_test(VStrokeToolSwitchTest.cpp
    TEST_NAME VStrokeToolSwitchTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )

kis_add_test(VEditApplyTest.cpp
    TEST_NAME VEditApplyTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VClearPasteTest.cpp
    TEST_NAME VClearPasteTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
# nothing keeps a document's image after it closes (a window)
kis_add_test(VImageLifetimeTest.cpp
    TEST_NAME VImageLifetimeTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VScaleBrushSizeTest.cpp
    TEST_NAME VScaleBrushSizeTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VDeformTest.cpp
    TEST_NAME VDeformTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VPerspectiveTest.cpp
    TEST_NAME VPerspectiveTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VSelectionTransformTest.cpp
    TEST_NAME VSelectionTransformTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
kis_add_test(VSelectShapesTest.cpp
    TEST_NAME VSelectShapesTest
    LINK_LIBRARIES kritavstroke kritaui kritatestsdk
    )
foreach(_target ${vstroke_point_edit_test} ${vstroke_layer_test} ${vstroke_painter_test} ${vstroke_render_test} ${vstroke_line_preset_test} ${vstroke_recorder_test} ${vstroke_parity_test})
    target_compile_definitions(${_target} PRIVATE
        CSPBRUSH_DATA_DIR="${CMAKE_SOURCE_DIR}/plugins/paintops/cspbrush/tests/data/")
    target_include_directories(${_target} PRIVATE
        ${CMAKE_SOURCE_DIR}/plugins/paintops/defaultpaintops/brush
        ${CMAKE_SOURCE_DIR}/plugins/paintops/cspbrush
        ${CMAKE_BINARY_DIR}/plugins/paintops/defaultpaintops
        ${CMAKE_BINARY_DIR}/plugins/paintops/cspbrush)
endforeach()

# Trim on recorded strokes, and the ink size of CSP and pixel brushes
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VTrimKraTest.cpp")
kis_add_test(VTrimKraTest.cpp
    TEST_NAME VTrimKraTest
    LINK_LIBRARIES kritavstroke kritacspbrushpaintop_static kritalibpaintop kritaimage kritaui kritatestsdk
    TARGET_NAME_VAR vstroke_trim_kra_test
    )
target_include_directories(${vstroke_trim_kra_test} PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/paintops/cspbrush
    ${CMAKE_BINARY_DIR}/plugins/paintops/cspbrush)
endif()

foreach(_test VCurveWarpTest VCurveFitTest VCurveSubdivideSimplifyTest VCurveTrimTest)
    kis_add_test(${_test}.cpp
        TEST_NAME ${_test}
        LINK_LIBRARIES kritavstroke kritaflake kritaimage kritatestsdk
        )
endforeach()

# the Stroke tab on freehand layers (installed plugins, a window)
kis_add_test(VStrokeStyleTest.cpp
    TEST_NAME VStrokeStyleTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritaflake kritawidgets kritatestsdk
    )

# the transform tool on several layers at once (installed plugins, a window)
kis_add_test(VFastPreviewTest.cpp
    TEST_NAME VFastPreviewTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )

kis_add_test(VLiquifyModeSwitchTest.cpp
    TEST_NAME VLiquifyModeSwitchTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )

# what the canvas shows during a Liquify stroke on a freehand layer (installed plugins, a window)
kis_add_test(VLiquifyPreviewTest.cpp
    TEST_NAME VLiquifyPreviewTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )

# a Liquify stroke's history entry doesn't grow with its preview passes (installed plugins, a window)
kis_add_test(VLiquifyHistoryTest.cpp
    TEST_NAME VLiquifyHistoryTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )

# switching transform modes applies what is done so far (installed plugins, a window)
kis_add_test(VModeSwitchAppliesTest.cpp
    TEST_NAME VModeSwitchAppliesTest
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )

# Liquify timings on a large canvas (Design notes/liquify-performance-plan.md):
# outside the regular vstroke tests, run with `ctest -R VLiquifyBenchmark`
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VLiquifyBenchmark.cpp")
kis_add_test(VLiquifyBenchmark.cpp
    TEST_NAME VLiquifyBenchmark
    NAME_PREFIX "benchmarks-vstroke-"
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VLiquifyFileBenchmark.cpp")
kis_add_test(VLiquifyFileBenchmark.cpp
    TEST_NAME VLiquifyFileBenchmark
    NAME_PREFIX "benchmarks-vstroke-"
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VTransformBenchmark.cpp")
kis_add_test(VTransformBenchmark.cpp
    TEST_NAME VTransformBenchmark
    NAME_PREFIX "benchmarks-vstroke-"
    LINK_LIBRARIES kritavstroke kritaui kritaimage kritatestsdk
    )
endif()

# not a test: renders Edit Shapes' handles under candidate spacing thresholds
# (VPointEditTool::MinHandleSpacing), run with `ctest -R VHandleSpacingPreview`
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VHandleSpacingPreview.cpp")
kis_add_test(VHandleSpacingPreview.cpp
    TEST_NAME VHandleSpacingPreview
    LINK_LIBRARIES kritavstroke kritaflake kritaimage kritaui kritatestsdk
    )
endif()
