// SPDX-License-Identifier: GPL-2.0-or-later

#include "mainwindow.h"
#include "src/ui_mainwindow.h"

#include "HorizontalDock.h"
#include "SpectrumDock.h"
#include "TriggerDock.h"
#include "VoltageDock.h"

#include "configdialog.h"
#include "dockwindows.h"
#include "documents.h"
#include "dsomodel.h"
#include "dsowidget.h"
#include "exporting/exporterinterface.h"
#include "exporting/exporterregistry.h"
#include "experiment/experimentcatalog.h"
#include "experiment/experimentlibrarywidget.h"
#include "experiment/experimentmanager.h"
#include "experiment/mp4videorecorder.h"
#include "experiment/rawplaybackengine.h"
#include "experiment/rawrecordingengine.h"
#include "hantekdsocontrol.h"
#include "usb/scopedevice.h"
#include "viewconstants.h"

#include "dsosettings.h"

#include <QDesktopServices>
#include <QDateTime>
#include <QCloseEvent>
#include <QDockWidget>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QValidator>

#include "OH_VERSION.h"

MainWindow::MainWindow( HantekDsoControl *dsoControl, DsoSettings *settings, ExporterRegistry *exporterRegistry,
                        ExperimentManager *experimentManager, ExperimentCatalog *experimentCatalog,
                        RawRecordingEngine *rawRecorder, RawPlaybackEngine *rawPlayback, QWidget *parent )
    : QMainWindow( parent ), ui( new Ui::MainWindow ), dsoSettings( settings ), exporterRegistry( exporterRegistry ),
      dsoControl( dsoControl ),
      experimentManager( experimentManager ), experimentCatalog( experimentCatalog ), rawRecorder( rawRecorder ),
      rawPlayback( rawPlayback ), videoRecorder( std::make_unique< Mp4VideoRecorder >() ) {

    if ( dsoSettings->scope.verboseLevel > 1 )
        qDebug() << " MainWindow::MainWindow()";

    // suppress nasty warnings, e.g. "kf5.kio.core: Invalid URL ..." or "qt.qpa.xcb: QXcbConnection: XCB error: 3 (BadWindow) ..."
    QLoggingCategory::setFilterRules( "kf5.kio.core=false\nqt.qpa.xcb=false" );
    // The selected laboratory variant has a light shell, regardless of the
    // operating-system palette; use matching dark glyphs in its toolbar.
    const QString iconPath = QStringLiteral( ":/images/lighttheme/" );
    elapsedTime.start();

    ui->setupUi( this );
    // Variant 3 keeps the oscilloscope plotting widget intact while moving
    // navigation to a compact, vertically docked laboratory tool rail.
    ui->toolBar->setOrientation( Qt::Vertical );
    addToolBar( Qt::LeftToolBarArea, ui->toolBar );
    ui->toolBar->setToolButtonStyle( Qt::ToolButtonTextUnderIcon );
    ui->toolBar->setIconSize( QSize( 24, 24 ) );
    ui->toolBar->setMovable( false );
    ui->toolBar->setFloatable( false );
    ui->toolBar->setMinimumWidth( 88 );
    ui->toolBar->setMaximumWidth( 104 );
    ui->toolBar->setStyleSheet( QStringLiteral(
        "QToolBar { background: #f4f6f9; border: 0; border-right: 1px solid #d9e0e8; spacing: 3px; padding: 6px 4px; }"
        "QToolButton { min-width: 74px; max-width: 92px; min-height: 54px; padding: 4px 2px; border-radius: 5px; }"
        "QToolButton:hover { background: #e5edf7; }"
        "QToolButton:checked { background: #d7e9ff; color: #075fc4; }"
        "QToolBar::separator { height: 1px; background: #d9e0e8; margin: 4px 8px; }" ) );
    setStyleSheet( QStringLiteral(
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #94a3b8; border-radius: 3px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #2563eb; background: #eff6ff; }"
        "QCheckBox::indicator:checked { border-color: #2563eb; background: #2563eb; image: url(:/images/lighttheme/check.svg); }"
        "QCheckBox::indicator:checked:hover { background: #1d4ed8; }"
        "QCheckBox::indicator:disabled { border-color: #cbd5e1; background: #e2e8f0; }"
        "QCheckBox::indicator:checked:disabled { border-color: #94a3b8; background: #94a3b8; image: url(:/images/lighttheme/check.svg); }"
        "QCheckBox:focus { outline: 2px solid #60a5fa; outline-offset: 2px; }" ) );
    // This workspace design is intentionally light; trace colors and the
    // oscilloscope's own plot rendering remain controlled by DsoWidget.
    QPalette workspacePalette = palette();
    workspacePalette.setColor( QPalette::Window, QColor( 246, 248, 251 ) );
    workspacePalette.setColor( QPalette::WindowText, QColor( 30, 41, 59 ) );
    workspacePalette.setColor( QPalette::Base, QColor( 255, 255, 255 ) );
    workspacePalette.setColor( QPalette::AlternateBase, QColor( 241, 245, 249 ) );
    workspacePalette.setColor( QPalette::Text, QColor( 30, 41, 59 ) );
    workspacePalette.setColor( QPalette::Button, QColor( 244, 246, 249 ) );
    workspacePalette.setColor( QPalette::ButtonText, QColor( 30, 41, 59 ) );
    workspacePalette.setColor( QPalette::Highlight, QColor( 191, 219, 254 ) );
    workspacePalette.setColor( QPalette::HighlightedText, QColor( 15, 23, 42 ) );
    setPalette( workspacePalette );
    iconPause = QIcon( iconPath + "pause.svg" );
    iconPlay = QIcon( iconPath + "play.svg" );
    ui->actionSampling->setIcon( iconPause );
    QList< QKeySequence > shortcuts; // provide multiple shortcuts for ui->actionSampling.
    // 1st entry in list is shown as shortcut in menu
#ifdef Q_OS_WIN
    shortcuts << QKeySequence( Qt::Key::Key_S ); // WIN: <space> can be grabbed by buttons, e.g. CH1
#endif
    shortcuts << QKeySequence( Qt::Key::Key_Space );
    shortcuts << QKeySequence( Qt::Key::Key_Pause );
#ifndef Q_OS_WIN
    shortcuts << QKeySequence( Qt::Key::Key_S ); // else put this shortcut at the end of the list
#endif
    ui->actionSampling->setShortcuts( shortcuts );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionSampling->setToolTip( tr( "Start and stop the sampling" ) );
    else
        ui->actionSampling->setToolTip( QString() );
    ui->actionRefresh->setIcon( QIcon( iconPath + "refresh.svg" ) );
    ui->actionRefresh->setShortcut( Qt::Key::Key_R );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionRefresh->setToolTip( tr( "Refresh the screen trace for slow 'Roll' mode" ) );
    else
        ui->actionRefresh->setToolTip( QString() );
    ui->actionPhosphor->setIcon( QIcon( iconPath + "phosphor.svg" ) );
    ui->actionPhosphor->setShortcut( Qt::Key::Key_P );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionPhosphor->setToolTip( tr( "Let the traces fade out slowly" ) );
    else
        ui->actionPhosphor->setToolTip( QString() );
    ui->actionHistogram->setIcon( QIcon( iconPath + "histogram.svg" ) );
    ui->actionHistogram->setShortcut( Qt::Key::Key_H );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionHistogram->setToolTip( tr( "Show a histogram of the voltage levels on the right side of the trace" ) );
    else
        ui->actionHistogram->setToolTip( QString() );
    ui->actionZoom->setIcon( QIcon( iconPath + "zoom.svg" ) );
    ui->actionZoom->setShortcut( Qt::Key::Key_Z );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionZoom->setToolTip( tr( "Zoom the range between the markers '1' and '2'" ) );
    else
        ui->actionZoom->setToolTip( QString() );
    ui->actionMeasure->setIcon( QIcon( iconPath + "measure.svg" ) );
    ui->actionMeasure->setShortcut( Qt::Key::Key_M );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionMeasure->setToolTip( tr( "Enable cursor measurements" ) );
    else
        ui->actionMeasure->setToolTip( QString() );
    ui->actionOpen->setIcon( QIcon( iconPath + "open.svg" ) );
    ui->actionOpen->setToolTip( tr( "Load scope settings from a config file" ) );
    ui->actionSave->setIcon( QIcon( iconPath + "save.svg" ) );
    ui->actionSave->setToolTip( tr( "Save the scope settings to the default location" ) );
    ui->actionSave_as->setIcon( QIcon( iconPath + "save_as.svg" ) );
    ui->actionSave_as->setToolTip( tr( "Save the scope settings to a user defined file" ) );
    ui->actionExit->setIcon( QIcon( iconPath + "exit.svg" ) );
    ui->actionExit->setToolTip( tr( "Exit the application" ) );
    ui->actionSettings->setIcon( QIcon( iconPath + "settings.svg" ) );
    ui->actionSettings->setToolTip( tr( "Define scope settings, analysis parameters and colors" ) );
    ui->actionSettings->setText( tr( "Настройки" ) );
    ui->actionCalibrateOffset->setIcon( QIcon( iconPath + "offset.svg" ) );
    ui->actionCalibrateOffset->setToolTip( tr( "Short-circuit both inputs and slowly select all voltage gain settings" ) );
    ui->actionManualCommand->setIcon( QIcon( iconPath + "terminal.svg" ) );
    ui->actionManualCommand->setToolTip( tr( "Send low level commands directly to the scope: 'CC XX XX'" ) );
    ui->actionUserManual->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionUserManual->setToolTip( tr( "Read the fine manual" ) );
    ui->actionACmodification->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionACmodification->setToolTip( tr( "Documentation how to add HW for AC coupled inputs" ) );
    ui->actionFrequencyGeneratorModification->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionFrequencyGeneratorModification->setToolTip(
        tr( "Documentation how to get jitter-free calibration frequency output" ) );
    ui->actionAbout->setIcon( QIcon( iconPath + "about.svg" ) );
    ui->actionAbout->setToolTip( tr( "Show info about the scope's HW and SW" ) );
    ui->actionAboutQt->setIcon( QIcon( iconPath + "qt.svg" ) );
    ui->actionAboutQt->setToolTip( tr( "Show info about Qt" ) );

    // Window title
    setWindowIcon( QIcon( ":/images/OpenHantek.svg" ) );
    setWindowTitle( dsoControl->getDevice()->isRealHW()
                        ? tr( "OpenHantek6022 (%1) - Device %2 (FW%3)" )
                              .arg( QString::fromStdString( VERSION ), dsoControl->getModel()->name )
                              .arg( dsoControl->getDevice()->getFwVersion(), 4, 16, QChar( '0' ) )
                        : tr( "OpenHantek6022 (%1) - " ).arg( QString::fromStdString( VERSION ) ) + tr( "Demo Mode" ) );

#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 6, 0 ) )
    setDockOptions( dockOptions() | QMainWindow::GroupedDragging );
#endif
    QAction *action;
    action = new QAction( QIcon( iconPath + "camera.svg" ), tr( "&Screenshot" ), this );
    action->setToolTip( tr( "Make an immediate screenshot of the program window and save it into the current directory" ) );
    connect( action, &QAction::triggered, this, [ this ]() { screenShot( SCREENSHOT, true ); } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "clone.svg" ), tr( "&Hardcopy" ), this );
    action->setToolTip( tr( "Make an immediate (printable) hardcopy of the display and save it into the current directory" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( HARDCOPY, true ); } );
    } );
    ui->menuExport->addAction( action );

    ui->menuExport->addSeparator();

    action = new QAction( QIcon( iconPath + "save_as.svg" ), tr( "Save Screenshot As .." ), this );
    action->setToolTip( tr( "Make a screenshot of the program window and define the storage location" ) );
    connect( action, &QAction::triggered, this, [ this ]() { screenShot( SCREENSHOT ); } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "save_as.svg" ), tr( "Save Hardcopy As .." ), this );
    action->setToolTip( tr( "Make a (printable) hardcopy of the display and define the storage location" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( HARDCOPY ); } );
    } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "print.svg" ), tr( "&Print Screen .." ), this );
    action->setToolTip( tr( "Send the hardcopy to a printer" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( PRINTER ); } );
    } );
    ui->menuExport->addAction( action );

    ui->menuExport->addSeparator();

    for ( auto *exporter : *exporterRegistry ) {
        action = new QAction( QIcon( iconPath + "exporter.svg" ), exporter->name(), this );
        action->setToolTip( tr( "Export captured data in %1 format for further processing" ).arg( exporter->format() ) );
        action->setCheckable( exporter->type() == ExporterInterface::Type::ContinuousExport );
        connect( action, &QAction::triggered, exporterRegistry, [ exporter, exporterRegistry ]( bool checked ) {
            exporterRegistry->setExporterEnabled( exporter,
                                                  exporter->type() == ExporterInterface::Type::ContinuousExport ? checked : true );
        } );
        ui->menuExport->addAction( action );
    }

    if ( dsoSettings->scope.toolTipVisible ) {
        ui->menuFile->setToolTipsVisible( true );
        ui->menuExport->setToolTipsVisible( true );
        ui->menuView->setToolTipsVisible( true );
        ui->menuOscilloscope->setToolTipsVisible( true );
        ui->menuHelp->setToolTipsVisible( true );
    }

    DsoSettingsScope *scope = &( dsoSettings->scope );
    const Dso::ControlSpecification *spec = dsoControl->getModel()->spec();

    registerDockMetaTypes();

    // Docking windows
    // Create dock windows before the dso widget, they fix messed up settings

    VoltageDock *voltageDock = new VoltageDock( scope, spec, this );
    HorizontalDock *horizontalDock = new HorizontalDock( scope, spec, this );
    TriggerDock *triggerDock = new TriggerDock( scope, spec, this );
    SpectrumDock *spectrumDock = new SpectrumDock( scope, this );

    addDockWidget( Qt::RightDockWidgetArea, voltageDock );
    addDockWidget( Qt::RightDockWidgetArea, horizontalDock );
    addDockWidget( Qt::RightDockWidgetArea, triggerDock );
    addDockWidget( Qt::RightDockWidgetArea, spectrumDock );
    QAction *analysisAction = spectrumDock->toggleViewAction();
    analysisAction->setText( QStringLiteral( "Анализ" ) );
    analysisAction->setIcon( QIcon( iconPath + "analysis.svg" ) );
    analysisAction->setToolTip( QStringLiteral( "Показать или скрыть существующие FFT-каналы и параметры спектра" ) );
    ui->toolBar->addAction( analysisAction );

    restoreGeometry( dsoSettings->mainWindowGeometry );

    // Central oszilloscope widget
    dsoWidget = new DsoWidget( &dsoSettings->scope, &dsoSettings->view, spec, this );
    setCentralWidget( dsoWidget );

    // The research library is a dockable workspace, while the original scope stays
    // the central widget and retains the upstream graph layout and interaction.
    libraryWidget = new ExperimentLibraryWidget( experimentCatalog, experimentManager, this );
    libraryDock = new QDockWidget( tr( "Библиотека исследований" ), this );
    libraryDock->setObjectName( QStringLiteral( "researchLibraryDock" ) );
    libraryDock->setMinimumHeight( 205 );
    libraryDock->setWidget( libraryWidget );
    addDockWidget( Qt::BottomDockWidgetArea, libraryDock );
    auto *libraryAction = new QAction( tr( "Библиотека" ), this );
    libraryAction->setCheckable( true );
    libraryAction->setIcon( QIcon( iconPath + "book.svg" ) );
    libraryAction->setToolTip( tr( "Открыть библиотеку экспериментов, фотографий, видео и записей сигналов" ) );
    ui->toolBar->addSeparator();
    ui->toolBar->addAction( ui->actionSettings );
    ui->toolBar->addSeparator();
    ui->toolBar->addAction( libraryAction );
    connect( libraryAction, &QAction::toggled, libraryDock, &QDockWidget::setVisible );
    connect( libraryDock, &QDockWidget::visibilityChanged, libraryAction, &QAction::setChecked );
    connect( libraryWidget, &ExperimentLibraryWidget::openRecordingRequested, this, &MainWindow::openRecording );
    connect( libraryWidget, &ExperimentLibraryWidget::activeExperimentChanged, this, [ this ]( const QString &path ) {
        statusBar()->showMessage( tr( "Активный эксперимент: %1" ).arg( QFileInfo( path ).fileName() ), 5000 );
    } );
    connect( experimentCatalog, &ExperimentCatalog::scanFailed, this,
             [ this ]( const QString &message ) { statusBar()->showMessage( message, 10000 ); } );
    experimentCatalog->startRescan();

    auto *screenshotAction = new QAction( QIcon( iconPath + "camera.svg" ), tr( "Скриншот" ), this );
    screenshotAction->setText( tr( "Снимок" ) );
    screenshotAction->setToolTip( tr( "Сохранить снимок рабочего пространства в активный эксперимент" ) );
    connect( screenshotAction, &QAction::triggered, this, [ this ] { screenShot( SCREENSHOT, true ); } );

    recordDataAction = new QAction( tr( "Запись данных" ), this );
    recordDataAction->setCheckable( true );
    dataRecordIcon = QIcon( iconPath + "exporter.svg" );
    recordDataAction->setIcon( dataRecordIcon );
    recordDataAction->setText( tr( "Данные" ) );
    recordDataAction->setToolTip( tr( "Записывать измерения CH1 и CH2 в формате OHRAW" ) );
    connect( recordDataAction, &QAction::triggered, this, &MainWindow::toggleDataRecording );

    recordVideoAction = new QAction( tr( "Видео" ), this );
    recordVideoAction->setCheckable( true );
    videoRecordIcon = QIcon( iconPath + "camera.svg" );
    recordVideoAction->setIcon( videoRecordIcon );
    recordVideoAction->setToolTip( tr( "Снимать окно осциллографа в файл MP4" ) );
    ui->toolBar->addSeparator();
    ui->toolBar->addAction( screenshotAction );
    ui->toolBar->addAction( recordVideoAction );
    ui->toolBar->addAction( recordDataAction );
    const bool restoredWorkspace = restoreState( dsoSettings->mainWindowState, 2 );
    if ( !restoredWorkspace ) {
        // A versioned layout invalidates the old top-toolbar arrangement once.
        // Subsequent starts restore the user's own dock proportions and visibility.
        libraryDock->show();
        QTimer::singleShot( 0, this, [ this ] {
            resizeDocks( { libraryDock }, { qMax( 235, height() / 3 ) }, Qt::Vertical );
        } );
    }
    libraryAction->setChecked( !libraryDock->isHidden() );
    recordVideoAction->setEnabled( Mp4VideoRecorder::isSupported() );
    if ( !Mp4VideoRecorder::isSupported() )
        recordVideoAction->setText( tr( "Нет видео" ) );
    connect( recordVideoAction, &QAction::triggered, this, &MainWindow::toggleVideoRecording );
    videoFrameTimer = new QTimer( this );
    connect( videoFrameTimer, &QTimer::timeout, this, &MainWindow::appendVideoFrame );

    connect( rawRecorder, &RawRecordingEngine::recordingFinished, this,
             [ this ]( const QString &path, quint64 frames, quint64 dropped ) {
                 recordDataAction->setChecked( false );
                 recordDataAction->setEnabled( true );
                 recordDataAction->setText( tr( "Данные" ) );
                 recordDataAction->setIcon( dataRecordIcon );
                 QString error;
                 if ( !this->experimentManager->registerDataFile( path, &error ) )
                     statusBar()->showMessage( tr( "Файл записан, ошибка регистрации в библиотеке: %1" ).arg( error ), 12000 );
                 else
                     statusBar()->showMessage( tr( "OHRAW записан: %1 блоков; пропущено: %2" ).arg( frames ).arg( dropped ), 8000 );
                 this->experimentCatalog->startRescan();
                 if ( closingAfterRecording )
                     QTimer::singleShot( 0, this, &QWidget::close );
             } );
    connect( rawRecorder, &RawRecordingEngine::recordingFailed, this, [ this ]( const QString &message ) {
        recordDataAction->setChecked( false );
        recordDataAction->setEnabled( true );
        recordDataAction->setText( tr( "Данные" ) );
        recordDataAction->setIcon( dataRecordIcon );
        QMessageBox::critical( this, tr( "Запись измерений" ), message );
        if ( closingAfterRecording )
            QTimer::singleShot( 0, this, &QWidget::close );
    } );
    connect( rawRecorder, &RawRecordingEngine::recordingStarted, this,
             [ this ]( const QString &path ) { statusBar()->showMessage( tr( "Запись CH1/CH2: %1" ).arg( path ) ); } );
    connect( rawRecorder, &RawRecordingEngine::droppedBlocksChanged, this,
             [ this ]( quint64 count ) { statusBar()->showMessage( tr( "Предупреждение: пропущено блоков записи: %1" ).arg( count ) ); } );

    deviceCommandWidgets = { voltageDock, horizontalDock, triggerDock, spectrumDock, dsoWidget };
    deviceCommandActions = { ui->actionOpen, ui->actionSampling, ui->actionRefresh, ui->actionCalibrateOffset,
                             ui->actionManualCommand };

    if ( dsoControl->getDevice()->isRealHW() ) { // enable online calibration and manual command input
        // Command field inside the status bar
        commandEdit = new QLineEdit( this );
        // allowed commands are either control command "cc E0..E6 xx [xx ...]" or frequency command "freq nn"
        QRegularExpressionValidator *v =
            new QRegularExpressionValidator( QRegularExpression( "(cc|CC) [eE][0-6]( [0-9a-fA-F]{1,2})+|freq \\d{1,6}" ), this );
        commandEdit->setValidator( v );
        commandEdit->hide();
        statusBar()->addPermanentWidget( commandEdit, 1 );

        connect( ui->actionCalibrateOffset, &QAction::toggled, this, [ this, dsoControl, scope ]( bool active ) {
            if ( active )
                active = ( QMessageBox::Apply ==
                           QMessageBox::information( this, tr( "Calibrate Offset" ),
                                                     tr( "Short-circuit both inputs and slowly select all voltage gain settings" ),
                                                     QMessageBox::Apply | QMessageBox::Abort, QMessageBox::Abort ) );
            if ( scope->verboseLevel > 2 )
                qDebug() << "  Calibrate offset" << active;
            ui->actionCalibrateOffset->setChecked( active );
            dsoControl->calibrateOffset( active );
            scope->liveCalibrationActive = active;
        } );

        // disable calibration e.g. if zero signal too noisy or offset too big
        connect( dsoControl, &HantekDsoControl::liveCalibrationError, this, [ this, scope ]() {
            if ( scope->verboseLevel > 2 )
                qDebug() << "  Live calibration error";
            scope->liveCalibrationActive = false;           // set incactive first to avoid ..
            ui->actionCalibrateOffset->setChecked( false ); // .. calibration storage actions
        } );

        connect( ui->actionManualCommand, &QAction::toggled, this, [ this ]( bool checked ) {
            commandEdit->setVisible( checked );
            if ( checked )
                commandEdit->setFocus();
        } );

        connect( commandEdit, &QLineEdit::returnPressed, this, [ this, dsoControl ]() {
            Dso::ErrorCode errorCode = dsoControl->stringCommand( commandEdit->text() );
            commandEdit->clear();
            ui->actionManualCommand->setChecked( false );
            if ( errorCode != Dso::ErrorCode::NONE )
                statusBar()->showMessage( tr( "Invalid command" ), 3000 );
        } );

    } else { // do not show these actions
        ui->actionCalibrateOffset->setVisible( false );
        ui->actionManualCommand->setVisible( false );
    }

    // Connect signals that display text in statusbar
    connect( dsoControl, &HantekDsoControl::statusMessage, this, [ this ]( QString text, int timeout ) {
        ui->actionManualCommand->setChecked( false );
        statusBar()->showMessage( text, timeout );
    } );

    // Connect signals to DSO controller and widget
    connect( horizontalDock, &HorizontalDock::samplerateChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setSamplerate( dsoSettings->scope.horizontal.samplerate ); } );
    connect( horizontalDock, &HorizontalDock::samplerateChanged, dsoWidget,
             [ this ]() { dsoWidget->updateSamplerate( dsoSettings->scope.horizontal.samplerate ); } );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, triggerDock, &TriggerDock::timebaseChanged );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setRecordTime( dsoSettings->scope.horizontal.timebase * DIVS_TIME ); } );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, dsoWidget,
             [ this ]() { dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase ); } );
    connect( spectrumDock, &SpectrumDock::frequencybaseChanged, dsoWidget,
             [ this ]( double frequencybase ) { dsoWidget->updateFrequencybase( frequencybase ); } );
    connect( dsoControl, &HantekDsoControl::samplerateCalculated, horizontalDock,
             [ this, horizontalDock, spectrumDock ]( double samplerate, unsigned oversample ) {
                 // The timebase was set, let's adapt the samplerate accordingly
                 dsoSettings->scope.horizontal.samplerate = samplerate;
                 horizontalDock->setSamplerate( samplerate );
                 spectrumDock->setSamplerate( samplerate ); // mind the Nyquest frequency
                 dsoWidget->updateSamplerate( samplerate );
                 dsoWidget->updateOversample( oversample );
             } );
    connect( horizontalDock, &HorizontalDock::calfreqChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setCalFreq( dsoSettings->scope.horizontal.calfreq ); } );
    connect( horizontalDock, &HorizontalDock::formatChanged, spectrumDock, [ = ]( Dso::GraphFormat format ) {
        ui->actionHistogram->setEnabled( format == Dso::GraphFormat::TY );
        spectrumDock->enableSpectrumDock( format == Dso::GraphFormat::TY );
    } );

    connect( triggerDock, &TriggerDock::modeChanged, dsoControl, &HantekDsoControl::setTriggerMode );
    connect( triggerDock, &TriggerDock::modeChanged, dsoWidget, &DsoWidget::updateTriggerMode );
    connect( triggerDock, &TriggerDock::modeChanged, horizontalDock, &HorizontalDock::triggerModeChanged );
    connect( triggerDock, &TriggerDock::modeChanged, this, [ this ]( Dso::TriggerMode mode ) {
        ui->actionRefresh->setVisible( Dso::TriggerMode::ROLL == mode && dsoSettings->scope.horizontal.samplerate < 10e3 );
    } );
    connect( dsoControl, &HantekDsoControl::samplerateCalculated, this, [ this ]( double samplerate ) {
        ui->actionRefresh->setVisible( Dso::TriggerMode::ROLL == dsoSettings->scope.trigger.mode && samplerate < 10e3 );
    } );
    connect( triggerDock, &TriggerDock::sourceChanged, dsoControl, &HantekDsoControl::setTriggerSource );
    connect( triggerDock, &TriggerDock::sourceChanged, dsoWidget, &DsoWidget::updateTriggerSource );
    connect( triggerDock, &TriggerDock::smoothChanged, dsoControl, &HantekDsoControl::setTriggerSmooth );
    // should we send the smooth mode also to dsoWidget?
    connect( triggerDock, &TriggerDock::slopeChanged, dsoControl, &HantekDsoControl::setTriggerSlope );
    connect( triggerDock, &TriggerDock::slopeChanged, dsoWidget, &DsoWidget::updateTriggerSlope );
    connect( dsoWidget, &DsoWidget::triggerPositionChanged, dsoControl, &HantekDsoControl::setTriggerPosition );
    connect( dsoWidget, &DsoWidget::triggerLevelChanged, dsoControl, &HantekDsoControl::setTriggerLevel );

    auto usedChanged = [ this, dsoControl, spec ]( ChannelID channel, unsigned channelMask ) {
        if ( channel > spec->channels )
            return;
        if ( dsoSettings->scope.verboseLevel > 2 )
            qDebug().noquote() << "  MW::uC()" << channel << QString::number( channelMask, 2 );
        bool mathUsed = 3 == channelMask; // dsoSettings->scope.anyUsed( spec->channels );

        // Normal channel, check if voltage/spectrum or math channel is used
        if ( channel < spec->channels )
            dsoControl->setChannelUsed( channel, mathUsed || dsoSettings->scope.anyUsed( channel ) );
        // Math channel, update all channels
        else if ( channel == spec->channels ) {
            for ( ChannelID c = 0; c < spec->channels; ++c )
                dsoControl->setChannelUsed( c, ( ( c + 1 ) & channelMask ) || dsoSettings->scope.anyUsed( c ) );
        }
    };
    connect( voltageDock, &VoltageDock::usedChannelChanged, usedChanged );
    connect( spectrumDock, &SpectrumDock::usedChannelChanged, usedChanged );

    connect( voltageDock, &VoltageDock::modeChanged, dsoWidget, &DsoWidget::updateMathMode );
    connect( voltageDock, &VoltageDock::gainChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, double gain ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setGain( channel, gain );
    } );
    connect( voltageDock, &VoltageDock::probeAttnChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, double probeAttn ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setProbe( channel, probeAttn );
    } );
    connect( voltageDock, &VoltageDock::invertedChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, bool inverted ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setChannelInverted( channel, inverted );
    } );
    connect( voltageDock, &VoltageDock::couplingChanged, dsoWidget, &DsoWidget::updateVoltageCoupling );
    connect( voltageDock, &VoltageDock::couplingChanged, dsoControl,
             [ dsoControl, spec ]( ChannelID channel, Dso::Coupling coupling ) {
                 if ( channel > spec->channels )
                     return;
                 dsoControl->setCoupling( channel, coupling );
             } );
    connect( voltageDock, &VoltageDock::gainChanged, dsoWidget, &DsoWidget::updateVoltageGain );
    connect( voltageDock, &VoltageDock::usedChannelChanged, dsoWidget, &DsoWidget::updateVoltageUsed );
    connect( spectrumDock, &SpectrumDock::usedChannelChanged, dsoWidget, &DsoWidget::updateSpectrumUsed );
    connect( spectrumDock, &SpectrumDock::magnitudeChanged, dsoWidget, &DsoWidget::updateSpectrumMagnitude );

    // Started/stopped signals from oscilloscope
    connect( dsoControl, &HantekDsoControl::showSamplingStatus, this, [ this ]( bool enabled ) {
        QSignalBlocker blocker( ui->actionSampling );
        if ( enabled ) {
            ui->actionSampling->setIcon( iconPause );
            ui->actionSampling->setText( tr( "Стоп" ) );
            ui->actionSampling->setStatusTip( tr( "Stop the oscilloscope" ) );
        } else {
            ui->actionSampling->setIcon( iconPlay );
            ui->actionSampling->setText( tr( "Пуск" ) );
            ui->actionSampling->setStatusTip( tr( "Start the oscilloscope" ) );
        }
        ui->actionSampling->setChecked( enabled );
    } );
    connect( ui->actionSampling, &QAction::triggered, dsoControl, &HantekDsoControl::enableSamplingUI );
    ui->actionSampling->setChecked( dsoControl->isSamplingUI() );

    connect( ui->actionRefresh, &QAction::triggered, dsoControl, &HantekDsoControl::restartSampling );

    connect( dsoControl, &HantekDsoControl::samplerateLimitsChanged, horizontalDock, &HorizontalDock::setSamplerateLimits );
    connect( dsoControl, &HantekDsoControl::samplerateSet, horizontalDock, &HorizontalDock::setSamplerateSteps );

    // Load settings to GUI
    connect( this, &MainWindow::settingsLoaded, voltageDock, &VoltageDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, horizontalDock, &HorizontalDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, spectrumDock, &SpectrumDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, triggerDock, &TriggerDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, dsoControl, &HantekDsoControl::applySettings );
    connect( this, &MainWindow::settingsLoaded, dsoWidget, &DsoWidget::updateSlidersSettings );

    connect( ui->actionOpen, &QAction::triggered, this, [ this, spec ]() {
        QString configFileName = QFileDialog::getOpenFileName( this, tr( "Open file" ), "", tr( "Settings (*.conf)" ), nullptr,
                                                               QFileDialog::DontUseNativeDialog );
        if ( !configFileName.isEmpty() ) {
            dsoSettings->loadFromFile( configFileName );
            restoreGeometry( dsoSettings->mainWindowGeometry );
            restoreState( dsoSettings->mainWindowState, 2 );

            emit settingsLoaded( &dsoSettings->scope, spec );

            dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase );

            for ( ChannelID channel = 0; channel < spec->channels; ++channel ) {
                dsoWidget->updateVoltageUsed( channel, dsoSettings->scope.voltage[ channel ].used );
                dsoWidget->updateSpectrumUsed( channel, dsoSettings->scope.spectrum[ channel ].used );
            }
        }
    } );

    connect( ui->actionSave, &QAction::triggered, this, [ this ]() {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState( 2 );
        dsoSettings->save();
    } );

    connect( ui->actionSave_as, &QAction::triggered, this, [ this ]() {
        QString configFileName = QFileDialog::getSaveFileName( this, tr( "Save settings" ), "", tr( "Settings (*.conf)" ), nullptr,
                                                               QFileDialog::DontUseNativeDialog );
        if ( configFileName.isEmpty() )
            return;
        if ( !configFileName.endsWith( ".conf" ) )
            configFileName.append( ".conf" );
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState( 2 );
        dsoSettings->saveToFile( configFileName );
    } );

    connect( ui->actionExit, &QAction::triggered, this, &QWidget::close );

    connect( ui->actionSettings, &QAction::triggered, this, [ this ]() {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState( 2 );

        DsoConfigDialog *configDialog = new DsoConfigDialog( dsoSettings, this );
        configDialog->setModal( true );
        configDialog->show();
    } );

    connect( ui->actionPhosphor, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.digitalPhosphor = enabled;

        if ( dsoSettings->view.digitalPhosphor )
            ui->actionPhosphor->setStatusTip( tr( "Disable fading of previous graphs" ) );
        else
            ui->actionPhosphor->setStatusTip( tr( "Enable fading of previous graphs" ) );
    } );
    ui->actionPhosphor->setChecked( dsoSettings->view.digitalPhosphor );

    connect( ui->actionHistogram, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->scope.histogram = enabled;

        if ( dsoSettings->scope.histogram )
            ui->actionHistogram->setStatusTip( tr( "Hide histogram" ) );
        else
            ui->actionHistogram->setStatusTip( tr( "Show histogram" ) );
    } );
    ui->actionHistogram->setChecked( dsoSettings->scope.histogram );
    ui->actionHistogram->setEnabled( scope->horizontal.format == Dso::GraphFormat::TY );

    connect( ui->actionZoom, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.zoom = enabled;

        if ( dsoSettings->view.zoom )
            ui->actionZoom->setStatusTip( tr( "Hide magnified scope" ) );
        else
            ui->actionZoom->setStatusTip( tr( "Show magnified scope" ) );

        dsoWidget->updateZoom( enabled );
    } );
    ui->actionZoom->setChecked( dsoSettings->view.zoom );

    connect( ui->actionMeasure, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.cursorsVisible = enabled;

        if ( dsoSettings->view.cursorsVisible )
            ui->actionMeasure->setStatusTip( tr( "Hide measurements" ) );
        else
            ui->actionMeasure->setStatusTip( tr( "Show measurements" ) );

        dsoWidget->updateCursorGrid( enabled );
    } );
    ui->actionMeasure->setChecked( dsoSettings->view.cursorsVisible );

    connect( ui->actionUserManual, &QAction::triggered, this, [ this ]() { openDocument( UserManualName ); } );

    connect( ui->actionACmodification, &QAction::triggered, this, [ this ]() { openDocument( ACModificationName ); } );

    connect( ui->actionFrequencyGeneratorModification, &QAction::triggered, this,
             [ this ]() { openDocument( FrequencyGeneratorModificationName ); } );

    connect( ui->actionAbout, &QAction::triggered, this, [ this ]() {
        QString deviceSpec = dsoSettings->deviceName == "DEMO"
                                 ? tr( "<p>Demo Mode</p>" )
                                 : tr( "<p>Device: %1 (%2), FW%3</p>" )
                                       .arg( dsoSettings->deviceName, dsoSettings->deviceID ) // device type, ser. num
                                       .arg( dsoSettings->deviceFW, 4, 16,
                                             QChar( '0' ) ); // FW version
        QMessageBox::about(
            this, QString( "%1 (%2)" ).arg( QCoreApplication::applicationName(), VERSION ),
            QString( tr( "<p>Open source software for Hantek6022 USB oscilloscopes</p>"
                         "<p>Maintainer: Martin Homuth-Rosemann</p>"
                         "<p>Copyright &copy; 2010, 2011 Oliver Haag</p>"
                         "<p>Copyright &copy; 2012-%1 OpenHantek community<br/>"
                         "<a href='https://github.com/OpenHantek'>https://github.com/OpenHantek</a></p>"
                         "<p>Open source firmware copyright &copy; 2019-%1 Martin Homuth-Rosemann<br/>"
                         "<a href='https://github.com/Ho-Ro/Hantek6022API'>https://github.com/Ho-Ro/Hantek6022API</a></p>" ) )
                    .arg( QDate::currentDate().year() ) // latest year for (c)

                + deviceSpec // "DEVICE (SERIALNUMBER) FW_VERSION" or "Demo Mode"

                + tr( "<p>Graphic: %1 - GLSL version %2</p>"
                      "<p>Qt version: %3</p>" )
                      .arg( GlScope::getOpenGLversion(), GlScope::getGLSLversion(), // graphic info
                            qVersion() ) +                                          // Qt lib version info
                tr( "<p>Running since %1 seconds.</p>" ).arg( elapsedTime.elapsed() / 1000 ) );
    } );

    connect( ui->actionAboutQt, &QAction::triggered, this, [ this ]() {
        QMessageBox::aboutQt( this, QString( "%1 (%2)" ).arg( QCoreApplication::applicationName(), VERSION ) );
    } );

    emit settingsLoaded( &dsoSettings->scope, spec ); // trigger the previously connected docks, widgets, etc.

    dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase );

    for ( ChannelID channel = 0; channel < spec->channels; ++channel ) {
        dsoWidget->updateVoltageUsed( channel, dsoSettings->scope.voltage[ channel ].used );
        dsoWidget->updateSpectrumUsed( channel, dsoSettings->scope.spectrum[ channel ].used );
    }
}


