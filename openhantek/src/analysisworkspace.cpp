// SPDX-License-Identifier: GPL-2.0-or-later

#include "analysisworkspace.h"

#include <QComboBox>
#include <QDateTime>
#include <QDataStream>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QThreadPool>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#include "post/analysissettings.h"
#include "post/ppresult.h"
#include "scopesettings.h"
#include "viewconstants.h"

namespace {
constexpr int kPlotLeft = 72;
constexpr int kPlotTop = 14;
constexpr int kPlotRight = 34;
constexpr int kPlotBottom = 45;
constexpr int kHistoryLimit = 512;

QColor heatColor( double value ) {
    const double v = qBound( 0.0, value, 1.0 );
    if ( v < 0.25 )
        return QColor::fromRgbF( 0.0, 4.0 * v, 0.55 + 1.8 * v );
    if ( v < 0.5 )
        return QColor::fromRgbF( 0.0, 1.0, 1.0 - 2.0 * ( v - 0.25 ) );
    if ( v < 0.75 )
        return QColor::fromRgbF( 4.0 * ( v - 0.5 ), 1.0, 0.0 );
    return QColor::fromRgbF( 1.0, 1.0 - 3.0 * ( v - 0.75 ), 0.0 );
}

QString axisValue( double value, bool frequency ) {
    if ( frequency && std::abs( value ) >= 1000.0 )
        return QString::number( value / 1000.0, 'g', 3 ) + QStringLiteral( "k" );
    return QString::number( value, 'g', 3 );
}

void writeU16( QDataStream &stream, quint16 value ) { stream << value; }
void writeU32( QDataStream &stream, quint32 value ) { stream << value; }

quint32 crc32( const QByteArray &bytes ) {
    quint32 crc = 0xffffffffU;
    for ( const unsigned char byte : bytes ) {
        crc ^= byte;
        for ( int bit = 0; bit < 8; ++bit )
            crc = ( crc >> 1 ) ^ ( 0xedb88320U & quint32( -( qint32( crc & 1U ) ) ) );
    }
    return ~crc;
}

struct ZipEntry {
    QByteArray name;
    QByteArray content;
    quint32 crc = 0;
    quint32 offset = 0;
};

bool writeZip( const QString &path, const QVector< QPair< QString, QByteArray > > &files, QString *error ) {
    QSaveFile file( path );
    if ( !file.open( QIODevice::WriteOnly ) ) {
        if ( error )
            *error = file.errorString();
        return false;
    }
    QDataStream out( &file );
    out.setByteOrder( QDataStream::LittleEndian );
    QVector< ZipEntry > entries;
    entries.reserve( files.size() );
    for ( const auto &item : files ) {
        ZipEntry entry{ item.first.toUtf8(), item.second, crc32( item.second ), quint32( file.pos() ) };
        writeU32( out, 0x04034b50U );
        writeU16( out, 20 ); writeU16( out, 0 ); writeU16( out, 0 );
        writeU16( out, 0 ); writeU16( out, 0 );
        writeU32( out, entry.crc ); writeU32( out, quint32( entry.content.size() ) );
        writeU32( out, quint32( entry.content.size() ) );
        writeU16( out, quint16( entry.name.size() ) ); writeU16( out, 0 );
        out.writeRawData( entry.name.constData(), entry.name.size() );
        out.writeRawData( entry.content.constData(), entry.content.size() );
        entries.push_back( std::move( entry ) );
    }

    const quint32 centralOffset = quint32( file.pos() );
    for ( const ZipEntry &entry : entries ) {
        writeU32( out, 0x02014b50U );
        writeU16( out, 20 ); writeU16( out, 20 ); writeU16( out, 0 ); writeU16( out, 0 );
        writeU16( out, 0 ); writeU16( out, 0 );
        writeU32( out, entry.crc ); writeU32( out, quint32( entry.content.size() ) );
        writeU32( out, quint32( entry.content.size() ) );
        writeU16( out, quint16( entry.name.size() ) ); writeU16( out, 0 ); writeU16( out, 0 );
        writeU16( out, 0 ); writeU16( out, 0 ); writeU32( out, 0 ); writeU32( out, entry.offset );
        out.writeRawData( entry.name.constData(), entry.name.size() );
    }
    const quint32 centralSize = quint32( file.pos() ) - centralOffset;
    writeU32( out, 0x06054b50U );
    writeU16( out, 0 ); writeU16( out, 0 );
    writeU16( out, quint16( entries.size() ) ); writeU16( out, quint16( entries.size() ) );
    writeU32( out, centralSize ); writeU32( out, centralOffset ); writeU16( out, 0 );
    if ( out.status() != QDataStream::Ok || !file.commit() ) {
        if ( error )
            *error = file.errorString().isEmpty() ? QObject::tr( "Не удалось записать ZIP-архив." ) : file.errorString();
        return false;
    }
    return true;
}

QByteArray csvNumber( double value ) {
    return QString::number( value, 'g', 12 ).toLatin1();
}

QByteArray imagePng( const QImage &image ) {
    QByteArray data;
    QBuffer buffer( &data );
    buffer.open( QIODevice::WriteOnly );
    image.save( &buffer, "PNG" );
    return data;
}

int fftSizeFor( int count ) {
    const int limit = std::min( count, 8192 );
    int size = 1;
    while ( size * 2 <= limit )
        size *= 2;
    return size;
}

int stftSizeFor( int count ) {
    const int limit = std::min( count, 4096 );
    int size = 1;
    while ( size * 2 <= limit )
        size *= 2;
    return size;
}

void fftInPlace( QVector< std::complex< double > > &values ) {
    const int n = values.size();
    for ( int i = 1, j = 0; i < n; ++i ) {
        int bit = n >> 1;
        for ( ; j & bit; bit >>= 1 )
            j ^= bit;
        j ^= bit;
        if ( i < j )
            std::swap( values[ i ], values[ j ] );
    }
    for ( int length = 2; length <= n; length <<= 1 ) {
        const std::complex< double > root = std::polar( 1.0, -2.0 * M_PI / length );
        for ( int start = 0; start < n; start += length ) {
            std::complex< double > factor( 1.0, 0.0 );
            for ( int i = 0; i < length / 2; ++i ) {
                const auto even = values[ start + i ];
                const auto odd = factor * values[ start + i + length / 2 ];
                values[ start + i ] = even + odd;
                values[ start + i + length / 2 ] = even - odd;
                factor *= root;
            }
        }
    }
}
} // namespace

