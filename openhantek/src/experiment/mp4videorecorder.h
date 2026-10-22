// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QImage>
#include <QString>
#include <QSize>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

/// Bounded asynchronous MP4 recorder backed by Windows Media Foundation.
/// Frames are the rendered scope window; encoding is kept off the GUI thread.
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
    bool isRecording() const { return recording.load(); }
    bool isBusy() const { return worker.joinable(); }
    quint64 droppedFrameCount() const { return droppedFrames.load(); }
    QString filePath() const { return outputPath; }

  private:
    struct QueuedFrame {
        QImage image;
        qint64 timestamp100ns = 0;
    };
    static constexpr std::size_t MaxQueuedFrames = 4;
    std::atomic_bool recording{ false };
    std::atomic< quint64 > droppedFrames{ 0 };
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::condition_variable startupChanged;
    std::deque< QueuedFrame > frameQueue;
    std::chrono::steady_clock::time_point recordingStarted;
    bool stopRequested = false;
    bool startupCompleted = false;
    QString workerError;
    QString outputPath;

    void encodeFrames( const QString &path, const QSize &frameSize, int framesPerSecond );
};
