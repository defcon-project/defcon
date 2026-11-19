// Copyright (c) 2020-2023 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/forms/ui_appearancewidget.h>

#include <qt/appearancewidget.h>
#include <qt/optionsmodel.h>
#include <qt/starfieldwidget.h>

#include <util/system.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDataWidgetMapper>
#include <QSettings>
#include <QSlider>

AppearanceWidget::AppearanceWidget(QWidget* parent) :
    QWidget(parent),
    ui{new Ui::AppearanceWidget()}
{
    ui->setupUi(this);

    for (const QString& entry : GUIUtil::listThemes()) {
        ui->theme->addItem(entry, QVariant(entry));
    }

    GUIUtil::FontFamily fontSystem = GUIUtil::FontFamily::SystemDefault;
    GUIUtil::FontFamily fontMontserrat = GUIUtil::FontFamily::Montserrat;
    GUIUtil::FontFamily fontRoboto = GUIUtil::FontFamily::Roboto;

    ui->fontFamily->addItem(GUIUtil::fontFamilyToString(fontSystem), QVariant(static_cast<int>(fontSystem)));
    ui->fontFamily->addItem(GUIUtil::fontFamilyToString(fontMontserrat), QVariant(static_cast<int>(fontMontserrat)));
    ui->fontFamily->addItem(GUIUtil::fontFamilyToString(fontRoboto), QVariant(static_cast<int>(fontRoboto)));

    updateWeightSlider();

    // Written straight to the settings rather than left to the mapper, so the
    // sky reacts while the dialog is still open. Cancelling puts it back, the
    // same way the theme and the fonts are put back.
    prevAnimateNightSky = QSettings().value("fAnimateNightSky", true).toBool();
    ui->animateNightSky->setChecked(prevAnimateNightSky);
    // The night sky rests: its option is kept, with its stored value, and not shown.
    ui->lblNightSky->setVisible(!SkyTiers::dormant());
    ui->animateNightSky->setVisible(!SkyTiers::dormant());

    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    connect(ui->theme, &QComboBox::currentTextChanged, this, &AppearanceWidget::updateTheme);
    connect(ui->fontFamily, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AppearanceWidget::updateFontFamily);
    // Choosing the already displayed SystemDefault still expresses intent;
    // mapper population does not. Cancelling restores this marker as well.
    connect(ui->fontFamily, QOverload<int>::of(&QComboBox::activated), [this](int index) {
        if (!GUIUtil::hasExplicitFontFamily()) {
            GUIUtil::setFontFamily(static_cast<GUIUtil::FontFamily>(ui->fontFamily->itemData(index).toInt()));
            // The family drawn can change with the choice (Abyss's Roboto gives way): so can the weight drawn.
            showFontWeightNormal();
        }
    });
    connect(ui->fontScaleSlider, &QSlider::valueChanged, this, &AppearanceWidget::updateFontScale);
    // Moving the slider chooses a weight; the value the mapper fills it with does not.
    connect(ui->fontWeightNormalSlider, &QSlider::valueChanged, [this](auto nValue) {
        updateFontWeightNormal(nValue, false, m_populating ? GUIUtil::hasExplicitFontWeightNormal() : true);
    });
    // A new bold weight can make room for the Regular drawn: the normal slider shows it then.
    connect(ui->fontWeightBoldSlider, &QSlider::valueChanged, [this](auto nValue) {
        updateFontWeightBold(nValue);
        showFontWeightNormal();
    });

    connect(ui->animateNightSky, &QCheckBox::toggled, [=](bool checked) {
        QSettings().setValue("fAnimateNightSky", checked);
        Q_EMIT appearanceChanged();
    });

    connect(ui->theme, &QComboBox::currentTextChanged, [=]() { Q_EMIT appearanceChanged(); });
    connect(ui->fontFamily, &QComboBox::currentTextChanged, [=]() { Q_EMIT appearanceChanged(); });
    connect(ui->fontScaleSlider, &QSlider::sliderReleased, [=]() { Q_EMIT appearanceChanged(); });
    connect(ui->fontWeightNormalSlider, &QSlider::sliderReleased, [=]() { Q_EMIT appearanceChanged(); });
    connect(ui->fontWeightBoldSlider, &QSlider::sliderReleased, [=]() { Q_EMIT appearanceChanged(); });
}

AppearanceWidget::~AppearanceWidget()
{
    if (fAcceptChanges) {
        // The mapper stores the slider's own value, as before: a slider that
        // shows the Regular drawn in its place gets it back first.
        putBackFontWeightNormal();
        mapper->submit();
    } else {
        if (prevTheme != GUIUtil::getActiveTheme()) {
            updateTheme(prevTheme);
        }
        if (prevFontFamily != GUIUtil::getFontFamily() || prevFontFamilyExplicit != GUIUtil::hasExplicitFontFamily()) {
            GUIUtil::setFontFamily(prevFontFamily, prevFontFamilyExplicit);
        }
        if (prevScale != GUIUtil::getFontScale()) {
            GUIUtil::setFontScale(prevScale);
        }
        if (prevWeightNormal != GUIUtil::getFontWeightNormal() || prevWeightNormalExplicit != GUIUtil::hasExplicitFontWeightNormal()) {
            GUIUtil::setFontWeightNormal(prevWeightNormal, prevWeightNormalExplicit);
        }
        if (prevWeightBold != GUIUtil::getFontWeightBold()) {
            GUIUtil::setFontWeightBold(prevWeightBold);
        }
        if (prevAnimateNightSky != QSettings().value("fAnimateNightSky", true).toBool()) {
            QSettings().setValue("fAnimateNightSky", prevAnimateNightSky);
        }
    }
    delete ui;
}

