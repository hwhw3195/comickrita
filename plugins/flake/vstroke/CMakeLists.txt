add_subdirectory(tests)

set(kritavstroke_SOURCES
    curve/VCurveData.cpp
    curve/VCurveOutline.cpp
    shape/VCurveShape.cpp
    shape/VCurveConverter.cpp
    curve/VCurveFit.cpp
    curve/VCurveOpacity.cpp
    curve/VCurveSimplify.cpp
    curve/VCurveTrim.cpp
    curve/VCurveWarp.cpp
    curve/VFalloff.cpp
    tools/VEditActions.cpp
    tools/VEditTool.cpp
    shape/VInkSize.cpp
    shape/VGeomCurveShape.cpp
    layer/VFreehandLayer.cpp
    transform/VSelectionSplit.cpp
    transform/VLiquify.cpp
    transform/VDeform.cpp
    transform/VMoveSession.cpp
    tools/VPointEdit.cpp
    layer/VSelectionCover.cpp
    tools/VPointEditTool.cpp
    curve/VStrokePrimitive.cpp
    shape/VStrokePainter.cpp
    shape/VStrokePresetStore.cpp
    curve/VStrokeCurves.cpp
    shape/VStrokeShape.cpp
    shape/VStrokeShapeFactory.cpp
    shape/VStrokeRenderCache.cpp
    layer/VStrokeRecorderFacade.cpp
    tools/VStrokeTool.cpp
    tools/VStrokeToolSwitcher.cpp
)

# The lag log (VTrace.h), for debugging only: off, the timing points in the
# sources compile to nothing. On, it still records only when Krita starts
# with KRITA_VSTROKE_TRACE set (a file path, or 1).
option(VSTROKE_TRACE "Build the vstroke lag log (debugging)" OFF)
if (VSTROKE_TRACE)
    list(APPEND kritavstroke_SOURCES VTrace.cpp)
endif()

# shared: the tool plugin and the action plugin use the same code and state
kis_add_library(kritavstroke SHARED ${kritavstroke_SOURCES})
generate_export_header(kritavstroke BASE_NAME kritavstroke EXPORT_MACRO_NAME KRITAVSTROKE_EXPORT)
# the sources are grouped in subfolders; includes stay flat ("VCurveData.h")
target_include_directories(kritavstroke PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/curve
    ${CMAKE_CURRENT_SOURCE_DIR}/shape
    ${CMAKE_CURRENT_SOURCE_DIR}/transform
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/layer
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kritavstroke kritaimage kritaflake kritaui kritalibpaintop)
if (VSTROKE_TRACE)
    target_compile_definitions(kritavstroke PUBLIC VSTROKE_TRACE)
    if (WIN32)
        # VTrace: the process's working set
        target_link_libraries(kritavstroke psapi)
    endif()
endif()
set_target_properties(kritavstroke PROPERTIES
    VERSION ${GENERIC_KRITA_LIB_VERSION} SOVERSION ${GENERIC_KRITA_LIB_SOVERSION}
)
install(TARGETS kritavstroke ${INSTALL_TARGETS_DEFAULT_ARGS})

# the shapes and the tools
kis_add_library(krita_vstroke MODULE VStrokePlugin.cpp)
target_link_libraries(krita_vstroke kritavstroke)
install(TARGETS krita_vstroke DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

# the layer actions (a view plugin)
kis_add_library(kritavstrokeactions MODULE VStrokeActions.cpp)
target_link_libraries(kritavstrokeactions kritavstroke)
install(TARGETS kritavstrokeactions DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install(FILES vstroke.action VEditTool.action DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)
