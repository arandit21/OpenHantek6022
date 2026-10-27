// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QFrame>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <QVector>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;
class QMouseEvent;
class QWheelEvent;
class QToolButton;
class QPushButton;
class QEvent;
class QTimer;
class PPresult;
struct DsoSettingsScope;
struct DsoSettingsAnalysis;

class AnalysisPlot final : public QWidget {
    Q_OBJECT
  public:
    explicit AnalysisPlot( QWidget *parent = nullptr );
    void setCurves( const QString &xLabel, const QString &yLabel, const QVector< QVector< QPointF > > &curves,
                    const QVector< QColor > &colors, bool xMonotonic = true );
    void setSpectrogram( const QString &xLabel, const QString &yLabel, const QImage &image, double secondsPerRow,
                         double maxFrequencyHz );
    QImage renderImage( const QSize &size = QSize( 900, 480 ) ) const;
    void resetView();
    void setAxisLabels( const QString &horizontal, const QString &vertical );

  protected:
    void paintEvent( QPaintEvent *event ) override;
    void wheelEvent( QWheelEvent *event ) override;
    void mousePressEvent( QMouseEvent *event ) override;
    void mouseMoveEvent( QMouseEvent *event ) override;
    void mouseReleaseEvent( QMouseEvent *event ) override;
    void mouseDoubleClickEvent( QMouseEvent *event ) override;

  private:
    QString xLabel;
    QString yLabel;
    bool xIsFrequency = false;
    bool yIsFrequency = false;
    QVector< QVector< QPointF > > curves;
    QVector< QColor > colors;
    QRectF curveBounds;
    bool hasCurveBounds = false;
    bool curvesHaveMonotonicX = true;
    QImage spectrogram;
    double secondsPerRow = 0.0;
    double maxFrequencyHz = 0.0;
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    QPoint lastMouse;
    bool dragging = false;
};

class AnalysisPanel final : public QFrame {
    Q_OBJECT
  public:
    explicit AnalysisPanel( const QString &title, QWidget *parent = nullptr );
    AnalysisPlot *plot() const { return graph; }
    QImage renderImage( const QSize &size = QSize( 900, 480 ) ) const;
    void setMaximized( bool maximized );
    void setTitle( const QString &title );

  protected:
    void changeEvent( QEvent *event ) override;

  signals:
    void maximizeRequested( AnalysisPanel *panel, bool maximized );

  private:
    AnalysisPlot *graph;
    QLabel *titleLabel = nullptr;
    QToolButton *autoButton = nullptr;
    QToolButton *maximizeButton;
    bool panelMaximized = false;
};

class AnalysisWorkspace final : public QWidget {
    Q_OBJECT
  public:
    AnalysisWorkspace( const DsoSettingsScope *scope, const DsoSettingsAnalysis *fftSettings, const QString &modelName,
                       bool realDevice, QWidget *parent = nullptr );
    ~AnalysisWorkspace() override;
    void setData( const std::shared_ptr< PPresult > &data );

  protected:
    void changeEvent( QEvent *event ) override;

  private slots:
    void exportAnalysis();
    void setMaximizedPanel( AnalysisPanel *panel, bool maximized );

  private:
    struct SpectrogramRow {
        double timeSeconds = 0.0;
        QVector< float > levelsDbV;
    };
    void rebuildGrid();
    void refreshLatestData();
    void scheduleSpectrogram();
    void acceptSpectrogram( int channel, double rowDurationSeconds, double maxFrequency,
                            const QVector< SpectrogramRow > &rows );

    const DsoSettingsScope *scope;
    const DsoSettingsAnalysis *fftSettings;
    QString modelName;
    bool realDevice;
    QComboBox *spectrogramChannel;
    QLabel *dataStatus;
    QLabel *titleLabel = nullptr;
    QLabel *spectrogramLabel = nullptr;
    QPushButton *exportButton = nullptr;
    QGridLayout *grid;
    QTimer *refreshTimer;
    std::array< AnalysisPanel *, 4 > panels;
    AnalysisPanel *maximizedPanel = nullptr;
    std::shared_ptr< PPresult > latestData;
    QVector< SpectrogramRow > spectrogramRows;
    double spectrogramRowSeconds = 0.0;
    double spectrogramMaxFrequency = 0.0;
    double accumulatedSampleTime = 0.0;
    double latestBlockBaseTime = 0.0;
    int displayedStftChannel = 0;
    quint32 displayedBlockTag = 0;
    int displayedSampleCount = 0;
    bool hasDisplayedBlock = false;
    quint32 lastScheduledStftTag = 0;
    int lastScheduledStftChannel = -1;
    std::atomic_bool stftBusy = false;
    bool stftNeedsRefresh = false;
};