MainWindow::~MainWindow() {
    if ( dsoSettings->scope.verboseLevel > 1 )
        qDebug() << " MainWindow::~MainWindow()";
    delete ui;
}

void MainWindow::toggleDataRecording() {
    if ( recordDataAction->isChecked() ) {
        if ( !ui->actionSampling->isChecked() ) {
            recordDataAction->setChecked( false );
            QMessageBox::information( this, tr( "Запись измерений" ),
                                      tr( "Сначала запустите непрерывный захват осциллографа." ) );
            return;
        }
        QString error;
        const QString path = experimentManager->nextRawDataPath( &error );
        if ( path.isEmpty() || !rawRecorder->start( path, &error ) ) {
            recordDataAction->setChecked( false );
            QMessageBox::critical( this, tr( "Запись измерений" ), error.isEmpty() ? tr( "Не удалось начать запись." ) : error );
            return;
        }
        recordDataAction->setText( tr( "Стоп данных" ) );
        recordDataAction->setToolTip( tr( "Остановить запись измерений CH1/CH2" ) );
        recordDataAction->setIcon( style()->standardIcon( QStyle::SP_MediaStop ) );
        return;
    }
    if ( rawRecorder->isRecording() ) {
        rawRecorder->stop();
        recordDataAction->setEnabled( false );
        recordDataAction->setText( tr( "Завершение записи…" ) );
    }
}

