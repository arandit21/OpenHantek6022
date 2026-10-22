// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "post/processor.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class RawRecordingEngine : public QObject, public Processor {
    Q_OBJECT

  public:
    explicit RawRecordingEngine( QObject *parent = nullptr );
    ~RawRecordingEngine() override;

    bool start( const QString &filePath, QString *errorMessage = nullptr );
    void stop();
    bool isRecording() const { return recording.load(); }
    bool isBusy() const { return !workerFinished.load(); }
    void process( PPresult *data ) override;

  signals:
    void recordingStarted( const QString &filePath );
    void recordingFinished( const QString &filePath, quint64 frameCount, quint64 droppedBlocks );
    void recordingFailed( const QString &message );
    void droppedBlocksChanged( quint64 droppedBlocks );

  private:
    struct Frame {
        quint64 timestampUtcMs = 0;
        quint32 sourceTag = 0;
        double intervalSeconds = 0.0;
        quint32 droppedBefore = 0;
        std::vector< double > channels[ 2 ];
    };

    static constexpr std::size_t MaxQueuedFrames = 12;
    std::atomic_bool recording{ false };
    std::atomic_bool workerFinished{ true };
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque< Frame > queue;
    std::thread writerThread;
    bool stopRequested = false;
    quint32 pendingDroppedBlocks = 0;
    quint64 droppedBlockCount = 0;
    quint32 lastSourceTag = 0;
    bool hasLastSourceTag = false;
    QString outputPath;

    void writeQueuedFrames();
};
