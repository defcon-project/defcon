// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Render an SVG with Qt's own renderer -- the only renderer whose opinion
// matters here, since the wallet will use it and not rsvg or a browser.
//
// Qt 5's SVG module implements SVG Tiny 1.2 with additions, and its support for
// `clip-path` has always been partial. The logo as supplied (src/qt/res/src/)
// is built entirely out of clipped full-canvas fills, so if clipping is ignored
// every group paints the whole square and the mark becomes a stack of coloured
// rectangles. The coverage figure printed at the end is what tells the two
// apart: the flattened drawing that ships covers about half the canvas, the
// unflattened source covers all of it.
//
// It is also the renderer build-os-icons.sh expects: the .ico and .icns have to
// hold the pixels Qt would draw, not the pixels some other renderer would.
//
//   ./svgrender <in.svg> <out.png> [size] [hue-shift saturation-reduction]
//
// Build it against a system Qt that carries the Svg module:
//
//   g++ -std=c++17 -fPIC svgrender.cpp -o svgrender $(pkg-config --cflags --libs Qt5Svg Qt5Gui)
//
// Set QT_QPA_PLATFORM=offscreen where there is no display.

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextStream>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        QTextStream(stdout) << "usage: svgrender <in.svg> <out.png> [size]\n";
        return 2;
    }
    const QString in = QString::fromLocal8Bit(argv[1]);
    const QString out = QString::fromLocal8Bit(argv[2]);
    const int size = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 512;

    QSvgRenderer renderer(in);
    QTextStream report(stdout);
    report << "valid            " << (renderer.isValid() ? "yes" : "NO") << "\n";
    if (!renderer.isValid()) return 1;
    report << "default size     " << renderer.defaultSize().width() << "x" << renderer.defaultSize().height() << "\n";

    // Render at the drawing's own aspect ratio. Forcing a square stretches a
    // wide wordmark and makes the coverage figure meaningless.
    const QSize natural = renderer.defaultSize();
    const int height = natural.width() > 0 ? qMax(1, size * natural.height() / natural.width()) : size;
    QImage image(size, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter);
    painter.end();

    // Optional colour rotation, copied from NetworkStyle::rotateColors so a
    // generated test-network icon is the same image the wallet would make for
    // itself, quirks of QColor::setHsl included.
    if (argc > 5) {
        const int hueShift = QString::fromLocal8Bit(argv[4]).toInt();
        const int saturationReduction = QString::fromLocal8Bit(argv[5]).toInt();
        image = image.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                QColor col;
                col.setRgba(line[x]);
                int h, s, l, a;
                col.getHsl(&h, &s, &l, &a);
                h += hueShift;
                s = qMax(s - saturationReduction, 0);
                col.setHsl(h, s, l, a);
                line[x] = col.rgba();
            }
        }
        report << "rotated          hue+" << hueShift << " saturation-" << saturationReduction << "\n";
    }

    // A count of how much of the canvas the artwork actually covers. A mark on
    // a transparent ground covers well under half; a clip-path that was ignored
    // fills nearly all of it, which is the failure this is looking for.
    qint64 opaque = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 8) ++opaque;
        }
    }
    const double coverage = 100.0 * double(opaque) / double(image.width() * image.height());
    report << "canvas covered   " << QString::number(coverage, 'f', 1) << "%\n";
    report << "corner pixel     alpha=" << qAlpha(image.pixel(2, 2)) << " (a mark leaves the corners empty)\n";

    image.save(out);
    report << "wrote            " << out << "\n";
    report.flush();
    return 0;
}
