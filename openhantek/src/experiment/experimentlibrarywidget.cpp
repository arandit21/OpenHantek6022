// SPDX-License-Identifier: GPL-2.0-or-later

#include "experimentlibrarywidget.h"

#include "experimentmanager.h"

#include <QComboBox>
#include <QAction>
#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QFont>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScrollArea>
#include <QSlider>
#include <QDialog>
#include <QMenu>
#include <QMessageBox>
#include <QShortcut>
#include <QKeySequence>
#include <QPoint>
#include <QSize>
#include <QStringList>
#include <QUuid>
#include <QLabel>
#include <QSignalBlocker>
#include <QSet>
#include <QSettings>
#include <QPixmap>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QListWidget>
#include <QListView>
#include <QTimer>
#include <QIcon>
#include <QStyle>
#include <QThreadPool>
#include <QRunnable>
#include <QPointer>
#include <QMetaObject>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QFileDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#include <QProcess>
#endif

namespace {
QString typeLabel( const QString &type ) {
    if ( type == QLatin1String( "experiment" ) ) return QStringLiteral( "Эксперимент" );
    if ( type == QLatin1String( "recording" ) ) return QStringLiteral( "Запись сигнала" );
    if ( type == QLatin1String( "photo" ) ) return QStringLiteral( "Фотография" );
    if ( type == QLatin1String( "video" ) ) return QStringLiteral( "Видео" );
    if ( type == QLatin1String( "imported" ) ) return QStringLiteral( "Импортированные данные" );
    if ( type == QLatin1String( "analysis" ) ) return QStringLiteral( "Результат анализа" );
    if ( type == QLatin1String( "metadata" ) ) return QStringLiteral( "Метаданные" );
    return QStringLiteral( "Файл" );
}

QString formattedSize( qint64 size ) {
    if ( size < 0 ) return QStringLiteral( "—" );
    if ( size < 1024 ) return QStringLiteral( "%1 Б" ).arg( size );
    if ( size < 1024 * 1024 ) return QStringLiteral( "%1 КиБ" ).arg( double( size ) / 1024.0, 0, 'f', 1 );
    if ( size < 1024LL * 1024LL * 1024LL )
        return QStringLiteral( "%1 МиБ" ).arg( double( size ) / ( 1024.0 * 1024.0 ), 0, 'f', 1 );
    return QStringLiteral( "%1 ГиБ" ).arg( double( size ) / ( 1024.0 * 1024.0 * 1024.0 ), 0, 'f', 2 );
}

QStringList splitDelimitedRow( const QString &line, QChar delimiter ) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for ( qsizetype i = 0; i < line.size(); ++i ) {
        const QChar character = line.at( i );
        if ( character == QLatin1Char( '"' ) ) {
            if ( quoted && i + 1 < line.size() && line.at( i + 1 ) == QLatin1Char( '"' ) ) {
                field.append( QLatin1Char( '"' ) );
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if ( character == delimiter && !quoted ) {
            fields.append( field );
            field.clear();
        } else {
            field.append( character );
        }
    }
    fields.append( field );
    return fields;
}

QString jsonValueText( const QJsonValue &value ) {
    if ( value.isObject() )
        return QString::fromUtf8( QJsonDocument( value.toObject() ).toJson( QJsonDocument::Compact ) );
    if ( value.isArray() )
        return QString::fromUtf8( QJsonDocument( value.toArray() ).toJson( QJsonDocument::Compact ) );
    if ( value.isNull() || value.isUndefined() )
        return QString();
    return value.toVariant().toString();
}

QString cleanPath( const QString &path ) {
    return path.isEmpty() ? QString() : QDir::fromNativeSeparators( QDir::cleanPath( path ) );
}

Qt::CaseSensitivity pathCaseSensitivity() {
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool isPathWithin( const QString &root, const QString &path ) {
    const QString normalizedRoot = cleanPath( root );
    const QString normalizedPath = cleanPath( path );
    if ( normalizedRoot.isEmpty() || normalizedPath.isEmpty() )
        return false;
    return normalizedPath == normalizedRoot || normalizedPath.startsWith( normalizedRoot + QLatin1Char( '/' ), pathCaseSensitivity() );
}
} // namespace

ExperimentLibraryWidget::ExperimentLibraryWidget( ExperimentCatalog *catalog, ExperimentManager *manager, QWidget *parent )
    : QWidget( parent ), catalog( catalog ), manager( manager ) {
    setObjectName( "researchLibrary" );
    auto *root = new QVBoxLayout( this );
    root->setContentsMargins( 12, 10, 12, 10 );
    root->setSpacing( 8 );

    auto *toolbar = new QHBoxLayout;
    auto *heading = new QLabel( QStringLiteral( "Библиотека исследований" ), this );
    QFont headingFont = heading->font();
    headingFont.setBold( true );
    headingFont.setPointSize( headingFont.pointSize() + 1 );
    heading->setFont( headingFont );
    heading->setMinimumWidth( 175 );
    heading->setMaximumWidth( 195 );
    searchEdit = new QLineEdit( this );
    searchEdit->setPlaceholderText( QStringLiteral( "Поиск по экспериментам и файлам…" ) );
    searchEdit->setClearButtonEnabled( true );
    sortCombo = new QComboBox( this );
    sortCombo->addItem( QStringLiteral( "Сначала новые" ), QStringLiteral( "recent" ) );
    sortCombo->addItem( QStringLiteral( "Сначала старые" ), QStringLiteral( "oldest" ) );
    sortCombo->setToolTip( QStringLiteral( "Порядок отображения материалов" ) );
    sortCombo->setFixedWidth( 132 );
    auto *viewMode = new QComboBox( this );
    viewMode->addItem( QStringLiteral( "Таблица" ) );
    viewMode->addItem( QStringLiteral( "Плитки" ) );
    viewMode->setToolTip( QStringLiteral( "Режим отображения материалов" ) );
    viewMode->setFixedWidth( 92 );
    experimentFilter = new QComboBox( this );
    experimentFilter->addItem( QStringLiteral( "Все эксперименты" ), QString() );
    experimentFilter->setToolTip( QStringLiteral( "Фильтр по эксперименту" ) );
    experimentFilter->setFixedWidth( 142 );
    auto *newExperimentButton = new QPushButton( QStringLiteral( "Новый эксперимент" ), this );
    newExperimentButton->setFixedWidth( 132 );
    auto *rescanButton = new QPushButton( QStringLiteral( "Пересканировать" ), this );
    rescanButton->setIcon( style()->standardIcon( QStyle::SP_BrowserReload ) );
    rescanButton->setText( QString() );
    rescanButton->setFixedWidth( 32 );
    rescanButton->setToolTip( QStringLiteral( "Обновить список материалов" ) );
    auto *trashButton = new QPushButton( QStringLiteral( "Корзина" ), this );
    trashButton->setIcon( style()->standardIcon( QStyle::SP_TrashIcon ) );
    trashButton->setText( QString() );
    trashButton->setFixedWidth( 32 );
    trashButton->setToolTip( QStringLiteral( "Восстановить материал из корзины" ) );
    auto *emptyTrashButton = new QPushButton( QStringLiteral( "Очистить корзину" ), this );
    emptyTrashButton->setIcon( style()->standardIcon( QStyle::SP_DialogDiscardButton ) );
    emptyTrashButton->setText( QString() );
    emptyTrashButton->setFixedWidth( 32 );
    emptyTrashButton->setToolTip( QStringLiteral( "Очистить корзину" ) );
    toolbar->addWidget( heading );
    toolbar->addSpacing( 10 );
    toolbar->addWidget( searchEdit, 1 );
    toolbar->addWidget( sortCombo );
    toolbar->addWidget( viewMode );
    toolbar->addWidget( experimentFilter );
    toolbar->addWidget( newExperimentButton );
    toolbar->addWidget( rescanButton );
    toolbar->addWidget( trashButton );
    toolbar->addWidget( emptyTrashButton );
    root->addLayout( toolbar );

    auto *splitter = new QSplitter( Qt::Horizontal, this );
    navigation = new QTreeWidget( splitter );
    navigation->setHeaderHidden( true );
    navigation->setMinimumWidth( 180 );
    auto addCategory = [ this ]( const QString &label, const QString &category ) {
        auto *item = new QTreeWidgetItem( navigation, { label } );
        item->setData( 0, Qt::UserRole, category );
        return item;
    };
    addCategory( QStringLiteral( "Все материалы" ), QStringLiteral( "all" ) );
    addCategory( QStringLiteral( "Эксперименты" ), QStringLiteral( "projects" ) );
    addCategory( QStringLiteral( "Активный эксперимент" ), QStringLiteral( "active" ) );
    addCategory( QStringLiteral( "Записи сигналов" ), QStringLiteral( "recordings" ) );
    addCategory( QStringLiteral( "Фотографии" ), QStringLiteral( "photos" ) );
    addCategory( QStringLiteral( "Видео" ), QStringLiteral( "videos" ) );
    addCategory( QStringLiteral( "Импортированные данные" ), QStringLiteral( "imports" ) );
    addCategory( QStringLiteral( "Результаты анализа" ), QStringLiteral( "analysis" ) );
    navigation->setCurrentItem( navigation->topLevelItem( 0 ) );

    libraryViews = new QStackedWidget( splitter );
    table = new QTableWidget( libraryViews );
    table->setColumnCount( 5 );
    table->setHorizontalHeaderLabels( { QStringLiteral( "Название" ), QStringLiteral( "Тип" ),
                                        QStringLiteral( "Эксперимент" ), QStringLiteral( "Изменено" ),
                                        QStringLiteral( "Размер" ) } );
    table->setSelectionBehavior( QAbstractItemView::SelectRows );
    table->setSelectionMode( QAbstractItemView::ExtendedSelection );
    table->setEditTriggers( QAbstractItemView::NoEditTriggers );
    table->setContextMenuPolicy( Qt::CustomContextMenu );
    table->setStyleSheet( QStringLiteral(
        "QTableWidget::item:selected { background: #2563eb; color: white; }"
        "QTableWidget::item:selected:!active { background: #475569; color: white; }"
        "QTableWidget::item { padding: 4px; }" ) );
    table->setSortingEnabled( false );
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection( true );
    table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::Stretch );
    table->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );

