/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WlClipboard.h"

#include "base/Log.h"

#include <algorithm>
#include <chrono>
#include <common/Settings.h>
#include <functional>
#include <string_view>

#include <QDateTime>
#include <QDeadlineTimer>
#include <QProcess>
#include <QStandardPaths>

namespace {

inline static const auto s_copyApp = QStringLiteral("wl-copy");
inline static const auto s_pasteApp = QStringLiteral("wl-paste");

// wl-clipboard args
inline static const auto s_listTypes = QStringLiteral("--list-types");
inline static const auto s_isPrimary = QStringLiteral("--primary");
inline static const auto s_noNewLine = QStringLiteral("-n");
inline static const auto s_readType = QStringLiteral("-t%1");
inline static const auto s_writeType = QStringLiteral("--type");
inline static const auto s_clear = QStringLiteral("--clear");

// MIME types for different clipboard formats
inline static const auto s_mimeTypeText = QStringLiteral("text/plain;charset=utf-8");
inline static const auto s_mimeTypeTextPlain = QStringLiteral("text/plain");
inline static const auto s_mimeTypeHtml = QStringLiteral("text/html");
inline static const auto s_mimeTypeBmp = QStringLiteral("image/bmp");

// Printed by wl-paste, with exit code 1, when the selection is empty
const char *const s_nothingCopied = "Nothing is copied";

// Timings (milliseconds)
const int kCacheValidityMs = 100;
const int kMonitorIntervalMs = 1000;
const int kMonitorStepMs = 50;
const int kCommandTimeoutMs = 2000;
const int kReadTimeoutMs = 5000;

// The type list is short, this only guards against a misbehaving owner
const std::size_t kMaxTypeListBytes = 64 * 1024;

int remainingMs(const QDeadlineTimer &deadline)
{
  return static_cast<int>(std::max<qint64>(0, deadline.remainingTime()));
}
} // namespace

WlClipboard::WlClipboard(ClipboardID id) : m_id(id), m_useClipboard(id == kClipboardClipboard)
{
  // nothing to do
}

WlClipboard::~WlClipboard()
{
  stopMonitoring();
}

ClipboardID WlClipboard::getID() const
{
  return m_id;
}

bool WlClipboard::isAvailable()
{
  return !QStandardPaths::findExecutable(s_copyApp).isEmpty() && !QStandardPaths::findExecutable(s_pasteApp).isEmpty();
}

bool WlClipboard::isEnabled()
{
  return Settings::value(Settings::Core::UseWlClipboard).toBool();
}

void WlClipboard::startMonitoring()
{
  if (m_monitoring) {
    return;
  }
  m_stopMonitoring = false;
  m_monitoring = true;
  m_monitorThread = std::make_unique<std::thread>(&WlClipboard::monitorClipboard, this);
}

void WlClipboard::stopMonitoring()
{
  if (!m_monitoring) {
    return;
  }

  m_stopMonitoring = true;
  m_monitoring = false;

  if (m_monitorThread && m_monitorThread->joinable()) {
    m_monitorThread->join();
  }
  m_monitorThread.reset();
}

void WlClipboard::checkForChanges()
{
  checkForChanges(QDeadlineTimer(kReadTimeoutMs));
}

void WlClipboard::checkForChanges(const QDeadlineTimer &deadline)
{
  const auto sequence = ++m_checkSequence;

  // Read without holding the lock, a failed or late read keeps the previous result
  const auto fingerprint = readFingerprint(deadline);
  if (!fingerprint) {
    return;
  }

  std::scoped_lock<std::mutex> lock(m_changeMutex);

  // A check or publish that started later has already recorded a newer state
  if (sequence < m_appliedSequence) {
    return;
  }
  m_appliedSequence = sequence;

  if (fingerprint == m_lastFingerprint) {
    return;
  }
  m_lastFingerprint = fingerprint;

  // Back to the content we published ourselves, nothing new to report
  if (fingerprint == m_ownFingerprint) {
    return;
  }
  m_ownFingerprint.reset();
  m_hasChanged = true;

  std::scoped_lock<std::mutex> cacheLock(m_cacheMutex);
  invalidateCache();
}

