// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "experimentcatalog.h"

#include <QVector>
#include <QHash>
#include <QIcon>
#include <QStringList>
#include <QWidget>

class ExperimentManager;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QMenu;
class QStackedWidget;
class QTimer;
class QTableWidget;
class QTreeWidget;
class QResizeEvent;
class QPoint;

class ExperimentLibraryWidget : public QWidget {
    Q_OBJECT

  public:
    ExperimentLibraryWidget( ExperimentCatalog *catalog, ExperimentManager *manager, QWidget *parent = nullptr );
    void refreshRecords();

  signals:
    void openRecordingRequested( const QString &path );
    void activeExperimentChanged( const QString &path );

  private slots:
    void updateDetails();
    void openSelected();
    void showSelectedInFolder();
    void startIndexRefresh();
    void deleteSelected();
    void restoreFromTrash();
    void emptyTrash();
    void chooseStorageRoot();
    void showContextMenu( const QPoint &position );

  private:
    void resizeEvent( QResizeEvent *event ) override;

    ExperimentCatalog *catalog;
    ExperimentManager *manager;
    QTreeWidget *navigation = nullptr;
    QLineEdit *searchEdit = nullptr;
    QComboBox *sortCombo = nullptr;
    QComboBox *experimentFilter = nullptr;
    QComboBox *viewModeCombo = nullptr;
    QTableWidget *table = nullptr;
    QListWidget *tiles = nullptr;
    QStackedWidget *libraryViews = nullptr;
    QTimer *thumbnailTimer = nullptr;
    int thumbnailIndex = 0;
    QStringList rememberedSelectionIds;
    QHash< QString, QIcon > thumbnailCache;
    bool thumbnailLoading = false;
    quint64 thumbnailGeneration = 0;
    QLabel *detailsTitle = nullptr;
    QLabel *detailsText = nullptr;
    QLabel *previewImage = nullptr;
    QTableWidget *dataPreview = nullptr;
    QLabel *indexStatus = nullptr;
    QPushButton *indexDetailsButton = nullptr;
    QPushButton *openButton = nullptr;
    QPushButton *showFolderButton = nullptr;
    QVector< ExperimentCatalogItem > rows;
    ExperimentCatalogItem selectedItem;
    bool hasSelection = false;
    QString lastIndexError;

    QString selectedCategory() const;
    QVector< ExperimentCatalogItem > selectedRecords() const;
    void previewSelected();
    void loadNextThumbnails();
    void openImageViewer( const QString &path );
    void openVideoPlayer( const QString &path );
    bool moveToTrash( const ExperimentCatalogItem &item, QString *errorMessage );
    bool updateExperimentManifest( const QString &path, const QString &type, bool restore, QString *errorMessage );
    void setIndexStatus( const QString &text, bool error = false );
};
