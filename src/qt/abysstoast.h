// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_ABYSSTOAST_H
#define BITCOIN_QT_ABYSSTOAST_H

#include <QFrame>

QT_BEGIN_NAMESPACE
class QGraphicsOpacityEffect;
class QLabel;
class QPropertyAnimation;
QT_END_NAMESPACE

/**
 * A short line that confirms something just happened ("Address copied"), in
 * Abyss only. It sits in its parent's layout, keeps its place while hidden so
 * that nothing moves when it appears, fades in, stays a moment and fades out:
 * two seconds in all. It never takes the focus, never blocks and never asks
 * for anything; a confirmation that matters stays a dialog. Hidden with its
 * window, or when the window loses the focus, it stops at once, so no
 * animation runs where nobody sees it.
 */
class AbyssToast : public QFrame
{
    Q_OBJECT

public:
    //! The whole appearance, fade in and out included.
    static constexpr int DURATION_MS{2000};

    explicit AbyssToast(QWidget* parent);

    //! Show `text`, starting the two seconds again if it is already showing.
    void showMessage(const QString& text);
    //! Whether it is on its way in, showing, or on its way out.
    bool isShowing() const;
    QString text() const;

protected:
    bool event(QEvent* event) override;

private:
    QLabel* m_text;
    QGraphicsOpacityEffect* m_opacity;
    QPropertyAnimation* m_fade;
    void finish();
};

#endif // BITCOIN_QT_ABYSSTOAST_H