AnalysisPlot::AnalysisPlot( QWidget *parent ) : QWidget( parent ) {
    setMinimumSize( 260, 170 );
    setMouseTracking( true );
    setFocusPolicy( Qt::StrongFocus );
}

void AnalysisPlot::setAxisLabels( const QString &horizontal, const QString &vertical ) {
    xLabel = horizontal;
    yLabel = vertical;
    const QString frequencyLabel = QCoreApplication::translate( "AnalysisWorkspace", "Частота, Гц" );
    xIsFrequency = xLabel == frequencyLabel;
    yIsFrequency = yLabel == frequencyLabel;
    update();
}

void AnalysisPlot::setCurves( const QString &xAxis, const QString &yAxis,
                              const QVector< QVector< QPointF > > &newCurves, const QVector< QColor > &newColors,
                              bool xMonotonic ) {
    setAxisLabels( xAxis, yAxis );
    curves = newCurves;
    colors = newColors;
    curvesHaveMonotonicX = xMonotonic;
    hasCurveBounds = false;
    for ( const auto &curve : curves ) {
        for ( const QPointF &point : curve ) {
            if ( !std::isfinite( point.x() ) || !std::isfinite( point.y() ) )
                continue;
            if ( !hasCurveBounds ) {
                curveBounds = QRectF( point.x(), point.y(), 0.0, 0.0 );
                hasCurveBounds = true;
            } else {
                const double left = std::min( curveBounds.left(), point.x() );
                const double right = std::max( curveBounds.right(), point.x() );
                const double top = std::min( curveBounds.top(), point.y() );
                const double bottom = std::max( curveBounds.bottom(), point.y() );
                curveBounds = QRectF( QPointF( left, top ), QPointF( right, bottom ) );
            }
        }
    }
    spectrogram = QImage();
    update();
}

void AnalysisPlot::setSpectrogram( const QString &xAxis, const QString &yAxis, const QImage &image,
                                   double rowDuration, double maxFrequency ) {
    setAxisLabels( xAxis, yAxis );
    spectrogram = image;
    secondsPerRow = rowDuration;
    maxFrequencyHz = maxFrequency;
    hasCurveBounds = false;
    curves.clear();
    colors.clear();
    update();
}

void AnalysisPlot::resetView() {
    zoom = 1.0;
    panX = 0.0;
    panY = 0.0;
    update();
}

QImage AnalysisPlot::renderImage( const QSize &size ) const {
    Q_UNUSED( size );
    const QSize imageSize = this->size().isEmpty() ? QSize( 900, 480 ) : this->size();
    QImage image( imageSize, QImage::Format_ARGB32_Premultiplied );
    image.fill( QColor( "#f8fafc" ) );
    QPainter painter( &image );
    const_cast< AnalysisPlot * >( this )->render( &painter );
    painter.end();
    return image;
}