void MainWindow::toggleVideoRecording() {
    if ( recordVideoAction->isChecked() ) {
        QDialog settingsDialog( this );
        settingsDialog.setWindowTitle( tr( "Параметры записи видео" ) );
        auto *layout = new QVBoxLayout( &settingsDialog );
        auto *size = new QComboBox( &settingsDialog );
        size->addItem( tr( "Размер окна (%1 × %2)" ).arg( width() ).arg( height() ), QSize( width(), height() ) );
        size->addItem( QStringLiteral( "1280 × 720" ), QSize( 1280, 720 ) );
        size->addItem( QStringLiteral( "1920 × 1080" ), QSize( 1920, 1080 ) );
        const int savedSize = size->findData( QSettings().value( "experiment/videoSize", size->itemData( 0 ) ) );
        size->setCurrentIndex( savedSize < 0 ? 0 : savedSize );
        auto *fps = new QComboBox( &settingsDialog );
        fps->addItem( QStringLiteral( "15 кадр/с" ), 15 );
        fps->addItem( QStringLiteral( "25 кадр/с" ), 25 );
        fps->addItem( QStringLiteral( "30 кадр/с" ), 30 );
        const int savedFps = fps->findData( QSettings().value( "experiment/videoFps", 30 ) );
        fps->setCurrentIndex( savedFps < 0 ? 2 : savedFps );
        auto *buttons = new QHBoxLayout;
        auto *startButton = new QPushButton( tr( "Начать запись" ), &settingsDialog );
        auto *cancelButton = new QPushButton( tr( "Отмена" ), &settingsDialog );
        buttons->addStretch( 1 );
        buttons->addWidget( startButton );
        buttons->addWidget( cancelButton );
        layout->addWidget( new QLabel( tr( "Разрешение видео" ), &settingsDialog ) );
        layout->addWidget( size );
        layout->addWidget( new QLabel( tr( "Частота кадров" ), &settingsDialog ) );
        layout->addWidget( fps );
        layout->addLayout( buttons );
        connect( startButton, &QPushButton::clicked, &settingsDialog, &QDialog::accept );
        connect( cancelButton, &QPushButton::clicked, &settingsDialog, &QDialog::reject );
        if ( settingsDialog.exec() != QDialog::Accepted ) {
            recordVideoAction->setChecked( false );
            return;
        }
        videoSize = size->currentData().toSize();
        videoFps = fps->currentData().toInt();
        QSettings().setValue( "experiment/videoSize", videoSize );
        QSettings().setValue( "experiment/videoFps", videoFps );
        QString error;
        const QString path = experimentManager->nextVideoPath( &error );
        if ( path.isEmpty() || !videoRecorder->start( path, videoSize, videoFps, &error ) ) {
            recordVideoAction->setChecked( false );
            QMessageBox::critical( this, tr( "Запись MP4" ), error.isEmpty() ? tr( "Не удалось начать запись видео." ) : error );
            return;
        }
        videoElapsed.start();
        videoFrameTimer->start( qMax( 1, 1000 / videoFps ) );
        recordVideoAction->setText( tr( "Стоп видео" ) );
        statusBar()->showMessage( tr( "Запись MP4 начата: %1" ).arg( path ) );
        return;
    }
    if ( videoRecorder->isBusy() ) {
        videoFrameTimer->stop();
        QString error;
        if ( !videoRecorder->stop( &error ) ) {
            QMessageBox::critical( this, tr( "Запись MP4" ), error );
        } else {
            const QString path = videoRecorder->filePath();
            if ( !experimentManager->registerVideo( path, &error ) )
                statusBar()->showMessage( tr( "Видео записано, но не добавлено в библиотеку: %1" ).arg( error ), 10000 );
            else
                statusBar()->showMessage( tr( "Видео сохранено: %1 (%2 с, пропущено кадров: %3)" )
                                              .arg( path ).arg( videoElapsed.isValid() ? videoElapsed.elapsed() / 1000 : 0 )
                                              .arg( videoRecorder->droppedFrameCount() ), 8000 );
            experimentCatalog->startRescan();
        }
    }
    recordVideoAction->setChecked( false );
    recordVideoAction->setText( tr( "Видео" ) );
    recordVideoAction->setIcon( videoRecordIcon );
}

