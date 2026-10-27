// SPDX-License-Identifier: GPL-2.0-or-later

#include "medialibrarywidget.h"

#include <QDateTime>
#include <QEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString libraryPath( QStandardPaths::StandardLocation location, const QString &kind ) {
    const QString base = QStandardPaths::writableLocation( location );
    return QDir( base ).filePath( QStringLiteral( "OpenHantek/%1" ).arg( kind ) );
}
} // namespace

MediaLibraryWidget::MediaLibraryWidget( QWidget *parent ) : QWidget( parent ) {
    QDir().mkpath( photosDirectory() );
    QDir().mkpath( videosDirectory() );

    auto *root = new QVBoxLayout( this );
    root->setContentsMargins( 8, 8, 8, 8 );
    root->setSpacing( 6 );
    tabs = new QTabWidget( this );
    photoList = new QListWidget( tabs );
    videoList = new QListWidget( tabs );
    photoList->setObjectName( QStringLiteral( "libraryPhotos" ) );
    videoList->setObjectName( QStringLiteral( "libraryVideos" ) );
    photoPreview = new QLabel( tabs );
    photoPreview->setFixedHeight( 120 );
    photoPreview->setAlignment( Qt::AlignCenter );
    photoPreview->setText( tr( "Выберите фотографию" ) );
    photoPreview->setStyleSheet( QStringLiteral( "background:#e8edf3; border:1px solid #cbd5e1; border-radius:5px;" ) );
    videoPreview = new QLabel( tabs );
    videoPreview->setFixedHeight( 160 );
    videoPreview->setAlignment( Qt::AlignCenter );
    videoPreview->setText( tr( "Выберите видео" ) );
    videoPreview->setStyleSheet( QStringLiteral(
        "background:#e8edf3; color:#475569; border:1px solid #cbd5e1; border-radius:5px; padding:8px;" ) );

    auto makePage = [ this, root ]( QListWidget *list, QWidget *preview, const QString &title ) {
        auto *page = new QWidget( tabs );
        auto *layout = new QVBoxLayout( page );
        layout->setContentsMargins( 4, 4, 4, 4 );
        layout->setSpacing( 6 );
        layout->addWidget( list, 1 );
        layout->addWidget( preview, 0 );
        tabs->addTab( page, title );
        Q_UNUSED( root );
        return page;
    };
    makePage( photoList, photoPreview, tr( "Фото" ) );
    makePage( videoList, videoPreview, tr( "Видео" ) );
    root->addWidget( tabs, 1 );

    auto *buttons = new QHBoxLayout;
    openButton = new QPushButton( tr( "Открыть" ), this );
    folderButton = new QPushButton( tr( "Папка" ), this );
    removeButton = new QPushButton( tr( "Удалить" ), this );
    buttons->addWidget( openButton );
    buttons->addWidget( folderButton );
    buttons->addWidget( removeButton );
    root->addLayout( buttons );

    connect( photoList, &QListWidget::currentRowChanged, this, &MediaLibraryWidget::updatePhotoPreview );
    connect( videoList, &QListWidget::currentRowChanged, this, &MediaLibraryWidget::updateVideoPreview );
    connect( openButton, &QPushButton::clicked, this, &MediaLibraryWidget::openSelected );
    connect( folderButton, &QPushButton::clicked, this, &MediaLibraryWidget::showSelectedInFolder );
    connect( removeButton, &QPushButton::clicked, this, &MediaLibraryWidget::deleteSelected );
    refresh();
}

QString MediaLibraryWidget::photosDirectory() {
    return libraryPath( QStandardPaths::PicturesLocation, QStringLiteral( "Фото" ) );
}

QString MediaLibraryWidget::videosDirectory() {
    return libraryPath( QStandardPaths::MoviesLocation, QStringLiteral( "Видео" ) );
}

QString MediaLibraryWidget::newPhotoPath() const {
    const QString stamp = QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_HHmmss_zzz" ) );
    return QDir( photosDirectory() ).filePath( stamp + QStringLiteral( ".png" ) );
}

QString MediaLibraryWidget::newVideoPath() const {
    const QString stamp = QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_HHmmss_zzz" ) );
    return QDir( videosDirectory() ).filePath( stamp + QStringLiteral( ".mp4" ) );
}

void MediaLibraryWidget::refreshList( QListWidget *list, const QString &directory, const QStringList &filters ) {
    const QString selected = list->currentItem() ? list->currentItem()->data( Qt::UserRole ).toString() : QString();
    list->clear();
    const QFileInfoList files = QDir( directory ).entryInfoList( filters, QDir::Files, QDir::Time | QDir::Reversed );
    for ( const QFileInfo &file : files ) {
        auto *item = new QListWidgetItem( file.fileName(), list );
        item->setData( Qt::UserRole, file.absoluteFilePath() );
        item->setToolTip( file.absoluteFilePath() );
        if ( file.absoluteFilePath() == selected )
            list->setCurrentItem( item );
    }
    if ( list->count() && list->currentRow() < 0 )
        list->setCurrentRow( list->count() - 1 );
}