void AppearanceWidget::setModel(OptionsModel* _model)
{
    this->model = _model;

    if (_model) {
        const QSignalBlocker font_family_blocker(ui->fontFamily);
        mapper->setModel(_model);
        mapper->addMapping(ui->theme, OptionsModel::Theme);
        mapper->addMapping(ui->fontFamily, OptionsModel::FontFamily);
        mapper->addMapping(ui->fontScaleSlider, OptionsModel::FontScale);
        mapper->addMapping(ui->fontWeightNormalSlider, OptionsModel::FontWeightNormal);
        mapper->addMapping(ui->fontWeightBoldSlider, OptionsModel::FontWeightBold);
        mapper->addMapping(ui->animateNightSky, OptionsModel::AnimateNightSky);
        m_populating = true;
        mapper->toFirst();
        m_populating = false;
        showFontWeightNormal();
    }
}

void AppearanceWidget::accept()
{
    fAcceptChanges = true;
}

void AppearanceWidget::updateTheme(const QString& theme)
{
    QString newValue = theme.isEmpty() ? ui->theme->currentData().toString() : theme;
    if (GUIUtil::getActiveTheme() != newValue) {
        QSettings().setValue("theme", newValue);
        // Force loading the theme
        if (model) {
            GUIUtil::loadTheme(true);
        }
        showFontWeightNormal();
        Q_EMIT themeChanged(newValue != prevTheme);
    }
}

void AppearanceWidget::updateFontFamily(int index)
{
    GUIUtil::setFontFamily(static_cast<GUIUtil::FontFamily>(ui->fontFamily->itemData(index).toInt()));
    updateWeightSlider(true);
    showFontWeightNormal();
}

void AppearanceWidget::updateFontScale(int nScale)
{
    GUIUtil::setFontScale(nScale);
}

void AppearanceWidget::updateFontWeightNormal(int nValue, bool fForce, bool fChosen)
{
    int nSliderValue = nValue;
    if (nValue > ui->fontWeightBoldSlider->value() && !fForce) {
        nSliderValue = ui->fontWeightBoldSlider->value();
    }
    const QSignalBlocker blocker(ui->fontWeightNormalSlider);
    ui->fontWeightNormalSlider->setValue(nSliderValue);
    // The slider holds a value of its own again.
    m_shows_stand_in = false;
    GUIUtil::setFontWeightNormal(GUIUtil::supportedWeightFromIndex(ui->fontWeightNormalSlider->value()), fChosen);
}

void AppearanceWidget::showFontWeightNormal()
{
    // The slider shows the Regular only where Abyss draws it in place of the
    // weight held while none was chosen, and keeps its own value aside
    // meanwhile. Elsewhere it holds what the settings or the user put in it,
    // as before.
    const QFont::Weight drawn = GUIUtil::getPresentationFontWeightNormal();
    int index = -1;
    if (!GUIUtil::hasExplicitFontWeightNormal() && drawn != GUIUtil::getFontWeightNormal()) {
        index = GUIUtil::supportedWeightToIndex(drawn);
        if (index > ui->fontWeightBoldSlider->value()) {
            index = -1;
        }
    }
    if (index == -1) {
        putBackFontWeightNormal();
        return;
    }
    if (!m_shows_stand_in) {
        m_normal_slider_value = ui->fontWeightNormalSlider->value();
        m_shows_stand_in = true;
    }
    const QSignalBlocker blocker(ui->fontWeightNormalSlider);
    ui->fontWeightNormalSlider->setValue(index);
}

void AppearanceWidget::putBackFontWeightNormal()
{
    if (!m_shows_stand_in) {
        return;
    }
    m_shows_stand_in = false;
    const QSignalBlocker blocker(ui->fontWeightNormalSlider);
    ui->fontWeightNormalSlider->setValue(m_normal_slider_value);
}

void AppearanceWidget::updateFontWeightBold(int nValue, bool fForce)
{
    int nSliderValue = nValue;
    // The normal slider's own value: the Regular it may show in its place is no limit.
    const int nNormalValue = m_shows_stand_in ? m_normal_slider_value : ui->fontWeightNormalSlider->value();
    if (nValue < nNormalValue && !fForce) {
        nSliderValue = nNormalValue;
    }
    const QSignalBlocker blocker(ui->fontWeightBoldSlider);
    ui->fontWeightBoldSlider->setValue(nSliderValue);
    GUIUtil::setFontWeightBold(GUIUtil::supportedWeightFromIndex(ui->fontWeightBoldSlider->value()));
}

void AppearanceWidget::updateWeightSlider(const bool fForce)
{
    int nMaximum = GUIUtil::getSupportedWeights().size() - 1;

    ui->fontWeightNormalSlider->setMinimum(0);
    ui->fontWeightNormalSlider->setMaximum(nMaximum);

    ui->fontWeightBoldSlider->setMinimum(0);
    ui->fontWeightBoldSlider->setMaximum(nMaximum);

    if (fForce || !GUIUtil::isSupportedWeight(prevWeightNormal) || !GUIUtil::isSupportedWeight(prevWeightBold)) {
        int nIndexNormal = GUIUtil::supportedWeightToIndex(GUIUtil::getSupportedFontWeightNormalDefault());
        int nIndexBold = GUIUtil::supportedWeightToIndex(GUIUtil::getSupportedFontWeightBoldDefault());
        assert(nIndexNormal != -1 && nIndexBold != -1);
        // Another family's default weight is not a choice. A weight of this
        // family the slider does not offer (the bold one, say, given as an
        // argument) resets both as before, and leaves the normal weight's
        // choice as it was.
        updateFontWeightNormal(nIndexNormal, true, fForce ? false : GUIUtil::hasExplicitFontWeightNormal());
        updateFontWeightBold(nIndexBold, true);
    }
}
