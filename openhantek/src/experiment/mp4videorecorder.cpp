// SPDX-License-Identifier: GPL-2.0-or-later

#include "mp4videorecorder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <system_error>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <objbase.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

static QString mediaFoundationError( HRESULT result, const QString &operation ) {
    return QStringLiteral( "%1 (Media Foundation: 0x%2)" )
        .arg( operation )
        .arg( quint32( result ), 8, 16, QLatin1Char( '0' ) );
}
#endif

static QString videoRecorderText( const char *text ) {
    return QCoreApplication::translate( "Mp4VideoRecorder", text );
}

Mp4VideoRecorder::~Mp4VideoRecorder() {
    stop();
}

bool Mp4VideoRecorder::isSupported() {
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

bool Mp4VideoRecorder::start( const QString &path, const QSize &frameSize, int framesPerSecond, QString *errorMessage ) {
    if ( worker.joinable() ) {
        QString previousError;
        if ( !stop( &previousError ) && !previousError.isEmpty() ) {
            if ( errorMessage )
                *errorMessage = previousError;
            return false;
        }
    }
    if ( recording.load() ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "Video recording is already active." );
        return false;
    }
#if !defined(Q_OS_WIN)
    Q_UNUSED( path )
    Q_UNUSED( frameSize )
    Q_UNUSED( framesPerSecond )
    if ( errorMessage )
        *errorMessage = videoRecorderText( "MP4 recording is currently available only on Windows." );
    return false;
#else
    const QSize evenSize( frameSize.width() & ~1, frameSize.height() & ~1 );
    if ( path.isEmpty() || evenSize.width() < 16 || evenSize.height() < 16 || framesPerSecond < 1 || framesPerSecond > 60 ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "The video path or capture area is invalid." );
        return false;
    }
    if ( QFileInfo::exists( path ) ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "The selected video file already exists; refusing to overwrite it." );
        return false;
    }
    if ( !QDir().mkpath( QFileInfo( path ).absolutePath() ) ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "Could not create the video output directory." );
        return false;
    }

    {
        std::lock_guard< std::mutex > lock( queueMutex );
        frameQueue.clear();
        stopRequested = false;
        startupCompleted = false;
        workerError.clear();
        droppedFrames.store( 0 );
        outputPath = QFileInfo( path ).absoluteFilePath();
        recordingStarted = std::chrono::steady_clock::now();
    }
    try {
        worker = std::thread( &Mp4VideoRecorder::encodeFrames, this, outputPath, evenSize, framesPerSecond );
    } catch ( const std::system_error &error ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "Could not start the video encoder thread: %1" )
                                .arg( QString::fromLocal8Bit( error.what() ) );
        return false;
    }

    std::unique_lock< std::mutex > lock( queueMutex );
    startupChanged.wait( lock, [ this ] { return startupCompleted; } );
    const QString startError = workerError;
    const bool started = recording.load();
    lock.unlock();
    if ( !started ) {
        if ( errorMessage )
            *errorMessage = startError.isEmpty() ? videoRecorderText( "Could not start the MP4 encoder." ) : startError;
        if ( worker.joinable() )
            worker.join();
        return false;
    }
    return true;
#endif
}

bool Mp4VideoRecorder::appendFrame( const QImage &frame, QString *errorMessage ) {
    if ( frame.isNull() ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "The video frame is empty." );
        return false;
    }
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        if ( !recording.load() || stopRequested ) {
            if ( errorMessage )
                *errorMessage = workerError.isEmpty() ? videoRecorderText( "There is no active video recording." ) : workerError;
            return false;
        }
        if ( frameQueue.size() >= MaxQueuedFrames ) {
            // A lagging encoder may drop a frame, but the capture event loop never
            // blocks and memory use cannot grow without bound.
            droppedFrames.fetch_add( 1 );
            return true;
        }
        const auto elapsed = std::chrono::steady_clock::now() - recordingStarted;
        const qint64 timestamp100ns = std::chrono::duration_cast< std::chrono::nanoseconds >( elapsed ).count() / 100;
        frameQueue.push_back( QueuedFrame{ frame, timestamp100ns } );
    }
    queueChanged.notify_one();
    return true;
}

bool Mp4VideoRecorder::stop( QString *errorMessage ) {
    if ( !worker.joinable() ) {
        recording.store( false );
        return true;
    }
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        stopRequested = true;
        recording.store( false );
    }
    queueChanged.notify_one();
    worker.join();
    std::lock_guard< std::mutex > lock( queueMutex );
    if ( !workerError.isEmpty() ) {
        if ( errorMessage )
            *errorMessage = workerError;
        return false;
    }
    return true;
}

