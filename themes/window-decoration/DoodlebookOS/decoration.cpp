/*
 * SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
 * SPDX-FileCopyrightText: 2014 Hugo Pereira Da Costa <hugo.pereira@free.fr>
 * SPDX-FileCopyrightText: 2018 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
 * SPDX-FileCopyrightText: 2021 Paul McAuley <kde@paulmcauley.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "decoration.h"

#include "settingsprovider.h"

#include "button.h"

#include "boxshadowrenderer.h"

#include <KDecoration3/DecorationButtonGroup>
#include <KDecoration3/DecorationShadow>
#include <KDecoration3/ScaleHelpers>

#include <KColorUtils>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KSharedConfig>

#include <KColorScheme>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <QTimer>
#include <QSvgRenderer>
#include <QHoverEvent>
#include <QMouseEvent>

static constexpr const char s_svgArrowDown[] =
    R"(<svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="#e3e3e3"><path d="M465-363.5q-7-2.5-13-8.5L268-556q-11-11-11-28t11-28q11-11 28-11t28 11l156 156 156-156q11-11 28-11t28 11q11 11 11 28t-11 28L508-372q-6 6-13 8.5t-15 2.5q-8 0-15-2.5Z"/></svg>)";

K_PLUGIN_FACTORY_WITH_JSON(DoodlebookOSDecoFactory, "metadata.json", registerPlugin<DoodlebookOS::Decoration>(); registerPlugin<DoodlebookOS::Button>();)

namespace
{
struct ShadowParams {
    ShadowParams()
        : offset(QPoint(0, 0))
        , radius(0)
        , opacity(0)
    {
    }

    ShadowParams(const QPoint &offset, int radius, qreal opacity)
        : offset(offset)
        , radius(radius)
        , opacity(opacity)
    {
    }

    QPoint offset;
    int radius;
    qreal opacity;
};

struct CompositeShadowParams {
    CompositeShadowParams() = default;

    CompositeShadowParams(const QPoint &offset, const ShadowParams &shadow1, const ShadowParams &shadow2)
        : offset(offset)
        , shadow1(shadow1)
        , shadow2(shadow2)
    {
    }

    bool isNone() const
    {
        return qMax(shadow1.radius, shadow2.radius) == 0;
    }

    QPoint offset;
    ShadowParams shadow1;
    ShadowParams shadow2;
};

const CompositeShadowParams s_shadowParams[] = {
    // None
    CompositeShadowParams(),
    // Small
    CompositeShadowParams(QPoint(0, 2), ShadowParams(QPoint(0, 1), 20, 0.14), ShadowParams(QPoint(0, 3), 10, 0.14)),
    // Medium
    CompositeShadowParams(QPoint(0, 3), ShadowParams(QPoint(0, 2), 26, 0.16), ShadowParams(QPoint(0, 5), 14, 0.16)),
    // Large (Default)
    CompositeShadowParams(QPoint(0, 4), ShadowParams(QPoint(0, 2), 32, 0.18), ShadowParams(QPoint(0, 6), 16, 0.18)),
    // Very large
    CompositeShadowParams(QPoint(0, 6), ShadowParams(QPoint(0, 3), 44, 0.20), ShadowParams(QPoint(0, 8), 22, 0.20)),
};

inline CompositeShadowParams lookupShadowParams(int size)
{
    switch (size) {
    case DoodlebookOS::InternalSettings::ShadowNone:
        return s_shadowParams[0];
    case DoodlebookOS::InternalSettings::ShadowSmall:
        return s_shadowParams[1];
    case DoodlebookOS::InternalSettings::ShadowMedium:
        return s_shadowParams[2];
    case DoodlebookOS::InternalSettings::ShadowLarge:
        return s_shadowParams[3];
    case DoodlebookOS::InternalSettings::ShadowVeryLarge:
        return s_shadowParams[4];
    default:
        // Fallback to the Large size.
        return s_shadowParams[3];
    }
}
}

namespace DoodlebookOS
{
using KDecoration3::ColorGroup;
using KDecoration3::ColorRole;

//________________________________________________________________
static int g_sDecoCount = 0;
static int g_shadowSizeEnum = InternalSettings::ShadowLarge;
static int g_shadowStrength = 255;
static QColor g_shadowColor = Qt::black;
static std::shared_ptr<KDecoration3::DecorationShadow> g_sShadow;
static std::shared_ptr<KDecoration3::DecorationShadow> g_sShadowInactive;

//________________________________________________________________
Decoration::Decoration(QObject *parent, const QVariantList &args)
    : KDecoration3::Decoration(parent, args)
    , m_animation(new QVariantAnimation(this))
    , m_shadowAnimation(new QVariantAnimation(this))
    , m_arrowAnimation(new QVariantAnimation(this))
{
    g_sDecoCount++;

    m_arrowAnimation->setStartValue(0.0);
    m_arrowAnimation->setEndValue(1.0);
    m_arrowAnimation->setDuration(150);
    m_arrowAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_arrowAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_arrowOpacity = value.toReal();
        if (m_arrowHitRect.isValid()) {
            update(m_arrowHitRect.adjusted(-2, -2, 2, 2));
        } else {
            update();
        }
    });
}

//________________________________________________________________
Decoration::~Decoration()
{
    g_sDecoCount--;
    if (g_sDecoCount == 0) {
        // last deco destroyed, clean up shadow
        g_sShadow.reset();
    }
}

//________________________________________________________________
void Decoration::setOpacity(qreal value)
{
    if (m_opacity == value) {
        return;
    }
    m_opacity = value;
    update();
}

//________________________________________________________________
QColor Decoration::titleBarColor() const
{
    return window()->color(ColorGroup::Active, ColorRole::TitleBar);
}

//________________________________________________________________
QColor Decoration::fontColor() const
{
    return window()->color(ColorGroup::Active, ColorRole::Foreground);
}

//________________________________________________________________
bool Decoration::init()
{
    // active state change animation
    // It is important start and end value are of the same type, hence 0.0 and not just 0
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    // Linear to have the same easing as Breeze animations
    m_animation->setEasingCurve(QEasingCurve::Linear);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        setOpacity(value.toReal());
    });

    m_shadowAnimation->setStartValue(0.0);
    m_shadowAnimation->setEndValue(1.0);
    m_shadowAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_shadowAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_shadowOpacity = value.toReal();
        updateShadow();
    });

    // use DBus connection to update on breeze configuration change
    auto dbus = QDBusConnection::sessionBus();
    dbus.connect(QString(),
                 QStringLiteral("/KGlobalSettings"),
                 QStringLiteral("org.kde.KGlobalSettings"),
                 QStringLiteral("notifyChange"),
                 this,
                 SLOT(reconfigure()));

    reconfigure();
    updateTitleBar();
    auto s = settings();
    connect(s.get(), &KDecoration3::DecorationSettings::borderSizeChanged, this, &Decoration::recalculateBorders);

    // a change in font might cause the borders to change
    connect(s.get(), &KDecoration3::DecorationSettings::fontChanged, this, &Decoration::recalculateBorders);
    connect(s.get(), &KDecoration3::DecorationSettings::spacingChanged, this, &Decoration::recalculateBorders);

    // buttons
    connect(s.get(), &KDecoration3::DecorationSettings::spacingChanged, this, &Decoration::updateButtonsGeometryDelayed);
    connect(s.get(), &KDecoration3::DecorationSettings::decorationButtonsLeftChanged, this, &Decoration::updateButtonsGeometryDelayed);
    connect(s.get(), &KDecoration3::DecorationSettings::decorationButtonsRightChanged, this, &Decoration::updateButtonsGeometryDelayed);

    // full reconfiguration
    connect(s.get(), &KDecoration3::DecorationSettings::reconfigured, this, &Decoration::reconfigure);
    connect(s.get(), &KDecoration3::DecorationSettings::reconfigured, SettingsProvider::self(), &SettingsProvider::reconfigure, Qt::UniqueConnection);
    connect(s.get(), &KDecoration3::DecorationSettings::reconfigured, this, &Decoration::updateButtonsGeometryDelayed);

    connect(window(), &KDecoration3::DecoratedWindow::activeChanged, this, &Decoration::recalculateBorders);
    connect(window(), &KDecoration3::DecoratedWindow::adjacentScreenEdgesChanged, this, &Decoration::recalculateBorders);
    connect(window(), &KDecoration3::DecoratedWindow::maximizedHorizontallyChanged, this, &Decoration::recalculateBorders);
    connect(window(), &KDecoration3::DecoratedWindow::maximizedVerticallyChanged, this, &Decoration::recalculateBorders);
    connect(window(), &KDecoration3::DecoratedWindow::shadedChanged, this, &Decoration::recalculateBorders);
    connect(window(), &KDecoration3::DecoratedWindow::captionChanged, this, [this]() {
        // update the caption area
        update(titleBar());
    });

    connect(window(), &KDecoration3::DecoratedWindow::activeChanged, this, &Decoration::updateAnimationState);
    connect(this, &KDecoration3::Decoration::bordersChanged, this, &Decoration::updateTitleBar);
    connect(this, &KDecoration3::Decoration::bordersChanged, this, &Decoration::updateButtonsGeometry);
    connect(window(), &KDecoration3::DecoratedWindow::adjacentScreenEdgesChanged, this, &Decoration::updateTitleBar);
    connect(window(), &KDecoration3::DecoratedWindow::widthChanged, this, &Decoration::updateTitleBar);
    connect(window(), &KDecoration3::DecoratedWindow::maximizedChanged, this, &Decoration::updateTitleBar);
    connect(window(), &KDecoration3::DecoratedWindow::maximizedChanged, this, &Decoration::setOpaque);

    connect(window(), &KDecoration3::DecoratedWindow::widthChanged, this, &Decoration::updateButtonsGeometry);
    connect(window(), &KDecoration3::DecoratedWindow::maximizedChanged, this, &Decoration::updateButtonsGeometry);
    connect(window(), &KDecoration3::DecoratedWindow::adjacentScreenEdgesChanged, this, &Decoration::updateButtonsGeometry);
    connect(window(), &KDecoration3::DecoratedWindow::shadedChanged, this, &Decoration::updateButtonsGeometry);

    connect(window(), &KDecoration3::DecoratedWindow::nextScaleChanged, this, &Decoration::updateScale);
    connect(window(), &KDecoration3::DecoratedWindow::scaleChanged, this, &Decoration::updateScale);

    createButtons();
    updateScale();
    updateShadow();
    return true;
}

//________________________________________________________________
void Decoration::updateTitleBar()
{
    setTitleBar(QRectF(0, 0, window()->width(), borderTop()));
}

//________________________________________________________________
void Decoration::updateAnimationState()
{
    if (m_shadowAnimation->duration() > 0) {
        m_shadowAnimation->setDirection(window()->isActive() ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
        m_shadowAnimation->setEasingCurve(window()->isActive() ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
        if (m_shadowAnimation->state() != QAbstractAnimation::Running) {
            m_shadowAnimation->start();
        }

    } else {
        updateShadow();
    }

    if (m_animation->duration() > 0) {
        m_animation->setDirection(window()->isActive() ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
        if (m_animation->state() != QAbstractAnimation::Running) {
            m_animation->start();
        }

    } else {
        update();
    }
}

//________________________________________________________________
qreal Decoration::borderSize(bool bottom, qreal scale) const
{
    const qreal pixelSize = KDecoration3::pixelSize(scale);
    const qreal baseSize = std::max<qreal>(pixelSize, KDecoration3::snapToPixelGrid(settings()->smallSpacing(), scale));

    switch (settings()->borderSize()) {
    case KDecoration3::BorderSize::None:
        return 0;
    case KDecoration3::BorderSize::NoSides:
        if (bottom) {
            return KDecoration3::snapToPixelGrid(std::max(4.0, baseSize + Metrics::Frame_FrameRadius), scale);
        } else {
            return 0;
        }
    default:
    case KDecoration3::BorderSize::Tiny:
        if (bottom) {
            return KDecoration3::snapToPixelGrid(std::max(4.0, baseSize), scale);
        } else {
            return baseSize;
        }
    case KDecoration3::BorderSize::Normal:
        return baseSize * 2;
    case KDecoration3::BorderSize::Large:
        return baseSize * 3;
    case KDecoration3::BorderSize::VeryLarge:
        return baseSize * 4;
    case KDecoration3::BorderSize::Huge:
        return baseSize * 5;
    case KDecoration3::BorderSize::VeryHuge:
        return baseSize * 6;
    case KDecoration3::BorderSize::Oversized:
        return baseSize * 10;
    }
}

//________________________________________________________________
void Decoration::reconfigure()
{
    m_internalSettings = SettingsProvider::self()->internalSettings(this);

    setScaledCornerRadius();

    // animation

    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    const KConfigGroup cg(config, QStringLiteral("KDE"));

    m_animation->setDuration(0);
    // Syncing anis between client and decoration is troublesome, so we're not using
    // any animations right now.
    // m_animation->setDuration( cg.readEntry("AnimationDurationFactor", 1.0f) * 100.0f );

    // But the shadow is fine to animate like this!
    m_shadowAnimation->setDuration(cg.readEntry("AnimationDurationFactor", 1.0f) * 100.0f);

    // borders
    recalculateBorders();

    // shadow
    updateShadow();
}

QMarginsF Decoration::bordersFor(qreal scale) const
{
    const qreal left = isLeftEdge() ? 0 : borderSize(false, scale);
    const qreal right = isRightEdge() ? 0 : borderSize(false, scale);
    const qreal bottom = (window()->isShaded() || isBottomEdge()) ? 0 : borderSize(true, scale);

    qreal top = 0;
    if (hideTitleBar()) {
        top = bottom;
    } else {
        top = KDecoration3::snapToPixelGrid(45.0 / scale, scale);
    }
    return QMarginsF(left, top, right, bottom);
}

void Decoration::recalculateBorders()
{
    setBorders(bordersFor(window()->nextScale()));

    // extended sizes
    const qreal extSize = KDecoration3::snapToPixelGrid(settings()->largeSpacing(), window()->nextScale());
    qreal extTop = 0;
    if (!isMaximizedVertically()) {
        extTop = extSize;
    }
    qreal extSides = 0;
    qreal extBottom = 0;
    if (hasNoBorders()) {
        if (!isMaximizedHorizontally()) {
            extSides = extSize;
        }
        if (!isMaximizedVertically()) {
            extBottom = extSize;
        }

    } else if (hasNoSideBorders() && !isMaximizedHorizontally()) {
        extSides = extSize;
    }

    setResizeOnlyBorders(QMarginsF(extSides, extTop, extSides, extBottom));

    const bool isTiled = isTopEdge() || isLeftEdge() || isBottomEdge() || isRightEdge();

    qreal topLeftRadius = 0;
    qreal topRightRadius = 0;
    qreal bottomLeftRadius = 0;
    qreal bottomRightRadius = 0;
    if (!isTiled && hasNoBorders() && m_internalSettings->roundedCorners()) {
        if (!isBottomEdge()) {
            if (!isLeftEdge()) {
                bottomLeftRadius = m_scaledCornerRadius;
            }
            if (!isRightEdge()) {
                bottomRightRadius = m_scaledCornerRadius;
            }
        }
        if (hideTitleBar()) {
            if (!isTopEdge()) {
                if (!isLeftEdge()) {
                    topLeftRadius = m_scaledCornerRadius;
                }
                if (!isRightEdge()) {
                    topRightRadius = m_scaledCornerRadius;
                }
            }
        }
    }
    setBorderRadius(KDecoration3::BorderRadius(topLeftRadius, topRightRadius, bottomRightRadius, bottomLeftRadius));

    if (isMaximized() || !outlinesEnabled()) {
        setBorderOutline(KDecoration3::BorderOutline());
    } else {
        const auto color = KColorUtils::mix(window()->color(ColorGroup::Active, ColorRole::Frame),
                                            window()->palette().text().color(),
                                            0.20);
        const qreal thickness = std::max(KDecoration3::pixelSize(window()->nextScale()), KDecoration3::snapToPixelGrid(1, window()->nextScale()));

        qreal topLeftRightRadius = 0;
        qreal bottomLeftRadius = 0;
        qreal bottomRightRadius = 0;
        if (!isTiled && (!hideTitleBar() || m_internalSettings->roundedCorners())) {
            topLeftRightRadius = m_scaledCornerRadius;
        }
        if (!isTiled && (!hasNoBorders() || m_internalSettings->roundedCorners())) {
            bottomLeftRadius = m_scaledCornerRadius;
            bottomRightRadius = m_scaledCornerRadius;
        }

        const auto radius = KDecoration3::BorderRadius(topLeftRightRadius, topLeftRightRadius, bottomRightRadius, bottomLeftRadius);
        setBorderOutline(KDecoration3::BorderOutline(thickness, color, radius));
    }
}

//________________________________________________________________
void Decoration::createButtons()
{
    m_leftButtons = new KDecoration3::DecorationButtonGroup(KDecoration3::DecorationButtonGroup::Position::Left, this, &Button::create);
    m_rightButtons = new KDecoration3::DecorationButtonGroup(KDecoration3::DecorationButtonGroup::Position::Right, this, &Button::create);
    updateButtonsGeometry();
}

//________________________________________________________________
void Decoration::updateButtonsGeometryDelayed()
{
    QTimer::singleShot(0, this, &Decoration::updateButtonsGeometry);
}

//________________________________________________________________
void Decoration::updateButtonsGeometry()
{
    const qreal scale = effectiveScale();

    // adjust button position
    const auto buttonList = m_leftButtons->buttons() + m_rightButtons->buttons();
    for (KDecoration3::DecorationButton *button : buttonList) {
        auto btn = static_cast<Button *>(button);
        btn->reconfigure();
        const QSizeF preferredSize = btn->preferredSize();
        btn->setGeometry(QRectF(QPointF(0, 0), preferredSize));
        btn->setPadding(QMargins(0, 0, 0, 0));
    }

    // left buttons: App Icon (Menu) - 20px physical from left, (45-25)/2 = 10px vertical
    if (!m_leftButtons->buttons().isEmpty()) {
        const qreal leftMargin = KDecoration3::snapToPixelGrid(20.0 / scale, scale);
        const qreal vPos = KDecoration3::snapToPixelGrid((45.0 - 25.0) / 2.0 / scale, scale);
        m_leftButtons->setSpacing(0);
        m_leftButtons->setPos(QPointF(leftMargin, vPos));
    }

    // right buttons: Minimize, Maximize, Close
    if (!m_rightButtons->buttons().isEmpty()) {
        // Button width is 34px physical. Desired icon pitch is 45px physical.
        // Gap between buttons = 45px - 34px = 11px physical
        const qreal spacing = KDecoration3::snapToPixelGrid((45.0 - 34.0) / scale, scale);
        m_rightButtons->setSpacing(spacing);

        // Right margin from button right edge to window right edge:
        // Target icon center distance from window right edge: 34px physical.
        // Button width is 34px physical -> center is 17px physical from right edge.
        // Distance from button right edge to window right edge = 34px - 17px = 17px physical.
        const qreal rightMarginToBtn = KDecoration3::snapToPixelGrid((34.0 - 34.0 / 2.0) / scale, scale);
        m_rightButtons->setPos(QPointF(size().width() - m_rightButtons->geometry().width() - rightMarginToBtn, 0));
    }

    update();
}

//________________________________________________________________
void Decoration::paint(QPainter *painter, const QRectF &repaintRegion)
{
    // TODO: optimize based on repaintRegion
    auto s = settings();
    const bool isTiled = isTopEdge() || isLeftEdge() || isBottomEdge() || isRightEdge();
    // paint background
    if (!window()->isShaded()) {
        painter->fillRect(rect(), Qt::transparent);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(window()->color(ColorGroup::Active, ColorRole::Frame));

        // clip away the top part
        if (!hideTitleBar()) {
            painter->setClipRect(QRectF(0, borderTop(), size().width(), size().height() - borderTop()), Qt::IntersectClip);
        }

        if (s->isAlphaChannelSupported()) {
            if (hasNoBorders() || isTiled) {
                painter->drawRoundedRect(rect(), 0, 0);
            } else {
                painter->drawRoundedRect(rect(), m_scaledCornerRadius, m_scaledCornerRadius);
            }
        } else {
            painter->drawRect(rect());
        }

        painter->restore();
    }

    if (!hideTitleBar()) {
        paintTitleBar(painter, repaintRegion);
    }

    if (hasBorders() && !s->isAlphaChannelSupported()) {
        const QColor borderColor = borderOutline().color();
        if (borderColor.alphaF() > 0) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(borderColor);
            painter->drawRect(rect().adjusted(0, 0, -1, -1));
            painter->restore();
        }
    }
}

//________________________________________________________________
void Decoration::paintTitleBar(QPainter *painter, const QRectF &repaintRegion)
{
    QRectF rect(QPointF(0, 0), QSizeF(size().width(), borderTop()));
    QBrush frontBrush;
    QBrush backBrush(this->titleBarColor());
    const bool isTiled = isTopEdge() || isLeftEdge() || isBottomEdge() || isRightEdge();

    if (!rect.intersects(repaintRegion)) {
        return;
    }

    painter->save();
    painter->setPen(Qt::NoPen);

    // render a linear gradient on title area
    if (window()->isActive() && m_internalSettings->drawBackgroundGradient()) {
        QLinearGradient gradient(0, 0, 0, rect.height());
        gradient.setColorAt(0.0, titleBarColor().lighter(120));
        gradient.setColorAt(0.8, titleBarColor());

        frontBrush = gradient;

    } else {
        frontBrush = titleBarColor();

        painter->setBrush(titleBarColor());
    }

    if (isMaximized() || isTiled || !settings()->isAlphaChannelSupported()) {
        painter->setBrush(backBrush);
        painter->drawRect(rect);

        painter->setBrush(frontBrush);
        painter->drawRect(rect);
    } else if (window()->isShaded()) {
        painter->setBrush(backBrush);
        painter->drawRoundedRect(rect, m_scaledCornerRadius, m_scaledCornerRadius);

        painter->setBrush(frontBrush);
        painter->drawRoundedRect(rect, m_scaledCornerRadius, m_scaledCornerRadius);

    } else {
        painter->setClipRect(rect, Qt::IntersectClip);

        auto drawThe = [this, painter](const QRectF &r) {
            painter->drawRoundedRect(r, m_scaledCornerRadius, m_scaledCornerRadius);
            // remove the rounding on the bottom
            painter->drawRect(QRectF(r.bottomLeft() - QPointF(0, m_scaledCornerRadius), r.bottomRight()));
        };

        painter->setBrush(backBrush);
        drawThe(rect);

        painter->setBrush(frontBrush);
        drawThe(rect);
    }

    painter->restore();

    // draw caption
    QFont captionFont = settings()->font();
    captionFont.setFamily(QStringLiteral("Google Sans Flex"));
    const qreal scale = effectiveScale();
    const qreal fontDps = KDecoration3::snapToPixelGrid((27.0 * 0.54) / scale, scale);
    captionFont.setPointSizeF(fontDps * 72.0 / 96.0);
    captionFont.setWeight(QFont::Medium);
    painter->setFont(captionFont);
    painter->setPen(fontColor());
    const auto [captionRectangle, alignment] = captionRect();
    const qreal arrowGap = KDecoration3::snapToPixelGrid(14.0 / scale, scale);
    const qreal arrowSize = KDecoration3::snapToPixelGrid((10.0 * 960.0 / 446.0) / scale, scale);
    const qreal maxTextWidth = std::max((qreal)0.0, captionRectangle.width() - arrowGap - arrowSize);
    const QString caption = painter->fontMetrics().elidedText(window()->caption(), Qt::ElideMiddle, maxTextWidth);
    painter->drawText(captionRectangle, alignment | Qt::TextSingleLine, caption);

    m_arrowHitRect = arrowHitRect();
    if (!caption.isEmpty() && m_arrowHitRect.isValid()) {
        static const QColor s_hoverColor(QStringLiteral("#3d3e3f"));
        if (m_arrowPressed) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(s_hoverColor.darker(115));
            painter->setRenderHint(QPainter::Antialiasing);
            painter->drawEllipse(m_arrowHitRect);
            painter->restore();
        } else if (m_arrowOpacity > 0.0) {
            QColor c(s_hoverColor);
            c.setAlphaF(m_arrowOpacity);
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(c);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->drawEllipse(m_arrowHitRect);
            painter->restore();
        }

        const QRectF arrowRect(m_arrowHitRect.center().x() - arrowSize / 2.0,
                               m_arrowHitRect.center().y() - arrowSize / 2.0,
                               arrowSize, arrowSize);

        static QSvgRenderer arrowRenderer;
        static bool arrowLoaded = false;
        if (!arrowLoaded) {
            arrowRenderer.load(QByteArray::fromRawData(s_svgArrowDown, qstrlen(s_svgArrowDown)));
            arrowLoaded = true;
        }

        if (arrowRenderer.isValid()) {
            const int renderSize = 64;
            static QHash<quint32, QImage> arrowCache;
            const quint32 colorKey = fontColor().rgba();
            auto it = arrowCache.constFind(colorKey);
            if (it == arrowCache.constEnd()) {
                QImage img(renderSize, renderSize, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);

                QPainter imgPainter(&img);
                imgPainter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
                arrowRenderer.render(&imgPainter, QRectF(0, 0, renderSize, renderSize));
                imgPainter.end();

                QPainter tintPainter(&img);
                tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tintPainter.fillRect(img.rect(), fontColor());
                tintPainter.end();

                it = arrowCache.insert(colorKey, img);
            }
            painter->drawImage(arrowRect, it.value());
        }
    }

    // draw all buttons
    m_leftButtons->paint(painter, repaintRegion);
    m_rightButtons->paint(painter, repaintRegion);
}

//________________________________________________________________
int Decoration::buttonSize() const
{
    const int baseSize = settings()->gridUnit();
    switch (m_internalSettings->buttonSize()) {
    case InternalSettings::ButtonTiny:
        return baseSize;
    case InternalSettings::ButtonSmall:
        return baseSize * 1.5;
    default:
    case InternalSettings::ButtonDefault:
        return baseSize * 2;
    case InternalSettings::ButtonLarge:
        return baseSize * 2.5;
    case InternalSettings::ButtonVeryLarge:
        return baseSize * 3.5;
    }
}

//________________________________________________________________
qreal Decoration::captionHeight() const
{
    return borderTop();
}

//________________________________________________________________
qreal Decoration::effectiveScale() const
{
    if (window()) {
        if (window()->nextScale() > 0) {
            return window()->nextScale();
        }
        if (window()->scale() > 0) {
            return window()->scale();
        }
    }
    return 1.25;
}

//________________________________________________________________
QPair<QRectF, Qt::Alignment> Decoration::captionRect() const
{
    if (hideTitleBar()) {
        return qMakePair(QRectF(), Qt::AlignCenter);
    } else {
        const qreal scale = effectiveScale();
        const qreal leftMargin = KDecoration3::snapToPixelGrid(20.0 / scale, scale);
        const qreal titleGap = KDecoration3::snapToPixelGrid(11.0 / scale, scale);

        qreal leftOffset = leftMargin;
        if (!m_leftButtons->buttons().isEmpty()) {
            leftOffset = m_leftButtons->geometry().x() + m_leftButtons->geometry().width() + titleGap;
        }

        const qreal rightOffset = m_rightButtons->buttons().isEmpty()
            ? KDecoration3::snapToPixelGrid(34.0 / scale, scale)
            : size().width() - m_rightButtons->geometry().x() + titleGap;

        const QRectF maxRect(leftOffset, 0, std::max((qreal)0.0, size().width() - leftOffset - rightOffset), captionHeight());
        return qMakePair(maxRect, Qt::AlignVCenter | Qt::AlignLeft);
    }
}

//________________________________________________________________
void Decoration::updateShadow()
{
    auto s = settings();

    // Animated case, no cached shadow object
    if ((m_shadowAnimation->state() == QAbstractAnimation::Running) && (m_shadowOpacity != 0.0) && (m_shadowOpacity != 1.0)) {
        setShadow(createShadowObject(0.5 + m_shadowOpacity * 0.5));
        return;
    }

    if (g_shadowSizeEnum != m_internalSettings->shadowSize() || g_shadowStrength != m_internalSettings->shadowStrength()
        || g_shadowColor != m_internalSettings->shadowColor()) {
        g_sShadow.reset();
        g_sShadowInactive.reset();
        g_shadowSizeEnum = m_internalSettings->shadowSize();
        g_shadowStrength = m_internalSettings->shadowStrength();
        g_shadowColor = m_internalSettings->shadowColor();
    }

    auto &shadow = (window()->isActive()) ? g_sShadow : g_sShadowInactive;
    if (!shadow) {
        g_sShadow = createShadowObject(1.0);
        g_sShadowInactive = createShadowObject(0.5);
    }
    setShadow(shadow);
}

//________________________________________________________________
std::shared_ptr<KDecoration3::DecorationShadow> Decoration::createShadowObject(const float strengthScale)
{
    CompositeShadowParams params = lookupShadowParams(m_internalSettings->shadowSize());
    if (params.isNone()) {
        // If shadows are disabled, return nothing
        return nullptr;
    }

    auto withOpacity = [](const QColor &color, qreal opacity) -> QColor {
        QColor c(color);
        c.setAlphaF(opacity);
        return c;
    };

    const QSize boxSize =
        BoxShadowRenderer::calculateMinimumBoxSize(params.shadow1.radius).expandedTo(BoxShadowRenderer::calculateMinimumBoxSize(params.shadow2.radius));

    BoxShadowRenderer shadowRenderer;
    shadowRenderer.setBorderRadius(m_scaledCornerRadius + 0.5);
    shadowRenderer.setBoxSize(boxSize);

    const qreal strength = m_internalSettings->shadowStrength() / 255.0 * strengthScale;
    shadowRenderer.addShadow(params.shadow1.offset, params.shadow1.radius, withOpacity(m_internalSettings->shadowColor(), params.shadow1.opacity * strength));
    shadowRenderer.addShadow(params.shadow2.offset, params.shadow2.radius, withOpacity(m_internalSettings->shadowColor(), params.shadow2.opacity * strength));

    QImage shadowTexture = shadowRenderer.render();

    QPainter painter(&shadowTexture);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF outerRect = shadowTexture.rect();

    QRectF boxRect(QPoint(0, 0), boxSize);
    boxRect.moveCenter(outerRect.center());

    // Mask out inner rect.
    const QMarginsF padding = QMarginsF(boxRect.left() - outerRect.left() - Metrics::Shadow_Overlap - params.offset.x(),
                                        boxRect.top() - outerRect.top() - Metrics::Shadow_Overlap - params.offset.y(),
                                        outerRect.right() - boxRect.right() - Metrics::Shadow_Overlap + params.offset.x(),
                                        outerRect.bottom() - boxRect.bottom() - Metrics::Shadow_Overlap + params.offset.y());
    QRectF innerRect = outerRect - padding;
    // Push the shadow slightly under the window, which helps avoiding glitches with fractional scaling
    // TODO fix this more properly
    innerRect.adjust(2, 2, -2, -2);

    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    painter.drawRoundedRect(innerRect, m_scaledCornerRadius + 0.5, m_scaledCornerRadius + 0.5);

    painter.end();

    auto ret = std::make_shared<KDecoration3::DecorationShadow>();
    ret->setPadding(padding);
    ret->setInnerShadowRect(QRectF(outerRect.center(), QSizeF(1, 1)));
    ret->setShadow(shadowTexture);
    return ret;
}

void Decoration::setScaledCornerRadius()
{
    const qreal scale = window()->nextScale() > 0 ? window()->nextScale() : 1.25;
    m_scaledCornerRadius = KDecoration3::snapToPixelGrid(12.0 / scale, scale);
}

QRectF Decoration::arrowHitRect() const
{
    if (hideTitleBar()) {
        return QRectF();
    }
    const auto [captionRectangle, alignment] = captionRect();
    const qreal scale = effectiveScale();
    const qreal fontDps = KDecoration3::snapToPixelGrid((27.0 * 0.54) / scale, scale);
    QFont captionFont = settings()->font();
    captionFont.setFamily(QStringLiteral("Google Sans Flex"));
    captionFont.setPointSizeF(fontDps * 72.0 / 96.0);
    captionFont.setWeight(QFont::Medium);

    const qreal arrowGap = KDecoration3::snapToPixelGrid(14.0 / scale, scale);
    const qreal arrowSize = KDecoration3::snapToPixelGrid((10.0 * 960.0 / 446.0) / scale, scale);
    const qreal maxTextWidth = std::max((qreal)0.0, captionRectangle.width() - arrowGap - arrowSize);

    QFontMetricsF fm(captionFont);
    const QString caption = fm.elidedText(window()->caption(), Qt::ElideMiddle, maxTextWidth);
    if (caption.isEmpty()) {
        return QRectF();
    }

    const qreal textWidth = fm.horizontalAdvance(caption);
    const qreal circleD = KDecoration3::snapToPixelGrid(28.0 / scale, scale);
    const QPointF arrowCenter(captionRectangle.x() + textWidth + arrowGap + arrowSize / 2.0,
                              captionRectangle.y() + captionRectangle.height() / 2.0);
    return QRectF(arrowCenter.x() - circleD / 2.0, arrowCenter.y() - circleD / 2.0, circleD, circleD);
}

void Decoration::updateArrowAnimation(bool hovered)
{
    m_arrowAnimation->setDirection(hovered ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
    if (m_arrowAnimation->state() != QAbstractAnimation::Running) {
        m_arrowAnimation->start();
    }
}

void Decoration::hoverMoveEvent(QHoverEvent *event)
{
    m_arrowHitRect = arrowHitRect();
    const bool hovered = m_arrowHitRect.isValid() && m_arrowHitRect.contains(event->position());
    if (hovered != m_arrowHovered) {
        m_arrowHovered = hovered;
        updateArrowAnimation(hovered);
    }
    KDecoration3::Decoration::hoverMoveEvent(event);
}

void Decoration::hoverLeaveEvent(QHoverEvent *event)
{
    if (m_arrowHovered) {
        m_arrowHovered = false;
        updateArrowAnimation(false);
    }
    KDecoration3::Decoration::hoverLeaveEvent(event);
}

void Decoration::mousePressEvent(QMouseEvent *event)
{
    m_arrowHitRect = arrowHitRect();
    if (m_arrowHitRect.isValid() && m_arrowHitRect.contains(event->position())) {
        if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
            m_arrowPressed = true;
            update(m_arrowHitRect.adjusted(-2, -2, 2, 2));
            event->accept();
            return;
        }
    }
    KDecoration3::Decoration::mousePressEvent(event);
}

void Decoration::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_arrowPressed) {
        m_arrowPressed = false;
        update(m_arrowHitRect.adjusted(-2, -2, 2, 2));
        if (m_arrowHitRect.isValid() && m_arrowHitRect.contains(event->position())) {
            requestShowWindowMenu(m_arrowHitRect.toRect());
        }
        event->accept();
        return;
    }
    KDecoration3::Decoration::mouseReleaseEvent(event);
}

void Decoration::updateScale()
{
    setScaledCornerRadius();
    recalculateBorders();
    for (auto btn : m_leftButtons->buttons()) {
        static_cast<Button *>(btn)->reconfigure();
    }
    for (auto btn : m_rightButtons->buttons()) {
        static_cast<Button *>(btn)->reconfigure();
    }
    updateButtonsGeometry();
}
} // namespace

#include "decoration.moc"
