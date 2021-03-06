/*
 *      Copyright (C) 2014-2016 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "GUIConfigurationWizard.h"
#include "games/controllers/guicontrols/GUIFeatureButton.h"
#include "games/controllers/Controller.h"
#include "games/controllers/ControllerFeature.h"
#include "input/joysticks/IButtonMap.h"
#include "input/joysticks/IButtonMapCallback.h"
#include "input/InputManager.h"
#include "peripherals/Peripherals.h"
#include "threads/SingleLock.h"
#include "utils/log.h"

using namespace GAME;

#define ESC_KEY_CODE  27
#define SKIPPING_DETECTION_MS  200

// Duration to wait for axes to neutralize after mapping is finished
#define POST_MAPPING_WAIT_TIME_MS  (5 * 1000)

CGUIConfigurationWizard::CGUIConfigurationWizard(bool bEmulation, unsigned int controllerNumber /* = 0 */) :
  CThread("GUIConfigurationWizard"),
  m_bEmulation(bEmulation),
  m_controllerNumber(controllerNumber)
{
  InitializeState();
}

void CGUIConfigurationWizard::InitializeState(void)
{
  m_currentButton = nullptr;
  m_currentDirection = JOYSTICK::ANALOG_STICK_DIRECTION::UNKNOWN;
  m_history.clear();
}

void CGUIConfigurationWizard::Run(const std::string& strControllerId, const std::vector<IFeatureButton*>& buttons)
{
  Abort();

  {
    CSingleLock lock(m_stateMutex);

    // Set Run() parameters
    m_strControllerId = strControllerId;
    m_buttons = buttons;

    // Reset synchronization variables
    m_inputEvent.Reset();
    m_motionlessEvent.Reset();
    m_bInMotion.clear();

    // Initialize state variables
    InitializeState();
  }

  Create();
}

void CGUIConfigurationWizard::OnUnfocus(IFeatureButton* button)
{
  CSingleLock lock(m_stateMutex);

  if (button == m_currentButton)
    Abort(false);
}

bool CGUIConfigurationWizard::Abort(bool bWait /* = true */)
{
  if (IsRunning())
  {
    StopThread(false);

    m_inputEvent.Set();
    m_motionlessEvent.Set();

    if (bWait)
      StopThread(true);

    return true;
  }
  return false;
}

void CGUIConfigurationWizard::Process(void)
{
  CLog::Log(LOGDEBUG, "Starting configuration wizard");

  InstallHooks();

  {
    CSingleLock lock(m_stateMutex);
    for (IFeatureButton* button : m_buttons)
    {
      // Allow other threads to access the button we're using
      m_currentButton = button;

      while (!button->IsFinished())
      {
        // Allow other threads to access which direction the analog stick is on
        m_currentDirection = button->GetDirection();

        // Wait for input
        {
          CSingleExit exit(m_stateMutex);

          CLog::Log(LOGDEBUG, "%s: Waiting for input for feature \"%s\"", m_strControllerId.c_str(), button->Feature().Name().c_str());

          if (!button->PromptForInput(m_inputEvent))
            Abort(false);
        }

        if (m_bStop)
          break;
      }

      button->Reset();

      if (m_bStop)
        break;
    }

    // Finished mapping
    InitializeState();
  }

  for (auto callback : ButtonMapCallbacks())
    callback.second->SaveButtonMap();

  bool bInMotion;

  {
    CSingleLock lock(m_motionMutex);
    bInMotion = !m_bInMotion.empty();
  }

  if (bInMotion)
  {
    CLog::Log(LOGDEBUG, "Configuration wizard: waiting %ums for axes to neutralize", POST_MAPPING_WAIT_TIME_MS);
    m_motionlessEvent.WaitMSec(POST_MAPPING_WAIT_TIME_MS);
  }

  RemoveHooks();

  CLog::Log(LOGDEBUG, "Configuration wizard ended");
}

