// SPDX-License-Identifier: GPL-2.0-or-later

#include "mp4videorecorder.h"

#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>

#if defined(Q_OS_WIN) && defined(_MSC_VER)
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <codecapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

struct Mp4VideoRecorder::NativeState {
    ComPtr< IMFSinkWriter > writer;
    DWORD streamIndex = 0;
    bool comInitialized = false;
};

static QString mediaFoundationError( HRESULT result, const QString &operation ) {
    return QStringLiteral( "%1 (Media Foundation: 0x%2)" )
        .arg( operation )
        .arg( quint32( result ), 8, 16, QLatin1Char( '0' ) );
}
#endif

static QString videoRecorderText( const char *text ) {
    return QCoreApplication::translate( "Mp4VideoRecorder", text );
}

Mp4VideoRecorder::~Mp4VideoRecorder() { stop(); }

bool Mp4VideoRecorder::isSupported() {
#if defined(Q_OS_WIN) && defined(_MSC_VER)
    return true;
#else
    return false;
#endif
}

bool Mp4VideoRecorder::start( const QString &path, const QSize &frameSize, int framesPerSecond, QString *errorMessage ) {
    if ( recording ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "Video recording is already active." );
        return false;
    }
#if !defined(Q_OS_WIN) || !defined(_MSC_VER)
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
    if ( !QDir().mkpath( QFileInfo( path ).absolutePath() ) ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "Could not create the video output directory." );
        return false;
    }

    HRESULT result = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    const bool comWasAlreadyInitialized = result == RPC_E_CHANGED_MODE;
    if ( FAILED( result ) && !comWasAlreadyInitialized ) {
        if ( errorMessage )
            *errorMessage = mediaFoundationError( result, videoRecorderText( "Could not initialize COM for video encoding" ) );
        return false;
    }
    const bool initializedComHere = !comWasAlreadyInitialized;
    result = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    if ( FAILED( result ) ) {
        if ( initializedComHere )
            CoUninitialize();
        if ( errorMessage )
            *errorMessage = mediaFoundationError( result, videoRecorderText( "Could not initialize the MP4 encoder" ) );
        return false;
    }

    native = new NativeState;
    native->comInitialized = initializedComHere;
    const std::wstring nativeOutputPath = QDir::toNativeSeparators( path ).toStdWString();
    result = MFCreateSinkWriterFromURL( nativeOutputPath.c_str(), nullptr, nullptr, &native->writer );
    if ( FAILED( result ) )
        goto failed;

    {
        ComPtr< IMFMediaType > outputType;
        result = MFCreateMediaType( &outputType );
        if ( FAILED( result ) )
            goto failed;
        result = outputType->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
        if ( SUCCEEDED( result ) )
            result = outputType->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_H264 );
        if ( SUCCEEDED( result ) )
            result = outputType->SetUINT32( MF_MT_AVG_BITRATE, 4'000'000 );
        if ( SUCCEEDED( result ) )
            result = outputType->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeSize( outputType.Get(), MF_MT_FRAME_SIZE, UINT32( evenSize.width() ),
                                         UINT32( evenSize.height() ) );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( outputType.Get(), MF_MT_FRAME_RATE, UINT32( framesPerSecond ), 1 );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
        if ( SUCCEEDED( result ) )
            result = outputType->SetUINT32( MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main );
        if ( SUCCEEDED( result ) )
            result = native->writer->AddStream( outputType.Get(), &native->streamIndex );
        if ( FAILED( result ) )
            goto failed;
    }

    {
        ComPtr< IMFMediaType > inputType;
        result = MFCreateMediaType( &inputType );
        if ( FAILED( result ) )
            goto failed;
        result = inputType->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
        if ( SUCCEEDED( result ) )
            result = inputType->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
        if ( SUCCEEDED( result ) )
            result = inputType->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeSize( inputType.Get(), MF_MT_FRAME_SIZE, UINT32( evenSize.width() ),
                                         UINT32( evenSize.height() ) );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( inputType.Get(), MF_MT_FRAME_RATE, UINT32( framesPerSecond ), 1 );
        if ( SUCCEEDED( result ) )
            result = MFSetAttributeRatio( inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
        if ( SUCCEEDED( result ) )
            result = inputType->SetUINT32( MF_MT_DEFAULT_STRIDE, UINT32( evenSize.width() * 4 ) );
        if ( SUCCEEDED( result ) )
            result = native->writer->SetInputMediaType( native->streamIndex, inputType.Get(), nullptr );
        if ( FAILED( result ) )
            goto failed;
    }

    result = native->writer->BeginWriting();
    if ( FAILED( result ) )
        goto failed;

    outputSize = evenSize;
    outputPath = path;
    frameRate = framesPerSecond;
    frameIndex = 0;
    recording = true;
    return true;

failed:
    if ( errorMessage )
        *errorMessage = mediaFoundationError( result, videoRecorderText( "Could not start MP4 recording" ) );
    native->writer.Reset();
    delete native;
    native = nullptr;
    MFShutdown();
    if ( initializedComHere )
        CoUninitialize();
    return false;
#endif
}

