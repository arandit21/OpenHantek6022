// SPDX-License-Identifier: GPL-2.0-or-later

#include "experimentmanager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

namespace {
bool prepareWritableDirectory( const QString &path, QString *errorMessage ) {
    const QString absolutePath = QDir( path ).absolutePath();
    if ( path.trimmed().isEmpty() || !QDir().mkpath( absolutePath ) ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Не удалось создать каталог хранилища: %1" ).arg( absolutePath );
        return false;
    }

    QStorageInfo storage( absolutePath );
    storage.refresh();
    if ( storage.isValid() && !storage.isReady() ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Хранилище недоступно: %1 (устройство %2)" )
                                .arg( absolutePath, QString::fromUtf8( storage.device() ) );
        return false;
    }

    const QString probePath = QDir( absolutePath ).filePath(
        QStringLiteral( ".openhantek-write-test-%1.tmp" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
    QFile probe( probePath );
    if ( !probe.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Нет доступа на запись в %1: %2" ).arg( absolutePath, probe.errorString() );
        return false;
    }
    const QByteArray marker( "openhantek-storage-probe" );
    const bool written = probe.write( marker ) == marker.size() && probe.flush();
    const QString writeError = probe.errorString();
    probe.close();
    const bool removed = QFile::remove( probePath );
    if ( !written || !removed ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Проверочная запись в %1 не завершена: %2%3" )
                                .arg( absolutePath, written ? QString() : writeError,
                                      removed ? QString() : QStringLiteral( "; не удалось удалить временный файл " ) + probePath );
        return false;
    }
    if ( storage.isValid() && storage.bytesAvailable() == 0 ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "На диске хранилища нет свободного места: %1" ).arg( absolutePath );
        return false;
    }
    qInfo() << "OpenHantek storage probe passed:" << absolutePath
            << "volume=" << storage.rootPath() << "device=" << storage.device()
            << "filesystem=" << storage.fileSystemType() << "ready=" << storage.isReady()
            << "bytesAvailable=" << storage.bytesAvailable();
    return true;
}

QString directoryCreationDiagnostic( const QString &path, const QString &parentPath ) {
    const QFileInfo parentInfo( parentPath );
    QStorageInfo storage( parentPath );
    storage.refresh();
    return QStringLiteral( "Не удалось создать каталог %1. Родитель: %2; существует=%3; доступен для записи=%4; "
                           "устройство=%5; диск готов=%6; свободно байт=%7" )
        .arg( path, parentPath, parentInfo.exists() ? QStringLiteral( "да" ) : QStringLiteral( "нет" ),
              parentInfo.isWritable() ? QStringLiteral( "да" ) : QStringLiteral( "нет" ),
              QString::fromUtf8( storage.device() ), storage.isReady() ? QStringLiteral( "да" ) : QStringLiteral( "нет" ),
              storage.isValid() ? QString::number( storage.bytesAvailable() ) : QStringLiteral( "неизвестно" ) );
}
} // namespace

ExperimentManager::ExperimentManager() {
    const QString documents = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation );
    if ( documents.isEmpty() )
        qWarning() << "QStandardPaths DocumentsLocation is empty";
    const QString defaultParent = documents.isEmpty()
        ? QStandardPaths::writableLocation( QStandardPaths::AppLocalDataLocation ) : documents;
    if ( defaultParent.isEmpty() ) {
        startupError = QStringLiteral( "Qt не вернул ни DocumentsLocation, ни AppLocalDataLocation." );
        qCritical().noquote() << startupError;
        return;
    }
    const QString savedRoot = QSettings().value( QStringLiteral( "experiment/storageRoot" ) ).toString();
    storageRootPath = QDir( savedRoot.isEmpty()
                                ? QDir( defaultParent ).filePath( QStringLiteral( "OpenHantek Laboratory" ) )
                                : savedRoot ).absolutePath();
    projectsRootPath = QDir( storageRootPath ).filePath( QStringLiteral( "Projects" ) );
    qInfo() << "OpenHantek storage roots:" << "DocumentsLocation=" << documents
            << "storage=" << storageRootPath << "projects=" << projectsRootPath;
    QString storageError;
    const bool storageReady = prepareWritableDirectory( storageRootPath, &storageError );
    const bool projectsReady = storageReady && QDir().mkpath( projectsRootPath ) &&
                               prepareWritableDirectory( projectsRootPath, &storageError );
    if ( !projectsReady ) {
        startupError = storageError.isEmpty() ? directoryCreationDiagnostic( projectsRootPath, storageRootPath ) : storageError;
        qWarning().noquote() << startupError;
        return;
    }