void MainWindow::appendVideoFrame() {
    if ( !videoRecorder || !videoRecorder->isRecording() ) {
        videoFrameTimer->stop();
        if ( videoRecorder && videoRecorder->isBusy() ) {
            QString error;
            videoRecorder->stop( &error );
            recordVideoAction->setChecked( false );
            recordVideoAction->setText( tr( "Видео" ) );
            recordVideoAction->setIcon( videoRecordIcon );
            if ( !error.isEmpty() )
                QMessageBox::critical( this, tr( "Запись MP4" ), error );
        }
        return;
    }
    QImage frame = grab().toImage();
    QString error;
    if ( !videoRecorder->appendFrame( frame, &error ) ) {
        videoFrameTimer->stop();
        QString finalizeError;
        videoRecorder->stop( &finalizeError );
        recordVideoAction->setChecked( false );
        recordVideoAction->setText( tr( "Видео" ) );
        recordVideoAction->setIcon( videoRecordIcon );
        QMessageBox::critical( this, tr( "Запись MP4" ), finalizeError.isEmpty() ? error : finalizeError );
    } else {
        const qint64 seconds = videoElapsed.elapsed() / 1000;
        const quint64 dropped = videoRecorder->droppedFrameCount();
        const QString dropText = dropped ? tr( " · пропущено %1 кадров" ).arg( dropped ) : QString();
        statusBar()->showMessage( tr( "Запись MP4 · %1 с · %2 кадр/с%3" ).arg( seconds ).arg( videoFps ).arg( dropText ) );
    }
}

