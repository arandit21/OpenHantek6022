// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "post/ppresult.h"
#include <QElapsedTimer>
#include <QIcon>
#include <QLineEdit>
#include <QList>
#include <QMainWindow>
#include <QSize>
#include <memory>

#include "scopesettings.h"
#include "usb/devicereconnectionsupervisor.h"

class SpectrumGenerator;
class HantekDsoControl;
class DsoSettings;
class ExporterRegistry;
class ExperimentManager;
class ExperimentCatalog;
class RawRecordingEngine;
class RawPlaybackEngine;
class ExperimentLibraryWidget;
class Mp4VideoRecorder;
class QDockWidget;
class QResizeEvent;
class QTimer;
class QToolButton;
class DsoWidget;
class HorizontalDock;
class TriggerDock;
class SpectrumDock;
class VoltageDock;
class QAction;

namespace Ui {
class MainWindow;
}

/// \brief The main window of the application.
/// The main window contains the classic oszilloscope-screen and the gui
/// elements used to control the oszilloscope.
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow( HantekDsoControl *dsoControl, DsoSettings *dsoSettings, ExporterRegistry *exporterRegistry,
                         ExperimentManager *experimentManager, ExperimentCatalog *experimentCatalog,
                         RawRecordingEngine *rawRecorder, RawPlaybackEngine *rawPlayback, QWidget *parent = nullptr );
    ~MainWindow() override;
    QElapsedTimer elapsedTime;

  public slots:
    void showNewData( std::shared_ptr< PPresult > newData );
    void exporterStatusChanged( const QString &exporterName, const QString &status );
    void exporterProgressChanged();
    void deviceConnectionStateChanged( DeviceConnectionState state, const QString &message );

  protected:
    void closeEvent( QCloseEvent *event ) override;
    void resizeEvent( QResizeEvent *event ) override;

  private:
    Ui::MainWindow *ui;
    QIcon iconPause;
    QIcon iconPlay;
    QLineEdit *commandEdit = nullptr;
    QString lastSaveAsDir = "";

    // Central widgets
    DsoWidget *dsoWidget;

    // Settings used for the whole program
    DsoSettings *dsoSettings;
    ExporterRegistry *exporterRegistry;
    HantekDsoControl *dsoControl;
    ExperimentManager *experimentManager;
    ExperimentCatalog *experimentCatalog;
    RawRecordingEngine *rawRecorder;
    RawPlaybackEngine *rawPlayback;
    std::unique_ptr< Mp4VideoRecorder > videoRecorder;
    ExperimentLibraryWidget *libraryWidget = nullptr;
    QDockWidget *libraryDock = nullptr;
    QList< QDockWidget * > parameterDocks;
    QToolButton *libraryRevealButton = nullptr;
    QToolButton *parameterRevealButton = nullptr;
    QTimer *videoFrameTimer = nullptr;
    QAction *recordDataAction = nullptr;
    QAction *recordVideoAction = nullptr;
    QIcon dataRecordIcon;
    QIcon videoRecordIcon;
    bool resumeSamplingAfterPlayback = false;
    int videoFps = 30;
    QSize videoSize;
    QElapsedTimer videoElapsed;
    bool closingAfterRecording = false;

    // Taking screenshots
    enum screenshotType_t { SCREENSHOT, HARDCOPY, PRINTER };
    screenshotType_t screenshotType;
    void screenShot( screenshotType_t screenshotType = SCREENSHOT, bool autoSave = false );
    void toggleDataRecording();
    void toggleVideoRecording();
    void appendVideoFrame();
    void openRecording( const QString &path );
    void updatePanelHandles();

    bool openDocument( QString docName );
    void setDeviceCommandUiEnabled( bool enabled );

    QList< QWidget * > deviceCommandWidgets;
    QList< QAction * > deviceCommandActions;

  signals:
    void settingsLoaded( DsoSettingsScope *scope, const Dso::ControlSpecification *spec );
};
