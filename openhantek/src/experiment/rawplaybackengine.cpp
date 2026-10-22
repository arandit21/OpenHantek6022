// SPDX-License-Identifier: GPL-2.0-or-later

#include "rawplaybackengine.h"

#include "post/ppresult.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <QDebug>

namespace {
constexpr char RawMagic[ 8 ] = { 'O', 'H', 'R', 'A', 'W', '0', '1', '\n' };
constexpr quint32 MaxHeaderBytes = 1024 * 1024;
constexpr quint32 MaxSamplesPerChannel = 10'000'000;
constexpr quint32 MaxRecordBytes = 160'000'128;
constexpr quint32 MaxIndexedFrames = 2'000'000;

void calculateBasicMeasurements( DataChannel *channel ) {
    const auto &samples = channel->voltage.samples;
    if ( samples.empty() )
        return;
    double sum = 0.0;
    double sumSquares = 0.0;
    channel->vmin = std::numeric_limits< double >::max();
    channel->vmax = std::numeric_limits< double >::lowest();
    for ( const double sample : samples ) {
        if ( !std::isfinite( sample ) )
            continue;
        sum += sample;
        sumSquares += sample * sample;
        channel->vmin = std::min( channel->vmin, sample );
        channel->vmax = std::max( channel->vmax, sample );
    }
    channel->dc = sum / double( samples.size() );
    channel->rms = std::sqrt( sumSquares / double( samples.size() ) );
    double acSquares = 0.0;
    for ( const double sample : samples ) {
        const double ac = sample - channel->dc;
        acSquares += ac * ac;
    }
    channel->ac = std::sqrt( acSquares / double( samples.size() ) );
}
} // namespace

RawPlaybackEngine::RawPlaybackEngine( QObject *parent ) : QObject( parent ) {
    timer.setSingleShot( true );
    connect( &timer, &QTimer::timeout, this, &RawPlaybackEngine::advance );
}

