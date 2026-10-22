// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QCoreApplication>
#include <QString>

/// Owns the on-disk location and manifest for the active experiment.
/// The manager does not own measurement or GUI objects.
class ExperimentManager {
    Q_DECLARE_TR_FUNCTIONS( ExperimentManager )

  public:
    ExperimentManager();

    QString projectsRoot() const { return projectsRootPath; }
    QString activeExperiment() const { return activeExperimentPath; }

    /// Create a new experiment folder with the standard directory tree and manifest.
    bool createExperiment( const QString &name, const QString &description = QString(), QString *errorMessage = nullptr );

    /// Return the active experiment, creating a dated default experiment if needed.
    QString ensureActiveExperiment( QString *errorMessage = nullptr );
    /// Make an existing experiment the active target for new recordings and media.
    bool activateExperiment( const QString &path, QString *errorMessage = nullptr );

    /// Generate a unique path in the active experiment's Images directory.
    QString nextImagePath( QString *errorMessage = nullptr );
    /// Generate a unique path in the active experiment's Video directory.
    QString nextVideoPath( QString *errorMessage = nullptr );
    /// Generate a unique path in the active experiment's Raw data directory.
    QString nextRawDataPath( QString *errorMessage = nullptr );

    /// Add a relative image path to experiment.json after the image was saved.
    bool registerImage( const QString &absoluteImagePath, QString *errorMessage = nullptr );
    bool registerVideo( const QString &absoluteVideoPath, QString *errorMessage = nullptr );
    bool registerDataFile( const QString &absoluteDataPath, QString *errorMessage = nullptr );

  private:
    QString projectsRootPath;
    QString activeExperimentPath;
    QString startupError;

    bool createExperimentAt( const QString &folderName, const QString &name, const QString &description,
                             QString *errorMessage );
    bool ensureExperimentDirectories( const QString &path, QString *errorMessage ) const;
    bool registerFile( const QString &absolutePath, const QString &manifestKey, QString *errorMessage ) const;
    bool updateManifest( const QString &experimentPath, const QString &relativePath, const QString &manifestKey,
                         QString *errorMessage ) const;
};
