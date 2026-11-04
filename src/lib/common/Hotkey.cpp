/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "Hotkey.h"

#include <QSettings>
#include <QStringList>

QString Hotkey::text() const
{
  return m_keySequence.isMouseButton() ? kMousebutton.arg(m_keySequence.toString())
                                       : kKeystroke.arg(m_keySequence.toString());
}

Action &Hotkey::actionAt(int index)
{
  return m_actions[index];
}

void Hotkey::addAction(const Action &action)
{
  if (m_actions.contains(action))
    return;
  m_actions.append(action);
}

void Hotkey::removeActionAt(int index)
{
  if (index < 0 || index >= m_actions.size())
    return;
  m_actions.removeAt(index);
}

void Hotkey::loadSettings(QSettings &settings)
{
  m_keySequence.loadSettings(settings);

  m_actions.clear();
  int num = settings.beginReadArray(kSectionActions);
  for (int i = 0; i < num; i++) {
    settings.setArrayIndex(i);
    Action a;
    a.loadSettings(settings);
    m_actions.append(a);
  }

  settings.endArray();
}

void Hotkey::saveSettings(QSettings &settings) const
{
  m_keySequence.saveSettings(settings);

  settings.beginWriteArray(kSectionActions);
  for (int i = 0; i < m_actions.size(); i++) {
    settings.setArrayIndex(i);
    m_actions.at(i).saveSettings(settings);
  }
  settings.endArray();
}

bool Hotkey::operator==(const Hotkey &hk) const
{
  return m_keySequence == hk.keySequence() && m_actions == hk.actions();
}

QTextStream &operator<<(QTextStream &outStream, const Hotkey &hotkey)
{
  // Don't write a config if there is no actions
  if (hotkey.actions().size() == 0)
    return outStream;

  // The server reads "keystroke(k) = press actions; release actions",
  // so actions marked active-on-release go after a single ';'.
  QStringList onPress;
  QStringList onRelease;
  for (const auto &action : hotkey.actions()) {
    (action.activeOnRelease() ? onRelease : onPress).append(action.text());
  }

  QString outText = QStringLiteral("\t%1 = ").arg(hotkey.text());
  outText.append(onPress.join(QStringLiteral(", ")));
  if (!onRelease.isEmpty()) {
    outText.append(QStringLiteral("; ")).append(onRelease.join(QStringLiteral(", ")));
  }
  outText.append(QStringLiteral("\n"));

  outStream << outText;
  return outStream;
}