bool RawPlaybackEngine::open( const QString &filePath, QString *errorMessage ) {
    close();
    file.setFileName( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        if ( errorMessage )
            *errorMessage = tr( "Could not open measurement recording: %1" ).arg( file.errorString() );
        return false;
    }

    char magic[ sizeof( RawMagic ) ]{};
    if ( file.read( magic, sizeof( magic ) ) != qint64( sizeof( magic ) ) || std::memcmp( magic, RawMagic, sizeof( magic ) ) != 0 ) {
        close();
        if ( errorMessage )
            *errorMessage = tr( "This is not a supported OpenHantek raw measurement file." );
        return false;
    }

    QDataStream stream( &file );
    stream.setByteOrder( QDataStream::LittleEndian );
    stream.setVersion( QDataStream::Qt_6_0 );
    quint32 headerSize = 0;
    stream >> headerSize;
    if ( stream.status() != QDataStream::Ok || headerSize == 0 || headerSize > MaxHeaderBytes ||
         quint64( headerSize ) > quint64( file.bytesAvailable() ) ) {
        close();
        if ( errorMessage )
            *errorMessage = tr( "The measurement recording header is invalid or truncated." );
        return false;
    }
    const QByteArray header = file.read( headerSize );
    QJsonParseError parseError;
    const QJsonDocument headerDocument = QJsonDocument::fromJson( header, &parseError );
    if ( header.size() != int( headerSize ) || parseError.error != QJsonParseError::NoError || !headerDocument.isObject() ||
         headerDocument.object().value( "schema" ).toString() != QStringLiteral( "org.openhantek.raw-capture/1" ) ) {
        close();
        if ( errorMessage )
            *errorMessage = tr( "The measurement recording header is not a valid OHRAW v1 header." );
        return false;
    }

    quint32 accumulatedGap = 0;
    while ( !file.atEnd() ) {
        quint32 payloadSize = 0;
        stream >> payloadSize;
        if ( stream.status() != QDataStream::Ok || payloadSize == 0 || payloadSize > MaxRecordBytes ||
             quint64( payloadSize ) > quint64( file.bytesAvailable() ) ) {
            close();
            if ( errorMessage )
                *errorMessage = tr( "The recording ends with a truncated or oversized data block." );
            return false;
        }
        const qint64 payloadOffset = file.pos();
        char typeByte = 0;
        if ( file.read( &typeByte, 1 ) != 1 ) {
            close();
            if ( errorMessage )
                *errorMessage = tr( "Could not read a complete measurement block." );
            return false;
        }
        const quint8 type = quint8( typeByte );
        if ( type == 1 ) {
            constexpr quint32 frameHeaderBytes = 33;
            if ( payloadSize < frameHeaderBytes || !file.seek( payloadOffset ) ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "A measurement block has an incomplete header." );
                return false;
            }
            const QByteArray frameHeader = file.read( frameHeaderBytes );
            QDataStream record( frameHeader );
            record.setByteOrder( QDataStream::LittleEndian );
            record.setVersion( QDataStream::Qt_6_0 );
            quint32 tag = 0;
            quint64 timestamp = 0;
            double interval = 0.0;
            quint32 ch1Count = 0;
            quint32 ch2Count = 0;
            quint32 droppedBefore = 0;
            record >> tag >> timestamp >> interval >> ch1Count >> ch2Count >> droppedBefore;
            Q_UNUSED( tag )
            const quint64 expectedSize = 1u + 4u + 8u + 8u + 4u + 4u + 4u + 8ull * ( quint64( ch1Count ) + ch2Count );
            if ( record.status() != QDataStream::Ok || !std::isfinite( interval ) || interval <= 0.0 ||
                 ch1Count > MaxSamplesPerChannel || ch2Count > MaxSamplesPerChannel ||
                 ( ch1Count == 0 && ch2Count == 0 ) || expectedSize != payloadSize ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "A measurement block has invalid channel sizes or sampling interval." );
                return false;
            }
            if ( frames.size() >= MaxIndexedFrames ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "This recording contains too many blocks to index safely." );
                return false;
            }
            const quint32 totalGap = quint32( std::min< quint64 >( quint64( droppedBefore ) + accumulatedGap,
                                                                    std::numeric_limits< quint32 >::max() ) );
            frames.push_back( { payloadOffset, payloadSize, timestamp, totalGap } );
            accumulatedGap = 0;
            if ( !file.seek( payloadOffset + payloadSize ) ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "Could not seek to the next measurement block." );
                return false;
            }
        } else if ( type == 2 ) {
            if ( payloadSize != 13 ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "A recording gap marker is invalid." );
                return false;
            }
            const QByteArray gapPayload = file.read( payloadSize - 1 );
            QDataStream record( gapPayload );
            record.setByteOrder( QDataStream::LittleEndian );
            record.setVersion( QDataStream::Qt_6_0 );
            quint32 dropped = 0;
            quint64 timestamp = 0;
            record >> dropped >> timestamp;
            Q_UNUSED( timestamp )
            if ( record.status() != QDataStream::Ok || gapPayload.size() != 12 ) {
                close();
                if ( errorMessage )
                    *errorMessage = tr( "A recording gap marker is invalid." );
                return false;
            }
            accumulatedGap = quint32( std::min< quint64 >( quint64( accumulatedGap ) + dropped,
                                                            std::numeric_limits< quint32 >::max() ) );
        } else {
            close();
            if ( errorMessage )
                *errorMessage = tr( "The recording contains an unknown data block." );
            return false;
        }
    }

    if ( frames.empty() ) {
        close();
        if ( errorMessage )
            *errorMessage = tr( "The recording contains no measurement frames." );
        return false;
    }
    currentFrameIndex = -1;
    return true;
}

void RawPlaybackEngine::close() {
    pause();
    waitingForFrame = false;
    pendingSeek = -1;
    inFlightSequence = 0;
    file.close();
    frames.clear();
    currentFrameIndex = -1;
}

void RawPlaybackEngine::play() {
    if ( !file.isOpen() || frames.empty() || playing )
        return;
    if ( currentFrameIndex < 0 || currentFrameIndex >= int( frames.size() ) - 1 ) {
        if ( waitingForFrame )
            pendingSeek = 0;
        else
            seek( 0 );
    }
    playing = true;
    emit playbackChanged( true );
    if ( !waitingForFrame )
        timer.start( 1 );
}

void RawPlaybackEngine::pause() {
    if ( !playing )
        return;
    playing = false;
    timer.stop();
    emit playbackChanged( false );
}

void RawPlaybackEngine::seek( int frameIndex ) {
    if ( !file.isOpen() || frames.empty() )
        return;
    frameIndex = std::clamp( frameIndex, 0, int( frames.size() ) - 1 );
    timer.stop();
    if ( waitingForFrame ) {
        pendingSeek = frameIndex;
        return;
    }
    QString error;
    if ( !emitFrame( frameIndex, &error ) ) {
        pause();
        emit playbackChanged( false );
        qWarning() << "Could not read playback frame:" << error;
    }
}