void AnalysisPlot::paintEvent( QPaintEvent * ) {
    QPainter painter( this );
    painter.setRenderHint( QPainter::Antialiasing, false );
    painter.fillRect( rect(), QColor( "#f8fafc" ) );
    QRectF plotRect = rect().adjusted( kPlotLeft, kPlotTop, -kPlotRight, -kPlotBottom );
    if ( plotRect.width() < 10 || plotRect.height() < 10 )
        return;

    double xMin = 0.0, xMax = 1.0, yMin = 0.0, yMax = 1.0;
    if ( !spectrogram.isNull() ) {
        xMax = std::max( secondsPerRow * spectrogram.width(), 1e-9 );
        yMax = std::max( maxFrequencyHz, 1e-9 );
    } else {
        if ( hasCurveBounds ) {
            xMin = curveBounds.left();
            xMax = curveBounds.right();
            yMin = curveBounds.top();
            yMax = curveBounds.bottom();
        } else {
            yMin = -1.0;
            yMax = 1.0;
        }
    }
    if ( xMax - xMin < 1e-12 ) { xMin -= 0.5; xMax += 0.5; }
    if ( yMax - yMin < 1e-12 ) { yMin -= 0.5; yMax += 0.5; }
    const double xBase = xMax - xMin, yBase = yMax - yMin;
    const double xCenter = ( xMin + xMax ) * 0.5 + panX * xBase;
    const double yCenter = ( yMin + yMax ) * 0.5 + panY * yBase;
    const double xRange = xBase / zoom, yRange = yBase / zoom;
    xMin = xCenter - xRange * 0.5; xMax = xCenter + xRange * 0.5;
    yMin = yCenter - yRange * 0.5; yMax = yCenter + yRange * 0.5;

    painter.fillRect( plotRect, Qt::white );
    painter.setFont( QFont( font().family(), 8 ) );
    for ( int tick = 0; tick <= 10; ++tick ) {
        const double xf = double( tick ) / 10.0;
        const double yf = double( tick ) / 10.0;
        const qreal x = plotRect.left() + xf * plotRect.width();
        const qreal y = plotRect.bottom() - yf * plotRect.height();
        painter.setPen( QPen( QColor( "#dbe3ec" ), 1, Qt::DotLine ) );
        painter.drawLine( QPointF( x, plotRect.top() ), QPointF( x, plotRect.bottom() ) );
        painter.drawLine( QPointF( plotRect.left(), y ), QPointF( plotRect.right(), y ) );
        painter.setPen( QColor( "#475569" ) );
        if ( plotRect.width() >= 700 || tick % 2 == 0 )
            painter.drawText( QRectF( x - 34, plotRect.bottom() + 5, 68, 16 ), Qt::AlignHCenter | Qt::AlignTop,
                              axisValue( xMin + xf * xRange, xIsFrequency ) );
        painter.drawText( QRectF( 0, y - 8, kPlotLeft - 8, 16 ), Qt::AlignRight | Qt::AlignVCenter,
                          axisValue( yMin + yf * yRange, yIsFrequency ) );
    }

    if ( !spectrogram.isNull() ) {
        painter.save();
        painter.setClipRect( plotRect );
        painter.drawImage( plotRect, spectrogram );
        painter.restore();
        QRectF bar( plotRect.right() + 9, plotRect.top(), 10, plotRect.height() );
        QLinearGradient gradient( bar.topLeft(), bar.bottomLeft() );
        gradient.setColorAt( 0.0, heatColor( 1.0 ) );
        gradient.setColorAt( 0.25, heatColor( 0.75 ) );
        gradient.setColorAt( 0.5, heatColor( 0.5 ) );
        gradient.setColorAt( 0.75, heatColor( 0.25 ) );
        gradient.setColorAt( 1.0, heatColor( 0.0 ) );
        painter.fillRect( bar, gradient );
        painter.save();
        painter.translate( width() - 9, plotRect.center().y() );
        painter.rotate( -90 );
        painter.setPen( QColor( "#334155" ) );
        painter.drawText( QRectF( -plotRect.height() / 2, -10, plotRect.height(), 18 ), Qt::AlignCenter,
                          tr( "Интенсивность, дБВ" ) );
        painter.restore();
    } else {
        painter.save();
        painter.setClipRect( plotRect.adjusted( -1, -1, 1, 1 ) );
        // The plots are much narrower than an acquisition block. Reduce the
        // number of vertices to screen resolution while preserving local
        // peaks for time and frequency curves. XY traces use uniform sampling
        // because their x values are not ordered.
        const int maxPoints = std::min( 200000, std::max( 256, int( plotRect.width() * 2.0 * zoom ) ) );
        for ( int c = 0; c < curves.size(); ++c ) {
            const auto &curve = curves[ c ];
            if ( curve.isEmpty() )
                continue;
            QPainterPath path;
            const int stride = std::max( 1, int( std::ceil( double( curve.size() ) / maxPoints ) ) );
            bool started = false;
            auto appendPoint = [&]( const QPointF &point ) {
                if ( !std::isfinite( point.x() ) || !std::isfinite( point.y() ) )
                    return;
                const QPointF mapped( plotRect.left() + ( point.x() - xMin ) / xRange * plotRect.width(),
                                      plotRect.bottom() - ( point.y() - yMin ) / yRange * plotRect.height() );
                if ( !started ) { path.moveTo( mapped ); started = true; } else path.lineTo( mapped );
            };
            if ( stride == 1 ) {
                for ( const QPointF &point : curve )
                    appendPoint( point );
            } else if ( curvesHaveMonotonicX ) {
                for ( int first = 0; first < curve.size(); first += stride ) {
                    const int end = std::min( int( curve.size() ), first + stride );
                    int minIndex = first;
                    int maxIndex = first;
                    for ( int index = first + 1; index < end; ++index ) {
                        if ( std::isfinite( curve[ index ].y() ) ) {
                            if ( !std::isfinite( curve[ minIndex ].y() ) || curve[ index ].y() < curve[ minIndex ].y() )
                                minIndex = index;
                            if ( !std::isfinite( curve[ maxIndex ].y() ) || curve[ index ].y() > curve[ maxIndex ].y() )
                                maxIndex = index;
                        }
                    }
                    if ( minIndex <= maxIndex ) {
                        appendPoint( curve[ minIndex ] );
                        if ( maxIndex != minIndex )
                            appendPoint( curve[ maxIndex ] );
                    } else {
                        appendPoint( curve[ maxIndex ] );
                        appendPoint( curve[ minIndex ] );
                    }
                }
            } else {
                for ( int index = 0; index < curve.size(); index += stride )
                    appendPoint( curve[ index ] );
                if ( ( curve.size() - 1 ) % stride != 0 )
                    appendPoint( curve.last() );
            }
            painter.setPen( QPen( colors.value( c, QColor( "#2563eb" ) ), 1.8, Qt::SolidLine, Qt::RoundCap,
                                  Qt::RoundJoin ) );
            painter.drawPath( path );
        }
        painter.restore();
    }

    painter.setPen( QPen( QColor( "#64748b" ), 1 ) );
    painter.drawRect( plotRect );
    painter.setPen( QColor( "#334155" ) );
    painter.drawText( QRectF( plotRect.left(), height() - 22, plotRect.width(), 18 ), Qt::AlignCenter, xLabel );
    painter.save();
    painter.translate( 14, plotRect.center().y() );
    painter.rotate( -90 );
    painter.drawText( QRectF( -plotRect.height() / 2, -10, plotRect.height(), 18 ), Qt::AlignCenter, yLabel );
    painter.restore();
}

void AnalysisPlot::wheelEvent( QWheelEvent *event ) {
    const double factor = std::pow( 1.2, event->angleDelta().y() / 120.0 );
    zoom = qBound( 0.2, zoom * factor, 100.0 );
    update();
    event->accept();
}

void AnalysisPlot::mousePressEvent( QMouseEvent *event ) {
    if ( event->button() == Qt::LeftButton ) { dragging = true; lastMouse = event->position().toPoint(); setCursor( Qt::ClosedHandCursor ); }
    QWidget::mousePressEvent( event );
}

