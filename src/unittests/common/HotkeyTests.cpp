/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "HotkeyTests.h"

#include "common/Action.h"
#include "common/Hotkey.h"

#include <QTextStream>

namespace {

Action lockAction(Action::LockCursorMode mode, bool onRelease)
{
  Action action;
  action.setType(static_cast<int>(Action::Type::lockCursorToComputer));
  action.setLockCursorMode(static_cast<int>(mode));
  action.setActiveOnRelease(onRelease);
  return action;
}

Hotkey controlSlash()
{
  KeySequence sequence;
  sequence.appendKey(Qt::Key_Control, Qt::ControlModifier);
  sequence.appendKey(Qt::Key_Slash, Qt::ControlModifier);

  Hotkey hotkey;
  hotkey.setKeySequence(sequence);
  return hotkey;
}

QString streamed(const Hotkey &hotkey)
{
  QString out;
  QTextStream stream(&out);
  stream << hotkey;
  stream.flush();
  return out;
}

} // namespace

void HotkeyTests::stream_pressOnly_noSemicolon()
{
  auto hotkey = controlSlash();
  hotkey.addAction(lockAction(Action::LockCursorMode::off, false));
  hotkey.addAction(lockAction(Action::LockCursorMode::on, false));

  const auto line = streamed(hotkey);
  QVERIFY(!line.contains(QLatin1Char(';')));
  QVERIFY(line.endsWith(QStringLiteral(" = lockCursorToScreen(off), lockCursorToScreen(on)\n")));
}

void HotkeyTests::stream_pressAndRelease_releaseAfterSemicolon()
{
  auto hotkey = controlSlash();
  hotkey.addAction(lockAction(Action::LockCursorMode::off, false));
  hotkey.addAction(lockAction(Action::LockCursorMode::on, true));

  QVERIFY(streamed(hotkey).endsWith(QStringLiteral(" = lockCursorToScreen(off); lockCursorToScreen(on)\n")));
}

void HotkeyTests::stream_releaseOnly_leadingSemicolon()
{
  auto hotkey = controlSlash();
  hotkey.addAction(lockAction(Action::LockCursorMode::on, true));

  QVERIFY(streamed(hotkey).endsWith(QStringLiteral(" = ; lockCursorToScreen(on)\n")));
}

QTEST_MAIN(HotkeyTests)
