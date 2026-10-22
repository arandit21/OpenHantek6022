// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QImage>
#include <QString>
#include <QSize>
#include <QString>

/// Small synchronous MP4 recorder backed by Windows Media Foundation.
/// Frames are the rendered scope widget; no oscilloscope samples are encoded here.
class Mp4VideoRecorder {
  public:
    Mp4VideoRecorder() = default;
    ~Mp4VideoRecorder();
    Mp4VideoRecorder( const Mp4VideoRecorder & ) = delete;
    Mp4VideoRecorder &operator=( const Mp4VideoRecorder & ) = delete;

    static bool isSupported();
    bool start( const QString &path, const QSize &frameSize, int framesPerSecond, QString *errorMessage = nullptr );
    bool appendFrame( const QImage &frame, QString *errorMessage = nullptr );
    bool stop( QString *errorMessage = nullptr );
    bool isRecording() const { return recording; }
    QString filePath() const { return outputPath; }

  private:
    bool recording = false;
    quint64 frameIndex = 0;
    int frameRate = 10;
    QSize outputSize;
    QString outputPath;
#if defined(Q_OS_WIN) && defined(_MSC_VER)
    struct NativeState;
    NativeState *native = nullptr;
#endif
};