void RawPlaybackEngine::frameProcessed( quint64 sequence ) {
    if ( !waitingForFrame || sequence != inFlightSequence )
        return;
    waitingForFrame = false;
    inFlightSequence = 0;
    if ( pendingSeek >= 0 ) {
        const int requestedFrame = pendingSeek;
        pendingSeek = -1;
        QString error;
        if ( !emitFrame( requestedFrame, &error ) ) {
            pause();
            qWarning() << "Could not seek playback frame:" << error;
        }
        return;
    }
    if ( playing ) {
        const int nextIndex = currentFrameIndex + 1;
        if ( nextIndex >= int( frames.size() ) ) {
            timer.start( 1 );
            return;
        }
        int delay = 33;
        const quint64 now = frames[ size_t( currentFrameIndex ) ].timestampUtcMs;
        const quint64 next = frames[ size_t( nextIndex ) ].timestampUtcMs;
        if ( next > now )
            delay = int( std::clamp< quint64 >( next - now, 10, 250 ) );
        timer.start( delay );
    }
}

void RawPlaybackEngine::advance() {
    if ( !playing || waitingForFrame )
        return;
    const int nextIndex = currentFrameIndex + 1;
    if ( nextIndex >= int( frames.size() ) ) {
        pause();
        emit playbackFinished();
        return;
    }
    QString error;
    if ( !emitFrame( nextIndex, &error ) ) {
        pause();
        emit playbackChanged( false );
        qWarning() << "Could not read playback frame:" << error;
        return;
    }

}

bool RawPlaybackEngine::emitFrame( int frameIndex, QString *errorMessage ) {
    const FrameIndex index = frames[ size_t( frameIndex ) ];
    if ( !file.seek( index.payloadOffset ) ) {
        if ( errorMessage )
            *errorMessage = file.errorString();
        return false;
    }
    const QByteArray payload = file.read( index.payloadSize );
    if ( payload.size() != int( index.payloadSize ) ) {
        if ( errorMessage )
            *errorMessage = tr( "The measurement frame is truncated." );
        return false;
    }

    QDataStream record( payload );
    record.setByteOrder( QDataStream::LittleEndian );
    record.setVersion( QDataStream::Qt_6_0 );
    quint8 type = 0;
    quint32 tag = 0;
    quint64 timestamp = 0;
    double interval = 0.0;
    quint32 counts[ 2 ]{};
    quint32 dropped = 0;
    record >> type >> tag >> timestamp >> interval >> counts[ 0 ] >> counts[ 1 ] >> dropped;
    Q_UNUSED( timestamp )
    Q_UNUSED( dropped )
    const quint64 expectedSize = 1u + 4u + 8u + 8u + 4u + 4u + 4u + 8ull * ( quint64( counts[ 0 ] ) + counts[ 1 ] );
    if ( type != 1 || record.status() != QDataStream::Ok || !std::isfinite( interval ) || interval <= 0.0 ||
         counts[ 0 ] > MaxSamplesPerChannel || counts[ 1 ] > MaxSamplesPerChannel ||
         ( counts[ 0 ] == 0 && counts[ 1 ] == 0 ) || expectedSize != index.payloadSize ) {
        if ( errorMessage )
            *errorMessage = tr( "The measurement frame header is invalid." );
        return false;
    }

    auto result = std::make_shared< PPresult >( 3 );
    result->tag = tag;
    for ( ChannelID channel = 0; channel < 2; ++channel ) {
        DataChannel *output = result->modifiableData( channel );
        output->voltage.interval = interval;
        output->voltage.samples.resize( counts[ channel ] );
        for ( double &sample : output->voltage.samples )
            record >> sample;
        if ( record.status() != QDataStream::Ok ) {
            if ( errorMessage )
                *errorMessage = tr( "Could not decode measurement samples." );
            return false;
        }
        if ( std::any_of( output->voltage.samples.cbegin(), output->voltage.samples.cend(),
                          []( double value ) { return !std::isfinite( value ); } ) ) {
            if ( errorMessage )
                *errorMessage = tr( "The measurement frame contains a non-finite sample." );
            return false;
        }
        calculateBasicMeasurements( output );
    }
    currentFrameIndex = frameIndex;
    if ( index.droppedBefore )
        emit dataGapSkipped( index.droppedBefore );
    waitingForFrame = true;
    inFlightSequence = nextSequence++;
    emit frameReady( std::move( result ), inFlightSequence );
    emit positionChanged( currentFrameIndex, int( frames.size() ) );
    return true;
}
