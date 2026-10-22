// SPDX-License-Identifier: GPL-2.0-or-later

#include "experimentmanager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
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
#include <QUuid>

ExperimentManager::ExperimentManager() {
    const QString documents = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation );
    if ( documents.isEmpty() ) {
        startupError = tr( "The system did not provide a Documents directory." );
        return;
    }

    projectsRootPath = QDir( documents ).filePath( "OpenHantek Laboratory/Projects" );
    if ( !QDir().mkpath( projectsRootPath ) ) {
        startupError = tr( "Could not create the experiments directory: %1" ).arg( projectsRootPath );
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
            *errorMessage = tr( "Could not create the experiment directory: %1" ).arg( path );
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
