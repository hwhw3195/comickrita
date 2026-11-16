kis_add_tests(
    CspBrushEngineTest.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritaimage kritatestsdk
    )

kis_add_tests(
    CspBrushRenderTest.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritaimage kritaui kritatestsdk
    )

kis_add_tests(
    CspBrushWidgetTest.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritaimage kritaui kritatestsdk
    )

# with a resource database: the tip library
kis_add_tests(
    CspTipLibraryTest.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritalibbrush kritaresources kritaimage kritaui kritatestsdk
    )

kis_add_tests(
    CspBrushStrokeTest.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritaimage kritaui kritatestsdk
    )

# a benchmark, not a test: a public archive leaves it out
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CspBrushMixBenchmark.cpp")
kis_add_tests(
    CspBrushMixBenchmark.cpp
    LINK_LIBRARIES kritacspbrushpaintop_static kritalibpaintop kritaimage kritaui kritatestsdk
    )
endif()