bool Mp4VideoRecorder::appendFrame( const QImage &frame, QString *errorMessage ) {
    if ( !recording || frame.isNull() ) {
        if ( errorMessage )
            *errorMessage = videoRecorderText( "There is no active video recording or the frame is empty." );
        return false;
    }
#if !defined(Q_OS_WIN) || !defined(_MSC_VER)
    Q_UNUSED( errorMessage )
    return false;
#else
    const QImage rgb = frame.scaled( outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation )
                           .convertToFormat( QImage::Format_RGB32 );
    const DWORD frameBytes = DWORD( outputSize.width() * outputSize.height() * 4 );
    ComPtr< IMFMediaBuffer > buffer;
    HRESULT result = MFCreateMemoryBuffer( frameBytes, &buffer );
    if ( FAILED( result ) )
        goto failed;
    {
        BYTE *destination = nullptr;
        result = buffer->Lock( &destination, nullptr, nullptr );
        if ( FAILED( result ) )
            goto failed;
        for ( int y = 0; y < outputSize.height(); ++y )
            std::memcpy( destination + y * outputSize.width() * 4, rgb.constScanLine( y ), size_t( outputSize.width() * 4 ) );
        buffer->Unlock();
        result = buffer->SetCurrentLength( frameBytes );
        if ( FAILED( result ) )
            goto failed;
    }
    {
        ComPtr< IMFSample > sample;
        result = MFCreateSample( &sample );
        if ( SUCCEEDED( result ) )
            result = sample->AddBuffer( buffer.Get() );
        const LONGLONG frameDuration = 10'000'000 / frameRate;
        if ( SUCCEEDED( result ) )
            result = sample->SetSampleTime( LONGLONG( frameIndex ) * frameDuration );
        if ( SUCCEEDED( result ) )
            result = sample->SetSampleDuration( frameDuration );
        if ( SUCCEEDED( result ) )
            result = native->writer->WriteSample( native->streamIndex, sample.Get() );
        if ( FAILED( result ) )
            goto failed;
    }
    ++frameIndex;
    return true;

failed:
    if ( errorMessage )
        *errorMessage = mediaFoundationError( result, videoRecorderText( "Could not encode an MP4 frame" ) );
    return false;
#endif
}

bool Mp4VideoRecorder::stop( QString *errorMessage ) {
    if ( !recording )
        return true;
    recording = false;
#if !defined(Q_OS_WIN) || !defined(_MSC_VER)
    Q_UNUSED( errorMessage )
    return true;
#else
    HRESULT result = native->writer->Finalize();
    native->writer.Reset();
    const bool uninitializeCom = native->comInitialized;
    delete native;
    native = nullptr;
    MFShutdown();
    if ( uninitializeCom )
        CoUninitialize();
    if ( FAILED( result ) ) {
        if ( errorMessage )
            *errorMessage = mediaFoundationError( result, videoRecorderText( "Could not finalize the MP4 file" ) );
        return false;
    }
    return true;
#endif
}