    tiles = new QListWidget( libraryViews );
    tiles->setObjectName( "libraryTiles" );
    tiles->setViewMode( QListView::IconMode );
    tiles->setFlow( QListView::LeftToRight );
    tiles->setWrapping( true );
    tiles->setResizeMode( QListView::Adjust );
    tiles->setMovement( QListView::Static );
    tiles->setUniformItemSizes( true );
    tiles->setWordWrap( true );
    tiles->setGridSize( QSize( 176, 128 ) );
    tiles->setIconSize( QSize( 112, 72 ) );
    tiles->setSelectionMode( QAbstractItemView::ExtendedSelection );
    tiles->setContextMenuPolicy( Qt::CustomContextMenu );
    tiles->setStyleSheet( QStringLiteral(
        "QListWidget::item { border-radius: 6px; padding: 6px; }"
        "QListWidget::item:selected { background: #2563eb; color: white; }"
        "QListWidget::item:selected:!active { background: #475569; color: white; }" ) );
    libraryViews->addWidget( table );
    libraryViews->addWidget( tiles );

    auto *details = new QWidget( splitter );
    details->setMinimumWidth( 220 );
    auto *detailsLayout = new QVBoxLayout( details );
    detailsTitle = new QLabel( QStringLiteral( "Выберите материал" ), details );
    detailsTitle->setWordWrap( true );
    QFont titleFont = detailsTitle->font();
    titleFont.setBold( true );
    detailsTitle->setFont( titleFont );
    previewImage = new QLabel( details );
    previewImage->setObjectName( "libraryPreview" );
    previewImage->setAlignment( Qt::AlignCenter );
    previewImage->setMinimumHeight( 90 );
    previewImage->setMaximumHeight( 220 );
    previewImage->setStyleSheet( QStringLiteral( "background: palette(base); border: 1px solid palette(mid);" ) );
    previewImage->hide();
    dataPreview = new QTableWidget( details );
    dataPreview->setObjectName( "libraryDataPreview" );
    dataPreview->setEditTriggers( QAbstractItemView::NoEditTriggers );
    dataPreview->setSelectionMode( QAbstractItemView::NoSelection );
    dataPreview->setMaximumHeight( 220 );
    dataPreview->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
    dataPreview->verticalHeader()->hide();
    dataPreview->hide();
    detailsText = new QLabel( QStringLiteral( "Подробности эксперимента или файла появятся здесь." ), details );
    detailsText->setWordWrap( true );
    detailsText->setTextInteractionFlags( Qt::TextSelectableByMouse );
    openButton = new QPushButton( QStringLiteral( "Открыть" ), details );
    openButton->setEnabled( false );
    showFolderButton = new QPushButton( QStringLiteral( "Показать в папке" ), details );
    showFolderButton->setEnabled( false );
    detailsLayout->addWidget( detailsTitle );
    detailsLayout->addWidget( previewImage );
    detailsLayout->addWidget( dataPreview );
    detailsLayout->addWidget( detailsText );
    detailsLayout->addStretch( 1 );
    detailsLayout->addWidget( openButton );
    detailsLayout->addWidget( showFolderButton );
    splitter->addWidget( navigation );
    splitter->addWidget( libraryViews );
    splitter->addWidget( details );
    splitter->setStretchFactor( 1, 1 );
    splitter->setSizes( { 190, 760, 250 } );
    QSettings libraryLayoutSettings;
    const QByteArray savedSplitterState = libraryLayoutSettings.value( QStringLiteral( "laboratory/library/splitter" ) ).toByteArray();
    if ( !savedSplitterState.isEmpty() )
        splitter->restoreState( savedSplitterState );
    connect( splitter, &QSplitter::splitterMoved, this, [ splitter ] {
        QSettings settings;
        settings.setValue( QStringLiteral( "laboratory/library/splitter" ), splitter->saveState() );
    } );
    root->addWidget( splitter, 1 );

    indexStatus = new QLabel( this );
    indexStatus->setObjectName( "libraryStatus" );
    indexStatus->setWordWrap( true );
    root->addWidget( indexStatus );

    connect( navigation, &QTreeWidget::currentItemChanged, this, [ this ]() { refreshRecords(); } );
    connect( searchEdit, &QLineEdit::textChanged, this, [ this ]() { refreshRecords(); } );
    connect( sortCombo, &QComboBox::currentIndexChanged, this, [ this ]() { refreshRecords(); } );
    connect( viewMode, &QComboBox::currentIndexChanged, libraryViews, &QStackedWidget::setCurrentIndex );
    connect( viewMode, &QComboBox::currentIndexChanged, this, [ this ]() { refreshRecords(); } );
    connect( experimentFilter, &QComboBox::currentIndexChanged, this, [ this ]() { refreshRecords(); } );
    connect( table, &QTableWidget::itemSelectionChanged, this, &ExperimentLibraryWidget::updateDetails );
    connect( tiles, &QListWidget::itemSelectionChanged, this, &ExperimentLibraryWidget::updateDetails );
    connect( table, &QTableWidget::customContextMenuRequested, this, &ExperimentLibraryWidget::showContextMenu );
    connect( tiles, &QListWidget::customContextMenuRequested, this, &ExperimentLibraryWidget::showContextMenu );
    connect( tiles, &QListWidget::itemDoubleClicked, this, [ this ]() {
        if ( hasSelection && selectedItem.type == QLatin1String( "photo" ) ) openImageViewer( selectedItem.absolutePath );
        else openSelected();
    } );
    connect( table, &QTableWidget::itemDoubleClicked, this, [ this ]() {
        if ( hasSelection && selectedItem.type == QLatin1String( "photo" ) )
            openImageViewer( selectedItem.absolutePath );
        else
            openSelected();
    } );
    connect( openButton, &QPushButton::clicked, this, &ExperimentLibraryWidget::openSelected );
    connect( showFolderButton, &QPushButton::clicked, this, &ExperimentLibraryWidget::showSelectedInFolder );
    connect( rescanButton, &QPushButton::clicked, this, &ExperimentLibraryWidget::startIndexRefresh );
    connect( trashButton, &QPushButton::clicked, this, &ExperimentLibraryWidget::restoreFromTrash );
    connect( emptyTrashButton, &QPushButton::clicked, this, &ExperimentLibraryWidget::emptyTrash );
    auto *deleteShortcut = new QShortcut( QKeySequence::Delete, table );
    connect( deleteShortcut, &QShortcut::activated, this, &ExperimentLibraryWidget::deleteSelected );
    auto *tileDeleteShortcut = new QShortcut( QKeySequence::Delete, tiles );
    connect( tileDeleteShortcut, &QShortcut::activated, this, &ExperimentLibraryWidget::deleteSelected );
    auto *selectAllShortcut = new QShortcut( QKeySequence::SelectAll, table );
    connect( selectAllShortcut, &QShortcut::activated, table, &QTableWidget::selectAll );
    auto *tileSelectAllShortcut = new QShortcut( QKeySequence::SelectAll, tiles );
    connect( tileSelectAllShortcut, &QShortcut::activated, tiles, &QListWidget::selectAll );
    auto *openShortcut = new QShortcut( QKeySequence( Qt::Key_Return ), table );
    connect( openShortcut, &QShortcut::activated, this, &ExperimentLibraryWidget::openSelected );
    auto *tileOpenShortcut = new QShortcut( QKeySequence( Qt::Key_Return ), tiles );
    connect( tileOpenShortcut, &QShortcut::activated, this, &ExperimentLibraryWidget::openSelected );
    thumbnailTimer = new QTimer( this );
    thumbnailTimer->setInterval( 1 );
    connect( thumbnailTimer, &QTimer::timeout, this, &ExperimentLibraryWidget::loadNextThumbnails );
    connect( newExperimentButton, &QPushButton::clicked, this, [ this ]() {
        bool accepted = false;
        const QString name = QInputDialog::getText( this, QStringLiteral( "Новый эксперимент" ),
                                                     QStringLiteral( "Название исследования:" ), QLineEdit::Normal,
                                                     QString(), &accepted );
        if ( !accepted ) return;
        bool descriptionAccepted = false;
        const QString description = QInputDialog::getMultiLineText( this, QStringLiteral( "Описание эксперимента" ),
            QStringLiteral( "Задача, условия или комментарий (необязательно):" ), QString(), &descriptionAccepted );
        QString error;
        if ( !this->manager->createExperiment( name, descriptionAccepted ? description : QString(), &error ) ) {
            QMessageBox::critical( this, QStringLiteral( "Новый эксперимент" ), error );
            return;
        }
        emit activeExperimentChanged( this->manager->activeExperiment() );
        startIndexRefresh();
    } );
    connect( catalog, &ExperimentCatalog::scanProgress, this, [ this ]( int done, int total ) {
        setIndexStatus( QStringLiteral( "Обновление картотеки: просмотрено экспериментов %1 из %2…" ).arg( done ).arg( total ) );
    } );
    connect( catalog, &ExperimentCatalog::scanFinished, this, [ this ]( int files, int experiments ) {
        setIndexStatus( QStringLiteral( "Картотека обновлена: %1 экспериментов, %2 файлов. Индекс: %3" )
                            .arg( experiments ).arg( files ).arg( this->catalog->databasePath() ) );
        refreshRecords();
    } );
    connect( catalog, &ExperimentCatalog::scanFailed, this, [ this ]( const QString &message ) {
        setIndexStatus( QStringLiteral( "Ошибка обновления картотеки: %1" ).arg( message ), true );
    } );

    if ( catalog->isReady() )
        setIndexStatus( QStringLiteral( "Постоянная картотека SQLite: %1" ).arg( catalog->databasePath() ) );
    else
        setIndexStatus( catalog->errorString(), true );
    refreshRecords();
}

QString ExperimentLibraryWidget::selectedCategory() const {
    const QTreeWidgetItem *item = navigation ? navigation->currentItem() : nullptr;
    return item ? item->data( 0, Qt::UserRole ).toString() : QStringLiteral( "all" );
}