void AnalysisPlot::mouseMoveEvent( QMouseEvent *event ) {
    if ( dragging ) {
        const QPoint current = event->position().toPoint();
        const QRectF plotRect = rect().adjusted( kPlotLeft, kPlotTop, -kPlotRight, -kPlotBottom );
        if ( plotRect.width() > 0 && plotRect.height() > 0 ) {
            panX -= double( current.x() - lastMouse.x() ) / plotRect.width() / zoom;
            panY += double( current.y() - lastMouse.y() ) / plotRect.height() / zoom;
            lastMouse = current;
            update();
        }
    }
    QWidget::mouseMoveEvent( event );
}

void AnalysisPlot::mouseReleaseEvent( QMouseEvent *event ) {
    if ( event->button() == Qt::LeftButton ) { dragging = false; unsetCursor(); }
    QWidget::mouseReleaseEvent( event );
}

void AnalysisPlot::mouseDoubleClickEvent( QMouseEvent *event ) { resetView(); event->accept(); }

AnalysisPanel::AnalysisPanel( const QString &title, QWidget *parent ) : QFrame( parent ), graph( new AnalysisPlot( this ) ) {
    setFrameShape( QFrame::StyledPanel );
    setStyleSheet( QStringLiteral( "AnalysisPanel { background:#ffffff; border:1px solid #cbd5e1; border-radius:6px; }" ) );
    auto *layout = new QVBoxLayout( this );
    layout->setContentsMargins( 6, 5, 6, 6 );
    layout->setSpacing( 3 );
    auto *header = new QHBoxLayout;
    titleLabel = new QLabel( title, this );
    titleLabel->setStyleSheet( QStringLiteral( "font-weight:600; color:#1e293b; border:0;" ) );
    header->addWidget( titleLabel );
    header->addStretch();
    autoButton = new QToolButton( this );
    autoButton->setText( tr( "Авто" ) );
    autoButton->setToolTip( tr( "Автоматический масштаб" ) );
    connect( autoButton, &QToolButton::clicked, graph, &AnalysisPlot::resetView );
    header->addWidget( autoButton );
    maximizeButton = new QToolButton( this );
    maximizeButton->setText( QStringLiteral( "□" ) );
    maximizeButton->setToolTip( tr( "Развернуть панель" ) );
    connect( maximizeButton, &QToolButton::clicked, this, [ this ] {
        const bool maximize = maximizeButton->text() == QStringLiteral( "□" );
        setMaximized( maximize );
        emit maximizeRequested( this, maximize );
    } );
    header->addWidget( maximizeButton );
    layout->addLayout( header );
    layout->addWidget( graph, 1 );
}

QImage AnalysisPanel::renderImage( const QSize &size ) const { return graph->renderImage( size ); }

void AnalysisPanel::setTitle( const QString &title ) {
    if ( titleLabel ) titleLabel->setText( title );
}

void AnalysisPanel::changeEvent( QEvent *event ) {
    QFrame::changeEvent( event );
    if ( event->type() != QEvent::LanguageChange || autoButton == nullptr ) return;
    autoButton->setText( tr( "Авто" ) );
    autoButton->setToolTip( tr( "Автоматический масштаб" ) );
    setMaximized( panelMaximized );
}

void AnalysisPanel::setMaximized( bool maximized ) {
    panelMaximized = maximized;
    maximizeButton->setText( maximized ? QStringLiteral( "↙" ) : QStringLiteral( "□" ) );
    maximizeButton->setToolTip( maximized ? tr( "Вернуть расположение 2×2" ) : tr( "Развернуть панель" ) );
}

AnalysisWorkspace::AnalysisWorkspace( const DsoSettingsScope *scopeSettings, const DsoSettingsAnalysis *analysisSettings,
                                      const QString &deviceModelName, bool hasRealDevice, QWidget *parent )
    : QWidget( parent ), scope( scopeSettings ), fftSettings( analysisSettings ), modelName( deviceModelName ),
      realDevice( hasRealDevice ), spectrogramChannel( new QComboBox( this ) ), dataStatus( new QLabel( tr( "Ожидание данных захвата" ), this ) ),
      grid( new QGridLayout ), refreshTimer( new QTimer( this ) ) {
    refreshTimer->setSingleShot( true );
    refreshTimer->setInterval( 250 );
    connect( refreshTimer, &QTimer::timeout, this, &AnalysisWorkspace::refreshLatestData );
    auto *root = new QVBoxLayout( this );
    root->setContentsMargins( 8, 8, 8, 8 );
    auto *toolbar = new QHBoxLayout;
    titleLabel = new QLabel( tr( "Анализ сигналов" ), this );
    titleLabel->setStyleSheet( QStringLiteral( "font-size:16px; font-weight:700; color:#172554;" ) );
    toolbar->addWidget( titleLabel );
    toolbar->addSpacing( 12 );
    dataStatus->setStyleSheet( QStringLiteral( "color:#475569;" ) );
    toolbar->addWidget( dataStatus, 1 );
    spectrogramLabel = new QLabel( tr( "Канал спектрограммы:" ), this );
    toolbar->addWidget( spectrogramLabel );
    spectrogramChannel->addItems( { QStringLiteral( "CH1" ), QStringLiteral( "CH2" ) } );
    toolbar->addWidget( spectrogramChannel );
    exportButton = new QPushButton( tr( "Экспорт анализа" ), this );
    toolbar->addWidget( exportButton );
    root->addLayout( toolbar );

    panels = { new AnalysisPanel( tr( "Осциллограф CH1 / CH2" ), this ),
               new AnalysisPanel( tr( "FFT-анализатор" ), this ),
               new AnalysisPanel( tr( "Спектрограмма STFT" ), this ),
               new AnalysisPanel( tr( "XY-анализатор CH1 / CH2" ), this ) };
    grid->setContentsMargins( 0, 0, 0, 0 );
    grid->setSpacing( 8 );
    root->addLayout( grid, 1 );
    rebuildGrid();
    for ( AnalysisPanel *panel : panels )
        connect( panel, &AnalysisPanel::maximizeRequested, this, &AnalysisWorkspace::setMaximizedPanel );
    connect( exportButton, &QPushButton::clicked, this, &AnalysisWorkspace::exportAnalysis );
    connect( spectrogramChannel, qOverload< int >( &QComboBox::currentIndexChanged ), this, [ this ] {
        spectrogramRows.clear();
        accumulatedSampleTime = 0.0;
        latestBlockBaseTime = 0.0;
        lastScheduledStftChannel = -1;
        scheduleSpectrogram();
    } );
}