    const QString savedPath = QSettings().value( "experiment/activePath" ).toString();
    if ( !savedPath.isEmpty() && QFileInfo( QDir( savedPath ).filePath( "experiment.json" ) ).isFile() ) {
        activeExperimentPath = QDir( savedPath ).absolutePath();
        QString error;
        if ( !ensureExperimentDirectories( activeExperimentPath, &error ) ) {
            activeExperimentPath.clear();
            startupError = error;
        }
    }
}

bool ExperimentManager::setStorageRoot( const QString &path, QString *errorMessage ) {
    const QString newRoot = QDir( path ).absolutePath();
    QString error;
    if ( !prepareWritableDirectory( newRoot, &error ) ) {
        if ( errorMessage )
            *errorMessage = error;
        return false;
    }
    if ( newRoot.compare( storageRootPath,
#ifdef Q_OS_WIN
                          Qt::CaseInsensitive
#else
                          Qt::CaseSensitive
#endif
                          ) == 0 ) {
        QSettings().setValue( QStringLiteral( "experiment/storageRoot" ), storageRootPath );
        return true;
    }
    const QString newProjectsRoot = QDir( newRoot ).filePath( QStringLiteral( "Projects" ) );
    if ( !QDir().mkpath( newProjectsRoot ) || !prepareWritableDirectory( newProjectsRoot, &error ) ) {
        if ( errorMessage )
            *errorMessage = error.isEmpty() ? directoryCreationDiagnostic( newProjectsRoot, newRoot ) : error;
        return false;
    }

    storageRootPath = newRoot;
    projectsRootPath = newProjectsRoot;
    activeExperimentPath.clear();
    QSettings settings;
    settings.setValue( QStringLiteral( "experiment/storageRoot" ), storageRootPath );
    settings.remove( QStringLiteral( "experiment/activePath" ) );
    qInfo() << "OpenHantek storage root changed to" << storageRootPath;
    return true;
}

bool ExperimentManager::createExperiment( const QString &name, const QString &description, QString *errorMessage ) {
    if ( projectsRootPath.isEmpty() ) {
        if ( errorMessage )
            *errorMessage = startupError;
        return false;
    }

    QString safeName = name.trimmed();
    safeName.replace( QRegularExpression( "[\\\\/:*?\"<>|]" ), "_" );
    safeName.replace( QRegularExpression( "\\s+" ), "_" );
    if ( safeName.isEmpty() || safeName == "." || safeName == ".." )
        safeName = "Experiment";

    const QString prefix = QDate::currentDate().toString( "yyyy-MM-dd" ) + "_" + safeName;
    QString folderName = prefix;
    for ( unsigned suffix = 2; QFileInfo::exists( QDir( projectsRootPath ).filePath( folderName ) ); ++suffix )
        folderName = prefix + QString( "_%1" ).arg( suffix, 3, 10, QLatin1Char( '0' ) );

    return createExperimentAt( folderName, safeName, description, errorMessage );
}

QString ExperimentManager::ensureActiveExperiment( QString *errorMessage ) {
    if ( !activeExperimentPath.isEmpty() && QFileInfo( QDir( activeExperimentPath ).filePath( "experiment.json" ) ).isFile() )
        return activeExperimentPath;

    if ( projectsRootPath.isEmpty() ) {
        if ( errorMessage )
            *errorMessage = startupError;
        return QString();
    }

    const QString date = QDate::currentDate().toString( "yyyy-MM-dd" );
    for ( unsigned index = 1;; ++index ) {
        const QString name = QString( "Experiment_%1" ).arg( index, 3, 10, QLatin1Char( '0' ) );
        const QString folderName = date + "_" + name;
        const QString path = QDir( projectsRootPath ).filePath( folderName );
        if ( QFileInfo::exists( path ) )
            continue;
        if ( !createExperimentAt( folderName, name, QString(), errorMessage ) )
            return QString();
        return activeExperimentPath;
    }
}