void ExperimentLibraryWidget::refreshRecords() {
    QStringList selectedIds = rememberedSelectionIds;
    const QVector< ExperimentCatalogItem > currentSelection = selectedRecords();
    if ( !currentSelection.isEmpty() ) {
        selectedIds.clear();
        for ( const ExperimentCatalogItem &item : currentSelection ) selectedIds.append( item.id );
    }
    if ( !catalog || !catalog->isReady() ) {
        hasSelection = false;
        rememberedSelectionIds.clear();
        table->setRowCount( 0 );
        tiles->clear();
        return;
    }
    QString error;
    rows = catalog->query( selectedCategory(), searchEdit->text(), manager->activeExperiment(), &error );
    if ( !error.isEmpty() )
        setIndexStatus( QStringLiteral( "Ошибка чтения картотеки: %1" ).arg( error ), true );
    if ( sortCombo->currentData().toString() == QLatin1String( "oldest" ) )
        std::reverse( rows.begin(), rows.end() );

    const QString selectedExperimentId = experimentFilter->currentData().toString();
    const QString selectedExperimentLabel = experimentFilter->currentText();
    const QVector< ExperimentCatalogItem > projects = catalog->query( QStringLiteral( "projects" ), QString(), QString(), nullptr );
    bool filtersMatch = experimentFilter->count() == projects.size() + 1;
    if ( filtersMatch ) {
        for ( int i = 0; i < projects.size(); ++i )
            if ( experimentFilter->itemData( i + 1 ).toString() != projects.at( i ).id ) { filtersMatch = false; break; }
    }
    if ( !filtersMatch ) {
        const QSignalBlocker filterBlocker( experimentFilter );
        experimentFilter->clear();
        experimentFilter->addItem( QStringLiteral( "Все эксперименты" ), QString() );
        for ( const ExperimentCatalogItem &project : projects )
            experimentFilter->addItem( project.name, project.id );
        const int restoreIndex = experimentFilter->findData( selectedExperimentId );
        if ( restoreIndex >= 0 )
            experimentFilter->setCurrentIndex( restoreIndex );
        else if ( catalog->isScanning() && !selectedExperimentId.isEmpty() ) {
            experimentFilter->addItem( selectedExperimentLabel, selectedExperimentId );
            experimentFilter->setCurrentIndex( experimentFilter->count() - 1 );
        } else
            experimentFilter->setCurrentIndex( 0 );
    }
    const QString activeFilterId = experimentFilter->currentData().toString();
    if ( !activeFilterId.isEmpty() ) {
        rows.erase( std::remove_if( rows.begin(), rows.end(), [ &activeFilterId ]( const ExperimentCatalogItem &item ) {
            return item.experimentId != activeFilterId;
        } ), rows.end() );
    }

    const QSignalBlocker selectionBlocker( table->selectionModel() );
    const QSignalBlocker tileSelectionBlocker( tiles->selectionModel() );
    ++thumbnailGeneration;
    thumbnailLoading = false;
    thumbnailTimer->stop();
    table->setRowCount( rows.size() );
    tiles->clear();
    thumbnailIndex = 0;
    for ( int row = 0; row < rows.size(); ++row ) {
        const ExperimentCatalogItem &item = rows.at( row );
        auto *nameCell = new QTableWidgetItem( item.name );
        nameCell->setData( Qt::UserRole, item.id );
        table->setItem( row, 0, nameCell );
        table->setItem( row, 1, new QTableWidgetItem( typeLabel( item.type ) ) );
        table->setItem( row, 2, new QTableWidgetItem( item.experimentName ) );
        table->setItem( row, 3, new QTableWidgetItem( item.modifiedAt ) );
        table->setItem( row, 4, new QTableWidgetItem( item.type == QLatin1String( "experiment" )
                                                         ? QStringLiteral( "—" ) : formattedSize( item.size ) ) );
        if ( selectedIds.contains( item.id ) )
            table->selectRow( row );
        auto *tile = new QListWidgetItem( typeLabel( item.type ) + QLatin1Char( '\n' ) + item.name, tiles );
        tile->setData( Qt::UserRole, item.id );
        tile->setToolTip( QStringLiteral( "%1\n%2\n%3" ).arg( item.name, item.absolutePath,
            item.type == QLatin1String( "experiment" ) ? item.experimentName : formattedSize( item.size ) ) );
        if ( selectedIds.contains( item.id ) ) tile->setSelected( true );
    }
    if ( selectedIds.isEmpty() && !rows.isEmpty() ) {
        table->selectRow( 0 );
        tiles->item( 0 )->setSelected( true );
    }
    if ( thumbnailCache.size() > 500 ) thumbnailCache.clear();
    if ( !rows.isEmpty() ) thumbnailTimer->start();
    updateDetails();
    if ( rows.isEmpty() ) {
        detailsTitle->setText( QStringLiteral( "Пока пусто" ) );
        detailsText->setText( catalog->isScanning() ? QStringLiteral( "Индекс строится постепенно; материалы появятся по мере сканирования." )
                                                    : QStringLiteral( "В выбранном разделе пока нет проиндексированных материалов." ) );
        openButton->setEnabled( false );
    }
}

QVector< ExperimentCatalogItem > ExperimentLibraryWidget::selectedRecords() const {
    QVector< ExperimentCatalogItem > selected;
    if ( !table || !tiles || !libraryViews )
        return selected;
    QSet< QString > ids;
    if ( libraryViews->currentWidget() == table ) {
        for ( QTableWidgetItem *cell : table->selectedItems() ) {
            if ( cell->column() != 0 ) continue;
            const QString id = cell->data( Qt::UserRole ).toString();
            if ( ids.contains( id ) ) continue;
            ids.insert( id );
            for ( const ExperimentCatalogItem &item : rows )
                if ( item.id == id ) { selected.append( item ); break; }
        }
    } else {
        for ( QListWidgetItem *tile : tiles->selectedItems() ) {
            const QString id = tile->data( Qt::UserRole ).toString();
            if ( ids.contains( id ) ) continue;
            ids.insert( id );
            for ( const ExperimentCatalogItem &item : rows )
                if ( item.id == id ) { selected.append( item ); break; }
        }
    }
    return selected;
}

void ExperimentLibraryWidget::updateDetails() {
    const QVector< ExperimentCatalogItem > selected = selectedRecords();
    if ( selected.isEmpty() ) {
        rememberedSelectionIds.clear();
        hasSelection = false;
        openButton->setEnabled( false );
        showFolderButton->setEnabled( false );
        previewImage->clear();
        previewImage->hide();
        dataPreview->clear();
        dataPreview->hide();
        detailsTitle->setText( QStringLiteral( "Выберите материал" ) );
        detailsText->setText( QStringLiteral( "Подробности эксперимента или файла появятся здесь." ) );
        return;
    }
    selectedItem = selected.first();
    rememberedSelectionIds.clear();
    for ( const ExperimentCatalogItem &item : selected ) rememberedSelectionIds.append( item.id );
    hasSelection = true;
    showFolderButton->setEnabled( true );
    detailsTitle->setText( selected.size() > 1
                               ? QStringLiteral( "Выбрано материалов: %1" ).arg( selected.size() )
                               : selectedItem.name );
    QString details = QStringLiteral( "Тип: %1\nЭксперимент: %2\nСоздан: %3\nПуть: %4" )
                          .arg( typeLabel( selectedItem.type ), selectedItem.experimentName,
                                selectedItem.createdAt.isEmpty() ? QStringLiteral( "—" ) : selectedItem.createdAt,
                                selectedItem.absolutePath );
    if ( selectedItem.type != QLatin1String( "experiment" ) )
        details += QStringLiteral( "\nРазмер: %1\nИзменён: %2" ).arg( formattedSize( selectedItem.size ), selectedItem.modifiedAt );
    if ( !selectedItem.description.isEmpty() )
        details += QStringLiteral( "\n\nОписание:\n%1" ).arg( selectedItem.description );
    if ( selected.size() > 1 )
        details += QStringLiteral( "\n\nМожно переместить выбранные материалы в корзину." );
    detailsText->setText( details );
    openButton->setEnabled( selectedItem.type == QLatin1String( "experiment" ) ||
                            selectedItem.type == QLatin1String( "recording" ) ||
                            selectedItem.type == QLatin1String( "photo" ) ||
                            ( selectedItem.type == QLatin1String( "video" ) && QFileInfo::exists( selectedItem.absolutePath ) ) );
    openButton->setText( selectedItem.type == QLatin1String( "experiment" ) ? QStringLiteral( "Сделать активным" )
                         : selectedItem.type == QLatin1String( "recording" ) ? QStringLiteral( "Открыть запись" )
                         : selectedItem.type == QLatin1String( "photo" ) ? QStringLiteral( "Просмотреть" )
                         : selectedItem.type == QLatin1String( "video" ) ? QStringLiteral( "Воспроизвести видео" )
                         : QStringLiteral( "Недоступно" ) );
    previewSelected();
}

void ExperimentLibraryWidget::openSelected() {
    if ( !hasSelection ) return;
    QString error;
    if ( selectedItem.type == QLatin1String( "experiment" ) ) {
        if ( !manager->activateExperiment( selectedItem.absolutePath, &error ) ) {
            QMessageBox::critical( this, QStringLiteral( "Библиотека исследований" ), error );
            return;
        }
        emit activeExperimentChanged( selectedItem.absolutePath );
        setIndexStatus( QStringLiteral( "Активный эксперимент: %1" ).arg( selectedItem.name ) );
        refreshRecords();
    } else if ( selectedItem.type == QLatin1String( "recording" ) ) {
        emit openRecordingRequested( selectedItem.absolutePath );
    } else if ( selectedItem.type == QLatin1String( "photo" ) ) {
        openImageViewer( selectedItem.absolutePath );
    } else if ( selectedItem.type == QLatin1String( "video" ) ) {
        openVideoPlayer( selectedItem.absolutePath );
    }
}