void MainWindow::openRecording( const QString &path ) {
    QString error;
    if ( !rawPlayback->open( path, &error ) ) {
        QMessageBox::critical( this, tr( "Воспроизведение OHRAW" ), error );
        return;
    }
    resumeSamplingAfterPlayback = ui->actionSampling->isChecked();
    if ( resumeSamplingAfterPlayback )
        dsoControl->enableSamplingUI( false );

    auto *dialog = new QDialog( this );
    dialog->setAttribute( Qt::WA_DeleteOnClose );
    dialog->setWindowTitle( tr( "Запись сигнала — %1" ).arg( QFileInfo( path ).fileName() ) );
    dialog->resize( 520, 110 );
    auto *layout = new QVBoxLayout( dialog );
    auto *position = new QSlider( Qt::Horizontal, dialog );
    position->setRange( 0, qMax( 0, rawPlayback->frameCount() - 1 ) );
    auto *controls = new QHBoxLayout;
    auto *playPause = new QPushButton( tr( "Воспроизвести" ), dialog );
    auto *close = new QPushButton( tr( "Закрыть" ), dialog );
    auto *frameLabel = new QLabel( tr( "0 / %1" ).arg( rawPlayback->frameCount() ), dialog );
    controls->addWidget( playPause );
    controls->addWidget( position, 1 );
    controls->addWidget( frameLabel );
    controls->addWidget( close );
    layout->addLayout( controls );
    connect( playPause, &QPushButton::clicked, dialog, [ this, playPause ] {
        if ( rawPlayback->isPlaying() ) {
            rawPlayback->pause();
            playPause->setText( tr( "Воспроизвести" ) );
        } else {
            rawPlayback->play();
            playPause->setText( tr( "Пауза" ) );
        }
    } );
    connect( close, &QPushButton::clicked, dialog, &QDialog::accept );
    connect( position, &QSlider::sliderMoved, rawPlayback, &RawPlaybackEngine::seek );
    connect( rawPlayback, &RawPlaybackEngine::positionChanged, dialog,
             [ position, frameLabel ]( int frame, int count ) {
                 const QSignalBlocker blocker( position );
                 position->setMaximum( qMax( 0, count - 1 ) );
                 position->setValue( frame );
                 frameLabel->setText( QStringLiteral( "%1 / %2" ).arg( frame + 1 ).arg( count ) );
             } );
    connect( rawPlayback, &RawPlaybackEngine::playbackChanged, playPause,
             [ playPause ]( bool playing ) { playPause->setText( playing ? tr( "Пауза" ) : tr( "Воспроизвести" ) ); } );
    connect( rawPlayback, &RawPlaybackEngine::dataGapSkipped, dialog, [ this ]( quint32 blocks ) {
        statusBar()->showMessage( tr( "В исходной записи пропущено блоков: %1" ).arg( blocks ), 7000 );
    } );
    connect( dialog, &QDialog::finished, this, [ this ] {
        rawPlayback->close();
        if ( resumeSamplingAfterPlayback )
            dsoControl->enableSamplingUI( true );
        resumeSamplingAfterPlayback = false;
    } );
    dialog->show();
    rawPlayback->play();
}