bool ExperimentManager::activateExperiment( const QString &path, QString *errorMessage ) {
    const QString absolutePath = QDir( path ).absolutePath();
    if ( !QFileInfo( QDir( absolutePath ).filePath( "experiment.json" ) ).isFile() ) {
        if ( errorMessage )
            *errorMessage = tr( "The selected folder is not an OpenHantek experiment: %1" ).arg( absolutePath );
        return false;
    }
    if ( !ensureExperimentDirectories( absolutePath, errorMessage ) )
        return false;
    activeExperimentPath = absolutePath;
    QSettings().setValue( "experiment/activePath", activeExperimentPath );
    return true;
}

QString ExperimentManager::nextImagePath( QString *errorMessage ) {
    const QString experiment = ensureActiveExperiment( errorMessage );
    if ( experiment.isEmpty() )
        return QString();

    const QString imageDirectory = QDir( experiment ).filePath( "Media/Images" );
    const QString timestamp = QDateTime::currentDateTime().toString( "yyyyMMdd_hhmmss_zzz" );
    for ( unsigned index = 1;; ++index ) {
        const QString suffix = index == 1 ? QString() : QString( "_%1" ).arg( index, 2, 10, QLatin1Char( '0' ) );
        const QString filePath = QDir( imageDirectory ).filePath( timestamp + suffix + ".png" );
        if ( !QFileInfo::exists( filePath ) && !QFileInfo::exists( QFileInfo( filePath ).dir().filePath(
                                                       QFileInfo( filePath ).completeBaseName() + ".json" ) ) )
            return filePath;
    }
}

QString ExperimentManager::nextVideoPath( QString *errorMessage ) {
    const QString experiment = ensureActiveExperiment( errorMessage );
    if ( experiment.isEmpty() )
        return QString();

    const QString videoDirectory = QDir( experiment ).filePath( "Media/Video" );
    const QString timestamp = QDateTime::currentDateTime().toString( "yyyyMMdd_hhmmss_zzz" );
    for ( unsigned index = 1;; ++index ) {
        const QString suffix = index == 1 ? QString() : QString( "_%1" ).arg( index, 2, 10, QLatin1Char( '0' ) );
        const QString filePath = QDir( videoDirectory ).filePath( "scope_" + timestamp + suffix + ".mp4" );
        if ( !QFileInfo::exists( filePath ) )
            return filePath;
    }
}

QString ExperimentManager::nextRawDataPath( QString *errorMessage ) {
    const QString experiment = ensureActiveExperiment( errorMessage );
    if ( experiment.isEmpty() )
        return QString();

    const QString dataDirectory = QDir( experiment ).filePath( "Data/Raw" );
    const QString timestamp = QDateTime::currentDateTime().toString( "yyyyMMdd_hhmmss_zzz" );
    for ( unsigned index = 1;; ++index ) {
        const QString suffix = index == 1 ? QString() : QString( "_%1" ).arg( index, 2, 10, QLatin1Char( '0' ) );
        const QString filePath = QDir( dataDirectory ).filePath( "capture_" + timestamp + suffix + ".ohraw" );
        if ( !QFileInfo::exists( filePath ) && !QFileInfo::exists( filePath + ".partial" ) )
            return filePath;
    }
}

bool ExperimentManager::registerImage( const QString &absoluteImagePath, QString *errorMessage ) {
    return registerFile( absoluteImagePath, "images", errorMessage );
}

bool ExperimentManager::registerVideo( const QString &absoluteVideoPath, QString *errorMessage ) {
    return registerFile( absoluteVideoPath, "videos", errorMessage );
}

bool ExperimentManager::registerDataFile( const QString &absoluteDataPath, QString *errorMessage ) {
    return registerFile( absoluteDataPath, "sampleDataFiles", errorMessage );
}

bool ExperimentManager::registerFile( const QString &absolutePath, const QString &manifestKey, QString *errorMessage ) const {
    QString experiment;
    QDir directory = QFileInfo( absolutePath ).absoluteDir();
    while ( directory.exists() ) {
        if ( QFileInfo( directory.filePath( "experiment.json" ) ).isFile() ) {
            experiment = directory.absolutePath();
            break;
        }
        const QString parent = directory.absolutePath();
        if ( !directory.cdUp() || directory.absolutePath() == parent )
            break;
    }
    if ( experiment.isEmpty() ) {
        if ( errorMessage )
            *errorMessage = tr( "The saved file is not inside an experiment directory." );
        return false;
    }
    const QString relativePath = QDir( experiment ).relativeFilePath( QFileInfo( absolutePath ).absoluteFilePath() );
    return updateManifest( experiment, relativePath, manifestKey, errorMessage );
}