void ExperimentLibraryWidget::openVideoPlayer( const QString &path ) {
    if ( !QFileInfo( path ).isFile() ) {
        QMessageBox::warning( this, QStringLiteral( "Видеоплеер" ),
                              QStringLiteral( "Видеофайл не найден:\n%1" ).arg( path ) );
        return;
    }

    auto *dialog = new QDialog( this );
    dialog->setAttribute( Qt::WA_DeleteOnClose );
    dialog->setWindowTitle( QStringLiteral( "Видеоплеер — %1" ).arg( QFileInfo( path ).fileName() ) );
    dialog->resize( 960, 640 );
    auto *layout = new QVBoxLayout( dialog );
    auto *video = new QVideoWidget( dialog );
    video->setMinimumSize( 480, 270 );
    video->setStyleSheet( QStringLiteral( "background-color: #080b10;" ) );
    layout->addWidget( video, 1 );

    auto *status = new QLabel( QStringLiteral( "Загрузка видео…" ), dialog );
    status->setWordWrap( true );
    layout->addWidget( status );

    auto *player = new QMediaPlayer( dialog );
    auto *audio = new QAudioOutput( dialog );
    player->setAudioOutput( audio );
    player->setVideoOutput( video );
    player->setSource( QUrl::fromLocalFile( path ) );

    auto *transport = new QHBoxLayout;
    auto *playPause = new QPushButton( QStringLiteral( "Пауза" ), dialog );
    auto *stop = new QPushButton( QStringLiteral( "Стоп" ), dialog );
    auto *position = new QSlider( Qt::Horizontal, dialog );
    position->setRange( 0, 10000 );
    position->setEnabled( false );
    auto *time = new QLabel( QStringLiteral( "00:00 / 00:00" ), dialog );
    time->setMinimumWidth( 112 );
    auto *volumeLabel = new QLabel( QStringLiteral( "Громкость" ), dialog );
    auto *volume = new QSlider( Qt::Horizontal, dialog );
    volume->setRange( 0, 100 );
    volume->setValue( 70 );
    volume->setMaximumWidth( 110 );
    auto *speed = new QComboBox( dialog );
    speed->addItem( QStringLiteral( "0,5×" ), 0.5 );
    speed->addItem( QStringLiteral( "1×" ), 1.0 );
    speed->addItem( QStringLiteral( "1,5×" ), 1.5 );
    speed->addItem( QStringLiteral( "2×" ), 2.0 );
    speed->setCurrentIndex( 1 );
    auto *fullscreen = new QPushButton( QStringLiteral( "Во весь экран" ), dialog );
    transport->addWidget( playPause );
    transport->addWidget( stop );
    transport->addWidget( position, 1 );
    transport->addWidget( time );
    transport->addWidget( volumeLabel );
    transport->addWidget( volume );
    transport->addWidget( speed );
    transport->addWidget( fullscreen );
    layout->addLayout( transport );

    const auto formatTime = []( qint64 milliseconds ) {
        const qint64 seconds = qMax< qint64 >( 0, milliseconds ) / 1000;
        return seconds >= 3600
            ? QStringLiteral( "%1:%2:%3" ).arg( seconds / 3600, 2, 10, QLatin1Char( '0' ) )
                  .arg( ( seconds / 60 ) % 60, 2, 10, QLatin1Char( '0' ) ).arg( seconds % 60, 2, 10, QLatin1Char( '0' ) )
            : QStringLiteral( "%1:%2" ).arg( seconds / 60, 2, 10, QLatin1Char( '0' ) )
                  .arg( seconds % 60, 2, 10, QLatin1Char( '0' ) );
    };
    connect( playPause, &QPushButton::clicked, player, [ player ] {
        if ( player->playbackState() == QMediaPlayer::PlayingState ) player->pause();
        else player->play();
    } );
    connect( stop, &QPushButton::clicked, player, &QMediaPlayer::stop );
    connect( player, &QMediaPlayer::playbackStateChanged, playPause, [ playPause ]( QMediaPlayer::PlaybackState state ) {
        playPause->setText( state == QMediaPlayer::PlayingState ? QStringLiteral( "Пауза" ) : QStringLiteral( "Воспроизвести" ) );
    } );
    connect( player, &QMediaPlayer::durationChanged, dialog, [ position, time, player, formatTime ]( qint64 duration ) {
        position->setEnabled( duration > 0 );
        time->setText( QStringLiteral( "%1 / %2" ).arg( formatTime( player->position() ), formatTime( duration ) ) );
    } );
    connect( player, &QMediaPlayer::positionChanged, dialog, [ position, time, player, formatTime ]( qint64 current ) {
        const qint64 duration = player->duration();
        if ( duration > 0 && !position->isSliderDown() ) {
            const QSignalBlocker blocker( position );
            position->setValue( int( ( double( current ) / double( duration ) ) * 10000.0 ) );
        }
        time->setText( QStringLiteral( "%1 / %2" ).arg( formatTime( current ), formatTime( duration ) ) );
    } );
    connect( position, &QSlider::sliderMoved, player, [ player ]( int value ) {
        if ( player->duration() > 0 ) player->setPosition( qint64( value ) * player->duration() / 10000 );
    } );
    connect( volume, &QSlider::valueChanged, audio, [ audio ]( int value ) { audio->setVolume( value / 100.0 ); } );
    audio->setVolume( 0.7 );
    connect( speed, &QComboBox::currentIndexChanged, player, [ speed, player ]( int index ) {
        player->setPlaybackRate( speed->itemData( index ).toDouble() );
    } );
    connect( fullscreen, &QPushButton::clicked, dialog, [ dialog, fullscreen ] {
        if ( dialog->isFullScreen() ) {
            dialog->showNormal();
            fullscreen->setText( QStringLiteral( "Во весь экран" ) );
        } else {
            dialog->showFullScreen();
            fullscreen->setText( QStringLiteral( "Оконный режим" ) );
        }
    } );
    connect( player, &QMediaPlayer::mediaStatusChanged, status, [ status ]( QMediaPlayer::MediaStatus mediaStatus ) {
        if ( mediaStatus == QMediaPlayer::LoadedMedia || mediaStatus == QMediaPlayer::BufferedMedia )
            status->setText( QStringLiteral( "Видео загружено" ) );
        else if ( mediaStatus == QMediaPlayer::EndOfMedia )
            status->setText( QStringLiteral( "Воспроизведение завершено" ) );
        else if ( mediaStatus == QMediaPlayer::InvalidMedia )
            status->setText( QStringLiteral( "Не удалось открыть видео. Формат или кодек не поддерживается." ) );
    } );
    connect( player, &QMediaPlayer::errorOccurred, status,
             [ status ]( QMediaPlayer::Error, const QString &message ) {
        status->setText( QStringLiteral( "Ошибка воспроизведения: %1" ).arg( message ) );
    } );
    connect( dialog, &QDialog::finished, player, &QMediaPlayer::stop );
    dialog->show();
    player->play();
}

void ExperimentLibraryWidget::showSelectedInFolder() {
    if ( !hasSelection ) return;
#ifdef Q_OS_WIN
    QProcess::startDetached( QStringLiteral( "explorer.exe" ),
                             { QStringLiteral( "/select,%1" ).arg( QDir::toNativeSeparators( selectedItem.absolutePath ) ) } );
#else
    const QString folder = selectedItem.type == QLatin1String( "experiment" )
                               ? selectedItem.absolutePath : QFileInfo( selectedItem.absolutePath ).absolutePath();
    QDesktopServices::openUrl( QUrl::fromLocalFile( folder ) );
#endif
}

void ExperimentLibraryWidget::startIndexRefresh() {
    if ( !catalog->isReady() ) {
        setIndexStatus( catalog->errorString(), true );
        return;
    }
    setIndexStatus( QStringLiteral( "Запускается сканирование файлов экспериментов…" ) );
    catalog->startRescan();
}

void ExperimentLibraryWidget::setIndexStatus( const QString &text, bool error ) {
    indexStatus->setText( text );
    indexStatus->setStyleSheet( error ? QStringLiteral( "color: #ff8b80;" ) : QString() );
}

