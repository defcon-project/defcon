// Copyright (c) 2020-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_APPEARANCEWIDGET_H
#define BITCOIN_QT_APPEARANCEWIDGET_H

#include <QWidget>

#include <qt/guiutil.h>

namespace Ui {
class AppearanceWidget;
}

class OptionsModel;

class QDataWidgetMapper;
class QSlider;
class QComboBox;

class AppearanceWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AppearanceWidget(QWidget* parent = 0);
    ~AppearanceWidget();

    void setModel(OptionsModel* model);

Q_SIGNALS:
    void appearanceChanged();
    /** Notify the options dialog whether the selected theme differs from the saved one. */
    void themeChanged(bool restartRequired);

public Q_SLOTS:
    void accept();

private Q_SLOTS:
    void updateTheme(const QString& toTheme = QString());
    void updateFontFamily(int index);
    void updateFontScale(int nScale);
    //! fChosen: whether the weight is a choice (the slider moved), rather than set from the settings or reset.
    void updateFontWeightNormal(int nValue, bool fForce = false, bool fChosen = true);
    void updateFontWeightBold(int nValue, bool fForce = false);

private:
    Ui::AppearanceWidget* ui;
    QDataWidgetMapper* mapper;
    OptionsModel* model;
    bool fAcceptChanges{false};
    //! Set while the mapper fills the widgets from the settings: what the sliders get then is no choice.
    bool m_populating{false};
    //! Whether the normal slider shows the Regular Abyss draws in place of the weight held (showFontWeightNormal).
    bool m_shows_stand_in{false};
    //! The normal slider's own value, kept aside while it shows the Regular.
    int m_normal_slider_value{0};
    QString prevTheme{GUIUtil::getActiveTheme()};
    int prevScale{GUIUtil::getFontScale()};
    GUIUtil::FontFamily prevFontFamily{GUIUtil::getFontFamily()};
    bool prevFontFamilyExplicit{GUIUtil::hasExplicitFontFamily()};
    QFont::Weight prevWeightNormal{GUIUtil::getFontWeightNormal()};
    bool prevWeightNormalExplicit{GUIUtil::hasExplicitFontWeightNormal()};
    QFont::Weight prevWeightBold{GUIUtil::getFontWeightBold()};
    bool prevAnimateNightSky{true};

    void updateWeightSlider(bool fForce = false);
    //! While no normal weight was chosen, the normal slider shows the Regular Abyss draws in place of the weight held,
    //! without changing the weight, and keeps its own value aside; otherwise it holds its own value.
    void showFontWeightNormal();
    //! Gives the normal slider its own value back if it shows the Regular in its place.
    void putBackFontWeightNormal();
};

#endif // BITCOIN_QT_APPEARANCEWIDGET_H
