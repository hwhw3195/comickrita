add_subdirectory(tests)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../spray)

set(kritacspbrushpaintop_static_SOURCES
    CspBrushData.cpp
    CspEffector.cpp
    CspBrushParams.cpp
    CspColorMath.cpp
    CspTipSet.cpp
    CspTipImport.cpp
    CspTipCreator.cpp
    CspRibbonBuilder.cpp
    CspTextureStage.cpp
    CspStrokeBuffer.cpp
    CspColorMixer.cpp
    CspColorJitter.cpp
    KisCspBrushPaintOp.cpp
    KisCspBrushSettings.cpp
    CspEffectorEditor.cpp
    KisCspBrushOptionPage.cpp
    KisCspBrushPages.cpp
    KisCspBrushSettingsWidget.cpp
    ../spray/KisSprayRandomDistributions.cpp
)

kis_add_library(kritacspbrushpaintop_static STATIC ${kritacspbrushpaintop_static_SOURCES})
generate_export_header(kritacspbrushpaintop_static BASE_NAME kritacspbrushpaintop EXPORT_MACRO_NAME KRITACSPBRUSHPAINTOP_EXPORT)
target_link_libraries(kritacspbrushpaintop_static kritalibpaintop kritalibbrush kritaui)
if(LibMyPaint_FOUND)
    # spectral color mixing (Perceptual), the model of Krita's MyPaint engine
    target_link_libraries(kritacspbrushpaintop_static LibMyPaint::mypaint)
endif()

kis_add_library(kritacspbrushpaintop MODULE CspBrushPaintOpPlugin.cpp)
target_link_libraries(kritacspbrushpaintop kritacspbrushpaintop_static)

install(TARGETS kritacspbrushpaintop DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