void ExperimentLibraryWidget::previewSelected() {
    previewImage->clear();
    previewImage->hide();
    dataPreview->clear();
    dataPreview->hide();
    if ( selectedItem.type == QLatin1String( "imported" ) ||
         selectedItem.type == QLatin1String( "metadata" ) || selectedItem.type == QLatin1String( "analysis" ) ) {
        const QString extension = QFileInfo( selectedItem.absolutePath ).suffix().toLower();
        if ( extension == QLatin1String( "csv" ) || extension == QLatin1String( "tsv" ) ) {
            QFile file( selectedItem.absolutePath );
            if ( !file.open( QIODevice::ReadOnly ) ) {
                detailsText->setText( detailsText->text() + QStringLiteral( "\nПредпросмотр таблицы недоступен: файл не открывается." ) );
                return;
            }
            constexpr qint64 maxPreviewBytes = 1024 * 1024;
            QByteArray bytes = file.read( maxPreviewBytes + 1 );
            const bool truncated = bytes.size() > maxPreviewBytes;
            if ( truncated ) bytes.truncate( maxPreviewBytes );
            if ( bytes.startsWith( "\xEF\xBB\xBF" ) ) bytes.remove( 0, 3 );
            const QString text = QString::fromUtf8( bytes );
            QStringList lines = text.split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
            const QChar delimiter = extension == QLatin1String( "tsv" ) ? QLatin1Char( '\t' ) : QLatin1Char( ',' );
            QVector< QStringList > records;
            for ( const QString &line : lines ) {
                if ( records.size() >= 21 ) break;
                records.append( splitDelimitedRow( line.trimmed(), delimiter ) );
            }
            if ( records.isEmpty() ) {
                detailsText->setText( detailsText->text() + QStringLiteral( "\nФайл таблицы пуст." ) );
                return;
            }
            QStringList headers = records.takeFirst();
            if ( headers.size() > 8 ) headers = headers.mid( 0, 8 );
            while ( headers.size() < 8 && std::any_of( records.cbegin(), records.cend(), [ &headers ]( const QStringList &row ) {
                return row.size() > headers.size();
            } ) )
                headers.append( QStringLiteral( "Столбец %1" ).arg( headers.size() + 1 ) );
            const int columns = qMax( 1, qMin( 8, headers.size() ) );
            dataPreview->setColumnCount( columns );
            dataPreview->setHorizontalHeaderLabels( headers.mid( 0, columns ) );
            const int visibleRows = qMin( 20, records.size() );
            dataPreview->setRowCount( visibleRows );
            for ( int row = 0; row < visibleRows; ++row )
                for ( int column = 0; column < columns && column < records.at( row ).size(); ++column )
                    dataPreview->setItem( row, column, new QTableWidgetItem( records.at( row ).at( column ) ) );
            dataPreview->show();
            detailsText->setText( detailsText->text() + QStringLiteral( "\nТабличный просмотр: первые %1 строк, до 8 столбцов%2." )
                .arg( visibleRows ).arg( truncated ? QStringLiteral( " (входной фрагмент ограничен 1 МиБ)" ) : QString() ) );
            return;
        }
        if ( extension == QLatin1String( "json" ) ) {
            QFile file( selectedItem.absolutePath );
            if ( !file.open( QIODevice::ReadOnly ) ) {
                detailsText->setText( detailsText->text() + QStringLiteral( "\nJSON-предпросмотр недоступен: файл не открывается." ) );
                return;
            }
            constexpr qint64 maxPreviewBytes = 1024 * 1024;
            QByteArray bytes = file.read( maxPreviewBytes + 1 );
            const bool truncated = bytes.size() > maxPreviewBytes;
            if ( truncated ) bytes.truncate( maxPreviewBytes );
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson( bytes, &parseError );
            if ( parseError.error != QJsonParseError::NoError || truncated ) {
                detailsText->setText( detailsText->text() + QStringLiteral( "\nJSON-предпросмотр не построен: JSON повреждён или больше лимита 1 МиБ." ) );
                return;
            }
            QStringList headers;
            QVector< QStringList > values;
            if ( document.isObject() ) {
                const QJsonObject object = document.object();
                const QJsonValue dataValue = object.value( QStringLiteral( "data" ) );
                if ( dataValue.isArray() && !dataValue.toArray().isEmpty() && dataValue.toArray().first().isObject() ) {
                    const QJsonObject first = dataValue.toArray().first().toObject();
                    headers = first.keys();
                    for ( const QJsonValue &value : dataValue.toArray() ) {
                        if ( values.size() >= 20 || !value.isObject() ) break;
                        QStringList row;
                        const QJsonObject objectRow = value.toObject();
                        for ( const QString &key : headers )
                            row.append( jsonValueText( objectRow.value( key ) ) );
                        values.append( row );
                    }
                } else {
                    headers = { QStringLiteral( "Поле" ), QStringLiteral( "Значение" ) };
                    for ( auto it = object.begin(); it != object.end() && values.size() < 20; ++it ) {
                        values.append( { it.key(), jsonValueText( it.value() ) } );
                    }
                }
            } else if ( document.isArray() ) {
                headers = { QStringLiteral( "Значение" ) };
                for ( const QJsonValue &value : document.array() ) {
                    if ( values.size() >= 20 ) break;
                    values.append( { jsonValueText( value ) } );
                }
            }
            if ( headers.isEmpty() ) headers = { QStringLiteral( "Поле" ), QStringLiteral( "Значение" ) };
            const int columns = qMax( 1, qMin( 8, headers.size() ) );
            dataPreview->setColumnCount( columns );
            dataPreview->setHorizontalHeaderLabels( headers.mid( 0, columns ) );
            dataPreview->setRowCount( values.size() );
            for ( int row = 0; row < values.size(); ++row )
                for ( int column = 0; column < columns && column < values.at( row ).size(); ++column )
                    dataPreview->setItem( row, column, new QTableWidgetItem( values.at( row ).at( column ) ) );
            dataPreview->show();
            detailsText->setText( detailsText->text() + QStringLiteral( "\nJSON-предпросмотр: первые %1 строк." ).arg( values.size() ) );
            return;
        }
    }
    if ( selectedItem.type != QLatin1String( "photo" ) ) return;
    QImageReader reader( selectedItem.absolutePath );
    reader.setAutoTransform( true );
    const QSize sourceSize = reader.size();
    if ( sourceSize.isValid() )
        reader.setScaledSize( sourceSize.scaled( QSize( 480, 220 ), Qt::KeepAspectRatio ) );
    const QImage image = reader.read();
    if ( image.isNull() ) {
        previewImage->setText( QStringLiteral( "Не удалось прочитать изображение" ) );
        previewImage->show();
        return;
    }
    previewImage->setPixmap( QPixmap::fromImage( image ).scaled( QSize( 480, 220 ), Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation ) );
    previewImage->setToolTip( QStringLiteral( "%1 × %2 пикселей" ).arg( sourceSize.width() ).arg( sourceSize.height() ) );
    previewImage->show();
    detailsText->setText( detailsText->text() + QStringLiteral( "\nРазмер изображения: %1 × %2 px" )
                              .arg( sourceSize.width() ).arg( sourceSize.height() ) );
    const QString metadataPath = QFileInfo( selectedItem.absolutePath ).dir().filePath(
        QFileInfo( selectedItem.absolutePath ).completeBaseName() + QStringLiteral( ".json" ) );
    QFile metadata( metadataPath );
    if ( metadata.open( QIODevice::ReadOnly ) ) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson( metadata.readAll(), &error );
        if ( error.error == QJsonParseError::NoError && document.isObject() ) {
            const QString description = document.object().value( QStringLiteral( "description" ) ).toString();
            if ( !description.isEmpty() )
                detailsText->setText( detailsText->text() + QStringLiteral( "\n\nОписание снимка:\n%1" ).arg( description ) );
        }
    }
}

void ExperimentLibraryWidget::loadNextThumbnails() {
    if ( thumbnailLoading ) return;
    constexpr int batchSize = 18;
    struct ThumbnailTask { int row; QString path; QString cacheKey; };
    struct ThumbnailResult { int row; QString path; QString cacheKey; QImage image; };
    QVector< ThumbnailTask > tasks;
    int processed = 0;
    while ( thumbnailIndex < rows.size() && processed < batchSize ) {
        const int row = thumbnailIndex++;
        ++processed;
        const ExperimentCatalogItem &item = rows.at( row );
        QListWidgetItem *tile = tiles->item( row );
        if ( !tile ) continue;
        const QString cacheKey = item.absolutePath + QLatin1Char( '\n' ) + item.modifiedAt + QLatin1Char( '\n' ) + QString::number( item.size );
        const auto cached = thumbnailCache.constFind( cacheKey );
        if ( cached != thumbnailCache.cend() ) {
            tile->setIcon( cached.value() );
        } else if ( item.type == QLatin1String( "photo" ) ) {
            tasks.append( { row, item.absolutePath, cacheKey } );
        } else {
            tile->setIcon( style()->standardIcon( item.type == QLatin1String( "video" )
                ? QStyle::SP_MediaPlay : QStyle::SP_FileIcon ) );
        }
    }
    if ( thumbnailIndex >= rows.size() ) thumbnailTimer->stop();
    if ( tasks.isEmpty() ) return;

    thumbnailLoading = true;
    thumbnailTimer->stop();
    const quint64 generation = thumbnailGeneration;
    const QPointer< ExperimentLibraryWidget > guard( this );
    QThreadPool::globalInstance()->start( QRunnable::create( [ tasks, generation, guard ]() {
        QVector< ThumbnailResult > results;
        results.reserve( tasks.size() );
        for ( const ThumbnailTask &task : tasks ) {
            QImageReader reader( task.path );
            reader.setAutoTransform( true );
            const QSize size = reader.size();
            if ( size.isValid() ) reader.setScaledSize( size.scaled( QSize( 112, 72 ), Qt::KeepAspectRatio ) );
            results.append( { task.row, task.path, task.cacheKey, reader.read() } );
        }
        if ( !guard ) return;
    QMetaObject::invokeMethod( guard.data(), [ guard, generation, results = std::move( results ) ]() {
            if ( !guard || guard->thumbnailGeneration != generation ) return;
            guard->thumbnailLoading = false;
            for ( const ThumbnailResult &result : results ) {
                QListWidgetItem *tile = guard->tiles->item( result.row );
                if ( !tile ) continue;
                if ( result.image.isNull() ) {
                    tile->setToolTip( tile->toolTip() + QStringLiteral( "\nМиниатюра недоступна или файл повреждён." ) );
                    continue;
                }
                const QIcon icon( QPixmap::fromImage( result.image ) );
                guard->thumbnailCache.insert( result.cacheKey, icon );
                tile->setIcon( icon );
            }
            if ( guard->thumbnailIndex < guard->rows.size() ) guard->thumbnailTimer->start();
        }, Qt::QueuedConnection );
    } ) );
}

