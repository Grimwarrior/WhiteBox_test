/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "Tools/WhiteBoxUvEditorPane.h"
#include "EditorWhiteBoxComponent.h"
#include "SubComponentModes/EditorWhiteBoxTransformModeBus.h"
#include "Util/WhiteBoxEditorUtil.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/ComponentMode/EditorComponentModeBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <WhiteBox/EditorWhiteBoxComponentBus.h>

#include <QButtonGroup>
#include <QEvent>
#include <QGraphicsEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace WhiteBox
{
    namespace
    {
        // The first selected entity carrying a White Box, or an invalid pair.
        AZ::EntityComponentIdPair SelectedWhiteBox()
        {
            AzToolsFramework::EntityIdList selected;
            AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
                selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
            for (const AZ::EntityId entityId : selected)
            {
                AZ::Entity* entity = nullptr;
                AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationRequests::FindEntity, entityId);
                if (auto* whiteBox = entity != nullptr ? entity->FindComponent<EditorWhiteBoxComponent>() : nullptr)
                {
                    return AZ::EntityComponentIdPair(entityId, whiteBox->GetId());
                }
            }
            return AZ::EntityComponentIdPair();
        }
    } // namespace

    WhiteBoxUvEditorPane::WhiteBoxUvEditorPane(QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto* tools = new QHBoxLayout();
        tools->setSpacing(2);
        const auto makeButton = [this, tools](const QString& text, const QString& tip, const bool checkable)
        {
            auto* button = new QToolButton(this);
            button->setText(text);
            button->setToolTip(tip);
            button->setCheckable(checkable);
            button->setAutoRaise(true);
            tools->addWidget(button);
            return button;
        };
        m_move = makeButton(tr("Move"), tr("Drag selected UV points to move them. Ctrl snaps to 1/32."), true);
        m_rotate = makeButton(tr("Rotate"), tr("Drag to rotate the selection about its centre. Ctrl snaps to 15 degrees."), true);
        m_scale = makeButton(tr("Scale"), tr("Drag to scale the selection about its centre. Ctrl snaps to tenths."), true);
        m_move->setChecked(true);
        auto* toolGroup = new QButtonGroup(this);
        toolGroup->setExclusive(true);
        toolGroup->addButton(m_move);
        toolGroup->addButton(m_rotate);
        toolGroup->addButton(m_scale);
        tools->addSpacing(8);
        auto* flipU = makeButton(tr("Flip U"), tr("Mirror the selection (or everything) left to right."), false);
        auto* flipV = makeButton(tr("Flip V"), tr("Mirror the selection (or everything) top to bottom."), false);
        auto* rotate90 = makeButton(tr("Rotate 90"), tr("Turn the selection (or everything) a quarter turn clockwise."), false);
        auto* fit = makeButton(tr("Fit 0-1"), tr("Scale and move the selection (or everything) uniformly to fill the unit square."), false);
        tools->addSpacing(8);
        auto* frame = makeButton(tr("Frame"), tr("Frame the selection, or everything when nothing is selected. [F]"), false);
        auto* all = makeButton(tr("Select All"), tr("Select every UV point. [A]"), false);
        tools->addStretch();
        layout->addLayout(tools);

        m_canvas = new WhiteBoxUvCanvas(this);
        layout->addWidget(m_canvas, 1);
        m_status = new QLabel(this);
        layout->addWidget(m_status);

        m_canvas->m_onEdit = [this](const AZStd::vector<UvChange>& changes, const bool final) { ApplyEdit(changes, final); };
        m_canvas->m_onStatus = [this](const QString& message) { m_status->setText(message); };

        connect(m_move, &QToolButton::toggled, this, [this](bool on) { if (on) { m_canvas->SetTool(WhiteBoxUvCanvas::Tool::Move); } });
        connect(m_rotate, &QToolButton::toggled, this, [this](bool on) { if (on) { m_canvas->SetTool(WhiteBoxUvCanvas::Tool::Rotate); } });
        connect(m_scale, &QToolButton::toggled, this, [this](bool on) { if (on) { m_canvas->SetTool(WhiteBoxUvCanvas::Tool::Scale); } });
        connect(flipU, &QToolButton::clicked, this, [this]()
        {
            m_canvas->TransformTargets([](const AZ::Vector2& uv, const AZ::Vector2& centre)
            {
                return AZ::Vector2(2.0f * centre.GetX() - uv.GetX(), uv.GetY());
            });
        });
        connect(flipV, &QToolButton::clicked, this, [this]()
        {
            m_canvas->TransformTargets([](const AZ::Vector2& uv, const AZ::Vector2& centre)
            {
                return AZ::Vector2(uv.GetX(), 2.0f * centre.GetY() - uv.GetY());
            });
        });
        connect(rotate90, &QToolButton::clicked, this, [this]()
        {
            // V runs down the view, so (x, y) -> (-y, x) turns clockwise on screen.
            m_canvas->TransformTargets([](const AZ::Vector2& uv, const AZ::Vector2& centre)
            {
                const AZ::Vector2 local = uv - centre;
                return centre + AZ::Vector2(-local.GetY(), local.GetX());
            });
        });
        connect(fit, &QToolButton::clicked, this, [this]() { m_canvas->FitTargetsToUnitSquare(); });
        connect(frame, &QToolButton::clicked, this, [this]() { m_canvas->FrameSelection(); });
        connect(all, &QToolButton::clicked, this, [this]() { m_canvas->SelectAll(); });

        auto* refresh = new QTimer(this);
        connect(refresh, &QTimer::timeout, this, [this]() { Refresh(); });
        refresh->start(150);
        Refresh();

        AzFramework::EntityContextId editorEntityContextId = AzFramework::EntityContextId::CreateNull();
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
            editorEntityContextId, &AzToolsFramework::EditorEntityContextRequests::GetEditorEntityContextId);
        AzToolsFramework::ViewportEditorModeNotificationsBus::Handler::BusConnect(editorEntityContextId);
        EnsureEnabledInComponentMode(); // docked at startup, the pane may already be disabled
    }

    WhiteBoxUvEditorPane::~WhiteBoxUvEditorPane()
    {
        AzToolsFramework::ViewportEditorModeNotificationsBus::Handler::BusDisconnect();
    }

    void WhiteBoxUvEditorPane::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::EnabledChange && !isEnabled())
        {
            EnsureEnabledInComponentMode();
        }
    }

    void WhiteBoxUvEditorPane::OnEditorModeActivated(
        [[maybe_unused]] const AzToolsFramework::ViewportEditorModesInterface& editorModeState,
        const AzToolsFramework::ViewportEditorMode mode)
    {
        if (mode == AzToolsFramework::ViewportEditorMode::Component)
        {
            EnsureEnabledInComponentMode();
        }
    }

    void WhiteBoxUvEditorPane::EnsureEnabledInComponentMode()
    {
        // Deferred so it runs after whatever disabled the dock panes has finished.
        QTimer::singleShot(
            0, this,
            [this]()
            {
                namespace Cmf = AzToolsFramework::ComponentModeFramework;
                bool inComponentMode = false;
                Cmf::ComponentModeSystemRequestBus::BroadcastResult(inComponentMode, &Cmf::ComponentModeSystemRequests::InComponentMode);
                if (!inComponentMode)
                {
                    return; // outside component mode (game or simulation) the disable stands
                }
                // Whichever ancestor the editor disabled (the pane or its dock), and the grey effect it dims them with.
                for (QWidget* widget = this; widget != nullptr; widget = widget->parentWidget())
                {
                    if (!widget->isEnabled())
                    {
                        widget->setEnabled(true);
                    }
                    if (widget->graphicsEffect() != nullptr)
                    {
                        widget->setGraphicsEffect(nullptr);
                    }
                }
                for (QWidget* child : findChildren<QWidget*>())
                {
                    if (child->graphicsEffect() != nullptr)
                    {
                        child->setGraphicsEffect(nullptr);
                    }
                }
            });
    }

    void WhiteBoxUvEditorPane::Refresh()
    {
        // Mid-drag the canvas already holds the newest UVs; rebuilding would fight the gesture.
        if (m_canvas->IsDragging() || m_editing)
        {
            return;
        }
        m_pair = SelectedWhiteBox();
        auto* component = m_pair.GetEntityId().IsValid() ? FindWhiteBoxComponent(m_pair) : nullptr;
        const WhiteBoxMesh* mesh = component != nullptr ? component->GetWhiteBoxMesh() : nullptr;
        Api::PolygonHandles polygons;
        if (mesh != nullptr)
        {
            EditorWhiteBoxTransformModeRequestBus::EventResult(
                polygons, m_pair, &EditorWhiteBoxTransformModeRequests::GetSelectedPolygons);
        }
        m_canvas->SetModel(mesh != nullptr ? BuildUvModel(*mesh, polygons) : UvModel{});
    }

    void WhiteBoxUvEditorPane::ApplyEdit(const AZStd::vector<UvChange>& changes, const bool final)
    {
        auto* component = FindWhiteBoxComponent(m_pair);
        WhiteBoxMesh* mesh = component != nullptr ? component->GetWhiteBoxMesh() : nullptr;
        if (mesh == nullptr)
        {
            m_editing = false;
            return;
        }
        if (!m_editing)
        {
            // A parametric layer would regenerate over the edit, so it is frozen before the first change lands.
            m_editing = true;
            component->BakeParametricLayer(component->GetActiveLayerIndex());
        }
        const auto halfedgeCount = Api::MeshHalfedgeCount(*mesh);
        for (const auto& change : changes)
        {
            // The mesh may have changed under the view (undo); a corner that no longer exists is skipped.
            if (change.first.IsValid() && static_cast<AZ::u64>(change.first.Index()) < halfedgeCount)
            {
                Api::SetHalfedgeManualUv(*mesh, change.first, change.second);
            }
        }
        if (!final)
        {
            // Mid-drag only the picture updates; the collider and game bake wait for the gesture to end.
            if (component->AssetInUse())
            {
                EditorWhiteBoxComponentNotificationBus::Event(m_pair, &EditorWhiteBoxComponentNotifications::OnWhiteBoxMeshModified);
            }
            else
            {
                component->RebuildWhiteBoxDeferred();
            }
            return;
        }
        m_editing = false;
        AzToolsFramework::ScopedUndoBatch undoBatch("White Box UV Edit");
        component->SerializeWhiteBox();
        EditorWhiteBoxComponentNotificationBus::Event(m_pair, &EditorWhiteBoxComponentNotifications::OnWhiteBoxMeshModified);
        undoBatch.MarkEntityDirty(m_pair.GetEntityId());
    }
} // namespace WhiteBox
