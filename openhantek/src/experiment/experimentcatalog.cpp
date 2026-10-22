// SPDX-License-Identifier: GPL-2.0-or-later

#include "experimentcatalog.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

namespace {
constexpr int FilesPerBatch = 96;

QString stableId( const QString &value ) {
    return QString::fromLatin1( QCryptographicHash::hash( value.toUtf8(), QCryptographicHash::Sha256 ).toHex() );
}

QString describeSqlError( const QSqlError &error, const QString &path, bool opening ) {
    const QString detail = error.text().trimmed();
    const QString lower = detail.toLower();
    QString cause;
    if ( lower.contains( QStringLiteral( "locked" ) ) || lower.contains( QStringLiteral( "busy" ) ) )
        cause = QStringLiteral( "База занята или заблокирована" );
    else if ( lower.contains( QStringLiteral( "unable to open" ) ) || lower.contains( QStringLiteral( "readonly" ) ) ||
              lower.contains( QStringLiteral( "read-only" ) ) || lower.contains( QStringLiteral( "permission" ) ) )
        cause = QStringLiteral( "Нет доступа к файлу индекса" );
    else if ( lower.contains( QStringLiteral( "malformed" ) ) || lower.contains( QStringLiteral( "not a database" ) ) ||
              lower.contains( QStringLiteral( "corrupt" ) ) )
        cause = QStringLiteral( "SQLite сообщает о возможном повреждении базы" );
    else
        cause = opening ? QStringLiteral( "Не удалось открыть SQLite-базу" ) : QStringLiteral( "Ошибка схемы или SQL-запроса SQLite" );
    return QStringLiteral( "%1: %2. Файл: %3. SQLite: %4 (код %5)" )
        .arg( cause, error.driverText().trimmed(), path, detail, error.nativeErrorCode() );
}

bool testDirectoryWrite( const QString &directory, QString *errorMessage ) {
    const QString probePath = QDir( directory ).filePath(
        QStringLiteral( ".openhantek-catalog-probe-%1.tmp" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
    QFile probe( probePath );
    if ( !probe.open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Нет доступа на запись в каталог SQLite %1: %2" ).arg( directory, probe.errorString() );
        return false;
    }
    const QByteArray marker( "openhantek-sqlite-probe" );
    const bool written = probe.write( marker ) == marker.size() && probe.flush();
    const QString writeError = probe.errorString();
    probe.close();
    const bool removed = QFile::remove( probePath );
    if ( !written || !removed ) {
        if ( errorMessage )
            *errorMessage = QStringLiteral( "Проверка доступа к SQLite-каталогу %1 не завершена: %2%3" )
                                .arg( directory, written ? QString() : writeError,
                                      removed ? QString() : QStringLiteral( "; не удалён пробный файл " ) + probePath );
        return false;
    }
    return true;
}

} // namespace

ExperimentCatalog::ExperimentCatalog( const QString &projectsRoot, QObject *parent )
    : QObject( parent ), projectsRootPath( QDir( projectsRoot ).absolutePath() ) {
    legacyProjectsRootPaths = QSettings().value( QStringLiteral( "experiment/legacyProjectsRoots" ) ).toStringList();
    QString error;
    if ( !configureDatabase( &error ) ) {
        databaseError = error;
        qCritical().noquote() << databaseError;
    }
}

ExperimentCatalog::~ExperimentCatalog() {
    closeDatabase();
}

void ExperimentCatalog::closeDatabase() {
    if ( !connectionName.isEmpty() ) {
        const QString oldConnection = connectionName;
        database.close();
        database = QSqlDatabase();
        connectionName.clear();
        QSqlDatabase::removeDatabase( oldConnection );
    }
}

bool ExperimentCatalog::configureDatabase( QString *errorMessage ) {
    if ( projectsRootPath.isEmpty() ) {
        databaseError = QStringLiteral( "Путь проектов пуст: невозможно определить каталог индекса." );
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    const QString storageRoot = QFileInfo( projectsRootPath ).dir().absolutePath();
    const QString libraryDirectory = QDir( storageRoot ).filePath( QStringLiteral( "Library" ) );
    if ( !QDir().mkpath( libraryDirectory ) ) {
        databaseError = QStringLiteral( "Не удалось создать каталог SQLite %1" ).arg( libraryDirectory );
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    QStorageInfo storage( libraryDirectory );
    storage.refresh();
    if ( storage.isValid() && !storage.isReady() ) {
        databaseError = QStringLiteral( "Диск каталога SQLite недоступен: %1 (устройство %2)" )
                            .arg( libraryDirectory, QString::fromUtf8( storage.device() ) );
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    QString accessError;
    if ( !testDirectoryWrite( libraryDirectory, &accessError ) ) {
        databaseError = accessError;
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    if ( storage.isValid() && storage.bytesAvailable() == 0 ) {
        databaseError = QStringLiteral( "На диске каталога SQLite нет свободного места: %1" ).arg( libraryDirectory );
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    qInfo() << "OpenHantek SQLite storage probe passed:" << libraryDirectory
            << "volume=" << storage.rootPath() << "device=" << storage.device()
            << "filesystem=" << storage.fileSystemType() << "ready=" << storage.isReady()
            << "bytesAvailable=" << storage.bytesAvailable();
    const QString primaryCatalogPath = QDir( libraryDirectory ).filePath( QStringLiteral( "catalog.db" ) );
    const QString configuredRecoveryPath = QSettings().value( QStringLiteral( "experiment/recoveredCatalogPath" ) ).toString();
    const bool recoveryPathMatches = !configuredRecoveryPath.isEmpty() &&
        QDir( QFileInfo( configuredRecoveryPath ).absolutePath() ).absolutePath().compare(
#ifdef Q_OS_WIN
            QDir( libraryDirectory ).absolutePath(), Qt::CaseInsensitive
#else
            QDir( libraryDirectory ).absolutePath(), Qt::CaseSensitive
#endif
        ) == 0 && QFileInfo::exists( configuredRecoveryPath );
    catalogFilePath = recoveryPathMatches ? configuredRecoveryPath : primaryCatalogPath;
    if ( !recoveryPathMatches && !configuredRecoveryPath.isEmpty() )
        QSettings().remove( QStringLiteral( "experiment/recoveredCatalogPath" ) );
    if ( !QSqlDatabase::isDriverAvailable( QStringLiteral( "QSQLITE" ) ) ) {
        databaseError = QStringLiteral( "Драйвер Qt QSQLITE отсутствует. Проверьте сборку и каталог plugins/sqldrivers." );
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    const auto openAt = [ this ]( const QString &filePath ) {
        catalogFilePath = filePath;
        connectionName = QStringLiteral( "openhantek_catalog_%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) );
        database = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), connectionName );
        database.setDatabaseName( catalogFilePath );
        if ( initializeDatabase() )
            return true;
        closeDatabase();
        return false;
    };
    if ( !openAt( catalogFilePath ) ) {
        const QString initialError = databaseError;
        const QString failedCatalogPath = catalogFilePath;
        const bool tryNonDestructiveRecovery = databaseConfirmedCorrupt;
        if ( tryNonDestructiveRecovery ) {
            // Create an independent rebuildable index. The confirmed damaged database
            // and its WAL/SHM sidecars remain untouched as evidence and recovery source.
            const QString recoveredPath = QDir( libraryDirectory ).filePath(
                QStringLiteral( "catalog.rebuilt-%1.db" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
            databaseConfirmedCorrupt = false;
            if ( openAt( recoveredPath ) ) {
                QSettings().setValue( QStringLiteral( "experiment/recoveredCatalogPath" ), recoveredPath );
                qWarning().noquote() << QStringLiteral( "SQLite integrity check confirmed a damaged index. Original preserved at %1; using a fresh index at %2." )
                                           .arg( failedCatalogPath, recoveredPath );
                databaseError.clear();
                return true;
            }
            databaseError = QStringLiteral( "%1. Не удалось создать новый индекс из-за ошибки: %2" )
                                .arg( initialError, databaseError );
        }
        if ( errorMessage ) *errorMessage = databaseError;
        return false;
    }
    databaseConfirmedCorrupt = false;
    qInfo() << "OpenHantek SQLite catalog ready:" << catalogFilePath;
    databaseError.clear();
    return true;
}

bool ExperimentCatalog::setProjectsRoot( const QString &projectsRoot, QString *errorMessage ) {
    const QString newRoot = QDir( projectsRoot ).absolutePath();
    if ( newRoot.compare( projectsRootPath,
#ifdef Q_OS_WIN
                          Qt::CaseInsensitive
#else
                          Qt::CaseSensitive
#endif
                          ) == 0 && isReady() )
        return true;
    const QString oldRoot = projectsRootPath;
    const QString oldDatabasePath = catalogFilePath;
    const QStringList oldLegacyRoots = legacyProjectsRootPaths;
    if ( !oldRoot.isEmpty() && QFileInfo( oldRoot ).isDir() && !legacyProjectsRootPaths.contains( oldRoot ) ) {
        legacyProjectsRootPaths.append( oldRoot );
        QSettings().setValue( QStringLiteral( "experiment/legacyProjectsRoots" ), legacyProjectsRootPaths );
    }
    closeDatabase();
    projectsRootPath = newRoot;
    QString error;
    if ( configureDatabase( &error ) ) {
        qInfo() << "OpenHantek catalog switched to" << catalogFilePath;
        return true;
    }

    // Retain the previous usable index if the destination cannot be initialized.
    closeDatabase();
    projectsRootPath = oldRoot;
    legacyProjectsRootPaths = oldLegacyRoots;
    QSettings().setValue( QStringLiteral( "experiment/legacyProjectsRoots" ), legacyProjectsRootPaths );
    QString restoreError;
    const bool restored = configureDatabase( &restoreError );
    if ( !restored )
        databaseError = QStringLiteral( "%1. Прежний индекс также не открылся: %2" ).arg( error, restoreError );
    else
        databaseError.clear();
    catalogFilePath = restored ? catalogFilePath : oldDatabasePath;
    if ( errorMessage )
        *errorMessage = error;
    return false;
}

bool ExperimentCatalog::initializeDatabase() {
    databaseConfirmedCorrupt = false;
    if ( !database.open() ) {
        databaseError = describeSqlError( database.lastError(), catalogFilePath, true );
        return false;
    }
    QSqlQuery initialIntegrityCheck( database );
    if ( !initialIntegrityCheck.exec( QStringLiteral( "PRAGMA integrity_check" ) ) || !initialIntegrityCheck.next() ) {
        const QString detail = initialIntegrityCheck.lastError().text();
        const QString lower = detail.toLower();
        databaseConfirmedCorrupt = lower.contains( QStringLiteral( "malformed" ) ) ||
                                   lower.contains( QStringLiteral( "not a database" ) ) ||
                                   lower.contains( QStringLiteral( "corrupt" ) );
        databaseError = QStringLiteral( "Не удалось проверить целостность индекса до обновления схемы: %1. Файл: %2" )
                            .arg( detail, catalogFilePath );
        return false;
    }
    const QString initialIntegrityResult = initialIntegrityCheck.value( 0 ).toString();
    if ( initialIntegrityResult.compare( QStringLiteral( "ok" ), Qt::CaseInsensitive ) != 0 ) {
        databaseConfirmedCorrupt = true;
        databaseError = QStringLiteral( "SQLite integrity_check подтвердила повреждение (%1). Файл сохранён без изменений: %2" )
                            .arg( initialIntegrityResult, catalogFilePath );
        return false;
    }
    QSqlQuery query( database );
    const QStringList statements = {
        QStringLiteral( "PRAGMA foreign_keys = ON" ),
        QStringLiteral( "PRAGMA journal_mode = WAL" ),
        QStringLiteral( "CREATE TABLE IF NOT EXISTS experiments ("
                        "id TEXT PRIMARY KEY, manifest_id TEXT, path TEXT NOT NULL UNIQUE, name TEXT NOT NULL, "
                        "created_at TEXT NOT NULL, modified_at TEXT NOT NULL, description TEXT NOT NULL, scan_id TEXT NOT NULL)" ),
        QStringLiteral( "CREATE TABLE IF NOT EXISTS resources ("
                        "id TEXT PRIMARY KEY, experiment_id TEXT NOT NULL REFERENCES experiments(id) ON DELETE CASCADE, "
                        "relative_path TEXT NOT NULL, type TEXT NOT NULL, name TEXT NOT NULL, extension TEXT NOT NULL, "
                        "size INTEGER NOT NULL, modified_at TEXT NOT NULL, scan_id TEXT NOT NULL, "
                        "UNIQUE(experiment_id, relative_path))" ),
        QStringLiteral( "CREATE INDEX IF NOT EXISTS resources_type_name ON resources(type, name COLLATE NOCASE)" ),
        QStringLiteral( "CREATE INDEX IF NOT EXISTS resources_modified ON resources(modified_at DESC)" ),
        QStringLiteral( "CREATE INDEX IF NOT EXISTS experiments_name ON experiments(name COLLATE NOCASE)" )
    };
    for ( const QString &statement : statements ) {
        if ( !query.exec( statement ) ) {
            QSqlError sqlError = query.lastError();
            QString diagnosis = describeSqlError( sqlError, catalogFilePath, false );
            if ( database.isOpen() ) {
                QSqlQuery integrity( database );
                if ( integrity.exec( QStringLiteral( "PRAGMA integrity_check" ) ) && integrity.next() ) {
                    const QString check = integrity.value( 0 ).toString();
                    if ( check.compare( QStringLiteral( "ok" ), Qt::CaseInsensitive ) != 0 ) {
                        diagnosis += QStringLiteral( ". Результат PRAGMA integrity_check: %1" ).arg( check );
                        databaseConfirmedCorrupt = true;
                    }
                } else if ( !integrity.lastError().text().isEmpty() ) {
                    const QString integrityError = integrity.lastError().text();
                    diagnosis += QStringLiteral( ". Проверка целостности не выполнена: %1" ).arg( integrityError );
                    const QString lowerIntegrityError = integrityError.toLower();
                    databaseConfirmedCorrupt = lowerIntegrityError.contains( QStringLiteral( "malformed" ) ) ||
                                               lowerIntegrityError.contains( QStringLiteral( "not a database" ) ) ||
                                               lowerIntegrityError.contains( QStringLiteral( "corrupt" ) );
                }
            }
            databaseError = diagnosis;
            return false;
        }
    }
    return true;
}

QVector< ExperimentCatalogItem > ExperimentCatalog::query( const QString &category, const QString &search,
                                                            const QString &activeExperimentPath,
                                                            QString *errorMessage ) const {
    QVector< ExperimentCatalogItem > result;
    if ( !database.isOpen() ) {
        if ( errorMessage )
            *errorMessage = databaseError;
        return result;
    }

    const QString normalizedSearch = search.trimmed();
    const QString pattern = QLatin1Char( '%' ) + normalizedSearch + QLatin1Char( '%' );
    QSqlQuery query( database );
    if ( category == QLatin1String( "projects" ) || category == QLatin1String( "all" ) ||
         category == QLatin1String( "active" ) ) {
        QString sql = QStringLiteral( "SELECT id, name, path, created_at, modified_at, description "
                                      "FROM experiments WHERE (:search = '' OR name LIKE :pattern OR description LIKE :pattern)" );
        if ( category == QLatin1String( "active" ) )
            sql += QStringLiteral( " AND path = :activePath" );
        sql += QStringLiteral( " ORDER BY modified_at DESC, name COLLATE NOCASE" );
        query.prepare( sql );
        query.bindValue( QStringLiteral( ":search" ), normalizedSearch );
        query.bindValue( QStringLiteral( ":pattern" ), pattern );
        if ( category == QLatin1String( "active" ) )
            query.bindValue( QStringLiteral( ":activePath" ), QDir( activeExperimentPath ).absolutePath() );
        if ( !query.exec() ) {
            if ( errorMessage ) *errorMessage = query.lastError().text();
            return result;
        }
        while ( query.next() ) {
            ExperimentCatalogItem item;
            item.id = query.value( 0 ).toString();
            item.experimentId = item.id;
            item.type = QStringLiteral( "experiment" );
            item.name = query.value( 1 ).toString();
            item.experimentName = item.name;
            item.absolutePath = query.value( 2 ).toString();
            item.createdAt = query.value( 3 ).toString();
            item.modifiedAt = query.value( 4 ).toString();
            item.description = query.value( 5 ).toString();
            result.append( item );
        }
    }

    if ( category == QLatin1String( "projects" ) || category == QLatin1String( "active" ) )
        return result;

    QString sql = QStringLiteral(
        "SELECT r.id, r.experiment_id, r.type, r.name, e.name, e.path, r.relative_path, e.created_at, "
        "r.modified_at, e.description, r.size FROM resources r JOIN experiments e ON e.id = r.experiment_id "
        "WHERE (:search = '' OR r.name LIKE :pattern OR r.relative_path LIKE :pattern OR e.name LIKE :pattern)" );
    if ( category == QLatin1String( "recordings" ) ) sql += QStringLiteral( " AND r.type = 'recording'" );
    else if ( category == QLatin1String( "photos" ) ) sql += QStringLiteral( " AND r.type = 'photo'" );
    else if ( category == QLatin1String( "videos" ) ) sql += QStringLiteral( " AND r.type = 'video'" );
    else if ( category == QLatin1String( "imports" ) ) sql += QStringLiteral( " AND r.type = 'imported'" );
    else if ( category == QLatin1String( "analysis" ) ) sql += QStringLiteral( " AND r.type = 'analysis'" );
    else if ( category == QLatin1String( "active" ) ) sql += QStringLiteral( " AND e.path = :activePath" );
    sql += QStringLiteral( " ORDER BY r.modified_at DESC, e.name COLLATE NOCASE, r.name COLLATE NOCASE" );
    query.prepare( sql );
    query.bindValue( QStringLiteral( ":search" ), normalizedSearch );
    query.bindValue( QStringLiteral( ":pattern" ), pattern );
    if ( category == QLatin1String( "active" ) )
        query.bindValue( QStringLiteral( ":activePath" ), QDir( activeExperimentPath ).absolutePath() );
    if ( !query.exec() ) {
        if ( errorMessage ) *errorMessage = query.lastError().text();
        return result;
    }
    while ( query.next() ) {
        ExperimentCatalogItem item;
        item.id = query.value( 0 ).toString();
        item.experimentId = query.value( 1 ).toString();
        item.type = query.value( 2 ).toString();
        item.name = query.value( 3 ).toString();
        item.experimentName = query.value( 4 ).toString();
        item.absolutePath = QDir( query.value( 5 ).toString() ).filePath( query.value( 6 ).toString() );
        item.relativePath = query.value( 6 ).toString();
        item.createdAt = query.value( 7 ).toString();
        item.modifiedAt = query.value( 8 ).toString();
        item.description = query.value( 9 ).toString();
        item.size = query.value( 10 ).toLongLong();
        result.append( item );
    }
    return result;
}

void ExperimentCatalog::startRescan() {
    if ( scanning ) {
        rescanRequested = true;
        return;
    }
    if ( !database.isOpen() ) {
        emit scanFailed( databaseError );
        return;
    }
    pendingExperiments.clear();
    fileIterator.reset();
    QStringList roots = legacyProjectsRootPaths;
    roots.prepend( projectsRootPath );
    QStringList visitedRoots;
    for ( const QString &rootPath : roots ) {
        const QString absoluteRoot = QDir( rootPath ).absolutePath();
        if ( visitedRoots.contains( absoluteRoot, pathCaseSensitivity() ) )
            continue;
        visitedRoots.append( absoluteRoot );
        const QDir root( absoluteRoot );
        if ( !root.exists() )
            continue;
        const QFileInfoList directories = root.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                              QDir::Name | QDir::IgnoreCase );
        for ( const QFileInfo &directory : directories ) {
            if ( QFileInfo( directory.dir().filePath( directory.fileName() + "/experiment.json" ) ).isFile() &&
                 !pendingExperiments.contains( directory.absoluteFilePath(), pathCaseSensitivity() ) )
                pendingExperiments.append( directory.absoluteFilePath() );
        }
    }
    scanGeneration = QUuid::createUuid().toString( QUuid::WithoutBraces );
    completedExperiments = 0;
    indexedFiles = 0;
    scanning = true;
    emit scanProgress( 0, pendingExperiments.size() );
    QTimer::singleShot( 0, this, &ExperimentCatalog::processScanBatch );
}

bool ExperimentCatalog::beginNextExperiment() {
    if ( pendingExperiments.isEmpty() )
        return false;
    currentExperimentPath = pendingExperiments.takeFirst();
    const QFileInfo directoryInfo( currentExperimentPath );
    QFile manifestFile( QDir( currentExperimentPath ).filePath( "experiment.json" ) );
    QJsonObject manifest;
    if ( manifestFile.open( QIODevice::ReadOnly ) ) {
        const QJsonDocument document = QJsonDocument::fromJson( manifestFile.readAll() );
        if ( document.isObject() )
            manifest = document.object();
    }
    currentExperimentId = stableId( QDir::cleanPath( directoryInfo.absoluteFilePath() ).toCaseFolded() );
    currentExperimentName = manifest.value( QStringLiteral( "name" ) ).toString( directoryInfo.fileName() );
    currentExperimentDescription = manifest.value( QStringLiteral( "description" ) ).toString();
    currentExperimentCreatedAt = manifest.value( QStringLiteral( "createdAt" ) ).toString(
        directoryInfo.birthTime().isValid() ? directoryInfo.birthTime().toString( Qt::ISODateWithMs )
                                            : directoryInfo.lastModified().toString( Qt::ISODateWithMs ) );
    if ( !upsertExperiment( manifest.value( QStringLiteral( "id" ) ).toString() ) ) {
        emit scanFailed( databaseError );
        scanning = false;
        return false;
    }
    fileIterator = std::make_unique< QDirIterator >( currentExperimentPath,
        QDir::Files | QDir::NoSymLinks | QDir::Readable, QDirIterator::Subdirectories );
    return true;
}

bool ExperimentCatalog::upsertExperiment( const QString &manifestId ) {
    const QFileInfo info( currentExperimentPath );
    QSqlQuery query( database );
    query.prepare( QStringLiteral(
        "INSERT INTO experiments(id, manifest_id, path, name, created_at, modified_at, description, scan_id) "
        "VALUES(:id,:manifest,:path,:name,:created,:modified,:description,:scan) "
        "ON CONFLICT(path) DO UPDATE SET manifest_id=excluded.manifest_id, name=excluded.name, "
        "created_at=excluded.created_at, modified_at=excluded.modified_at, description=excluded.description, scan_id=excluded.scan_id" ) );
    query.bindValue( QStringLiteral( ":id" ), currentExperimentId );
    query.bindValue( QStringLiteral( ":manifest" ), manifestId );
    query.bindValue( QStringLiteral( ":path" ), QDir( currentExperimentPath ).absolutePath() );
    query.bindValue( QStringLiteral( ":name" ), currentExperimentName );
    query.bindValue( QStringLiteral( ":created" ), currentExperimentCreatedAt );
    query.bindValue( QStringLiteral( ":modified" ), info.lastModified().toString( Qt::ISODateWithMs ) );
    query.bindValue( QStringLiteral( ":description" ), currentExperimentDescription );
    query.bindValue( QStringLiteral( ":scan" ), scanGeneration );
    if ( !query.exec() ) {
        databaseError = QStringLiteral( "Не удалось обновить эксперимент в картотеке: %1" ).arg( query.lastError().text() );
        return false;
    }
    return true;
}

QString ExperimentCatalog::classifyFile( const QString &absolutePath, const QString &relativePath ) const {
    const QString normalized = relativePath.toLower();
    const QString extension = QFileInfo( absolutePath ).suffix().toLower();
    if ( normalized.startsWith( QStringLiteral( "analysis/" ) ) )
        return QStringLiteral( "analysis" );
    if ( extension == QLatin1String( "ohraw" ) )
        return QStringLiteral( "recording" );
    if ( extension == QLatin1String( "mp4" ) )
        return QStringLiteral( "video" );
    if ( extension == QLatin1String( "png" ) || extension == QLatin1String( "jpg" ) ||
         extension == QLatin1String( "jpeg" ) || extension == QLatin1String( "bmp" ) )
        return QStringLiteral( "photo" );
    if ( extension == QLatin1String( "json" ) ) {
        const QFileInfo fileInfo( absolutePath );
        const QString base = fileInfo.completeBaseName();
        for ( const QString &imageExtension : { QStringLiteral( "png" ), QStringLiteral( "jpg" ), QStringLiteral( "jpeg" ) } )
            if ( QFileInfo::exists( fileInfo.dir().filePath( base + QLatin1Char( '.' ) + imageExtension ) ) )
                return QStringLiteral( "metadata" );
    }
    if ( normalized.startsWith( QStringLiteral( "data/imported/" ) ) || extension == QLatin1String( "csv" ) ||
         extension == QLatin1String( "tsv" ) || extension == QLatin1String( "wav" ) )
        return QStringLiteral( "imported" );
    return QStringLiteral( "other" );
}

bool ExperimentCatalog::indexCurrentFile( const QString &path ) {
    const QFileInfo info( path );
    const QString relativePath = QDir( currentExperimentPath ).relativeFilePath( info.absoluteFilePath() );
    if ( relativePath == QLatin1String( "experiment.json" ) )
        return true;
    const QString resourceId = stableId( currentExperimentId + QLatin1Char( '/' ) + relativePath.toCaseFolded() );
    QSqlQuery query( database );
    query.prepare( QStringLiteral(
        "INSERT INTO resources(id,experiment_id,relative_path,type,name,extension,size,modified_at,scan_id) "
        "VALUES(:id,:experiment,:relative,:type,:name,:extension,:size,:modified,:scan) "
        "ON CONFLICT(experiment_id,relative_path) DO UPDATE SET type=excluded.type, name=excluded.name, "
        "extension=excluded.extension, size=excluded.size, modified_at=excluded.modified_at, scan_id=excluded.scan_id" ) );
    query.bindValue( QStringLiteral( ":id" ), resourceId );
    query.bindValue( QStringLiteral( ":experiment" ), currentExperimentId );
    query.bindValue( QStringLiteral( ":relative" ), relativePath );
    query.bindValue( QStringLiteral( ":type" ), classifyFile( path, relativePath ) );
    query.bindValue( QStringLiteral( ":name" ), info.completeBaseName() );
    query.bindValue( QStringLiteral( ":extension" ), info.suffix().toLower() );
    query.bindValue( QStringLiteral( ":size" ), info.size() );
    query.bindValue( QStringLiteral( ":modified" ), info.lastModified().toString( Qt::ISODateWithMs ) );
    query.bindValue( QStringLiteral( ":scan" ), scanGeneration );
    if ( !query.exec() ) {
        databaseError = QStringLiteral( "Не удалось добавить файл в картотеку: %1" ).arg( query.lastError().text() );
        return false;
    }
    ++indexedFiles;
    return true;
}

void ExperimentCatalog::processScanBatch() {
    if ( !scanning )
        return;
    if ( !fileIterator && !beginNextExperiment() ) {
        if ( !scanning )
            return;
        QSqlQuery cleanup( database );
        if ( !database.transaction() || !cleanup.exec( QStringLiteral( "DELETE FROM resources WHERE scan_id <> '%1'" ).arg( scanGeneration ) ) ||
             !cleanup.exec( QStringLiteral( "DELETE FROM experiments WHERE scan_id <> '%1'" ).arg( scanGeneration ) ) ||
             !database.commit() ) {
            database.rollback();
            scanning = false;
            emit scanFailed( QStringLiteral( "Не удалось завершить обновление индекса: %1" ).arg( cleanup.lastError().text() ) );
            return;
        }
        scanning = false;
        emit scanProgress( completedExperiments, completedExperiments );
        emit scanFinished( indexedFiles, completedExperiments );
        if ( rescanRequested ) {
            rescanRequested = false;
            QTimer::singleShot( 0, this, &ExperimentCatalog::startRescan );
        }
        return;
    }

    int processed = 0;
    while ( fileIterator && fileIterator->hasNext() && processed < FilesPerBatch ) {
        const QString path = fileIterator->next();
        if ( !indexCurrentFile( path ) ) {
            scanning = false;
            fileIterator.reset();
            emit scanFailed( databaseError );
            return;
        }
        ++processed;
    }
    if ( fileIterator && !fileIterator->hasNext() ) {
        fileIterator.reset();
        ++completedExperiments;
        emit scanProgress( completedExperiments, completedExperiments + pendingExperiments.size() );
    }
    QTimer::singleShot( 0, this, &ExperimentCatalog::processScanBatch );
}