void ExperimentLibraryWidget::openImageViewer( const QString &path ) {
    QImageReader reader( path );
    reader.setAutoTransform( true );
    const QImage image = reader.read();
    if ( image.isNull() ) {
        QMessageBox::warning( this, QStringLiteral( "Просмотр изображения" ),
                              QStringLiteral( "Не удалось открыть изображение: %1" ).arg( reader.errorString() ) );
        return;
    }
    QDialog dialog( this );
    dialog.setWindowTitle( QFileInfo( path ).fileName() );
    dialog.resize( qMin( image.width() + 40, 1200 ), qMin( image.height() + 90, 850 ) );
    auto *layout = new QVBoxLayout( &dialog );
    auto *scroll = new QScrollArea( &dialog );
    scroll->setWidgetResizable( true );
    auto *imageLabel = new QLabel( scroll );
    imageLabel->setAlignment( Qt::AlignCenter );
    imageLabel->setMinimumSize( 1, 1 );
    imageLabel->setScaledContents( false );
    scroll->setWidget( imageLabel );
    layout->addWidget( scroll, 1 );
    auto *controls = new QHBoxLayout;
    auto *zoom = new QSlider( Qt::Horizontal, &dialog );
    zoom->setRange( 10, 400 );
    zoom->setValue( 100 );
    auto *fit = new QPushButton( QStringLiteral( "По размеру окна" ), &dialog );
    auto *fullscreen = new QPushButton( QStringLiteral( "Во весь экран" ), &dialog );
    controls->addWidget( new QLabel( QStringLiteral( "Масштаб" ), &dialog ) );
    controls->addWidget( zoom, 1 );
    controls->addWidget( fit );
    controls->addWidget( fullscreen );
    layout->addLayout( controls );
    auto render = [ image, imageLabel, scroll ]( int percent ) {
        Q_UNUSED( scroll )
        const QSize target = image.size() * ( percent / 100.0 );
        imageLabel->setPixmap( QPixmap::fromImage( image ).scaled( target, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
        imageLabel->setMinimumSize( target );
    };
    connect( zoom, &QSlider::valueChanged, &dialog, render );
    connect( fit, &QPushButton::clicked, &dialog, [ &dialog, zoom, image ]() {
        const qreal factor = qMin( qreal( dialog.width() - 50 ) / image.width(), qreal( dialog.height() - 100 ) / image.height() );
        const int available = qMax( 10, qMin( 400, int( factor * 100.0 ) ) );
        zoom->setValue( available );
    } );
    connect( fullscreen, &QPushButton::clicked, &dialog, [ &dialog ]() {
        if ( dialog.isFullScreen() ) dialog.showNormal(); else dialog.showFullScreen();
    } );
    render( 100 );
    dialog.exec();
}

void ExperimentLibraryWidget::showContextMenu( const QPoint &position ) {
    QWidget *sourceView = libraryViews->currentWidget();
    if ( sourceView == tiles ) {
        QListWidgetItem *tile = tiles->itemAt( position );
        if ( tile && !tile->isSelected() ) {
            tiles->clearSelection();
            tile->setSelected( true );
        }
    } else {
        QTableWidgetItem *cell = table->itemAt( position );
        if ( cell && !cell->isSelected() ) {
            table->clearSelection();
            table->selectRow( cell->row() );
        }
    }
    const QVector< ExperimentCatalogItem > selected = selectedRecords();
    if ( selected.isEmpty() )
        return;
    QMenu menu( this );
    QAction *open = menu.addAction( QStringLiteral( "Открыть" ) );
    QAction *analytics = menu.addAction( QStringLiteral( "Открыть в аналитике" ) );
    QAction *preview = menu.addAction( QStringLiteral( "Предварительный просмотр" ) );
    QAction *exportFiles = menu.addAction( QStringLiteral( "Экспортировать копию…" ) );
    QAction *rename = menu.addAction( QStringLiteral( "Переименовать…" ) );
    QAction *description = menu.addAction( QStringLiteral( "Изменить описание…" ) );
    QAction *properties = menu.addAction( QStringLiteral( "Свойства" ) );
    QAction *folder = menu.addAction( QStringLiteral( "Показать в Проводнике" ) );
    menu.addSeparator();
    QAction *remove = menu.addAction( QStringLiteral( "Переместить в корзину…" ) );
    open->setEnabled( selected.size() == 1 && ( selected.first().type == QLatin1String( "experiment" ) ||
        selected.first().type == QLatin1String( "recording" ) || selected.first().type == QLatin1String( "photo" ) ||
        selected.first().type == QLatin1String( "video" ) ) );
    analytics->setEnabled( selected.size() == 1 && selected.first().type == QLatin1String( "recording" ) );
    preview->setEnabled( selected.size() == 1 && ( selected.first().type == QLatin1String( "photo" ) ||
                                                   selected.first().type == QLatin1String( "video" ) ) );
    exportFiles->setEnabled( std::all_of( selected.cbegin(), selected.cend(), []( const ExperimentCatalogItem &item ) {
        return item.type != QLatin1String( "experiment" ) && QFileInfo( item.absolutePath ).isFile();
    } ) );
    rename->setEnabled( selected.size() == 1 && selected.first().type != QLatin1String( "experiment" ) &&
                        selected.first().type != QLatin1String( "metadata" ) );
    description->setEnabled( selected.size() == 1 && ( selected.first().type == QLatin1String( "experiment" ) ||
                        selected.first().type == QLatin1String( "photo" ) ) );
    folder->setEnabled( selected.size() == 1 );
    QWidget *viewport = sourceView == tiles ? tiles->viewport() : table->viewport();
    QAction *chosen = menu.exec( viewport->mapToGlobal( position ) );
    if ( chosen == open ) openSelected();
    else if ( chosen == analytics ) openSelected();
    else if ( chosen == preview ) {
        if ( selected.first().type == QLatin1String( "photo" ) ) openImageViewer( selected.first().absolutePath );
        else if ( selected.first().type == QLatin1String( "video" ) ) openVideoPlayer( selected.first().absolutePath );
    } else if ( chosen == properties ) {
        const ExperimentCatalogItem &item = selected.first();
        QMessageBox::information( this, QStringLiteral( "Свойства" ),
            QStringLiteral( "%1\nТип: %2\nЭксперимент: %3\nРазмер: %4\nИзменён: %5\n\n%6" )
                .arg( item.name, typeLabel( item.type ), item.experimentName, formattedSize( item.size ),
                      item.modifiedAt, item.absolutePath ) );
    } else if ( chosen == exportFiles ) {
        const QString destinationDirectory = selected.size() == 1
            ? QString() : QFileDialog::getExistingDirectory( this, QStringLiteral( "Экспорт материалов" ) );
        if ( selected.size() > 1 && destinationDirectory.isEmpty() ) return;
        QStringList failures;
        for ( const ExperimentCatalogItem &item : selected ) {
            const QFileInfo sourceInfo( item.absolutePath );
            QString destination;
            if ( selected.size() == 1 ) {
                destination = QFileDialog::getSaveFileName( this, QStringLiteral( "Экспорт материала" ),
                    QDir::home().filePath( sourceInfo.fileName() ) );
                if ( destination.isEmpty() ) return;
            } else {
                destination = QDir( destinationDirectory ).filePath( sourceInfo.fileName() );
                for ( int suffix = 2; QFileInfo::exists( destination ); ++suffix )
                    destination = QDir( destinationDirectory ).filePath( sourceInfo.completeBaseName() +
                        QStringLiteral( "_%1." ).arg( suffix ) + sourceInfo.suffix() );
            }
            if ( !QFile::copy( item.absolutePath, destination ) ) {
                failures.append( sourceInfo.fileName() );
                continue;
            }
            if ( item.type == QLatin1String( "photo" ) ) {
                const QString sourceMetadata = sourceInfo.dir().filePath( sourceInfo.completeBaseName() + QStringLiteral( ".json" ) );
                const QString destinationMetadata = QFileInfo( destination ).dir().filePath(
                    QFileInfo( destination ).completeBaseName() + QStringLiteral( ".json" ) );
                if ( QFileInfo::exists( sourceMetadata ) && !QFile::copy( sourceMetadata, destinationMetadata ) )
                    failures.append( sourceInfo.fileName() + QStringLiteral( " (метаданные)" ) );
            }
        }
        if ( !failures.isEmpty() )
            QMessageBox::warning( this, QStringLiteral( "Экспорт" ),
                QStringLiteral( "Не удалось экспортировать: %1" ).arg( failures.join( QStringLiteral( ", " ) ) ) );
    } else if ( chosen == rename ) {
        const ExperimentCatalogItem item = selected.first();
        const QFileInfo info( item.absolutePath );
        bool accepted = false;
        const QString newBase = QInputDialog::getText( this, QStringLiteral( "Переименование" ),
            QStringLiteral( "Новое имя:" ), QLineEdit::Normal, info.completeBaseName(), &accepted ).trimmed();
        if ( !accepted || newBase.isEmpty() ) return;
        QString safeName = newBase;
        safeName.replace( QRegularExpression( "[\\\\/:*?\"<>|]" ), QStringLiteral( "_" ) );
        if ( safeName == QLatin1String( "." ) || safeName == QLatin1String( ".." ) ) {
            QMessageBox::warning( this, QStringLiteral( "Переименование" ), QStringLiteral( "Недопустимое имя." ) );
            return;
        }
        const QString newPath = info.dir().filePath( safeName + ( info.suffix().isEmpty() ? QString() : QLatin1Char( '.' ) + info.suffix() ) );
        const QString root = QDir( manager->projectsRoot() ).canonicalPath();
        if ( info.isSymLink() || !info.canonicalFilePath().startsWith( root + QDir::separator(), Qt::CaseInsensitive ) ||
             QFileInfo::exists( newPath ) ) {
            QMessageBox::warning( this, QStringLiteral( "Переименование" ), QStringLiteral( "Имя занято или путь находится вне хранилища." ) );
            return;
        }
        const QString oldSidecar = info.dir().filePath( info.completeBaseName() + QStringLiteral( ".json" ) );
        const QString newSidecar = info.dir().filePath( safeName + QStringLiteral( ".json" ) );
        const bool hasSidecar = item.type == QLatin1String( "photo" ) && QFileInfo::exists( oldSidecar );
        if ( !QFile::rename( item.absolutePath, newPath ) || ( hasSidecar && !QFile::rename( oldSidecar, newSidecar ) ) ) {
            if ( QFileInfo::exists( newPath ) ) QFile::rename( newPath, item.absolutePath );
            QMessageBox::warning( this, QStringLiteral( "Переименование" ), QStringLiteral( "Не удалось переименовать материал." ) );
            return;
        }
        if ( !updateExperimentManifest( item.absolutePath, item.type, false, nullptr ) ||
             !updateExperimentManifest( newPath, item.type, true, nullptr ) ) {
            if ( hasSidecar ) QFile::rename( newSidecar, oldSidecar );
            QFile::rename( newPath, item.absolutePath );
            updateExperimentManifest( item.absolutePath, item.type, true, nullptr );
            QMessageBox::warning( this, QStringLiteral( "Переименование" ), QStringLiteral( "Не удалось обновить experiment.json; исходное имя восстановлено." ) );
            return;
        }
        catalog->startRescan();
    } else if ( chosen == description ) {
        const ExperimentCatalogItem item = selected.first();
        const QString metadataPath = item.type == QLatin1String( "experiment" )
            ? QDir( item.absolutePath ).filePath( QStringLiteral( "experiment.json" ) )
            : QFileInfo( item.absolutePath ).dir().filePath( QFileInfo( item.absolutePath ).completeBaseName() + QStringLiteral( ".json" ) );
        QFile file( metadataPath );
        QJsonObject metadata;
        if ( file.open( QIODevice::ReadOnly ) ) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &parseError );
            if ( parseError.error != QJsonParseError::NoError || !document.isObject() ) {
                QMessageBox::warning( this, QStringLiteral( "Описание" ), QStringLiteral( "Метаданные повреждены; файл не изменён." ) );
                return;
            }
            metadata = document.object();
        } else if ( item.type == QLatin1String( "experiment" ) ) {
            QMessageBox::warning( this, QStringLiteral( "Описание" ), QStringLiteral( "Не удалось прочитать experiment.json." ) );
            return;
        }
        const QString current = item.type == QLatin1String( "experiment" ) ? metadata.value( QStringLiteral( "description" ) ).toString()
                                                                            : metadata.value( QStringLiteral( "description" ) ).toString();
        bool accepted = false;
        const QString value = QInputDialog::getMultiLineText( this, QStringLiteral( "Описание" ),
            QStringLiteral( "Описание эксперимента или снимка:" ), current, &accepted );
        if ( !accepted ) return;
        metadata.insert( QStringLiteral( "description" ), value );
        QSaveFile output( metadataPath );
        const QByteArray json = QJsonDocument( metadata ).toJson( QJsonDocument::Indented );
        if ( !output.open( QIODevice::WriteOnly ) || output.write( json ) != json.size() || !output.commit() )
            QMessageBox::warning( this, QStringLiteral( "Описание" ), QStringLiteral( "Не удалось сохранить описание." ) );
        else catalog->startRescan();
    }
    else if ( chosen == folder ) showSelectedInFolder();
    else if ( chosen == remove ) deleteSelected();
}