AnalysisWorkspace::~AnalysisWorkspace() = default;

void AnalysisWorkspace::changeEvent( QEvent *event ) {
    QWidget::changeEvent( event );
    if ( event->type() != QEvent::LanguageChange || titleLabel == nullptr ) return;
    titleLabel->setText( tr( "Анализ сигналов" ) );
    spectrogramLabel->setText( tr( "Канал спектрограммы:" ) );
    exportButton->setText( tr( "Экспорт анализа" ) );
    panels[ 0 ]->setTitle( tr( "Осциллограф CH1 / CH2" ) );
    panels[ 1 ]->setTitle( tr( "FFT-анализатор" ) );
    panels[ 2 ]->setTitle( tr( "Спектрограмма STFT" ) );
    panels[ 3 ]->setTitle( tr( "XY-анализатор CH1 / CH2" ) );
    panels[ 0 ]->plot()->setAxisLabels( tr( "Время, с" ), tr( "Напряжение, В" ) );
    panels[ 1 ]->plot()->setAxisLabels( tr( "Частота, Гц" ), tr( "Уровень, дБ%1" ).arg(
        scope->analysis.dBsuffixStrings[ scope->analysis.dBsuffixIndex ] ) );
    panels[ 2 ]->plot()->setAxisLabels( tr( "Время, с" ), tr( "Частота, Гц" ) );
    panels[ 3 ]->plot()->setAxisLabels( tr( "CH1, В" ), tr( "CH2, В" ) );
    if ( hasDisplayedBlock )
        dataStatus->setText( tr( "Тег блока %1 · %2 отсчётов · %3 Гц" )
            .arg( displayedBlockTag ).arg( displayedSampleCount )
            .arg( scope->horizontal.samplerate, 0, 'g', 6 ) );
    else
        dataStatus->setText( tr( "Ожидание данных захвата" ) );
}


void AnalysisWorkspace::rebuildGrid() {
    for ( AnalysisPanel *panel : panels )
        grid->removeWidget( panel );
    if ( maximizedPanel ) {
        for ( AnalysisPanel *panel : panels )
            panel->setVisible( panel == maximizedPanel );
        grid->addWidget( maximizedPanel, 0, 0, 2, 2 );
    } else {
        for ( int i = 0; i < int( panels.size() ); ++i ) {
            panels[ size_t( i ) ]->setVisible( true );
            grid->addWidget( panels[ size_t( i ) ], i / 2, i % 2 );
        }
    }
    grid->setRowStretch( 0, 1 ); grid->setRowStretch( 1, 1 );
    grid->setColumnStretch( 0, 1 ); grid->setColumnStretch( 1, 1 );
}

void AnalysisWorkspace::setMaximizedPanel( AnalysisPanel *panel, bool maximized ) {
    maximizedPanel = maximized ? panel : nullptr;
    for ( AnalysisPanel *item : panels )
        item->setMaximized( maximized && item == panel );
    rebuildGrid();
}

void AnalysisWorkspace::setData( const std::shared_ptr< PPresult > &data ) {
    if ( !data )
        return;
    latestData = data;
    if ( const DataChannel *firstChannel = data->data( 0 ); firstChannel && firstChannel->voltage.interval > 0.0 ) {
        latestBlockBaseTime = accumulatedSampleTime;
        accumulatedSampleTime += firstChannel->voltage.samples.size() * firstChannel->voltage.interval;
    }
    // Coalesce rapid acquisition blocks. The analysis page is a visualization,
    // so rendering every block adds UI and FFT work without improving readability.
    if ( !refreshTimer->isActive() )
        refreshTimer->start();
}

