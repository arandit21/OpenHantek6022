// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QImage>
#include <QWidget>

class QLabel;
class QListWidget;
class QResizeEvent;
class QTabWidget;
class QPushButton;
class QEvent;

/// Small file-system-backed media shelf. It intentionally contains only photos and videos.
class MediaLibraryWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit MediaLibraryWidget( QWidget *parent = nullptr );

    static QString photosDirectory();
    static QString videosDirectory();
    QString newPhotoPath() const;
    QString newVideoPath() const;
    void refresh();

  private:
    QTabWidget *tabs = nullptr;
    QListWidget *photoList = nullptr;
    QListWidget *videoList = nullptr;
    QLabel *photoPreview = nullptr;
    QLabel *videoPreview = nullptr;
    QImage photoImage;
    QPushButton *openButton = nullptr;
    QPushButton *folderButton = nullptr;
    QPushButton *removeButton = nullptr;

    void refreshList( QListWidget *list, const QString &directory, const QStringList &filters );
    QString selectedPath() const;
    void updatePhotoPreview();
    void scalePhotoPreview();
    void updateVideoPreview();
    void openSelected();
    void showSelectedInFolder();
    void deleteSelected();

  protected:
    void resizeEvent( QResizeEvent *event ) override;
    void changeEvent( QEvent *event ) override;
};