bool ExperimentLibraryWidget::updateExperimentManifest( const QString &path, const QString &type, bool restore,
                                                        QString *errorMessage ) {
    QDir directory = QFileInfo( path ).absoluteDir();
    QString experimentPath;
    while ( directory.exists() ) {
        if ( QFileInfo( directory.filePath( QStringLiteral( "experiment.json" ) ) ).isFile() ) {
            experimentPath = directory.absolutePath();
            break;
        }
        const QString current = directory.absolutePath();
        if ( !directory.cdUp() || directory.absolutePath() == current ) break;
    }
    if ( experimentPath.isEmpty() ) return true;
    QString key;
    if ( type == QLatin1String( "photo" ) ) key = QStringLiteral( "images" );
    else if ( type == QLatin1String( "video" ) ) key = QStringLiteral( "videos" );
    else if ( type == QLatin1String( "imported" ) || type == QLatin1String( "recording" ) ) key = QStringLiteral( "sampleDataFiles" );
    if ( key.isEmpty() ) return true;
    const QString relative = QDir( experimentPath ).relativeFilePath( QFileInfo( path ).absoluteFilePath() );
    const QString manifestPath = QDir( experimentPath ).filePath( QStringLiteral( "experiment.json" ) );
    QFile file( manifestPath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось прочитать метаданные эксперимента." );
        return false;
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &parseError );
    file.close();
    if ( parseError.error != QJsonParseError::NoError || !document.isObject() ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Файл experiment.json повреждён." );
        return false;
    }
    QJsonObject object = document.object();
    QJsonArray paths = object.value( key ).toArray();
    if ( restore ) {
        if ( !paths.contains( relative ) ) paths.append( relative );
    } else {
        for ( int i = paths.size() - 1; i >= 0; --i )
            if ( paths.at( i ).toString() == relative ) paths.removeAt( i );
    }
    object.insert( key, paths );
    QSaveFile output( manifestPath );
    const QByteArray json = QJsonDocument( object ).toJson( QJsonDocument::Indented );
    if ( !output.open( QIODevice::WriteOnly ) || output.write( json ) != json.size() || !output.commit() ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось безопасно обновить experiment.json." );
        return false;
    }
    return true;
}

