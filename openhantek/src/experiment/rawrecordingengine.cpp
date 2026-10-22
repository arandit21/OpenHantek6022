// SPDX-License-Identifier: GPL-2.0-or-later

#include "rawrecordingengine.h"

#include "post/ppresult.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <system_error>
#include <utility>

RawRecordingEngine::RawRecordingEngine( QObject *parent ) : QObject( parent ) {}

RawRecordingEngine::~RawRecordingEngine() {
    stop();
    if ( writerThread.joinable() )
        writerThread.join();
}

bool RawRecordingEngine::start( const QString &filePath, QString *errorMessage ) {
    if ( filePath.isEmpty() ) {
        if ( errorMessage )
            *errorMessage = tr( "The recording file path is empty." );
        return false;
    }
    if ( recording.load() ) {
        if ( errorMessage )
            *errorMessage = tr( "A measurement recording is already active." );
        return false;
    }
    if ( writerThread.joinable() && !workerFinished.load() ) {
        if ( errorMessage )
            *errorMessage = tr( "The previous recording is still being finalized." );
        return false;
    }
    if ( writerThread.joinable() )
        writerThread.join();

    {
        std::lock_guard< std::mutex > lock( queueMutex );
        queue.clear();
        pendingDroppedBlocks = 0;
        droppedBlockCount = 0;
        hasLastSourceTag = false;
        stopRequested = false;
        outputPath = QFileInfo( filePath ).absoluteFilePath();
        recording.store( true );
        workerFinished.store( false );
    }
    try {
        writerThread = std::thread( &RawRecordingEngine::writeQueuedFrames, this );
    } catch ( const std::system_error &error ) {
        recording.store( false );
        workerFinished.store( true );
        if ( errorMessage )
            *errorMessage = tr( "Could not start the recording writer: %1" ).arg( QString::fromLocal8Bit( error.what() ) );
        return false;
    }
    return true;
}

void RawRecordingEngine::stop() {
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        stopRequested = true;
        recording.store( false );
    }
    queueChanged.notify_all();
}

void RawRecordingEngine::process( PPresult *data ) {
    if ( !data || !recording.load() )
        return;

    Frame frame;
    frame.timestampUtcMs = quint64( QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() );
    frame.sourceTag = data->tag;
    for ( ChannelID channel = 0; channel < 2 && channel < data->channelCount(); ++channel ) {
        const DataChannel *const source = data->data( channel );
        if ( !source )
            continue;
        if ( frame.intervalSeconds <= 0.0 )
            frame.intervalSeconds = source->voltage.interval;
        frame.channels[ channel ] = source->voltage.samples;
    }
    if ( frame.channels[ 0 ].empty() && frame.channels[ 1 ].empty() )
        return;

    quint64 dropped = 0;
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        if ( stopRequested || !recording.load() )
            return;
        if ( hasLastSourceTag && frame.sourceTag == lastSourceTag )
            return; // the UI can redraw a previously processed frame
        lastSourceTag = frame.sourceTag;
        hasLastSourceTag = true;
        if ( queue.size() >= MaxQueuedFrames ) {
            ++pendingDroppedBlocks;
            dropped = ++droppedBlockCount;
        } else {
            frame.droppedBefore = pendingDroppedBlocks;
            pendingDroppedBlocks = 0;
            queue.push_back( std::move( frame ) );
        }
    }
    if ( dropped )
        emit droppedBlocksChanged( dropped );
    else
        queueChanged.notify_one();
}