void MediaLibraryWidget::refresh() {
    refreshList( photoList, photosDirectory(), { "*.png", "*.jpg", "*.jpeg", "*.bmp" } );
    refreshList( videoList, videosDirectory(), { "*.mp4", "*.mov", "*.avi", "*.mkv" } );
    updatePhotoPreview();
    updateVideoPreview();
}

QString MediaLibraryWidget::selectedPath() const {
    QListWidget *list = tabs->currentIndex() == 0 ? photoList : videoList;
    return list->currentItem() ? list->currentItem()->data( Qt::UserRole ).toString() : QString();
}

void MediaLibraryWidget::updatePhotoPreview() {
    const QString path = photoList->currentItem() ? photoList->currentItem()->data( Qt::UserRole ).toString() : QString();
    if ( path.isEmpty() ) {
        photoImage = QImage();
        photoPreview->setPixmap( QPixmap() );
        photoPreview->setFixedHeight( 120 );
        photoPreview->setText( tr( "Выберите фотографию" ) );
        return;
    }
    QImageReader reader( path );
    reader.setAutoTransform( true );
    photoImage = reader.read();
    if ( photoImage.isNull() ) {
        photoPreview->setPixmap( QPixmap() );
        photoPreview->setFixedHeight( 120 );
        photoPreview->setText( tr( "Не удалось открыть фотографию" ) );
        return;
    }
    photoPreview->setText( QString() );
    scalePhotoPreview();
}

void MediaLibraryWidget::scalePhotoPreview() {
    if ( photoImage.isNull() || photoPreview->size().isEmpty() )
        return;
    const int availableWidth = photoPreview->contentsRect().width() - 20;
    if ( availableWidth <= 0 )
        return;
    const QSize imageSize = photoImage.size().scaled( QSize( availableWidth, 240 ), Qt::KeepAspectRatio );
    const int previewHeight = qBound( 120, imageSize.height() + 20, 260 );
    if ( photoPreview->height() != previewHeight )
        photoPreview->setFixedHeight( previewHeight );
    photoPreview->setPixmap(
        QPixmap::fromImage( photoImage ).scaled( imageSize, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
}

void MediaLibraryWidget::updateVideoPreview() {
    const QString path = videoList->currentItem() ? videoList->currentItem()->data( Qt::UserRole ).toString() : QString();
    videoPreview->setPixmap( QPixmap() );
    videoPreview->setText( path.isEmpty() ? tr( "Выберите видео" )
                                          : tr( "▶\n%1\n\nНажмите «Открыть» для воспроизведения" )
                                                .arg( QFileInfo( path ).fileName() ) );
    videoPreview->setAlignment( Qt::AlignCenter );
    videoPreview->setWordWrap( true );
}

void MediaLibraryWidget::changeEvent( QEvent *event ) {
    QWidget::changeEvent( event );
    if ( event->type() != QEvent::LanguageChange || tabs == nullptr ) return;
    tabs->setTabText( 0, tr( "Фото" ) );
    tabs->setTabText( 1, tr( "Видео" ) );
    if ( openButton ) openButton->setText( tr( "Открыть" ) );
    if ( folderButton ) folderButton->setText( tr( "Папка" ) );
    if ( removeButton ) removeButton->setText( tr( "Удалить" ) );
    updatePhotoPreview();
    updateVideoPreview();
}

void MediaLibraryWidget::resizeEvent( QResizeEvent *event ) {
    QWidget::resizeEvent( event );
    QTimer::singleShot( 0, this, [ this ] { scalePhotoPreview(); } );
}

void MediaLibraryWidget::openSelected() {
    const QString path = selectedPath();
    if ( !path.isEmpty() )
        QDesktopServices::openUrl( QUrl::fromLocalFile( path ) );
}

void MediaLibraryWidget::showSelectedInFolder() {
    const QString path = selectedPath();
    if ( !path.isEmpty() )
        QDesktopServices::openUrl( QUrl::fromLocalFile( QFileInfo( path ).absolutePath() ) );
}

void MediaLibraryWidget::deleteSelected() {
    const QString path = selectedPath();
    if ( path.isEmpty() )
        return;
    QMessageBox confirmation( QMessageBox::Question, tr( "Удалить файл?" ), QFileInfo( path ).fileName(),
                              QMessageBox::NoButton, this );
    QPushButton *yesButton = confirmation.addButton( tr( "Да" ), QMessageBox::YesRole );
    QPushButton *noButton = confirmation.addButton( tr( "Нет" ), QMessageBox::NoRole );
    confirmation.setDefaultButton( noButton );
    confirmation.setEscapeButton( noButton );
    confirmation.exec();
    if ( confirmation.clickedButton() != yesButton )
        return;
    if ( !QFile::remove( path ) ) {
        QMessageBox::warning( this, tr( "Библиотека" ), tr( "Не удалось удалить файл." ) );
        return;
    }
    refresh();
}