bool ExperimentLibraryWidget::moveToTrash( const ExperimentCatalogItem &item, QString *errorMessage ) {
    const QString rootPath = cleanPath( QDir( manager->projectsRoot() ).canonicalPath() );
    const QFileInfo sourceInfo( item.absolutePath );
    const QString sourcePath = cleanPath( sourceInfo.canonicalFilePath() );
    if ( rootPath.isEmpty() || sourcePath.isEmpty() || sourceInfo.isSymLink() ||
         !isPathWithin( rootPath, sourcePath ) || sourcePath == rootPath ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Удаление разрешено только для файлов внутри хранилища экспериментов." );
        return false;
    }
    if ( item.type == QLatin1String( "experiment" ) && cleanPath( QFileInfo( sourcePath ).absolutePath() ) != rootPath ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Можно перемещать в корзину только корневую папку эксперимента." );
        return false;
    }
    if ( item.type != QLatin1String( "experiment" ) ) {
        QDir directory = QFileInfo( sourcePath ).absoluteDir();
        bool insideExperiment = false;
        while ( directory.exists() && isPathWithin( rootPath, directory.absolutePath() ) ) {
            if ( QFileInfo( directory.filePath( QStringLiteral( "experiment.json" ) ) ).isFile() ) {
                insideExperiment = true;
                break;
            }
            const QString current = directory.absolutePath();
            if ( !directory.cdUp() || directory.absolutePath() == current ) break;
        }
        if ( !insideExperiment ) {
            if ( errorMessage ) *errorMessage = QStringLiteral( "Файл не принадлежит управляемому эксперименту." );
            return false;
        }
    } else if ( !QFileInfo( QDir( sourcePath ).filePath( QStringLiteral( "experiment.json" ) ) ).isFile() ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Папка эксперимента недоступна." );
        return false;
    }
    if ( item.type == QLatin1String( "experiment" ) &&
         cleanPath( sourcePath ) == cleanPath( QDir( manager->activeExperiment() ).absolutePath() ) ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Сначала переключитесь на другой активный эксперимент." );
        return false;
    }
    const QString trashRoot = QDir( manager->projectsRoot() ).filePath( QStringLiteral( ".trash" ) );
    if ( !QDir().mkpath( trashRoot ) || QFileInfo( trashRoot ).isSymLink() ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось создать корзину экспериментов." );
        return false;
    }
    const QString canonicalTrashRoot = QFileInfo( trashRoot ).canonicalFilePath();
    if ( canonicalTrashRoot.isEmpty() || cleanPath( QFileInfo( canonicalTrashRoot ).absolutePath() ) != rootPath ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Путь корзины выходит за пределы хранилища экспериментов." );
        return false;
    }
    const QString entryPath = QDir( trashRoot ).filePath( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    if ( !QDir().mkpath( entryPath ) ) {
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось подготовить запись корзины." );
        return false;
    }
    const QString payload = QDir( entryPath ).filePath( QStringLiteral( "payload" ) );
    const QString companionSource = item.type == QLatin1String( "photo" )
        ? QFileInfo( sourcePath ).dir().filePath( QFileInfo( sourcePath ).completeBaseName() + QStringLiteral( ".json" ) )
        : QString();
    const bool hasCompanion = !companionSource.isEmpty() && QFileInfo::exists( companionSource );
    if ( hasCompanion && QFileInfo( companionSource ).isSymLink() ) {
        QDir( entryPath ).removeRecursively();
        if ( errorMessage ) *errorMessage = QStringLiteral( "Связанный JSON является ссылкой; операция отменена." );
        return false;
    }
    if ( item.type == QLatin1String( "experiment" ) ? !QDir().rename( sourcePath, payload ) : !QFile::rename( sourcePath, payload ) ) {
        QDir( entryPath ).removeRecursively();
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось переместить файл. Возможно, он открыт или занят." );
        return false;
    }
    const QString companionPayload = QDir( entryPath ).filePath( QStringLiteral( "photo-metadata.json" ) );
    if ( hasCompanion && !QFile::rename( companionSource, companionPayload ) ) {
        if ( item.type == QLatin1String( "experiment" ) ) QDir().rename( payload, sourcePath ); else QFile::rename( payload, sourcePath );
        QDir( entryPath ).removeRecursively();
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось переместить связанный JSON; исходный файл восстановлен." );
        return false;
    }
    if ( item.type != QLatin1String( "experiment" ) && !updateExperimentManifest( sourcePath, item.type, false, errorMessage ) ) {
        if ( hasCompanion ) QFile::rename( companionPayload, companionSource );
        QFile::rename( payload, sourcePath );
        QDir( entryPath ).removeRecursively();
        return false;
    }
    QJsonObject metadata;
    metadata.insert( QStringLiteral( "source" ), sourcePath );
    metadata.insert( QStringLiteral( "name" ), item.name );
    metadata.insert( QStringLiteral( "type" ), item.type );
    metadata.insert( QStringLiteral( "directory" ), item.type == QLatin1String( "experiment" ) );
    metadata.insert( QStringLiteral( "companion" ), hasCompanion ? companionSource : QString() );
    QSaveFile manifest( QDir( entryPath ).filePath( QStringLiteral( "entry.json" ) ) );
    const QByteArray json = QJsonDocument( metadata ).toJson( QJsonDocument::Indented );
    if ( !manifest.open( QIODevice::WriteOnly ) || manifest.write( json ) != json.size() || !manifest.commit() ) {
        if ( item.type == QLatin1String( "experiment" ) ) QDir().rename( payload, sourcePath ); else QFile::rename( payload, sourcePath );
        if ( hasCompanion ) QFile::rename( companionPayload, companionSource );
        QDir( entryPath ).removeRecursively();
        if ( item.type != QLatin1String( "experiment" ) ) updateExperimentManifest( sourcePath, item.type, true, nullptr );
        if ( errorMessage ) *errorMessage = QStringLiteral( "Не удалось сохранить сведения для восстановления." );
        return false;
    }
    return true;
}

void ExperimentLibraryWidget::deleteSelected() {
    const QVector< ExperimentCatalogItem > selected = selectedRecords();
    if ( selected.isEmpty() ) return;
    QStringList names;
    for ( const ExperimentCatalogItem &item : selected) names.append( item.name );
    const QString message = QStringLiteral( "Переместить в корзину %1 объект(ов)?\n\n%2\n\nФайлы можно будет восстановить." )
        .arg( selected.size() ).arg( names.mid( 0, 10 ).join( QStringLiteral( "\n" ) ) );
    if ( QMessageBox::question( this, QStringLiteral( "Удаление материалов" ), message,
                                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel ) != QMessageBox::Yes )
        return;
    int moved = 0;
    QStringList errors;
    for ( const ExperimentCatalogItem &item : selected ) {
        QString error;
        if ( moveToTrash( item, &error ) ) ++moved;
        else errors.append( item.name + QStringLiteral( ": " ) + error );
    }
    if ( moved ) {
        setIndexStatus( QStringLiteral( "В корзину перемещено: %1. Обновление картотеки…" ).arg( moved ) );
        catalog->startRescan();
    }
    if ( !errors.isEmpty() )
        QMessageBox::warning( this, QStringLiteral( "Не все материалы перемещены" ), errors.join( QLatin1Char( '\n' ) ) );
}

void ExperimentLibraryWidget::restoreFromTrash() {
    const QString trashRoot = QDir( manager->projectsRoot() ).filePath( QStringLiteral( ".trash" ) );
    if ( !QFileInfo::exists( trashRoot ) ) {
        QMessageBox::information( this, QStringLiteral( "Корзина" ), QStringLiteral( "В корзине нет материалов." ) );
        return;
    }
    const QString canonicalProjectsRoot = cleanPath( QFileInfo( manager->projectsRoot() ).canonicalFilePath() );
    if ( QFileInfo( trashRoot ).isSymLink() ||
         cleanPath( QFileInfo( trashRoot ).canonicalFilePath() ) != cleanPath( QDir( canonicalProjectsRoot ).filePath( QStringLiteral( ".trash" ) ) ) ) {
        QMessageBox::warning( this, QStringLiteral( "Корзина" ), QStringLiteral( "Путь корзины некорректен." ) );
        return;
    }
    const QFileInfoList entries = QDir( trashRoot ).entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks );
    QStringList labels;
    QStringList entryPaths;
    for ( const QFileInfo &entry : entries ) {
        QFile metadataFile( QDir( entry.absoluteFilePath() ).filePath( QStringLiteral( "entry.json" ) ) );
        if ( !metadataFile.open( QIODevice::ReadOnly ) ) continue;
        const QJsonObject metadata = QJsonDocument::fromJson( metadataFile.readAll() ).object();
        labels.append( metadata.value( QStringLiteral( "name" ) ).toString( entry.fileName() ) );
        entryPaths.append( entry.absoluteFilePath() );
    }
    if ( labels.isEmpty() ) {
        QMessageBox::information( this, QStringLiteral( "Корзина" ), QStringLiteral( "В корзине нет материалов." ) );
        return;
    }
    bool accepted = false;
    const QString label = QInputDialog::getItem( this, QStringLiteral( "Восстановление" ),
        QStringLiteral( "Выберите материал для восстановления:" ), labels, 0, false, &accepted );
    if ( !accepted ) return;
    const int index = labels.indexOf( label );
    if ( index < 0 ) return;
    const QString entryPath = entryPaths.at( index );
    if ( QFileInfo( entryPath ).isSymLink() || QFileInfo( entryPath ).canonicalFilePath().isEmpty() ||
         cleanPath( QFileInfo( QFileInfo( entryPath ).canonicalFilePath() ).absolutePath() ) !=
             cleanPath( QFileInfo( trashRoot ).canonicalFilePath() ) ) {
        QMessageBox::warning( this, QStringLiteral( "Восстановление" ), QStringLiteral( "Запись корзины находится вне управляемого каталога." ) );
        return;
    }
    QFile metadataFile( QDir( entryPath ).filePath( QStringLiteral( "entry.json" ) ) );
    if ( !metadataFile.open( QIODevice::ReadOnly ) ) return;
    const QJsonObject metadata = QJsonDocument::fromJson( metadataFile.readAll() ).object();
    const QString destination = metadata.value( QStringLiteral( "source" ) ).toString();
    const QString payload = QDir( entryPath ).filePath( QStringLiteral( "payload" ) );
    const bool isDirectory = metadata.value( QStringLiteral( "directory" ) ).toBool();
    const QString destinationParent = cleanPath( QFileInfo( destination ).absoluteDir().canonicalPath() );
    const QString normalizedRoot = cleanPath( canonicalProjectsRoot );
    bool safeParent = !destinationParent.isEmpty() &&
        isPathWithin( normalizedRoot, destinationParent ) &&
        ( isDirectory ? destinationParent == normalizedRoot : destinationParent != normalizedRoot );
    if ( safeParent && !isDirectory ) {
        QDir parent( destinationParent );
        bool belongsToExperiment = false;
        while ( isPathWithin( normalizedRoot, parent.absolutePath() ) ) {
            if ( QFileInfo( parent.filePath( QStringLiteral( "experiment.json" ) ) ).isFile() ) {
                belongsToExperiment = true;
                break;
            }
            if ( parent.absolutePath() == normalizedRoot || !parent.cdUp() ) break;
        }
        safeParent = belongsToExperiment;
    }
    if ( QFileInfo( payload ).isSymLink() ) safeParent = false;
    if ( destination.isEmpty() || !safeParent || QFileInfo::exists( destination ) ) {
        QMessageBox::warning( this, QStringLiteral( "Восстановление" ), QStringLiteral( "Путь восстановления занят или некорректен." ) );
        return;
    }
    if ( !QDir().mkpath( QFileInfo( destination ).absolutePath() ) ||
         ( isDirectory ? !QDir().rename( payload, destination ) : !QFile::rename( payload, destination ) ) ) {
        QMessageBox::warning( this, QStringLiteral( "Восстановление" ), QStringLiteral( "Не удалось восстановить файл." ) );
        return;
    }
    const QString companion = metadata.value( QStringLiteral( "type" ) ).toString() == QLatin1String( "photo" )
        ? QFileInfo( destination ).dir().filePath( QFileInfo( destination ).completeBaseName() + QStringLiteral( ".json" ) )
        : QString();
    const QString companionPayload = QDir( entryPath ).filePath( QStringLiteral( "photo-metadata.json" ) );
    if ( !companion.isEmpty() && QFileInfo::exists( companionPayload ) && !QFile::rename( companionPayload, companion ) ) {
        if ( isDirectory ) QDir().rename( destination, payload ); else QFile::rename( destination, payload );
        QMessageBox::warning( this, QStringLiteral( "Восстановление" ), QStringLiteral( "Не удалось восстановить связанный JSON. Файл возвращён в корзину." ) );
        return;
    }
    if ( !isDirectory && !updateExperimentManifest( destination, metadata.value( QStringLiteral( "type" ) ).toString(), true, nullptr ) )
        setIndexStatus( QStringLiteral( "Файл восстановлен, но experiment.json не обновлён." ), true );
    QDir( entryPath ).removeRecursively();
    catalog->startRescan();
    setIndexStatus( QStringLiteral( "Материал восстановлен: %1" ).arg( label ) );
}

void ExperimentLibraryWidget::emptyTrash() {
    const QString trashPath = QDir( manager->projectsRoot() ).filePath( QStringLiteral( ".trash" ) );
    const QString canonicalTrash = cleanPath( QFileInfo( trashPath ).canonicalFilePath() );
    const QString canonicalRoot = QFileInfo( manager->projectsRoot() ).canonicalFilePath();
    if ( !canonicalRoot.isEmpty() && cleanPath( QFileInfo( canonicalTrash ).absolutePath() ) != cleanPath( canonicalRoot ) ) {
        QMessageBox::warning( this, QStringLiteral( "Корзина" ), QStringLiteral( "Путь корзины выходит за пределы хранилища." ) );
        return;
    }
    if ( canonicalTrash.isEmpty() ) {
        QMessageBox::information( this, QStringLiteral( "Корзина" ), QStringLiteral( "Корзина пуста." ) );
        return;
    }
    const QFileInfoList entries = QDir( canonicalTrash ).entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks );
    if ( entries.isEmpty() ) {
        QMessageBox::information( this, QStringLiteral( "Корзина" ), QStringLiteral( "Корзина пуста." ) );
        return;
    }
    const QStringList names = [&entries]() {
        QStringList result;
        for ( const QFileInfo &entry : entries ) {
            QFile file( QDir( entry.absoluteFilePath() ).filePath( QStringLiteral( "entry.json" ) ) );
            if ( file.open( QIODevice::ReadOnly ) )
                result.append( QJsonDocument::fromJson( file.readAll() ).object().value( QStringLiteral( "name" ) ).toString( entry.fileName() ) );
            else
                result.append( entry.fileName() );
        }
        return result;
    }();
    const QString message = QStringLiteral( "Безвозвратно удалить %1 объект(ов)? Восстановление будет невозможно.\n\n%2" )
        .arg( entries.size() ).arg( names.mid( 0, 10 ).join( QStringLiteral( "\n" ) ) );
    if ( QMessageBox::warning( this, QStringLiteral( "Очистка корзины" ), message,
                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel ) != QMessageBox::Yes )
        return;
    int removed = 0;
    QStringList failures;
    for ( const QFileInfo &entry : entries ) {
        const QString canonicalEntry = entry.canonicalFilePath();
        if ( entry.isSymLink() || canonicalEntry.isEmpty() || cleanPath( QFileInfo( canonicalEntry ).absolutePath() ) != cleanPath( canonicalTrash ) ||
             !QDir( canonicalEntry ).removeRecursively() ) {
            failures.append( entry.fileName() );
            continue;
        }
        ++removed;
    }
    if ( removed ) setIndexStatus( QStringLiteral( "Безвозвратно удалено из корзины: %1" ).arg( removed ) );
    if ( !failures.isEmpty() )
        QMessageBox::warning( this, QStringLiteral( "Очистка корзины" ),
                              QStringLiteral( "Не удалось удалить: %1" ).arg( failures.join( QStringLiteral( ", " ) ) ) );
}