void Mp4VideoRecorder::encodeFrames( const QString &path, const QSize &frameSize, int framesPerSecond ) {
#if !defined(Q_OS_WIN)
    Q_UNUSED( path )
    Q_UNUSED( frameSize )
    Q_UNUSED( framesPerSecond )
#else
    const auto setStartupFailure = [ this ]( const QString &message ) {
        {
            std::lock_guard< std::mutex > lock( queueMutex );
            workerError = message;
            startupCompleted = true;
            recording.store( false );
        }
        startupChanged.notify_one();
    };

    HRESULT result = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    if ( FAILED( result ) ) {
        setStartupFailure( mediaFoundationError( result, videoRecorderText( "Could not initialize COM for video encoding" ) ) );
        return;
    }
    result = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    if ( FAILED( result ) ) {
        setStartupFailure( mediaFoundationError( result, videoRecorderText( "Could not initialize the MP4 encoder" ) ) );
        CoUninitialize();
        return;
    }

    ComPtr< IMFSinkWriter > writer;
    DWORD streamIndex = 0;
    const std::wstring nativeOutputPath = QDir::toNativeSeparators( path ).toStdWString();
    result = MFCreateSinkWriterFromURL( nativeOutputPath.c_str(), nullptr, nullptr, &writer );
    if ( SUCCEEDED( result ) ) {
        ComPtr< IMFMediaType > outputType;
        result = MFCreateMediaType( &outputType );
        if ( SUCCEEDED( result ) ) result = outputType->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
        if ( SUCCEEDED( result ) ) result = outputType->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_H264 );
        if ( SUCCEEDED( result ) ) result = outputType->SetUINT32( MF_MT_AVG_BITRATE, 4'000'000 );
        if ( SUCCEEDED( result ) ) result = outputType->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeSize( outputType.Get(), MF_MT_FRAME_SIZE, UINT32( frameSize.width() ),
                                         UINT32( frameSize.height() ) );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( outputType.Get(), MF_MT_FRAME_RATE, UINT32( framesPerSecond ), 1 );
        if ( SUCCEEDED( result ) ) result = MFSetAttributeRatio( outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
        if ( SUCCEEDED( result ) ) result = outputType->SetUINT32( MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main );
        if ( SUCCEEDED( result ) ) result = writer->AddStream( outputType.Get(), &streamIndex );
    }
    if ( SUCCEEDED( result ) ) {
        ComPtr< IMFMediaType > inputType;
        result = MFCreateMediaType( &inputType );
        if ( SUCCEEDED( result ) ) result = inputType->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
        if ( SUCCEEDED( result ) ) result = inputType->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
        if ( SUCCEEDED( result ) ) result = inputType->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeSize( inputType.Get(), MF_MT_FRAME_SIZE, UINT32( frameSize.width() ),
                                         UINT32( frameSize.height() ) );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( inputType.Get(), MF_MT_FRAME_RATE, UINT32( framesPerSecond ), 1 );
        if ( SUCCEEDED( result ) ) result = MFSetAttributeRatio( inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
        if ( SUCCEEDED( result ) ) result = inputType->SetUINT32( MF_MT_DEFAULT_STRIDE, UINT32( frameSize.width() * 4 ) );
        if ( SUCCEEDED( result ) ) result = writer->SetInputMediaType( streamIndex, inputType.Get(), nullptr );
    }
    if ( SUCCEEDED( result ) )
        result = writer->BeginWriting();
    if ( FAILED( result ) ) {
        setStartupFailure( mediaFoundationError( result, videoRecorderText( "Could not start MP4 recording" ) ) );
        writer.Reset();
        MFShutdown();
        CoUninitialize();
        QFile::remove( path );
        return;
    }

    {
        std::lock_guard< std::mutex > lock( queueMutex );
        startupCompleted = true;
        recording.store( true );
    }
    startupChanged.notify_one();

    while ( true ) {
        QueuedFrame queuedFrame;
        {
            std::unique_lock< std::mutex > lock( queueMutex );
            queueChanged.wait( lock, [ this ] { return stopRequested || !frameQueue.empty(); } );
            if ( frameQueue.empty() && stopRequested )
                break;
            queuedFrame = std::move( frameQueue.front() );
            frameQueue.pop_front();
        }

        const QImage rgb = queuedFrame.image.scaled( frameSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation )
                               .convertToFormat( QImage::Format_RGB32 );
        const DWORD frameBytes = DWORD( frameSize.width() * frameSize.height() * 4 );
        ComPtr< IMFMediaBuffer > buffer;
        result = MFCreateMemoryBuffer( frameBytes, &buffer );
        if ( SUCCEEDED( result ) ) {
            BYTE *destination = nullptr;
            result = buffer->Lock( &destination, nullptr, nullptr );
            if ( SUCCEEDED( result ) ) {
                for ( int y = 0; y < frameSize.height(); ++y )
                    std::memcpy( destination + y * frameSize.width() * 4, rgb.constScanLine( y ),
                                 size_t( frameSize.width() * 4 ) );
                const HRESULT unlockResult = buffer->Unlock();
                if ( SUCCEEDED( unlockResult ) )
                    result = buffer->SetCurrentLength( frameBytes );
                else
                    result = unlockResult;
            }
        }
        if ( SUCCEEDED( result ) ) {
            ComPtr< IMFSample > sample;
            result = MFCreateSample( &sample );
            if ( SUCCEEDED( result ) ) result = sample->AddBuffer( buffer.Get() );
            const LONGLONG duration = 10'000'000 / framesPerSecond;
            if ( SUCCEEDED( result ) ) result = sample->SetSampleTime( LONGLONG( queuedFrame.timestamp100ns ) );
            if ( SUCCEEDED( result ) ) result = sample->SetSampleDuration( duration );
            if ( SUCCEEDED( result ) ) result = writer->WriteSample( streamIndex, sample.Get() );
        }
        if ( FAILED( result ) ) {
            std::lock_guard< std::mutex > lock( queueMutex );
            workerError = mediaFoundationError( result, videoRecorderText( "Could not encode an MP4 frame" ) );
            stopRequested = true;
            frameQueue.clear();
            recording.store( false );
            break;
        }
    }

    const HRESULT finalizeResult = writer->Finalize();
    writer.Reset();
    if ( FAILED( finalizeResult ) ) {
        std::lock_guard< std::mutex > lock( queueMutex );
        if ( workerError.isEmpty() )
            workerError = mediaFoundationError( finalizeResult, videoRecorderText( "Could not finalize the MP4 file" ) );
    }
    MFShutdown();
    CoUninitialize();
    recording.store( false );
    bool failed = false;
    {
        std::lock_guard< std::mutex > lock( queueMutex );
        failed = !workerError.isEmpty();
        if ( failed )
            frameQueue.clear();
    }
    if ( failed )
        QFile::remove( path );
#endif
}
