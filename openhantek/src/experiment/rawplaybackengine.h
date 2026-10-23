// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QFile>
#include <QTimer>

#include <memory>
#include <vector>

class PPresult;

/// Reads the bounded OHRAW v1 frame format and replays captured voltage blocks.
class RawPlaybackEngine : public QObject {
    Q_OBJECT

  public:
    explicit RawPlaybackEngine( QObject *parent = nullptr );

    bool open( const QString &filePath, QString *errorMessage = nullptr );
    void close();
    void play();
    void pause();
    void seek( int frameIndex );
    void frameProcessed( quint64 sequence );
    bool isOpen() const { return file.isOpen(); }
    bool isPlaying() const { return playing; }
    int frameCount() const { return int( frames.size() ); }
    int currentFrame() const { return currentFrameIndex; }
    QString fileName() const { return file.fileName(); }

  signals:
    void frameReady( std::shared_ptr< PPresult > frame, quint64 sequence );
    void positionChanged( int frameIndex, int frameCount );
    void playbackChanged( bool playing );
    void playbackFinished();
    void dataGapSkipped( quint32 droppedBlocks );

  private slots:
    void advance();

  private:
    struct FrameIndex {
        qint64 payloadOffset = 0;
        quint32 payloadSize = 0;
        quint64 timestampUtcMs = 0;
        quint32 droppedBefore = 0;
    };

    QFile file;
    QTimer timer;
    std::vector< FrameIndex > frames;
    int currentFrameIndex = -1;
    bool playing = false;
    bool waitingForFrame = false;
    int pendingSeek = -1;
    quint64 nextSequence = 1;
    quint64 inFlightSequence = 0;
    bool emitFrame( int frameIndex, QString *errorMessage = nullptr );
};
