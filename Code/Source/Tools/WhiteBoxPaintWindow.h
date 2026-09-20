/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

#include <AzCore/Component/ComponentBus.h>
#include "SubComponentModes/WhiteBoxPaintSettings.h"
#include "Tools/WhiteBoxModelingWindow.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QWidget;

namespace AzToolsFramework
{
    class PropertyAssetCtrl;
}

namespace WhiteBox
{
    class EditorWhiteBoxComponent;

    //! What the paint brush carries: the material for Paint Material, the colour for Paint Color.
    //! The operation itself is picked in the viewport cluster, not here - this window only holds the
    //! payload that operation needs, and shows nothing at all for the two reset verbs, which have none.
    class WhiteBoxPaintWindow : public WhiteBoxModelingWindow
    {
    public:
        WhiteBoxPaintWindow(const AZ::EntityComponentIdPair& pair, QWidget* parent);
        //! Re-read the component after the cluster (or the pane) changes the operation.
        void RefreshValues();

    private:
        EditorWhiteBoxComponent* CurrentComponent() const;
        void ChooseColor();
        //! Paint the swatch button with @p color so the window shows what it will apply.
        void ApplySwatch(AZ::u32 color);
        void ApplyVisibility(FacePaintOperation operation);

        AZ::EntityComponentIdPair m_pair;
        QLabel* m_title = nullptr;
        QLabel* m_hint = nullptr;
        QLabel* m_materialLabel = nullptr;
        AzToolsFramework::PropertyAssetCtrl* m_material = nullptr;
        QLabel* m_colorLabel = nullptr;
        QPushButton* m_color = nullptr;
        QCheckBox* m_wholePolygon = nullptr;
        bool m_updating = false;
    };
} // namespace WhiteBox
