// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_STARFIELDWIDGET_H
#define BITCOIN_QT_STARFIELDWIDGET_H

#include <consensus/amount.h>

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QRegion>
#include <QString>
#include <QVector>
#include <QWidget>

class QPainter;
class QTimer;

/**
 * The balance-tied levels of the Abyss sky. Presentation only: a level is
 * read from the wallet's own totals and picks the drawn scene behind the
 * pages. Nothing here touches the wallet, staking or consensus, and nothing
 * is paid out: the reward is the view.
 */
namespace SkyTiers {
constexpr int COUNT = 11;
//! A level is left on the way down only below this share of its threshold
//! (in hundredths), so a balance resting on a threshold does not flip the sky.
constexpr int DOWN_PERCENT = 97;
//! The balance from which a level starts; level 0 has no threshold.
CAmount threshold(int tier);
//! The level for a balance, given the level shown now (hysteresis on the way down).
int tierForBalance(CAmount balance, int current);
//! The level's name as the badge shows it.
QString name(int tier);
} // namespace SkyTiers

/**
 * Abyss-only, locally drawn sky. The caller controls theme visibility and the
 * level; no node, wallet, location, network request or setting is read here.
 * Drawing the sky avoids shipping a fixed-size photograph or an animation
 * asset; stars stay sharp at the current DPR. The stylesheet still owns the
 * background colour, keeping the theme palette in one place.
 *
 * The sky stands still. Everything static for a level (the star field, the
 * real sky with its constellations, the Milky Way, the Moon and the level's
 * own scene) is drawn once into a cached image for the widget's size, pixel
 * ratio and level; the paint event only copies it. The real sky is projected
 * for the current sidereal time and quietly redrawn every ten minutes. Only
 * the few twinkling stars, a rare shooting star and the comet of one level
 * are painted live, through regional repaints.
 */
class StarfieldWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StarfieldWidget(QWidget* parent = nullptr);

    void setSkyVisible(bool visible);
    /** Off freezes motion and settles twinkle, and the sky keeps the time it
     *  had. On resumes continuously; the sky and the Moon catch up to the
     *  current date on the next tick. */
    void setAnimated(bool animated);
    /** The level whose scene to draw, 0 to SkyTiers::COUNT - 1. */
    void setSkyTier(int tier);
    int skyTier() const { return m_tier; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    /** Monotonic elapsed time including sleep; deterministic tests override it. */
    virtual qint64 clockMilliseconds() const;
    /** The system's UTC time, which the real sky and the Moon follow; deterministic tests override it. */
    virtual qint64 utcMilliseconds() const;

private:
    friend class AbyssTests;
    struct Star {
        QPointF pos;     //!< Position in the fixed virtual plane the widget shows the corner of.
        qreal radius;
        QColor colour;
        qreal rank;      //!< Which stars the sparse level 0 field keeps.
        bool bright;     //!< One of the few large ones; only the plain field shows them.
        qreal amplitude; //!< Zero for the steady majority.
        qreal period;    //!< Seconds per gentle twinkle.
        qreal phase;     //!< Offset avoids synchronized flashing.
    };
    /** Where a point of the real sky falls on the widget: the zenith at the
     *  centre, north up, east to the left as a chart held overhead reads, in a
     *  stereographic projection scaled so the top edge is fifty degrees down. */
    struct Projection {
        qreal centre_x, centre_y, radius, sidereal_hours, sin_lat, cos_lat;
        Projection(const QSize& size, qreal lst_hours);
        //! Returns false below about three degrees of altitude, never in frame.
        bool place(qreal ra_hours, qreal dec_degrees, QPointF& at, qreal& altitude) const;
    };

    static qint64 suspendAwareClockMilliseconds();
    void buildField();
    void updateVisibleStars();
    void redrawSteadySky();
    void onTick();
    /** What a tick at `motion_ms` repaints: what the live elements drew since
     *  the last tick, so nothing is left behind, and where they are now. */
    QRegion tickRegion(qint64 motion_ms);
    void updateTimerState();
    /** A visible but inactive window gets no frames; its real sky is still
     *  redrawn when the ten-minute slot turns, so it never shows a stale hour.
     *  This arms the single shot; onRefresh() redraws, from the event loop. */
    void updateRefreshTimer();
    void onRefresh();
    bool wantsAnimation() const;
    bool hasRealSky() const;
    bool hasShootingStars() const;
    qint64 motionMilliseconds() const;
    qint64 phaseTimeMilliseconds() const;
    qint64 skyBucket() const;
    static qreal lunarPhase(qint64 utc_ms);
    static qreal siderealHours(qint64 utc_ms, qreal longitude_degrees);
    static qreal longitudeGuess(qint64 utc_ms);
    static QImage renderMoon(qreal phase, qreal dpr);
    QPointF moonPosition() const;
    QRect moonRect() const;
    QPointF heroPosition() const;
    QRect starRect(const Star& star) const;
    QRect shootingRect(qint64 motion_ms) const;
    QRect cometRect(qint64 motion_ms) const;
    void startShootingStar(qint64 motion_ms);
    void paintShootingStar(QPainter& painter, qint64 motion_ms) const;
    void paintComet(QPainter& painter, qint64 motion_ms) const;
    void paintField(QPainter& painter) const;
    void paintCatalogue(QPainter& painter, const Projection& projection, qreal dim) const;
    void paintLines(QPainter& painter, const Projection& projection, qreal alpha) const;
    void paintMilkyWay(QPainter& painter, const Projection& projection, qreal strength) const;
    void paintMoon(QPainter& painter);
    void paintScene(QPainter& painter);
    void invalidateImages();

    QVector<Star> m_stars;
    QVector<int> m_visible;   //!< Stars inside the widget, with their halos.
    QVector<int> m_twinklers; //!< Visible stars painted live.
    QPixmap m_steady;         //!< Everything that stands still, for this size, DPR and level.
    QPixmap m_moon;
    QImage m_nebula;          //!< The level 7 cloud at its quarter resolution, computed once.
    qint64 m_clock_reference_ms{0};
    QTimer* m_timer{nullptr};
    QTimer* m_refresh{nullptr}; //!< Single shot at the next ten-minute slot, only while the window is inactive.
    qint64 m_pause_started_ms{0};
    qint64 m_paused_ms{0};     //!< Excluded from motion so enabling cannot jump.
    qint64 m_frozen_utc_ms{0}; //!< Off freezes the sky's time separately from paused motion.
    qint64 m_sky_bucket{-1};   //!< The ten-minute slot the real sky was last projected for.
    QPointF m_moon_position;
    int m_moon_phase_bucket{-1};
    qint64 m_live_motion_ms{0}; //!< The time the moving elements are painted at, set by the last tick.
    QRegion m_live_drawn;       //!< Where the moving elements were painted since the last tick.
    qint64 m_shooting_started_ms{-1};
    qint64 m_next_shooting_ms{0};
    QPointF m_shooting_from;
    QPointF m_shooting_direction;
    int m_tier{0};
    int m_steady_redraws{0}; //!< How often the cache was rebuilt; tests watch it.
    bool m_skyVisible{false};
    bool m_animated{true};
};

#endif // BITCOIN_QT_STARFIELDWIDGET_H
