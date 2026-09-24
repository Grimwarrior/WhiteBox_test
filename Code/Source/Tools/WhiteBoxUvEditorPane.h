/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "Tools/WhiteBoxUvCanvas.h"

#include <AzCore/Component/ComponentBus.h>
#include <AzToolsFramework/API/ViewportEditorModeTrackerNotificationBus.h>
#include <QWidget>

class QLabel;
class QToolButton;

namespace WhiteBox
{
    //! Dockable UV editor (Tools > White Box UV Editor) for the polygons selected in White Box Transform mode.
    //! Edited faces become Manual, so their UVs are kept through later modelling; a whole drag is one undo step.
    class WhiteBoxUvEditorPane
        : public QWidget
        , private AzToolsFramework::ViewportEditorModeNotificationsBus::Handler
    {
    public:
        static constexpr const char* PaneName = "White Box UV Editor";

        explicit WhiteBoxUvEditorPane(QWidget* parent = nullptr);
        ~WhiteBoxUvEditorPane() override;

    protected:
        //! The editor disables every dock pane when component mode begins; this one is part of White Box editing.
        void changeEvent(QEvent* event) override;

    private:
        // ViewportEditorModeNotificationsBus ...
        void OnEditorModeActivated(
            const AzToolsFramework::ViewportEditorModesInterface& editorModeState, AzToolsFramework::ViewportEditorMode mode) override;

        //! Undo the component-mode disable (and its grey dimming), deferred until the editor has finished applying it.
        void EnsureEnabledInComponentMode();
        //! Follow the selected White Box entity and its polygon selection; skipped mid-drag.
        void Refresh();
        void ApplyEdit(const AZStd::vector<UvChange>& changes, bool final);

        AZ::EntityComponentIdPair m_pair;
        bool m_editing = false; //!< Between the first change of a gesture and its final one.
        WhiteBoxUvCanvas* m_canvas = nullptr;
        QLabel* m_status = nullptr;
        QToolButton* m_move = nullptr;
        QToolButton* m_rotate = nullptr;
        QToolButton* m_scale = nullptr;
    };
} // namespace WhiteBox
