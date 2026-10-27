// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "post/ppresult.h"
#include <QElapsedTimer>
#include <QLineEdit>
#include <QList>
#include <QMainWindow>
#include <memory>

#include "scopesettings.h"
#include "usb/devicereconnectionsupervisor.h"

class SpectrumGenerator;
class HantekDsoControl;
class DsoSettings;
class ExporterRegistry;
class DsoWidget;
class HorizontalDock;
class TriggerDock;
class SpectrumDock;
class VoltageDock;
class QAction;
class QDockWidget;
class QToolButton;
class QTimer;
class MediaLibraryWidget;
class Mp4VideoRecorder;
class AnalysisWorkspace;
class QTabWidget;
class QEvent;
class QMenu;
class QTabBar;

/// Replace the active application and Qt UI translations at runtime.
void changeApplicationLanguage( const QString &language );

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
                         QWidget *parent = nullptr );
    ~MainWindow() override;
    QElapsedTimer elapsedTime;

  public slots:
    void showNewData( std::shared_ptr< PPresult > newData );
    void exporterStatusChanged( const QString &exporterName, const QString &status );
    void exporterProgressChanged();
    void deviceConnectionStateChanged( DeviceConnectionState state, const QString &message );

  protected:
    void closeEvent( QCloseEvent *event ) override;
    void changeEvent( QEvent *event ) override;

  private:
    Ui::MainWindow *ui;
    QIcon iconPause;
    QIcon iconPlay;
    QLineEdit *commandEdit = nullptr;
    QString lastSaveAsDir = "";

    // Central widgets
    DsoWidget *dsoWidget;
    QTabWidget *workspaceTabs = nullptr;
    AnalysisWorkspace *analysisWorkspace = nullptr;
    std::shared_ptr< PPresult > latestAnalysisData;
    MediaLibraryWidget *mediaLibrary = nullptr;
    QDockWidget *mediaLibraryDock = nullptr;
    QList< QDockWidget * > parameterDocks;
    QToolButton *libraryPanelHandle = nullptr;
    QToolButton *parameterPanelHandle = nullptr;
    QAction *libraryAction = nullptr;
    QAction *parametersAction = nullptr;
    QAction *videoAction = nullptr;
    QAction *snapshotAction = nullptr;
    QMenu *languageMenu = nullptr;
    QTabBar *workspaceSwitcher = nullptr;
    QString titleModelName;
    QString titleFirmware;
    bool titleRealDevice = false;
    void updateWindowTitle();
    QTimer *videoFrameTimer = nullptr;
    std::unique_ptr< Mp4VideoRecorder > videoRecorder;

    // Settings used for the whole program
    DsoSettings *dsoSettings;
    ExporterRegistry *exporterRegistry;

    // Taking screenshots
    enum screenshotType_t { SCREENSHOT, HARDCOPY, PRINTER };
    screenshotType_t screenshotType;
    void screenShot( screenshotType_t screenshotType = SCREENSHOT, bool autoSave = false );

    bool openDocument( QString docName );
    void setDeviceCommandUiEnabled( bool enabled );
    void updatePanelHandles();
    void toggleVideoRecording();
    void appendVideoFrame();

    QList< QWidget * > deviceCommandWidgets;
    QList< QAction * > deviceCommandActions;

  signals:
    void settingsLoaded( DsoSettingsScope *scope, const Dso::ControlSpecification *spec );
};