bool WlClipboard::hasChanged() const
{
  return m_hasChanged.load();
}

bool WlClipboard::takeChanged()
{
  if (!m_hasChanged.exchange(false)) {
    return false;
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  invalidateCache();
  return true;
}

void WlClipboard::setMaximumSize(std::size_t bytes)
{
  m_maximumSize = bytes;
}

bool WlClipboard::empty()
{
  if (!m_open) {
    return false;
  }

  m_dirty = true;
  for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
    m_pendingData[i].clear();
    m_pendingAvailable[i] = false;
  }
  return true;
}

void WlClipboard::add(Format format, const std::string &data)
{
  if (!m_open) {
    return;
  }

  if (format == Format::HTML) {
    return;
  }

  if (formatToMimeType(format).isEmpty()) {
    LOG_WARN("unsupported clipboard format: %d", format);
    return;
  }

  m_dirty = true;
  m_pendingData[static_cast<int>(format)] = data;
  m_pendingAvailable[static_cast<int>(format)] = true;
}

bool WlClipboard::open(Time time) const
{
  if (m_open) {
    LOG_DEBUG("failed to open clipboard: already opened");
    return false;
  }

  m_open = true;
  m_time = time;
  m_dirty = false;

  return true;
}

void WlClipboard::close() const
{
  if (!m_open) {
    return;
  }

  LOG_DEBUG("close clipboard");

  m_open = false;
  if (m_dirty) {
    m_dirty = false;
    publish();
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  invalidateCache();
}

IClipboard::Time WlClipboard::getTime() const
{
  return m_time;
}

bool WlClipboard::has(Format format) const
{
  if (!m_open) {
    return false;
  }

  // Answer from our own unpublished writes
  if (m_dirty) {
    return m_pendingAvailable[static_cast<int>(format)];
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);

  // Check cache validity
  Time currentTime = getCurrentTime();
  if (m_cached && (currentTime - m_cachedTime) < kCacheValidityMs) {
    return m_cachedAvailable[static_cast<int>(format)];
  }

  if (const auto availableTypes = getAvailableMimeTypes(QDeadlineTimer(kReadTimeoutMs)).value_or(QStringList());
      availableTypes.isEmpty()) {
    // No types available - mark all formats as unavailable
    for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
      m_cachedAvailable[i] = false;
      m_cachedData[i].clear();
    }
  } else {
    using enum IClipboard::Format;
    // Check each format against available types
    for (int i = 0; i < static_cast<int>(TotalFormats); ++i) {
      auto currentFormat = static_cast<Format>(i);
      const auto mimeType = formatToMimeType(currentFormat);

      m_cachedAvailable[i] = false;
      if (!mimeType.isEmpty()) {
        for (const auto &available : availableTypes) {
          if (available == mimeType || (currentFormat == Text && available == s_mimeTypeTextPlain) ||
              (currentFormat == HTML && available.startsWith(s_mimeTypeHtml))) {
            m_cachedAvailable[i] = true;
            break;
          }
        }
      }
    }
  }

  m_cached = true;
  m_cachedTime = currentTime;

  return m_cachedAvailable[static_cast<int>(format)];
}

std::string WlClipboard::get(Format format) const
{
  if (!m_open) {
    return std::string();
  }

  // Answer from our own unpublished writes
  if (m_dirty) {
    return m_pendingAvailable[static_cast<int>(format)] ? m_pendingData[static_cast<int>(format)] : std::string();
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);

  // Return cached data if available and valid
  if (m_cached && m_cachedAvailable[static_cast<int>(format)] && !m_cachedData[static_cast<int>(format)].empty()) {
    return m_cachedData[static_cast<int>(format)];
  }

  auto mimeType = formatToMimeType(format);
  if (mimeType.isEmpty()) {
    return std::string();
  }

  QStringList args = {s_noNewLine, s_readType.arg(mimeType)};
  if (!m_useClipboard)
    args.append(s_isPrimary);

  QByteArray output;
  bool oversized = false;
  const auto maxBytes = m_maximumSize.load();
  if (!readPaste(args, maxBytes, QDeadlineTimer(kReadTimeoutMs), output, oversized)) {
    if (oversized) {
      LOG_WARN("not reading clipboard data, exceeds limit: %zu KB", maxBytes / 1024);
    }
    output.clear();
  }

  auto data = output.toStdString();

  // Update cache
  m_cachedData[static_cast<int>(format)] = data;
  m_cachedAvailable[static_cast<int>(format)] = !data.empty();
  m_cached = true;
  m_cachedTime = getCurrentTime();

  return data;
}

