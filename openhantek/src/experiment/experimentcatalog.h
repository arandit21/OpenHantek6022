// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QDirIterator;

struct ExperimentCatalogItem {
    QString id;
    QString experimentId;
    QString type;
    QString name;
    QString experimentName;
    QString absolutePath;
    QString relativePath;
    QString createdAt;
    QString modifiedAt;
    QString description;
    qint64 size = 0;
};

/// Persistent, rebuildable index over experiment.json files and their files.
/// The manifests and files remain authoritative; SQLite is only a search index.
class ExperimentCatalog : public QObject {
    Q_OBJECT

  public:
    explicit ExperimentCatalog( const QString &projectsRoot, QObject *parent = nullptr );
    ~ExperimentCatalog() override;

    bool isReady() const { return database.isOpen(); }
    QString errorString() const { return databaseError; }
    QString databasePath() const { return catalogFilePath; }
    bool isScanning() const { return scanning; }
    QVector< ExperimentCatalogItem > query( const QString &category, const QString &search,
                                            const QString &activeExperimentPath = QString(),
                                            QString *errorMessage = nullptr ) const;
    void startRescan();

  signals:
    void scanProgress( int completedExperiments, int totalExperiments );
    void scanFinished( int indexedFiles, int indexedExperiments );
    void scanFailed( const QString &message );

  private:
    QString projectsRootPath;
    QString catalogFilePath;
    QString connectionName;
    QString databaseError;
    QSqlDatabase database;
    QString scanGeneration;
    QStringList pendingExperiments;
    QString currentExperimentPath;
    QString currentExperimentId;
    QString currentExperimentName;
    QString currentExperimentDescription;
    QString currentExperimentCreatedAt;
    std::unique_ptr< QDirIterator > fileIterator;
    int completedExperiments = 0;
    int indexedFiles = 0;
    bool scanning = false;
    bool rescanRequested = false;

    bool initializeDatabase();
    void processScanBatch();
    bool beginNextExperiment();
    bool indexCurrentFile( const QString &path );
    bool upsertExperiment( const QString &manifestId );
    QString classifyFile( const QString &absolutePath, const QString &relativePath ) const;
};
