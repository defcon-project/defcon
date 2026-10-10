// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/qrimagewidget.h>

#include <qt/guiutil.h>

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QFontDatabase>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h> /* for USE_QRCODE */
#endif

#ifdef USE_QRCODE
#include <qrencode.h>
#endif

QRImageWidget::QRImageWidget(QWidget *parent):
    QLabel(parent), contextMenu(nullptr)
{
    contextMenu = new QMenu(this);
    contextMenu->addAction(tr("&Save Image…"), this, &QRImageWidget::saveImage);
    contextMenu->addAction(tr("&Copy Image"), this, &QRImageWidget::copyImage);
}

bool QRImageWidget::setQR(const QString& data, const QString& text, int max_chars)
{
#ifdef USE_QRCODE
    setText("");
    if (data.isEmpty()) return false;

    // limit length
    if (data.length() > max_chars) {
        setText(tr("Resulting URI too long, try to reduce the text for label / message."));
        return false;
    }

    QRcode *code = QRcode_encodeString(data.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);

    if (!code) {
        setText(tr("Error encoding URI into QR Code."));
        return false;
    }

    // A QR is data, not a theme surface. Inverting it or tinting its quiet
    // zone makes the exported/clipboard image unreadable to scanners that
    // expect dark modules on white. Draw physical pixels at one INTEGER
    // module size; scaling a small bitmap to 296px gave neighbouring modules
    // different widths, including on fractional-DPI displays.
    constexpr int quiet_modules{4};
    const qreal scale = qApp->devicePixelRatio();
    const int modules = code->width + 2 * quiet_modules;
    const int qr_side = std::max(qRound(QR_IMAGE_SIZE * scale), modules);
    const int module_size = std::max(1, qr_side / modules);
    const int offset = (qr_side - code->width * module_size) / 2;
    QImage qrAddrImage(qr_side, qr_side + qRound(QR_IMAGE_MARGIN * scale), QImage::Format_RGB32);
    qrAddrImage.fill(Qt::white);
    for (int y = 0; y < code->width; ++y) {
        for (int x = 0; x < code->width; ++x) {
            if (!(code->data[y * code->width + x] & 1)) continue;
            for (int dy = 0; dy < module_size; ++dy) {
                for (int dx = 0; dx < module_size; ++dx) {
                    qrAddrImage.setPixel(offset + x * module_size + dx,
                                         offset + y * module_size + dy, qRgb(0, 0, 0));
                }
            }
        }
    }
    QRcode_free(code);
    qrAddrImage.setDevicePixelRatio(scale);
    if (!text.isEmpty()) {
        QPainter painter(&qrAddrImage);
        QFont font = GUIUtil::getFontNormal();
        font.setStretch(QFont::SemiCondensed);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        font.setPointSizeF(GUIUtil::calculateIdealFontSize(qr_side / scale - QR_IMAGE_MARGIN, text, font));
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRectF(0, qr_side / scale, qr_side / scale, QR_IMAGE_MARGIN),
                         Qt::AlignCenter, text);
    }
    setPixmap(QPixmap::fromImage(qrAddrImage));

    return true;
#else
    setText(tr("QR code support not available."));
    return false;
#endif
}

QImage QRImageWidget::exportImage()
{
    return GUIUtil::GetImage(this);
}

void QRImageWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && GUIUtil::HasPixmap(this)) {
        event->accept();
        QMimeData *mimeData = new QMimeData;
        mimeData->setImageData(exportImage());

        QDrag *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        drag->exec();
    } else {
        QLabel::mousePressEvent(event);
    }
}

void QRImageWidget::saveImage()
{
    if (!GUIUtil::HasPixmap(this))
        return;
    QString fn = GUIUtil::getSaveFileName(
        this, tr("Save QR Code"), QString(),
        /*: Expanded name of the PNG file format.
            See: https://en.wikipedia.org/wiki/Portable_Network_Graphics. */
        tr("PNG Image") + QLatin1String(" (*.png)"), nullptr);
    if (!fn.isEmpty())
    {
        exportImage().save(fn);
    }
}

void QRImageWidget::copyImage()
{
    if (!GUIUtil::HasPixmap(this))
        return;
    QApplication::clipboard()->setImage(exportImage());
}

void QRImageWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!GUIUtil::HasPixmap(this))
        return;
    contextMenu->exec(event->globalPos());
}