bool CGUIConfigurationWizard::MapPrimitive(JOYSTICK::IButtonMap* buttonMap,
                                           JOYSTICK::IActionMap* actionMap,
                                           const JOYSTICK::CDriverPrimitive& primitive)
{
  using namespace JOYSTICK;

  bool bHandled = false;

  // Handle esc key separately
  if (primitive.Type() == PRIMITIVE_TYPE::BUTTON &&
      primitive.Index() == ESC_KEY_CODE)
  {
    bHandled = Abort(false);
  }
  else if (m_history.find(primitive) != m_history.end())
  {
    // Primitive has already been mapped this round, ignore it
    bHandled = true;
  }
  else if (buttonMap->IsIgnored(primitive))
  {
    bHandled = true;
  }
  else
  {
    // Get the current state of the thread
    IFeatureButton* currentButton;
    ANALOG_STICK_DIRECTION currentDirection;
    {
      CSingleLock lock(m_stateMutex);
      currentButton = m_currentButton;
      currentDirection = m_currentDirection;
    }

    if (currentButton)
    {
      const CControllerFeature& feature = currentButton->Feature();

      CLog::Log(LOGDEBUG, "%s: mapping feature \"%s\" for device %s",
        m_strControllerId.c_str(), feature.Name().c_str(), buttonMap->DeviceName().c_str());

      switch (feature.Type())
      {
        case FEATURE_TYPE::SCALAR:
        {
          buttonMap->AddScalar(feature.Name(), primitive);
          bHandled = true;
          break;
        }
        case FEATURE_TYPE::ANALOG_STICK:
        {
          buttonMap->AddAnalogStick(feature.Name(), currentDirection, primitive);
          bHandled = true;
          break;
        }
        default:
          break;
      }

      if (bHandled)
      {
        m_history.insert(primitive);

        OnMotion(buttonMap);
        m_inputEvent.Set();
      }
    }
  }
  
  return bHandled;
}

void CGUIConfigurationWizard::OnEventFrame(const JOYSTICK::IButtonMap* buttonMap, bool bMotion)
{
  CSingleLock lock(m_motionMutex);

  if (m_bInMotion.find(buttonMap) != m_bInMotion.end() && !bMotion)
    OnMotionless(buttonMap);
}

void CGUIConfigurationWizard::OnMotion(const JOYSTICK::IButtonMap* buttonMap)
{
  CSingleLock lock(m_motionMutex);

  m_motionlessEvent.Reset();
  m_bInMotion.insert(buttonMap);
}

void CGUIConfigurationWizard::OnMotionless(const JOYSTICK::IButtonMap* buttonMap)
{
  m_bInMotion.erase(buttonMap);
  if (m_bInMotion.empty())
    m_motionlessEvent.Set();
}

bool CGUIConfigurationWizard::OnKeyPress(const CKey& key)
{
  return Abort(false);
}

bool CGUIConfigurationWizard::OnButtonPress(const std::string& button)
{
  return Abort(false);
}

void CGUIConfigurationWizard::InstallHooks(void)
{
  using namespace PERIPHERALS;

  g_peripherals.RegisterJoystickButtonMapper(this);
  g_peripherals.RegisterObserver(this);

  // If we're not using emulation, allow keyboard input to abort prompt
  if (!m_bEmulation)
    CInputManager::GetInstance().RegisterKeyboardHandler(this);

  CInputManager::GetInstance().RegisterMouseHandler(this);
}

void CGUIConfigurationWizard::RemoveHooks(void)
{
  using namespace PERIPHERALS;

  CInputManager::GetInstance().UnregisterMouseHandler(this);

  if (!m_bEmulation)
    CInputManager::GetInstance().UnregisterKeyboardHandler(this);

  g_peripherals.UnregisterObserver(this);
  g_peripherals.UnregisterJoystickButtonMapper(this);
}

void CGUIConfigurationWizard::Notify(const Observable& obs, const ObservableMessage msg)
{
  using namespace PERIPHERALS;

  switch (msg)
  {
    case ObservableMessagePeripheralsChanged:
    {
      g_peripherals.UnregisterJoystickButtonMapper(this);
      g_peripherals.RegisterJoystickButtonMapper(this);
      break;
    }
    default:
      break;
  }
}