void MainWindow::showNewData( std::shared_ptr< PPresult > newData ) {
    if ( dsoSettings->scope.verboseLevel > 5 )
        qDebug() << "     MainWindow::showNewData()" << newData->tag;
    dsoWidget->showNew( newData );
}


void MainWindow::exporterStatusChanged( const QString &exporterName, const QString &status ) {
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   MainWindow::exporterStatusChanged()" << exporterName << status;
    ui->statusbar->showMessage( tr( "%1: %2" ).arg( exporterName, status ) );
}


void MainWindow::exporterProgressChanged() {
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   MainWindow::exporterProgressChanged()";
    exporterRegistry->checkForWaitingExporters();
}


void MainWindow::deviceConnectionStateChanged( DeviceConnectionState state, const QString &message ) {
    const bool available = state == DeviceConnectionState::Connected;

    setDeviceCommandUiEnabled( available );

    if ( !available ) {
        QSignalBlocker samplingBlocker( ui->actionSampling );
        QSignalBlocker calibrationBlocker( ui->actionCalibrateOffset );
        QSignalBlocker manualCommandBlocker( ui->actionManualCommand );
        ui->actionSampling->setChecked( false );
        ui->actionSampling->setIcon( iconPlay );
        ui->actionSampling->setText( tr( "Пуск" ) );
        ui->actionSampling->setStatusTip( tr( "Oscilloscope unavailable" ) );
        ui->actionCalibrateOffset->setChecked( false );
        ui->actionManualCommand->setChecked( false );
        if ( commandEdit )
            commandEdit->hide();
    }

    if ( !message.isEmpty() )
        statusBar()->showMessage( message, available ? 3000 : 0 );
}


