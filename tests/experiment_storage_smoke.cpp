// SPDX-License-Identifier: GPL-2.0-or-later

#include "experiment/experimentcatalog.h"
#include "experiment/experimentmanager.h"
#include "experiment/rawplaybackengine.h"
#include "experiment/rawrecordingengine.h"
#include "post/ppresult.h"
#ifdef OPENHANTEK_TEST_MP4
#include "experiment/mp4videorecorder.h"
#endif

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#ifdef OPENHANTEK_TEST_MP4
#include <QPainter>
#include <QPen>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <thread>
#include <QPainterPath>
using Microsoft::WRL::ComPtr;
#endif

namespace {
bool check( bool condition, const QString &message ) {
    if ( !condition )
        qCritical().noquote() << "FAIL:" << message;
    return condition;
}

bool waitForScan( ExperimentCatalog &catalog, int *files, int *experiments ) {
    QEventLoop loop;
    bool finished = false;
    bool failed = false;
    const auto finishedConnection = QObject::connect(
        &catalog, &ExperimentCatalog::scanFinished, &loop,
        [ & ]( int fileCount, int experimentCount ) {
            *files = fileCount;
            *experiments = experimentCount;
            finished = true;
            loop.quit();
        } );
    const auto failedConnection = QObject::connect(
        &catalog, &ExperimentCatalog::scanFailed, &loop,
        [ & ]( const QString &error ) {
            qCritical().noquote() << "Catalog scan failed:" << error;
            failed = true;
            loop.quit();
        } );
    QTimer::singleShot( 15000, &loop, &QEventLoop::quit );
    catalog.startRescan();
    loop.exec();
    QObject::disconnect( finishedConnection );
    QObject::disconnect( failedConnection );
    return finished && !failed;
}

bool recordAndReplayRaw( ExperimentManager &manager ) {
    QString error;
    const QString path = manager.nextRawDataPath( &error );
    if ( !check( !path.isEmpty(), QStringLiteral( "Could not select OHRAW output path: %1" ).arg( error ) ) )
        return false;
    RawRecordingEngine recorder;
    QEventLoop loop;
    bool finished = false;
    bool failed = false;
    int writtenFrames = 0;
    QObject::connect( &recorder, &RawRecordingEngine::recordingFinished, &loop,
                      [ & ]( const QString &, quint64 count, quint64 ) {
        writtenFrames = int( count );
        finished = true;
        loop.quit();
    } );
    QObject::connect( &recorder, &RawRecordingEngine::recordingFailed, &loop, [ & ]( const QString &message ) {
        qCritical().noquote() << "OHRAW recording failed:" << message;
        failed = true;
        loop.quit();
    } );
    if ( !check( recorder.start( path, &error ), QStringLiteral( "Could not start OHRAW recording: %1" ).arg( error ) ) )
        return false;
    for ( unsigned int tag = 1; tag <= 5; ++tag ) {
        PPresult frame( 2 );
        frame.tag = tag;
        for ( ChannelID channel = 0; channel < 2; ++channel ) {
            DataChannel *data = frame.modifiableData( channel );
            data->voltage.interval = 0.00001;
            for ( int sample = 0; sample < 256; ++sample )
                data->voltage.samples.push_back( double( channel + 1 ) * 0.25 + double( sample ) / 2048.0 );
        }
        recorder.process( &frame );
    }
    recorder.stop();
    QTimer::singleShot( 10000, &loop, &QEventLoop::quit );
    loop.exec();
    if ( !check( finished && !failed && writtenFrames == 5,
                 QStringLiteral( "OHRAW finalization yielded %1 blocks." ).arg( writtenFrames ) ) )
        return false;
    QFile file( path );
    if ( !check( file.open( QIODevice::ReadOnly ) && file.size() > 512,
                 QStringLiteral( "OHRAW file is empty or missing." ) ) )
        return false;
    const QByteArray magic = file.read( 8 );
    QDataStream headerStream( &file );
    headerStream.setByteOrder( QDataStream::LittleEndian );
    headerStream.setVersion( QDataStream::Qt_6_0 );
    quint32 headerSize = 0;
    headerStream >> headerSize;
    const QByteArray header = file.read( headerSize );
    const QJsonObject metadata = QJsonDocument::fromJson( header ).object();
    if ( !check( magic == QByteArray( "OHRAW01\n", 8 ) && metadata.value( "channels" ).toArray().size() == 2,
                 QStringLiteral( "OHRAW v1 metadata is missing CH1/CH2." ) ) )
        return false;
    file.close();

    RawPlaybackEngine playback;
    if ( !check( playback.open( path, &error ) && playback.frameCount() == 5,
                 QStringLiteral( "Could not reopen OHRAW for playback: %1" ).arg( error ) ) )
        return false;
    bool received = false;
    QObject::connect( &playback, &RawPlaybackEngine::frameReady, &playback,
                      [ &playback, &received ]( std::shared_ptr< PPresult > frame, quint64 sequence ) {
        received = frame && frame->channelCount() >= 2 && !frame->data( 0 )->voltage.samples.empty() &&
                   !frame->data( 1 )->voltage.samples.empty();
        playback.frameProcessed( sequence );
    } );
    playback.seek( 3 );
    if ( !check( received, QStringLiteral( "Seeking in OHRAW did not replay both recorded channels." ) ) )
        return false;
    qInfo() << "PASS: OHRAW wrote and reopened five indexed CH1/CH2 frames for seek playback. File:" << path;
    return true;
}

#ifdef OPENHANTEK_TEST_MP4
bool recordAndDecodeMp4( ExperimentManager &manager, QString *recordedPath ) {
    if ( !Mp4VideoRecorder::isSupported() )
        return check( false, QStringLiteral( "MSVC Windows build did not enable MP4." ) );
    QString pathError;
    const QString path = manager.nextVideoPath( &pathError );
    if ( !check( !path.isEmpty(), QStringLiteral( "Could not select MP4 output path: %1" ).arg( pathError ) ) )
        return false;
    Mp4VideoRecorder recorder;
    QString error;
    if ( !check( recorder.start( path, QSize( 160, 120 ), 10, &error ),
                 QStringLiteral( "Could not initialize Media Foundation: %1" ).arg( error ) ) )
        return false;
    for ( int frame = 0; frame < 15; ++frame ) {
        QImage image( 160, 120, QImage::Format_RGB32 );
        image.fill( QColor( 250, 250, 250 ) );
        QPainter painter( &image );
        painter.setPen( QPen( QColor( 25, 105, 220 ), 3 ) );
        QPainterPath trace;
        for ( int x = 0; x < image.width(); ++x ) {
            const double phase = ( x + frame * 7 ) * 0.09;
            const double y = 60.0 + 35.0 * std::sin( phase );
            if ( x == 0 ) trace.moveTo( x, y ); else trace.lineTo( x, y );
        }
        painter.drawPath( trace );
        painter.end();
        if ( !check( recorder.appendFrame( image, &error ),
                     QStringLiteral( "MP4 frame queue rejected a frame: %1" ).arg( error ) ) ) {
            recorder.stop();
            return false;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    if ( !check( recorder.stop( &error ), QStringLiteral( "MP4 finalization failed: %1" ).arg( error ) ) )
        return false;
    if ( !check( QFileInfo( path ).isFile() && QFileInfo( path ).size() > 1024,
                 QStringLiteral( "Finalized MP4 is missing or empty." ) ) )
        return false;

    HRESULT result = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    if ( FAILED( result ) )
        return check( false, QStringLiteral( "Could not initialize COM for MP4 decode smoke test." ) );
    result = MFStartup( MF_VERSION, MFSTARTUP_FULL );
    if ( FAILED( result ) ) {
        CoUninitialize();
        return check( false, QStringLiteral( "Could not initialize Media Foundation for decode smoke test." ) );
    }
    ComPtr< IMFSourceReader > reader;
    const std::wstring nativePath = QDir::toNativeSeparators( path ).toStdWString();
    result = MFCreateSourceReaderFromURL( nativePath.c_str(), nullptr, &reader );
    int decodedFrames = 0;
    bool decodeComplete = SUCCEEDED( result );
    double durationSeconds = 0.0;
    if ( decodeComplete ) {
        for ( int attempts = 0; attempts < 256; ++attempts ) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr< IMFSample > sample;
            result = reader->ReadSample( MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample );
            if ( FAILED( result ) ) {
                decodeComplete = false;
                break;
            }
            if ( sample ) ++decodedFrames;
            if ( flags & MF_SOURCE_READERF_ENDOFSTREAM ) break;
            if ( attempts == 255 ) decodeComplete = false;
        }
        PROPVARIANT duration;
        PropVariantInit( &duration );
        const HRESULT durationResult = reader->GetPresentationAttribute( MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration );
        if ( SUCCEEDED( durationResult ) && duration.vt == VT_UI8 )
            durationSeconds = double( duration.uhVal.QuadPart ) / 10'000'000.0;
        PropVariantClear( &duration );
    }
    reader.Reset();
    MFShutdown();
    CoUninitialize();
    if ( !check( decodeComplete && decodedFrames >= 3,
                 QStringLiteral( "Finalized MP4 did not decode into video frames (decoded %1)." ).arg( decodedFrames ) ) )
        return false;
    if ( !check( durationSeconds >= 0.5 && durationSeconds < 10.0,
                 QStringLiteral( "MP4 duration is implausible: %1 seconds." ).arg( durationSeconds, 0, 'f', 3 ) ) )
        return false;
    if ( !check( manager.registerVideo( path, &pathError ),
                 QStringLiteral( "MP4 was recorded but not registered in the experiment: %1" ).arg( pathError ) ) )
        return false;
    if ( recordedPath ) *recordedPath = path;
    qInfo() << "PASS: Media Foundation encoded and decoded" << decodedFrames << "MP4 video frames; duration:"
            << durationSeconds << "seconds; dropped input frames:" << recorder.droppedFrameCount();
    return true;
}
#endif
} // namespace

int main( int argc, char **argv ) {
    QCoreApplication app( argc, argv );
    QCoreApplication::setOrganizationName( QStringLiteral( "OpenHantekTest" ) );
    QCoreApplication::setApplicationName( QStringLiteral( "ExperimentStorageSmoke" ) );

    QTemporaryDir profile;
    if ( !check( profile.isValid(), QStringLiteral( "Could not create isolated test profile." ) ) )
        return 1;
    QSettings::setDefaultFormat( QSettings::IniFormat );
    QSettings::setPath( QSettings::IniFormat, QSettings::UserScope, profile.path() );
    const QString firstRoot = QDir( profile.path() ).filePath( QStringLiteral( "storage-one" ) );
    QSettings().setValue( QStringLiteral( "experiment/storageRoot" ), firstRoot );

    ExperimentManager manager;
    if ( !check( manager.storageRoot() == QDir( firstRoot ).absolutePath(),
                 QStringLiteral( "The persisted storage location was not loaded." ) ) )
        return 1;
    QString error;
    if ( !check( manager.createExperiment( QStringLiteral( "storage smoke" ), QStringLiteral( "isolated test" ), &error ),
                 QStringLiteral( "Could not create an experiment: %1" ).arg( error ) ) )
        return 1;
    const QString oldExperimentPath = manager.activeExperiment();
    if ( !check( QFileInfo( QDir( oldExperimentPath ).filePath( QStringLiteral( "experiment.json" ) ) ).isFile(),
                 QStringLiteral( "New experiment manifest is missing." ) ) )
        return 1;
    QFile sample( QDir( oldExperimentPath ).filePath( QStringLiteral( "Data/Raw/sample.ohraw" ) ) );
    if ( !check( sample.open( QIODevice::WriteOnly ) && sample.write( "OHRAW test" ) == 10,
                 QStringLiteral( "Could not create a test resource in the experiment." ) ) )
        return 1;
    sample.close();

    ExperimentCatalog catalog( manager.projectsRoot() );
    if ( !check( catalog.isReady(), QStringLiteral( "SQLite catalog did not open: %1" ).arg( catalog.errorString() ) ) )
        return 1;
    int indexedFiles = 0;
    int indexedExperiments = 0;
    if ( !check( waitForScan( catalog, &indexedFiles, &indexedExperiments ),
                 QStringLiteral( "Initial experiment scan did not finish." ) ) )
        return 1;
    QString queryError;
    const auto recordings = catalog.query( QStringLiteral( "recordings" ), QString(), QString(), &queryError );
    if ( !check( queryError.isEmpty() && recordings.size() == 1,
                 QStringLiteral( "OHRAW resource was not indexed: %1 (count %2)" ).arg( queryError ).arg( recordings.size() ) ) )
        return 1;

    const QString secondRoot = QDir( profile.path() ).filePath( QStringLiteral( "storage-two" ) );
    if ( !check( manager.setStorageRoot( secondRoot, &error ),
                 QStringLiteral( "Could not switch storage root: %1" ).arg( error ) ) )
        return 1;
    if ( !check( catalog.setProjectsRoot( manager.projectsRoot(), &error ),
                 QStringLiteral( "Could not switch SQLite catalog: %1" ).arg( error ) ) )
        return 1;
    if ( !check( manager.createExperiment( QStringLiteral( "new location" ), QString(), &error ),
                 QStringLiteral( "Could not create in the selected storage root: %1" ).arg( error ) ) )
        return 1;
    if ( !check( waitForScan( catalog, &indexedFiles, &indexedExperiments ),
                 QStringLiteral( "Rescan after storage-root switch did not finish." ) ) )
        return 1;
    const auto projects = catalog.query( QStringLiteral( "projects" ), QString(), QString(), &queryError );
    if ( !check( queryError.isEmpty() && projects.size() == 2,
                 QStringLiteral( "Old and new experiment roots were not both indexed: %1 (%2)" )
                     .arg( queryError ).arg( projects.size() ) ) )
        return 1;
    if ( !recordAndReplayRaw( manager ) )
        return 1;
    if ( !waitForScan( catalog, &indexedFiles, &indexedExperiments ) )
        return 1;
    const auto recordingsAfterCapture = catalog.query( QStringLiteral( "recordings" ), QString(), QString(), &queryError );
    if ( !check( queryError.isEmpty() && recordingsAfterCapture.size() == 2,
                 QStringLiteral( "Saved OHRAW file is not visible in the library: %1 (%2)" )
                     .arg( queryError ).arg( recordingsAfterCapture.size() ) ) )
        return 1;

    const QString damagedRoot = QDir( profile.path() ).filePath( QStringLiteral( "damaged" ) );
    const QString damagedProjects = QDir( damagedRoot ).filePath( QStringLiteral( "Projects" ) );
    const QString damagedDb = QDir( damagedRoot ).filePath( QStringLiteral( "Library/catalog.db" ) );
    if ( !QDir().mkpath( QFileInfo( damagedDb ).absolutePath() ) || !QDir().mkpath( damagedProjects ) ) {
        check( false, QStringLiteral( "Could not create isolated corrupt-index fixture." ) );
        return 1;
    }
    {
        QSqlDatabase fixtureDb = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), QStringLiteral( "corrupt_fixture" ) );
        fixtureDb.setDatabaseName( damagedDb );
        if ( !fixtureDb.open() ) {
            check( false, QStringLiteral( "Could not create SQLite corruption fixture: %1" ).arg( fixtureDb.lastError().text() ) );
            return 1;
        }
        QSqlQuery createTable( fixtureDb );
        if ( !createTable.exec( QStringLiteral( "CREATE TABLE fixture (id INTEGER PRIMARY KEY, value TEXT)" ) ) ) {
            check( false, QStringLiteral( "Could not create SQLite fixture schema: %1" ).arg( createTable.lastError().text() ) );
            return 1;
        }
        createTable = QSqlQuery();
        fixtureDb.close();
        fixtureDb = QSqlDatabase();
        QSqlDatabase::removeDatabase( QStringLiteral( "corrupt_fixture" ) );
    }
    QFile corruptFile( damagedDb );
    if ( !corruptFile.open( QIODevice::ReadWrite ) || !corruptFile.seek( 100 ) || corruptFile.read( 1 ) != QByteArray( "\x0d", 1 ) ||
         !corruptFile.seek( 100 ) || corruptFile.write( "\x7f", 1 ) != 1 ) {
        check( false, QStringLiteral( "Could not corrupt the isolated SQLite B-tree page." ) );
        return 1;
    }
    corruptFile.seek( 0 );
    const QByteArray corruptBytes = corruptFile.readAll();
    corruptFile.close();
    QFile manifest( QDir( damagedProjects ).filePath( QStringLiteral( "preserved/experiment.json" ) ) );
    if ( !QDir().mkpath( QFileInfo( manifest ).absolutePath() ) || !manifest.open( QIODevice::WriteOnly ) ||
         manifest.write( "{\"name\":\"preserved experiment\",\"createdAt\":\"2026-10-22T00:00:00Z\"}" ) < 0 ) {
        check( false, QStringLiteral( "Could not create recovery manifest fixture." ) );
        return 1;
    }
    manifest.close();
    int damagedIndexedFiles = 0;
    int damagedIndexedExperiments = 0;
    bool usedRecoveryIndex = false;
    {
        ExperimentCatalog damagedCatalog( damagedProjects );
        if ( !check( damagedCatalog.isReady(), QStringLiteral( "Confirmed corrupt SQLite index was not rebuilt: %1" )
                                                       .arg( damagedCatalog.errorString() ) ) )
            return 1;
        usedRecoveryIndex = QFileInfo( damagedCatalog.databasePath() ).fileName().startsWith( QStringLiteral( "catalog.rebuilt-" ) );
        if ( !check( usedRecoveryIndex, QStringLiteral( "Catalog did not use a separate non-destructive recovery index." ) ) )
            return 1;
        if ( !check( waitForScan( damagedCatalog, &damagedIndexedFiles, &damagedIndexedExperiments ),
                     QStringLiteral( "Recovery index could not rebuild from experiment.json." ) ) )
            return 1;
        QString damagedQueryError;
        if ( !check( damagedCatalog.query( QStringLiteral( "projects" ), QStringLiteral( "preserved experiment" ), QString(),
                                            &damagedQueryError ).size() == 1 &&
                         damagedQueryError.isEmpty(),
                     QStringLiteral( "Recovery did not index the preserved experiment manifest: %1" ).arg( damagedQueryError ) ) )
            return 1;
    }
    QFile preservedFile( damagedDb );
    if ( !check( preservedFile.open( QIODevice::ReadOnly ) && preservedFile.readAll() == corruptBytes,
                 QStringLiteral( "Catalog initialization changed the original database file." ) ) )
        return 1;
    const QStringList renamed = QDir( QFileInfo( damagedDb ).absolutePath() ).entryList(
        { QStringLiteral( "catalog.db.corrupt-*" ) }, QDir::Files );
    if ( !check( renamed.isEmpty(), QStringLiteral( "Initialization renamed a database without permission." ) ) )
        return 1;

#ifdef OPENHANTEK_TEST_MP4
    QString recordedVideoPath;
    if ( !recordAndDecodeMp4( manager, &recordedVideoPath ) )
        return 1;
    if ( !waitForScan( catalog, &indexedFiles, &indexedExperiments ) )
        return 1;
    const auto videos = catalog.query( QStringLiteral( "videos" ), QString(), QString(), &queryError );
    if ( !check( queryError.isEmpty() && videos.size() == 1,
                 QStringLiteral( "Recorded MP4 was not indexed in the experiment library: %1" ).arg( queryError ) ) )
        return 1;
#endif

    qInfo() << "PASS: persisted storage root, write probe, experiment creation, SQLite indexing, old-root preservation,"
               "OHRAW classification, and corrupt database preservation.";
    return 0;
}
