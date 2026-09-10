// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STARFIELDWIDGET_H
#define BITCOIN_QT_STARFIELDWIDGET_H

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QTimer;

/**
 * The Abyss theme's backdrop: a still night sky in which only a handful of the
 * faintest stars breathe.
 *
 * It is drawn rather than shipped as an image because the wallet's Qt is built
 * with -no-gif and carries no QtSvg, so PNG is the only format it can load and
 * a PNG cannot animate. Drawing also answers the questions a photograph would
 * have raised: a stylesheet background-image is tiled rather than scaled, so an
 * image has to be cropped to the window and softens on a high-DPI screen, while
 * this costs no bytes in the binary and is correct at every size.
 *
 * The colour behind the sky still belongs to the stylesheet. This widget paints
 * the themed background first and puts stars on top of it, so the theme file
 * remains the one place the palette is decided.
 *
 * The widget knows nothing about themes: the caller says whether the sky is
 * wanted. That keeps the drawing testable on its own, and it is the reason the
 * preview used to tune these constants can build without the wallet.
 */
class StarfieldWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StarfieldWidget(QWidget* parent = nullptr);

    /** Show or hide the sky. Only the Abyss theme asks for it. */
    void setSkyVisible(bool visible);

    /** Whether the few twinkling stars may move. The sky stays either way. */
    void setAnimated(bool animated);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Star {
        QPointF pos;      //!< position in the virtual sky, not in the widget
        qreal radius;
        QColor colour;
        qreal amplitude;  //!< 0 for every star that holds steady
        qreal period;     //!< seconds for one breath
        qreal phase;      //!< radians, so no two twinklers march together
    };

    void buildSky();
    void redrawSteadySky();
    void onTick();
    void updateTimerState();
    bool wantsAnimation() const;
    QPointF widgetPos(const Star& star) const;
    QRect starRect(const Star& star) const;

    QVector<Star> m_stars;      //!< the whole sky, steady stars included
    QVector<int> m_twinklers;   //!< indices into m_stars, drawn live each frame
    QPixmap m_steady;           //!< every star that does not move, rendered once
    QElapsedTimer m_clock;
    QTimer* m_timer{nullptr};
    bool m_skyVisible{false};
    bool m_animated{true};
};

#endif // BITCOIN_QT_STARFIELDWIDGET_H
