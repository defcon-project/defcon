// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STARFIELDWIDGET_H
#define BITCOIN_QT_STARFIELDWIDGET_H

#include <QColor>
#include <QPixmap>
#include <QPointF>
#include <QRegion>
#include <QVector>
#include <QWidget>

class QImage;
class QTimer;

/**
 * Abyss-only, locally drawn sky. The caller controls theme visibility; no node,
 * wallet, location, network request or setting is read here.
 * Drawing the sky avoids shipping a fixed-size photograph or an animation
 * asset; stars stay sharp at the current DPR. The stylesheet still owns the
 * background colour, keeping the theme palette in one place.
 *
 * Steady stars are cached until sidereal rotation moves them by a quarter
 * physical pixel. Only the few twinklers and the small moving Moon need
 * intervening regional repaints. Lunar phase is a decorative mean-cycle
 * approximation, not an observer-specific ephemeris.
 */
class StarfieldWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StarfieldWidget(QWidget* parent = nullptr);

    void setSkyVisible(bool visible);
    /** Off freezes motion and settles twinkle. On resumes motion continuously;
     *  the Moon's phase catches up to the current approximate date. */
    void setAnimated(bool animated);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    /** Monotonic elapsed time including sleep; deterministic tests override it. */
    virtual qint64 clockMilliseconds() const;

private:
    friend class AbyssTests;
    struct Star {
        QPointF pos;      //!< Pole-relative position in the deterministic virtual disk.
        qreal radius;
        QColor colour;
        qreal amplitude; //!< Zero for the steady majority.
        qreal period;    //!< Seconds per gentle twinkle, independent of sky rotation.
        qreal phase;     //!< Offset avoids synchronized flashing.
    };

    static qint64 suspendAwareClockMilliseconds();
    void buildSky();
    void updateStarCandidates();
    void setRotationAngle(qreal angle);
    void redrawSteadySky();
    void onTick();
    void updateTimerState();
    bool wantsAnimation() const;
    qint64 motionMilliseconds() const;
    qint64 phaseTimeMilliseconds() const;
    static qreal rotationAngle(qint64 utc_ms);
    static qreal lunarPhase(qint64 utc_ms);
    static QImage renderMoon(qreal phase, qreal dpr);
    QPointF pole() const;
    QPointF widgetPos(const Star& star) const;
    QRect starRect(const Star& star) const;
    QPointF moonPosition(qint64 motion_ms) const;
    QRect moonRect() const;
    void invalidateImages();

    QVector<Star> m_stars;
    QVector<int> m_candidates; //!< Stars that can reach this viewport at any rotation.
    QVector<int> m_twinklers; //!< Indices of the few stars painted live.
    QPixmap m_steady;        //!< Reused backing store for the steady stars.
    QRegion m_sky_dirty;     //!< Old/new footprints pending cache repair.
    QPixmap m_moon;
    qint64 m_clock_reference_ms{0};
    QTimer* m_timer{nullptr};
    qint64 m_epoch_utc_ms{0};
    qint64 m_pause_started_ms{0};
    qint64 m_paused_ms{0};   //!< Excluded from motion so enabling cannot jump.
    qint64 m_frozen_utc_ms{0}; //!< Off freezes phase separately from paused motion.
    qreal m_sky_radius{6000.0};
    qreal m_angle{0};
    qreal m_cos_angle{1};
    qreal m_sin_angle{0};
    QPointF m_moon_position;
    int m_moon_phase_bucket{-1};
    bool m_skyVisible{false};
    bool m_animated{true};
};

#endif // BITCOIN_QT_STARFIELDWIDGET_H