void RawRecordingEngine::writeQueuedFrames() {
    struct FinishedFlag {
        std::atomic_bool &value;
        ~FinishedFlag() { value.store( true ); }
    } finished{ workerFinished };

    const QString finalPath = outputPath;
    const QString partialPath = finalPath + ".partial";
    QFile output( partialPath );
    if ( QFileInfo::exists( finalPath ) || !output.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        recording.store( false );
        emit recordingFailed( tr( "Could not create the measurement file: %1" ).arg( partialPath ) );
        return;
    }

    const char magic[ 8 ] = { 'O', 'H', 'R', 'A', 'W', '0', '1', '\n' };
    QJsonObject headerObject;
    headerObject.insert( "schema", QStringLiteral( "org.openhantek.raw-capture/1" ) );
    headerObject.insert( "encoding", QStringLiteral( "IEEE-754 float64 little-endian" ) );
    QJsonArray channelNames;
    channelNames.append( QStringLiteral( "CH1" ) );
    channelNames.append( QStringLiteral( "CH2" ) );
    headerObject.insert( "channels", channelNames );
    headerObject.insert( "unit", QStringLiteral( "V" ) );
    headerObject.insert( "sampleKind", QStringLiteral( "calibrated voltage samples from the device pipeline" ) );
    headerObject.insert( "sourceTag", QStringLiteral( "post-processing acquisition tag; duplicate redraws are omitted" ) );
    headerObject.insert( "timestampSource",
                         QStringLiteral( "software UTC wall clock in milliseconds; not a hardware timestamp" ) );
    headerObject.insert( "application", QCoreApplication::applicationName() );
    headerObject.insert( "applicationVersion", QCoreApplication::applicationVersion() );
    QByteArray header = QJsonDocument( headerObject ).toJson( QJsonDocument::Compact );

    QDataStream stream( &output );
    stream.setByteOrder( QDataStream::LittleEndian );
    stream.setVersion( QDataStream::Qt_6_0 );
    if ( output.write( magic, sizeof( magic ) ) != qint64( sizeof( magic ) ) ) {
        output.close();
        recording.store( false );
        emit recordingFailed( tr( "Could not write the measurement file header." ) );
        return;
    }
    stream << quint32( header.size() );
    if ( stream.writeRawData( header.constData(), header.size() ) != header.size() || stream.status() != QDataStream::Ok ) {
        output.close();
        recording.store( false );
        emit recordingFailed( tr( "Could not write the measurement file header." ) );
        return;
    }
    emit recordingStarted( finalPath );

    auto writeFrame = [ &stream ]( const Frame &frame ) {
        QByteArray payload;
        QDataStream record( &payload, QIODevice::WriteOnly );
        record.setByteOrder( QDataStream::LittleEndian );
        record.setVersion( QDataStream::Qt_6_0 );
        record << quint8( 1 ) << frame.sourceTag << frame.timestampUtcMs << frame.intervalSeconds
               << quint32( frame.channels[ 0 ].size() )
               << quint32( frame.channels[ 1 ].size() ) << frame.droppedBefore;
        for ( const double value : frame.channels[ 0 ] )
            record << value;
        for ( const double value : frame.channels[ 1 ] )
            record << value;
        if ( record.status() != QDataStream::Ok )
            return false;
        stream << quint32( payload.size() );
        return stream.writeRawData( payload.constData(), payload.size() ) == payload.size() && stream.status() == QDataStream::Ok;
    };
    auto writeTrailingGap = [ &stream ]( quint32 droppedBlocks ) {
        if ( droppedBlocks == 0 )
            return true;
        QByteArray payload;
        QDataStream record( &payload, QIODevice::WriteOnly );
        record.setByteOrder( QDataStream::LittleEndian );
        record.setVersion( QDataStream::Qt_6_0 );
        record << quint8( 2 ) << droppedBlocks << quint64( QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() );
        stream << quint32( payload.size() );
        return record.status() == QDataStream::Ok && stream.writeRawData( payload.constData(), payload.size() ) == payload.size() &&
               stream.status() == QDataStream::Ok;
    };

    quint64 writtenFrames = 0;
    bool writeSucceeded = true;
    for ( ;; ) {
        Frame frame;
        {
            std::unique_lock< std::mutex > lock( queueMutex );
            queueChanged.wait( lock, [ this ] { return stopRequested || !queue.empty(); } );
            if ( queue.empty() && stopRequested )
                break;
            frame = std::move( queue.front() );
            queue.pop_front();
        }
        if ( !writeFrame( frame ) ) {
            writeSucceeded = false;
            break;
        }
        ++writtenFrames;
    }

    quint32 trailingDropped = 0;
    quint64 totalDropped = 0;
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        trailingDropped = pendingDroppedBlocks;
        totalDropped = droppedBlockCount;
    }
    if ( writeSucceeded )
        writeSucceeded = writeTrailingGap( trailingDropped );
    if ( writeSucceeded )
        writeSucceeded = output.flush();
    output.close();
    recording.store( false );

    if ( writeSucceeded && writtenFrames == 0 ) {
        QFile::remove( partialPath );
        emit recordingFailed( tr( "No measurement samples arrived; an empty recording was discarded." ) );
        return;
    }

    if ( !writeSucceeded ) {
        emit recordingFailed( tr( "Writing stopped after an I/O error. The recoverable partial file is %1" ).arg( partialPath ) );
        return;
    }
    if ( QFileInfo::exists( finalPath ) || !QFile::rename( partialPath, finalPath ) ) {
        emit recordingFailed( tr( "The recording is complete but could not be renamed to %1" ).arg( finalPath ) );
        return;
    }
    emit recordingFinished( finalPath, writtenFrames, totalDropped );
}