void MainWindow::setDeviceCommandUiEnabled( bool enabled ) {
    for ( QWidget *widget : deviceCommandWidgets ) {
        if ( widget )
            widget->setEnabled( enabled );
    }

    for ( QAction *action : deviceCommandActions ) {
        if ( action )
            action->setEnabled( enabled );
    }

    if ( commandEdit )
        commandEdit->setEnabled( enabled );
}


// make screenshot (type == SCREENSHOT) from the complete program window with screen colors ...
// ... or a printable hardcopy (type == HARDCOPY) from the scope widget only ...
// ... with printer colors and scaled to double height for zoomed screens ...
// ... or send this hardcopy directly to the printer (type == PRINTER).
// autoSafe == true -> do not ask for filename, save as PNG with default name into active directory
void MainWindow::screenShot( screenshotType_t screenshotType, bool autoSafe ) {
    if ( dsoSettings->scope.verboseLevel > 2 )
        qDebug() << "  MainWindow::screenShot()" << screenshotType << autoSafe;
    QWidget *activeWindow = screenshotType == SCREENSHOT ? static_cast< QWidget * >( this )
                                                         : static_cast< QWidget * >( dsoWidget );
    QPixmap screenshot( activeWindow->size() ); // prepare a pixmap with the correct size
    int sw = screenshot.width();
    int sh = screenshot.height();
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   screenshot size:" << sw << "x" << sh;

    int exportScale = dsoSettings->view.exportScaleValue;
    if ( exportScale > 1 ) { // upscale - e.g. for HiDPI downscaled screen
        screenshot.setDevicePixelRatio( exportScale );
        screenshot = screenshot.scaled( sw *= exportScale, sh *= exportScale );
        if ( dsoSettings->scope.verboseLevel > 3 )
            qDebug() << "   screenshot size scaled:" << sw << "x" << sh;
    }

    QDateTime now = QDateTime::currentDateTime();
    QString docName = now.toString( tr( "yyyy-MM-dd hh:mm:ss" ) );
    QString fileName = now.toString( tr( "yyyyMMdd_hhmmss" ) );
    fileName = QDir( lastSaveAsDir ).absoluteFilePath( fileName );
    if ( autoSafe && screenshotType == SCREENSHOT && experimentManager ) {
        QString error;
        const QString experimentPath = experimentManager->nextImagePath( &error );
        if ( experimentPath.isEmpty() ) {
            QMessageBox::critical( this, tr( "Скриншот" ), error );
            return;
        }
        fileName = experimentPath;
    }
    statusBar()->showMessage( docName ); // show date in bottom line

    if ( screenshotType != SCREENSHOT && dsoSettings->view.zoom && dsoSettings->view.zoomImage &&
         dsoSettings->view.zoomHeightIndex == 0 ) {
        screenshot = screenshot.scaled( sw, sh *= 2 ); // make double height for 1:1 zoomed screen
    }

    activeWindow->render( &screenshot ); // take the screenshot
    statusBar()->clearMessage();         // remove bottom line
    dsoWidget->restoreScreenColors();    // switch back to normal colors (for HARDCOPY/PRINTER)

    // here we have a screeshot, now handle the different destinations.
    QPrinter printer( QPrinter::HighResolution );
#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 10, 0 ) )
    printer.setPdfVersion( QPrinter::PdfVersion_A1b );
#endif
#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 ) )
    printer.setPageSize( QPageSize( QPageSize::A4 ) );
#else
    printer.setPageSize( QPrinter::A4 );
#endif
    printer.setPageMargins( QMarginsF( 20, 20, 20, 20 ), QPageLayout::Millimeter );
    printer.setPageOrientation( sw > sh ? QPageLayout::Landscape : QPageLayout::Portrait );
    printer.setCreator( QCoreApplication::applicationName() );
    printer.setDocName( docName );

    if ( screenshotType != PRINTER ) { // save under filename
        QStringList filters;
        fileName += ".png";
        if ( autoSafe ) { // save under default name as PNG without asking
            if ( !screenshot.save( fileName ) )
                QMessageBox::critical( this, QCoreApplication::applicationName(),
                                       tr( "Write error\n%1" ).arg( QFileInfo{ fileName }.absoluteFilePath() ) );
            else if ( screenshotType == SCREENSHOT && experimentManager ) {
                QJsonObject metadata;
                metadata.insert( QStringLiteral( "schema" ), QStringLiteral( "org.openhantek.screenshot/1" ) );
                metadata.insert( QStringLiteral( "createdAtUtc" ), QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) );
                metadata.insert( QStringLiteral( "application" ), QCoreApplication::applicationName() );
                metadata.insert( QStringLiteral( "applicationVersion" ), QCoreApplication::applicationVersion() );
                metadata.insert( QStringLiteral( "capture" ), QStringLiteral( "main-window" ) );
                metadata.insert( QStringLiteral( "width" ), screenshot.width() );
                metadata.insert( QStringLiteral( "height" ), screenshot.height() );
                QSaveFile sidecar( QFileInfo( fileName ).dir().filePath( QFileInfo( fileName ).completeBaseName() + ".json" ) );
                const QByteArray bytes = QJsonDocument( metadata ).toJson( QJsonDocument::Indented );
                if ( sidecar.open( QIODevice::WriteOnly ) && sidecar.write( bytes ) == bytes.size() && sidecar.commit() ) {
                    QString error;
                    if ( !experimentManager->registerImage( fileName, &error ) )
                        statusBar()->showMessage( tr( "Скриншот сохранён, но не добавлен в библиотеку: %1" ).arg( error ), 10000 );
                    else
                        experimentCatalog->startRescan();
                } else {
                    QMessageBox::warning( this, tr( "Метаданные скриншота" ),
                                          tr( "Изображение сохранено, но JSON-метаданные записать не удалось: %1" )
                                              .arg( sidecar.errorString() ) );
                }
            }
            return;
        }
        filters << tr( "Image (*.png *.jpg)" ) << tr( "Portable Document Format (*.pdf)" );
        QFileDialog fileDialog( this, tr( "Save Screenshot" ), fileName, filters.join( ";;" ) );
        fileDialog.setOption( QFileDialog::DontUseNativeDialog );
        fileDialog.setAcceptMode( QFileDialog::AcceptSave );
        if ( fileDialog.exec() != QDialog::Accepted )
            return;
        lastSaveAsDir = fileDialog.directory().absolutePath();
        fileName = fileDialog.selectedFiles().first();
        if ( filters.indexOf( fileDialog.selectedNameFilter() ) == 0 ) { // save as image
            if ( !screenshot.save( fileName ) )
                QMessageBox::critical( this, QCoreApplication::applicationName(),
                                       tr( "Write error\n%1" ).arg( QFileInfo{ fileName }.absoluteFilePath() ) );
            return;
        }

        // else create a *.pdf with a scaled and centered image
        printer.setOutputFormat( QPrinter::PdfFormat );
        printer.setOutputFileName( fileName );
        // supports screen resolution up to about 9600 x 9600 pixel
        int resolution = 75;
        printer.setResolution( resolution );
        int pw = printer.pageLayout().paintRectPixels( resolution ).width();
        int ph = printer.pageLayout().paintRectPixels( resolution ).height();
        int scale = qMin( pw / sw, ph / sh );
        while ( scale < 2 && resolution < 1200 ) {
            resolution *= 2;
            printer.setResolution( resolution );
            pw = printer.pageLayout().paintRectPixels( resolution ).width();
            ph = printer.pageLayout().paintRectPixels( resolution ).height();
            scale = qMin( pw / sw, ph / sh );
        }
    } else { // Show the printing dialog
        printer.setDocName( fileName + ".pdf" );
        QPrintDialog dialog( &printer );
        dialog.setWindowTitle( tr( "Print oscillograph" ) );
        if ( dialog.exec() != QDialog::Accepted ) {
            return;
        }
    }
    // send the pixmap to *.pdf or printer
    int pw = printer.pageLayout().paintRectPixels( printer.resolution() ).width();
    int ph = printer.pageLayout().paintRectPixels( printer.resolution() ).height();

    int scale = qMin( pw / sw, ph / sh );

    // qDebug() << sw << sh << pw << ph << scale << resolution;
    if ( scale < 1 )
        qDebug() << "Screenshot size too big, page will be cropped";
    else if ( scale > 1 ) // upscale accordingly
        screenshot = screenshot.scaled( sw *= scale, sh *= scale );
    printer.newPage();
    QPainter p( &printer );
    p.drawPixmap( ( pw - sw ) / 2, ( ph - sh ) / 2, screenshot ); // center the picture
    p.end();
}


