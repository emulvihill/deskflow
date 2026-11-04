/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

//! Wayland clipboard implementation using wl-copy/wl-paste
/*!
This class implements clipboard functionality for Wayland environments
by using the wl-clipboard utilities (wl-copy and wl-paste).

Writes made between open() and close() are buffered and published with a
single wl-copy call on close(). Changes made by other applications are
detected by fingerprinting the clipboard content; content this object
published itself is not reported as a change.
*/
class WlClipboard : public QObject, public IClipboard
{
  Q_OBJECT
public:
  explicit WlClipboard(ClipboardID id);
  WlClipboard(WlClipboard const &) = delete;
  WlClipboard(WlClipboard &&) = delete;
  ~WlClipboard() override;

  WlClipboard &operator=(WlClipboard const &) = delete;
  WlClipboard &operator=(WlClipboard &&) = delete;

  //! Get clipboard ID
  ClipboardID getID() const;

  //! Check if wl-clipboard tools are available
  static bool isAvailable();

  //! Check if WlClipboard is enabled
  static bool isEnabled();

  //! Start monitoring clipboard changes
  void startMonitoring();

  //! Stop monitoring clipboard changes
  void stopMonitoring();

  //! Read the clipboard now and record whether another application changed it
  void checkForChanges();

  //! Check if clipboard has changed
  bool hasChanged() const;

  //! Return and clear the changed flag
  bool takeChanged();

  //! Reset the changed flag and clear cache
  void resetChanged();

  //! Limit in bytes for reading clipboard data
  void setMaximumSize(std::size_t bytes);

  // IClipboard overrides
  bool empty() override;
  void add(Format format, const std::string &data) override;
  bool open(Time time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format format) const override;
  std::string get(Format format) const override;

private:
  //! Convert IClipboard format to MIME type
  QString formatToMimeType(Format format) const;

  //! Get available MIME types from clipboard
  QStringList getAvailableMimeTypes() const;

  //! Run wl-paste and read at most maxBytes of its output
  bool readPaste(const QStringList &args, std::size_t maxBytes, QByteArray &output, bool &oversized) const;

  //! Write the buffered data to the clipboard with one wl-copy call
  void publish() const;

  //! Fingerprint of the current clipboard content, empty if it could not be read
  std::optional<std::size_t> readFingerprint() const;

  //! Monitor clipboard changes in background thread
  void monitorClipboard();

  //! Get current clipboard serial/timestamp
  Time getCurrentTime() const;

  //! Invalidate cached clipboard data, m_cacheMutex must be held
  void invalidateCache() const;

private:
  ClipboardID m_id;
  mutable bool m_open = false;
  mutable Time m_time = 0;
  std::atomic<std::size_t> m_maximumSize = SIZE_MAX;
  std::atomic<bool> m_hasChanged = false;

  // Data written between open() and close(), published on close()
  mutable bool m_dirty = false;
  mutable std::string m_pendingData[static_cast<int>(Format::TotalFormats)];
  mutable bool m_pendingAvailable[static_cast<int>(Format::TotalFormats)] = {};

  // Cached clipboard data, guarded by m_cacheMutex
  mutable std::mutex m_cacheMutex;
  mutable bool m_cached = false;
  mutable Time m_cachedTime = 0;
  mutable std::string m_cachedData[static_cast<int>(Format::TotalFormats)];
  mutable bool m_cachedAvailable[static_cast<int>(Format::TotalFormats)] = {};

  // Change detection, guarded by m_changeMutex (taken before m_cacheMutex)
  mutable std::mutex m_changeMutex;
  mutable std::optional<std::size_t> m_lastFingerprint;
  mutable std::optional<std::size_t> m_ownFingerprint;

  // Background monitoring
  std::unique_ptr<std::thread> m_monitorThread;
  std::atomic<bool> m_monitoring = false;
  std::atomic<bool> m_stopMonitoring = false;

  // Clipboard selection type (true = clipboard, false = primary)
  bool m_useClipboard;
};