QString WlClipboard::formatToMimeType(Format format) const
{
  switch (format) {
    using enum IClipboard::Format;
  case Text:
    return s_mimeTypeText;
  case HTML:
    return s_mimeTypeHtml;
  case Bitmap:
    return s_mimeTypeBmp;
  default:
    return {};
  }
}

std::optional<QStringList> WlClipboard::getAvailableMimeTypes(const QDeadlineTimer &deadline) const
{
  QStringList args = {s_listTypes};
  if (!m_useClipboard)
    args.append(s_isPrimary);

  // wl-paste fails when nothing is copied, which is an empty type list
  QByteArray output;
  bool oversized = false;
  bool nothingCopied = false;
  if (!readPaste(args, kMaxTypeListBytes, deadline, output, oversized, &nothingCopied)) {
    return nothingCopied ? std::optional<QStringList>(QStringList()) : std::nullopt;
  }

  const static QChar newLine = QLatin1Char('\n');
  return QString::fromLocal8Bit(output).split(newLine, Qt::SkipEmptyParts);
}

bool WlClipboard::readPaste(
    const QStringList &args, std::size_t maxBytes, const QDeadlineTimer &deadline, QByteArray &output,
    bool &oversized, bool *nothingCopied
) const
{
  output.clear();
  oversized = false;
  if (deadline.hasExpired()) {
    return false;
  }

  QProcess cmd;
  cmd.setProgram(s_pasteApp);
  cmd.setArguments(args);
  cmd.start(QIODevice::ReadOnly);
  if (!cmd.waitForStarted(remainingMs(deadline))) {
    LOG_WARN("failed to start wl-paste: %s", qPrintable(cmd.errorString()));
    return false;
  }

  // Read as the data arrives so an oversized selection is abandoned early
  for (;;) {
    output.append(cmd.readAllStandardOutput());
    if (static_cast<std::size_t>(output.size()) > maxBytes) {
      oversized = true;
      cmd.kill();
      cmd.waitForFinished();
      return false;
    }
    if (cmd.state() == QProcess::NotRunning) {
      break;
    }
    if (deadline.hasExpired()) {
      LOG_WARN("wl-paste did not finish in time, killing it");
      cmd.kill();
      cmd.waitForFinished();
      return false;
    }
    cmd.waitForReadyRead(remainingMs(deadline));
  }

  if (cmd.exitStatus() != QProcess::NormalExit || cmd.exitCode() != 0) {
    if (nothingCopied) {
      *nothingCopied = cmd.readAllStandardError().contains(s_nothingCopied);
    }
    return false;
  }
  return true;
}