bool MainWindow::openDocument( QString docName ) {
    QUrl url;
    if ( QFile( DocPath + docName ).exists() )
        url = QUrl::fromLocalFile( QFileInfo( DocPath + docName ).absoluteFilePath() );
    else
        url = QUrl( DocUrl + docName );
    if ( dsoSettings->scope.verboseLevel > 2 )
        qDebug() << "  " << url;
    return QDesktopServices::openUrl( url );
}


/// \brief Save the settings before exiting.
/// \param event The close event that should be handled.
void MainWindow::closeEvent( QCloseEvent *event ) {
    if ( dsoSettings->scope.verboseLevel > 2 )
        qDebug() << "  MainWindow::closeEvent()";
    if ( rawRecorder && rawRecorder->isBusy() ) {
        event->ignore();
        closingAfterRecording = true;
        recordDataAction->setEnabled( false );
        if ( rawRecorder->isRecording() )
            rawRecorder->stop();
        statusBar()->showMessage( tr( "Завершение файла записи перед закрытием…" ) );
        return;
    }
    if ( videoRecorder && videoRecorder->isBusy() ) {
        videoFrameTimer->stop();
        QString error;
        if ( !videoRecorder->stop( &error ) )
            qWarning() << "Could not finalize MP4 while closing:" << error;
        else {
            const QString path = videoRecorder->filePath();
            experimentManager->registerVideo( path );
        }
    }
    if ( dsoSettings->alwaysSave ) {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState( 2 );
        dsoSettings->save();
    }
    QMainWindow::closeEvent( event );
}
