/*
 *  SPDX-FileCopyrightText: 2026 Krita Vector Stroke contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VSTROKE_LAYER_TEST_H
#define VSTROKE_LAYER_TEST_H

#include <QObject>

/**
 * Vector strokes inside real vector layers: image cloning as autosave
 * does it; strokes on transformed layers; layer duplicate, copy/paste and
 * color-label references (with stock shapes too); stroke copy/paste;
 * image resolution changes; the shared brush presets; Clear and
 * color-mixing brushes as the tool handles them; the live stroke drawn
 * inside its layer; and a stress test
 * that adds, transforms, renders and removes many strokes on a large canvas.
 */
class VStrokeLayerTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();

    void testClonePlainShapeLayer();
    void testCloneImageForAutosave();

    void testStrokeOnTransformedLayer_data();
    void testStrokeOnTransformedLayer();
    void testDuplicateLayer_data();
    void testDuplicateLayer();
    void testPasteLayer_data();
    void testPasteLayer();
    void testMergeLabeledLayersReference_data();
    void testMergeLabeledLayersReference();
    void testPasteStroke_data();
    void testPasteStroke();
    void testImageResolutionChange();

    void testPresetSharing();
    void testSavePresetDefinitions();
    void testMissingPaintop();

    void testClearLayer();
    void testLayerIcon();
    void testColorMixingPreset();
    void testLiveOverlay_data();
    void testLiveOverlay();
    void testLiveOverlayLayerOrder();
    void testLiveOverlayBlends();
    void testCommittedStrokeBlends();
    void testKritaBlendModeOnLayer();
    void testToolSwitching();

    void testFreehandFlagSaved();
    void testFreehandHooks();
    void testLiquifyCommands();
    void testLiquifyLatePreviewPass();
    void testLiquifyUndoKeepsUntouchedStrokes();
    void testFastLiquifyRendersWhenMovesPause();
    void testConvertLayer();
    void testFreehandLayerClips();
    void testEditLeavesNoStalePixels_data();
    void testEditLeavesNoStalePixels();
    void testTrimLeavesNoGhost_data();
    void testTrimLeavesNoGhost();
    void testEditKeepsRendering();

    void testStress();
};

#endif // VSTROKE_LAYER_TEST_H