bool ExperimentManager::createExperimentAt( const QString &folderName, const QString &name, const QString &description,
                                            QString *errorMessage ) {
    const QDir projectsDirectory( projectsRootPath );
    const QString path = projectsDirectory.filePath( folderName );
    if ( !projectsDirectory.mkdir( folderName ) ) {
        if ( errorMessage )
            *errorMessage = directoryCreationDiagnostic( path, projectsRootPath );
        return false;
    }
    if ( !ensureExperimentDirectories( path, errorMessage ) ) {
        return false;
    }

    QJsonObject manifest;
    manifest.insert( "schema", "org.openhantek.experiment/1" );
    manifest.insert( "id", QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    manifest.insert( "name", name );
    manifest.insert( "createdAt", QDateTime::currentDateTime().toString( Qt::ISODateWithMs ) );
    manifest.insert( "description", description );
    manifest.insert( "application", QCoreApplication::applicationName() );
    manifest.insert( "applicationVersion", QCoreApplication::applicationVersion() );
    manifest.insert( "images", QJsonArray() );
    manifest.insert( "videos", QJsonArray() );
    manifest.insert( "sampleDataFiles", QJsonArray() );

    QSaveFile file( QDir( path ).filePath( "experiment.json" ) );
    const QByteArray json = QJsonDocument( manifest ).toJson( QJsonDocument::Indented );
    if ( !file.open( QIODevice::WriteOnly ) || file.write( json ) != json.size() || !file.commit() ) {
        if ( errorMessage )
            *errorMessage = tr( "Could not write experiment metadata to %1" ).arg( file.fileName() );
        return false;
    }

    activeExperimentPath = QDir( path ).absolutePath();
    QSettings().setValue( "experiment/activePath", activeExperimentPath );
    return true;
}

bool ExperimentManager::ensureExperimentDirectories( const QString &path, QString *errorMessage ) const {
    const QStringList directories = { "Media/Images", "Media/Video", "Media/Camera", "Data/Raw", "Data/Exports",
                                      "Analysis/Spectrum", "Analysis/Reports", "Workspace" };
    for ( const QString &relative : directories ) {
        if ( !QDir().mkpath( QDir( path ).filePath( relative ) ) ) {
            if ( errorMessage )
                *errorMessage = tr( "Could not create experiment directory: %1" ).arg( QDir( path ).filePath( relative ) );
            return false;
        }
    }
    return true;
}

bool ExperimentManager::updateManifest( const QString &experimentPath, const QString &relativePath, const QString &manifestKey,
                                        QString *errorMessage ) const {
    const QString manifestPath = QDir( experimentPath ).filePath( "experiment.json" );
    QFile source( manifestPath );
    if ( !source.open( QIODevice::ReadOnly ) ) {
        if ( errorMessage )
            *errorMessage = tr( "Could not read experiment metadata from %1" ).arg( manifestPath );
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson( source.readAll(), &parseError );
    source.close();
    if ( parseError.error != QJsonParseError::NoError || !document.isObject() ) {
        if ( errorMessage )
            *errorMessage = tr( "Experiment metadata is invalid: %1" ).arg( manifestPath );
        return false;
    }

    QJsonObject manifest = document.object();
    QJsonArray files = manifest.value( manifestKey ).toArray();
    if ( !relativePath.isEmpty() && !files.contains( relativePath ) ) {
        files.append( relativePath );
        manifest.insert( manifestKey, files );
    }
    QSaveFile destination( manifestPath );
    const QByteArray json = QJsonDocument( manifest ).toJson( QJsonDocument::Indented );
    if ( !destination.open( QIODevice::WriteOnly ) || destination.write( json ) != json.size() || !destination.commit() ) {
        if ( errorMessage )
            *errorMessage = tr( "Could not safely update experiment metadata: %1" ).arg( manifestPath );
        return false;
    }
    return true;
}