void WlClipboard::publish() const
{
  using enum IClipboard::Format;

  // wl-copy serves one type per call, prefer text when there is a choice
  QStringList args = {s_clear};
  const std::string *payload = nullptr;
  Format payloadFormat = Text;
  for (const auto format : {Text, Bitmap}) {
    if (m_pendingAvailable[static_cast<int>(format)]) {
      args = {s_writeType, formatToMimeType(format)};
      payload = &m_pendingData[static_cast<int>(format)];
      payloadFormat = format;
      break;
    }
  }
  if (!m_useClipboard)
    args.prepend(s_isPrimary);

  // One budget for the whole call so a stuck wl-copy cannot stall the caller
  QDeadlineTimer deadline(kCommandTimeoutMs);

  // The payload goes through stdin: no size limit, binary safe and never
  // mistaken for an option
  QProcess cmd;
  cmd.setProgram(s_copyApp);
  cmd.setArguments(args);
  cmd.start();
  if (!cmd.waitForStarted(remainingMs(deadline))) {
    LOG_WARN("failed to start wl-copy: %s", qPrintable(cmd.errorString()));
    return;
  }
  if (payload) {
    cmd.write(payload->data(), static_cast<qint64>(payload->size()));
    while (cmd.bytesToWrite() > 0 && !deadline.hasExpired()) {
      cmd.waitForBytesWritten(remainingMs(deadline));
    }
  }
  cmd.closeWriteChannel();

  // wl-copy exits once the compositor has accepted the new selection,
  // a forked child keeps serving it
  if (!cmd.waitForFinished(remainingMs(deadline))) {
    LOG_WARN("wl-copy did not finish in time, killing it");
    cmd.kill();
    cmd.waitForFinished();
    return;
  }
  if (cmd.exitStatus() != QProcess::NormalExit || cmd.exitCode() != 0) {
    LOG_WARN("wl-copy failed: %s", cmd.readAllStandardError().trimmed().constData());
    return;
  }

  // Remember our own content so it is not reported as a local change. It is
  // derived from the payload, the same way a check would see it, rather than
  // read back. It also replaces any local change not yet reported.
  std::size_t fingerprint;
  if (!payload) {
    fingerprint = fingerprintOf({}, std::nullopt);
  } else if (payloadFormat == Text) {
    fingerprint = fingerprintOf({}, std::string_view(*payload));
  } else {
    fingerprint = fingerprintOf({formatToMimeType(payloadFormat)}, std::nullopt);
  }
  {
    std::scoped_lock<std::mutex> lock(m_changeMutex);
    m_appliedSequence = ++m_checkSequence;
    m_ownFingerprint = fingerprint;
    m_lastFingerprint = fingerprint;
    m_hasChanged = false;
  }

  std::scoped_lock<std::mutex> cacheLock(m_cacheMutex);
  invalidateCache();
}

std::optional<std::size_t> WlClipboard::readFingerprint(const QDeadlineTimer &deadline) const
{
  const auto types = getAvailableMimeTypes(deadline);
  if (!types) {
    return std::nullopt;
  }

  // Hash the text itself so a new copy with the same types is still seen,
  // and so a re-offer of the same text by another owner is not
  for (const auto &textType : {s_mimeTypeText, s_mimeTypeTextPlain}) {
    if (!types->contains(textType)) {
      continue;
    }

    QStringList args = {s_noNewLine, s_readType.arg(textType)};
    if (!m_useClipboard)
      args.append(s_isPrimary);

    // Oversized text is never sent, its first bytes are enough to tell copies apart
    QByteArray data;
    bool oversized = false;
    if (!readPaste(args, m_maximumSize.load(), deadline, data, oversized) && !oversized) {
      return std::nullopt;
    }
    return fingerprintOf(*types, std::string_view(data.constData(), data.size()));
  }

  return fingerprintOf(*types, std::nullopt);
}

std::size_t WlClipboard::fingerprintOf(const QStringList &types, const std::optional<std::string_view> &text)
{
  if (text) {
    return std::hash<std::string_view>{}(*text);
  }
  return std::hash<std::string>{}(types.join(QLatin1Char('\n')).prepend(QStringLiteral("types:")).toStdString());
}

void WlClipboard::monitorClipboard()
{
  while (!m_stopMonitoring) {
    for (int waited = 0; waited < kMonitorIntervalMs && !m_stopMonitoring; waited += kMonitorStepMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorStepMs));
    }
    if (!m_stopMonitoring) {
      checkForChanges();
    }
  }
}

IClipboard::Time WlClipboard::getCurrentTime() const
{
  return static_cast<Time>(QDateTime::currentMSecsSinceEpoch());
}

void WlClipboard::resetChanged()
{
  m_hasChanged = false;

  // Clear cache when resetting change flag to force fresh data retrieval
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  invalidateCache();
}

void WlClipboard::invalidateCache() const
{
  m_cached = false;
  m_cachedTime = 0;
  for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
    m_cachedData[i].clear();
    m_cachedAvailable[i] = false;
  }
}