void AnalysisWorkspace::refreshLatestData() {
    const std::shared_ptr< PPresult > data = latestData;
    if ( !data )
        return;
    QVector< QVector< QPointF > > voltageCurves( 2 ), fftCurves( 2 ), xyCurves( 1 );
    const QColor ch1( "#f2a900" ), ch2( "#1769ff" );
    QVector< QColor > channelColors{ ch1, ch2 };
    int sampleCount = 0;
    for ( int channel = 0; channel < 2; ++channel ) {
        const DataChannel *channelData = data->data( ChannelID( channel ) );
        if ( !channelData )
            continue;
        const auto &samples = channelData->voltage.samples;
        sampleCount = std::max( sampleCount, int( samples.size() ) );
        const double dt = channelData->voltage.interval;
        voltageCurves[ channel ].reserve( int( samples.size() ) );
        for ( int i = 0; i < int( samples.size() ); ++i )
            voltageCurves[ channel ].push_back( QPointF( i * dt, samples[ size_t( i ) ] ) );
        const auto &spectrum = channelData->spectrum.samples;
        const double df = channelData->spectrum.interval;
        const double visibleMaxFrequency = std::min( scope->horizontal.frequencybase * DIVS_TIME,
                                                     df > 0.0 && !spectrum.empty() ? df * ( spectrum.size() - 1 ) : 0.0 );
        const size_t visibleBinCount = df > 0.0 && visibleMaxFrequency > 0.0
                                           ? std::min( spectrum.size(), size_t( std::floor( visibleMaxFrequency / df ) ) + 1 )
                                           : 0;
        fftCurves[ channel ].reserve( int( visibleBinCount ) );
        for ( size_t i = 0; i < visibleBinCount; ++i )
            fftCurves[ channel ].push_back( QPointF( i * df, spectrum[ size_t( i ) ] ) );
    }
    if ( data->channelCount() >= 2 ) {
        const auto &ch1Samples = data->data( 0 )->voltage.samples;
        const auto &ch2Samples = data->data( 1 )->voltage.samples;
        const size_t paired = std::min( ch1Samples.size(), ch2Samples.size() );
        xyCurves[ 0 ].reserve( int( paired ) );
        for ( size_t i = 0; i < paired; ++i )
            xyCurves[ 0 ].push_back( QPointF( ch1Samples[ i ], ch2Samples[ i ] ) );
    }
    panels[ 0 ]->plot()->setCurves( tr( "Время, с" ), tr( "Напряжение, В" ), voltageCurves, channelColors );
    const QString dbSuffix = scope->analysis.dBsuffixStrings[ scope->analysis.dBsuffixIndex ];
    panels[ 1 ]->plot()->setCurves( tr( "Частота, Гц" ), tr( "Уровень, дБ%1" ).arg( dbSuffix ),
                                   fftCurves, channelColors );
    panels[ 3 ]->plot()->setCurves( tr( "CH1, В" ), tr( "CH2, В" ), xyCurves, { QColor( "#7c3aed" ) }, false );
    displayedBlockTag = data->tag;
    displayedSampleCount = sampleCount;
    hasDisplayedBlock = true;
    dataStatus->setText( tr( "Тег блока %1 · %2 отсчётов · %3 Гц" )
                             .arg( data->tag ).arg( sampleCount ).arg( scope->horizontal.samplerate, 0, 'g', 6 ) );
    scheduleSpectrogram();
}

void AnalysisWorkspace::scheduleSpectrogram() {
    if ( !latestData )
        return;
    const int channel = spectrogramChannel->currentIndex();
    const DataChannel *channelData = latestData->data( ChannelID( channel ) );
    if ( !channelData || channelData->voltage.samples.size() < 64 || channelData->voltage.interval <= 0.0 ) {
        panels[ 2 ]->plot()->setSpectrogram( tr( "Время, с" ), tr( "Частота, Гц" ), QImage(), 0, 0 );
        return;
    }
    if ( latestData->tag == lastScheduledStftTag && channel == lastScheduledStftChannel )
        return;
    if ( stftBusy.exchange( true ) ) {
        stftNeedsRefresh = true;
        return;
    }
    stftNeedsRefresh = false;
    const std::shared_ptr< PPresult > block = latestData;
    lastScheduledStftTag = block->tag;
    lastScheduledStftChannel = channel;
    const double dt = channelData->voltage.interval;
    const double visibleMaxFrequency = std::min( 1.0 / ( 2.0 * dt ), scope->horizontal.frequencybase * DIVS_TIME );
    const double baseTime = latestBlockBaseTime;
    QPointer< AnalysisWorkspace > guard( this );
    QThreadPool::globalInstance()->start( QRunnable::create( [ guard, block, dt, channel, baseTime, visibleMaxFrequency ] {
        const DataChannel *data = block->data( ChannelID( channel ) );
        if ( !data )
            return;
        const auto &samples = data->voltage.samples;
        QVector< SpectrogramRow > rows;
        const int n = stftSizeFor( int( samples.size() ) );
        if ( n >= 64 ) {
            const int hop = n / 4;
            constexpr int maxRowsPerBlock = 64;
            const int first = std::max( 0, int( samples.size() ) - n - hop * ( maxRowsPerBlock - 1 ) );
            const int binCount = std::min( n / 2 + 1,
                                           int( std::floor( visibleMaxFrequency * dt * n ) ) + 1 );
            QVector< std::complex< double > > input( n );
            QVector< double > window( n );
            double windowSum = 0.0;
            for ( int i = 0; i < n; ++i ) {
                window[ i ] = 0.5 - 0.5 * std::cos( 2.0 * M_PI * i / ( n - 1 ) );
                windowSum += window[ i ];
            }
            for ( int start = first; start + n <= int( samples.size() ); start += hop ) {
                for ( int i = 0; i < n; ++i )
                    input[ i ] = std::complex< double >( samples[ size_t( start + i ) ] * window[ i ], 0.0 );
                fftInPlace( input );
                SpectrogramRow row;
                row.timeSeconds = baseTime + ( start + n / 2 ) * dt;
                row.levelsDbV.resize( binCount );
                for ( int bin = 0; bin < binCount; ++bin ) {
                    const double amplitude = std::abs( input[ bin ] ) * ( bin == 0 || bin == n / 2 ? 1.0 : 2.0 ) /
                                             std::max( windowSum, 1e-12 );
                    row.levelsDbV[ bin ] = float( 20.0 * std::log10( std::max( amplitude, 1e-12 ) ) );
                }
                rows.push_back( std::move( row ) );
            }
        }
        if ( guard ) {
            const quint32 tag = block->tag;
            QMetaObject::invokeMethod( guard, [ guard, channel, dt, n, rows, tag, visibleMaxFrequency ] {
                if ( guard ) {
                    guard->acceptSpectrogram( channel, n >= 4 ? dt * ( n / 4 ) : dt, visibleMaxFrequency, rows );
                    guard->stftBusy.store( false );
                    if ( guard->latestData && guard->latestData->tag != tag )
                        guard->stftNeedsRefresh = true;
                    if ( guard->stftNeedsRefresh )
                        guard->scheduleSpectrogram();
                }
            }, Qt::QueuedConnection );
        }
    } ) );
}

