/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogVideoOSD.h"

#include "GUIUserMessages.h"
#include "ServiceBroker.h"
#include "application/Application.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/WindowIDs.h"
#include "input/InputManager.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "input/mouse/MouseEvent.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

using namespace KODI;
using namespace PVR;

CGUIDialogVideoOSD::CGUIDialogVideoOSD(void)
    : CGUIDialog(WINDOW_DIALOG_VIDEO_OSD, "VideoOSD.xml")
{
  m_loadType = LOAD_ON_GUI_INIT;
}

CGUIDialogVideoOSD::~CGUIDialogVideoOSD(void) = default;

void CGUIDialogVideoOSD::FrameMove()
{
  if (m_autoClosing)
  {
    // check for movement of mouse or a submenu open
    if (CServiceBroker::GetInputManager().IsMouseActive()
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_AUDIO_OSD_SETTINGS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_SUBTITLE_OSD_SETTINGS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_VIDEO_OSD_SETTINGS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_CMS_OSD_SETTINGS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_VIDEO_BOOKMARKS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_PVR_OSD_CHANNELS)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_PVR_CHANNEL_GUIDE)
                           || CServiceBroker::GetGUI()->GetWindowManager().IsWindowActive(WINDOW_DIALOG_OSD_TELETEXT))
      // extend show time by original value
      SetAutoClose(m_showDuration);
  }
  CGUIDialog::FrameMove();
}

void CGUIDialogVideoOSD::Render()
{
  auto* winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem)
    // Without a window system there is no graphics context or render order to
    // restore, so skip the OSD pass entirely.
    return;

  auto& gfxContext = winSystem->GetGfxContext();
  const RENDER_ORDER renderOrder = gfxContext.GetRenderOrder();

  // Skip rendering the OSD during the front-to-back stage of the two-pass GUI
  // pipeline so it can be rendered in the later back-to-front stage without
  // being split across both passes while fullscreen video is composed under it.
  if (renderOrder == RENDER_ORDER_FRONT_TO_BACK)
    return;

  if (renderOrder == RENDER_ORDER_BACK_TO_FRONT)
    // When rendering during the back-to-front stage, temporarily switch to
    // ALL_BACK_TO_FRONT so the whole OSD is drawn together instead of
    // inheriting the split-pass dialog ordering.
    gfxContext.SetRenderOrder(RENDER_ORDER_ALL_BACK_TO_FRONT);

  CGUIDialog::Render();

  if (renderOrder == RENDER_ORDER_BACK_TO_FRONT)
    gfxContext.SetRenderOrder(renderOrder);
}

bool CGUIDialogVideoOSD::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_SHOW_OSD)
  {
    Close();
    return true;
  }

  return CGUIDialog::OnAction(action);
}

EVENT_RESULT CGUIDialogVideoOSD::OnMouseEvent(const CPoint& point, const MOUSE::CMouseEvent& event)
{
  if (event.m_id == ACTION_MOUSE_WHEEL_UP)
  {
    return g_application.OnAction(CAction(ACTION_ANALOG_SEEK_FORWARD, 0.5f)) ? EVENT_RESULT_HANDLED : EVENT_RESULT_UNHANDLED;
  }
  if (event.m_id == ACTION_MOUSE_WHEEL_DOWN)
  {
    return g_application.OnAction(CAction(ACTION_ANALOG_SEEK_BACK, 0.5f)) ? EVENT_RESULT_HANDLED : EVENT_RESULT_UNHANDLED;
  }

  return CGUIDialog::OnMouseEvent(point, event);
}

bool CGUIDialogVideoOSD::OnMessage(CGUIMessage& message)
{
  switch ( message.GetMessage() )
  {
  case GUI_MSG_VIDEO_MENU_STARTED:
    {
      // We have gone to the DVD menu, so close the OSD.
      Close();
    }
    break;
  case GUI_MSG_WINDOW_DEINIT:  // fired when OSD is hidden
    {
      // Remove our subdialogs if visible
      CGUIDialog *pDialog = CServiceBroker::GetGUI()->GetWindowManager().GetDialog(WINDOW_DIALOG_AUDIO_OSD_SETTINGS);
      if (pDialog && pDialog->IsDialogRunning())
        pDialog->Close(true);
      pDialog = CServiceBroker::GetGUI()->GetWindowManager().GetDialog(WINDOW_DIALOG_SUBTITLE_OSD_SETTINGS);
      if (pDialog && pDialog->IsDialogRunning())
        pDialog->Close(true);
    }
    break;
  }
  return CGUIDialog::OnMessage(message);
}
