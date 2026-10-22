// SPDX-License-Identifier: GPL-2.0-or-later

#include "experimentcatalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTimer>
#include <QUuid>

namespace {
constexpr int FilesPerBatch = 96;

QString stableId( const QString &value ) {
    return QString::fromLatin1( QCryptographicHash::hash( value.toUtf8(), QCryptographicHash::Sha256 ).toHex() );
}

} // namespace

ExperimentCatalog::ExperimentCatalog( const QString &projectsRoot, QObject *parent )
    : QObject( parent ), projectsRootPath( QDir( projectsRoot ).absolutePath() ) {
    const QString documents = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation );
    if ( documents.isEmpty() ) {
        databaseError = QStringLiteral( "Не удалось определить папку Документы для индекса библиотеки." );
        return;
    }
    const QString libraryDirectory = QDir( documents ).filePath( "OpenHantek Laboratory/Library" );
    if ( !QDir().mkpath( libraryDirectory ) ) {
        databaseError = QStringLiteral( "Не удалось создать папку библиотеки: %1" ).arg( libraryDirectory );
        return;
    }
    catalogFilePath = QDir( libraryDirectory ).filePath( "catalog.db" );
    connectionName = QStringLiteral( "openhantek_catalog_%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) );
    database = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), connectionName );
    database.setDatabaseName( catalogFilePath );
    if ( !initializeDatabase() ) {
        const QString initialError = databaseError;
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase( connectionName );

        // The SQLite file is a disposable index; preserve a damaged copy before rebuilding it.
        const QString backupSuffix = QStringLiteral( ".corrupt-%1" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) );
        const QStringList sidecars = { QStringLiteral( "-wal" ), QStringLiteral( "-shm" ) };
        const bool movedDatabase = QFile::rename( catalogFilePath, catalogFilePath + backupSuffix );
        for ( const QString &sidecar : sidecars )
            if ( QFileInfo::exists( catalogFilePath + sidecar ) )
                QFile::rename( catalogFilePath + sidecar, catalogFilePath + backupSuffix + sidecar );

        database = QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), connectionName );
        database.setDatabaseName( catalogFilePath );
        if ( movedDatabase && initializeDatabase() ) {
            databaseError.clear();
        } else {
            if ( databaseError.isEmpty() )
                databaseError = initialError;
            else
                databaseError = QStringLiteral( "%1; исходный индекс сохранён как повреждённая копия: %2" )
                                    .arg( databaseError, catalogFilePath + backupSuffix );
            database.close();
        }
    }
}

ExperimentCatalog::~ExperimentCatalog() {
    database.close();
    database = QSqlDatabase();
    if ( !connectionName.isEmpty() )
        QSqlDatabase::removeDatabase( connectionName );
}

bool ExperimentCatalog::initializeDatabase() {
    if ( !database.open() ) {
        databaseError = QStringLiteral( "Не удалось открыть индекс библиотеки: %1" ).arg( database.lastError().text() );
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
            databaseError = QStringLiteral( "Ошибка базы картотеки: %1" ).arg( query.lastError().text() );
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
    const QDir root( projectsRootPath );
    const QFileInfoList directories = root.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                                          QDir::Name | QDir::IgnoreCase );
    for ( const QFileInfo &directory : directories ) {
        if ( QFileInfo( directory.dir().filePath( directory.fileName() + "/experiment.json" ) ).isFile() )
            pendingExperiments.append( directory.absoluteFilePath() );
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