void AnalysisWorkspace::acceptSpectrogram( int channel, double rowDurationSeconds, double maxFrequency,
                                           const QVector< SpectrogramRow > &rows ) {
    if ( channel != spectrogramChannel->currentIndex() )
        return;
    displayedStftChannel = channel;
    spectrogramRowSeconds = rowDurationSeconds;
    spectrogramMaxFrequency = maxFrequency;
    for ( const SpectrogramRow &row : rows )
        spectrogramRows.push_back( row );
    while ( spectrogramRows.size() > kHistoryLimit )
        spectrogramRows.removeFirst();
    if ( spectrogramRows.isEmpty() )
        return;
    const int sourceBins = spectrogramRows.first().levelsDbV.size();
    // The plot is only a few hundred pixels tall; rasterizing thousands of
    // FFT bins on every refresh wastes CPU without adding visible detail.
    const int bins = std::min( sourceBins, 512 );
    QImage heat( spectrogramRows.size(), bins, QImage::Format_RGB32 );
    static const std::array< QRgb, 256 > heatColorLut = [] {
        std::array< QRgb, 256 > colors{};
        for ( int i = 0; i < int( colors.size() ); ++i )
            colors[ size_t( i ) ] = heatColor( double( i ) / ( colors.size() - 1 ) ).rgb();
        return colors;
    }();
    for ( int y = 0; y < bins; ++y ) {
        const int firstBin = y * sourceBins / bins;
        const int endBin = std::max( firstBin + 1, ( y + 1 ) * sourceBins / bins );
        auto *scanline = reinterpret_cast< QRgb * >( heat.scanLine( bins - 1 - y ) );
        for ( int x = 0; x < spectrogramRows.size(); ++x ) {
            const auto &row = spectrogramRows[ x ].levelsDbV;
            float peakDbV = -std::numeric_limits< float >::infinity();
            for ( int bin = firstBin; bin < endBin; ++bin )
                peakDbV = std::max( peakDbV, row[ bin ] );
            // A fixed -100..0 dBV display scale keeps colors comparable over time.
            const int colorIndex = qBound( 0, qRound( ( peakDbV + 100.0f ) * 2.55f ), 255 );
            scanline[ x ] = heatColorLut[ size_t( colorIndex ) ];
        }
    }
    panels[ 2 ]->plot()->setSpectrogram( tr( "Время, с" ), tr( "Частота, Гц" ), heat, spectrogramRowSeconds,
                                         spectrogramMaxFrequency );
}

void AnalysisWorkspace::exportAnalysis() {
    if ( !latestData ) {
        QMessageBox::information( this, tr( "Экспорт анализа" ), tr( "Пока нет данных для экспорта." ) );
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(
        this, tr( "Экспорт анализа" ), QDir::home().filePath( QStringLiteral( "OpenHantek_Analysis_%1.zip" )
                                                                  .arg( QDateTime::currentDateTime().toString( "yyyyMMdd_HHmmss" ) ) ),
        tr( "ZIP-архив (*.zip)" ) );
    if ( fileName.isEmpty() )
        return;
    bool accepted = false;
    const QString comment = QInputDialog::getMultiLineText( this, tr( "Комментарий к измерению" ), tr( "Комментарий:" ),
                                                            QString(), &accepted );
    if ( !accepted )
        return;

    QVector< QPair< QString, QByteArray > > files;
    QImage overview( 1800, 1000, QImage::Format_ARGB32_Premultiplied );
    overview.fill( QColor( "#e8edf3" ) );
    QPainter painter( &overview );
    painter.setPen( QColor( "#172554" ) ); painter.setFont( QFont( font().family(), 18, QFont::Bold ) );
    painter.drawText( QRect( 24, 10, 1750, 34 ), Qt::AlignLeft | Qt::AlignVCenter, tr( "Анализ измерения OpenHantek" ) );
    for ( int i = 0; i < 4; ++i ) {
        const QRect target( 20 + ( i % 2 ) * 890, 55 + ( i / 2 ) * 465, 870, 445 );
        painter.drawImage( target, panels[ size_t( i ) ]->renderImage( QSize( target.size() ) ) );
        painter.setPen( QColor( "#334155" ) ); painter.drawRect( target );
    }
    painter.end();
    files.push_back( { QStringLiteral( "overview.png" ), imagePng( overview ) } );
    const std::array< QString, 4 > imageNames = { QStringLiteral( "oscilloscope.png" ), QStringLiteral( "fft.png" ),
                                                  QStringLiteral( "spectrogram.png" ), QStringLiteral( "xy.png" ) };
    for ( int i = 0; i < 4; ++i )
        files.push_back( { imageNames[ size_t( i ) ], imagePng( panels[ size_t( i ) ]->renderImage() ) } );

    QByteArray samplesCsv = "time_s,ch1_voltage_V,ch2_voltage_V\n";
    const DataChannel *ch1 = latestData->data( 0 );
    const DataChannel *ch2 = latestData->data( 1 );
    const size_t sampleCount = std::max( ch1 ? ch1->voltage.samples.size() : 0, ch2 ? ch2->voltage.samples.size() : 0 );
    const double dt = ch1 && ch1->voltage.interval > 0 ? ch1->voltage.interval : ( ch2 ? ch2->voltage.interval : 0.0 );
    for ( size_t i = 0; i < sampleCount; ++i ) {
        samplesCsv += csvNumber( double( i ) * dt ) + ',';
        if ( ch1 && i < ch1->voltage.samples.size() ) samplesCsv += csvNumber( ch1->voltage.samples[ i ] );
        samplesCsv += ',';
        if ( ch2 && i < ch2->voltage.samples.size() ) samplesCsv += csvNumber( ch2->voltage.samples[ i ] );
        samplesCsv += '\n';
    }
    files.push_back( { QStringLiteral( "samples.csv" ), samplesCsv } );

    for ( int channel = 0; channel < 2; ++channel ) {
        const DataChannel *d = latestData->data( ChannelID( channel ) );
        QByteArray csv = "frequency_Hz,level_dB\n";
        if ( d && d->spectrum.interval > 0.0 ) {
            for ( size_t i = 0; i < d->spectrum.samples.size(); ++i )
                csv += csvNumber( double( i ) * d->spectrum.interval ) + ',' + csvNumber( d->spectrum.samples[ i ] ) + '\n';
        } else {
            csv = "# FFT samples unavailable for this capture block\nfrequency_Hz,level_dB\n";
        }
        files.push_back( { QStringLiteral( "fft_ch%1.csv" ).arg( channel + 1 ), csv } );
    }

    QByteArray stftCsv = "time_s";
    const int bins = spectrogramRows.isEmpty() ? 0 : spectrogramRows.first().levelsDbV.size();
    const double sampleRate = dt > 0.0 ? 1.0 / dt : 0.0;
    const int stftSize = stftSizeFor( int( sampleCount ) );
    for ( int bin = 0; bin < bins; ++bin )
        stftCsv += ",freq_Hz_" + csvNumber( stftSize > 0 ? double( bin ) * sampleRate / stftSize : 0.0 );
    stftCsv += '\n';
    for ( const SpectrogramRow &row : spectrogramRows ) {
        stftCsv += csvNumber( row.timeSeconds );
        for ( float value : row.levelsDbV ) stftCsv += ',' + csvNumber( value );
        stftCsv += '\n';
    }
    if ( spectrogramRows.isEmpty() )
        stftCsv = "# STFT unavailable: fewer than 64 calibrated samples in selected channel\ntime_s\n";
    files.push_back( { QStringLiteral( "spectrogram.csv" ), stftCsv } );

    const QString dbSuffix = scope->analysis.dBsuffixStrings[ scope->analysis.dBsuffixIndex ];
    QJsonObject metadata;
    metadata.insert( QStringLiteral( "captured_at_utc" ), QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) );
    metadata.insert( QStringLiteral( "model" ), modelName );
    metadata.insert( QStringLiteral( "mode" ), realDevice ? QStringLiteral( "REAL_DEVICE" ) : QStringLiteral( "DEMO" ) );
    metadata.insert( QStringLiteral( "data_representation" ), QStringLiteral( "calibrated_voltage_samples_from_PPresult" ) );
    metadata.insert( QStringLiteral( "raw_adc_samples" ), false );
    metadata.insert( QStringLiteral( "sample_rate_hz" ), dt > 0.0 ? 1.0 / dt : scope->horizontal.samplerate );
    metadata.insert( QStringLiteral( "timebase_s_per_div" ), scope->horizontal.timebase );
    QJsonArray channelInfo;
    for ( int channel = 0; channel < 2; ++channel ) {
        QJsonObject item;
        item.insert( QStringLiteral( "name" ), QStringLiteral( "CH%1" ).arg( channel + 1 ) );
        item.insert( QStringLiteral( "voltage_scale_V_per_div" ), scope->gain( unsigned( channel ) ) );
        item.insert( QStringLiteral( "probe_attenuation" ), scope->voltage[ size_t( channel ) ].probeAttn );
        item.insert( QStringLiteral( "units" ), QStringLiteral( "V" ) );
        channelInfo.append( item );
    }
    metadata.insert( QStringLiteral( "channels" ), channelInfo );
    QJsonObject trigger;
    trigger.insert( QStringLiteral( "mode_enum" ), int( scope->trigger.mode ) );
    trigger.insert( QStringLiteral( "source_channel_index_zero_based" ), scope->trigger.source );
    trigger.insert( QStringLiteral( "slope_enum" ), int( scope->trigger.slope ) );
    metadata.insert( QStringLiteral( "trigger" ), trigger );
    metadata.insert( QStringLiteral( "fft" ), QJsonObject{ { QStringLiteral( "window" ),
                                                        Dso::windowFunctionString( fftSettings->spectrumWindow ) },
                                                        { QStringLiteral( "level_units" ), QStringLiteral( "dB%1" ).arg( dbSuffix ) },
                                                        { QStringLiteral( "source" ), QStringLiteral( "existing OpenHantek FFT results in PPresult" ) } } );
    metadata.insert( QStringLiteral( "stft" ), QJsonObject{ { QStringLiteral( "window" ), QStringLiteral( "Hann" ) },
                                                           { QStringLiteral( "size_samples" ), stftSizeFor( int( sampleCount ) ) },
                                                           { QStringLiteral( "overlap_percent" ), 75 },
                                                           { QStringLiteral( "normalization" ), QStringLiteral( "single-sided amplitude divided by Hann coherent sum; dBV re 1 V" ) },
                                                           { QStringLiteral( "selected_channel" ), displayedStftChannel + 1 },
                                                           { QStringLiteral( "frequency_resolution_hz" ), dt > 0.0 ? 1.0 / ( dt * fftSizeFor( int( sampleCount ) ) ) : 0.0 } } );
    metadata.insert( QStringLiteral( "calibration_context" ), QStringLiteral( "Voltage values are calibrated PPresult voltage samples; raw ADC codes are not present in this export." ) );
    metadata.insert( QStringLiteral( "user_comment" ), comment );
    files.push_back( { QStringLiteral( "session.json" ), QJsonDocument( metadata ).toJson( QJsonDocument::Indented ) } );

    QString error;
    if ( !writeZip( fileName, files, &error ) )
        QMessageBox::warning( this, tr( "Экспорт анализа" ), error );
    else
        QMessageBox::information( this, tr( "Экспорт анализа" ), tr( "Архив сохранён: %1" ).arg( fileName ) );
}
