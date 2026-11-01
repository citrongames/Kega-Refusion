#pragma once
#include <QImage>
#include <QSize>

// Native console raster dimensions travel with the image independently of a
// filter's sample count. QByteArray pixel-only rewind storage restores them.
namespace VideoFrameGeometry {
inline const QString nativeWidthKey = QStringLiteral("KegaRefusion.NativeWidth");
inline const QString nativeHeightKey = QStringLiteral("KegaRefusion.NativeHeight");
inline bool validSize(const QSize& size) {
    return size.width() > 0 && size.height() > 0 && size.width() <= 4096 && size.height() <= 4096;
}
inline QSize nativeSize(const QImage& image) {
    bool widthOk = false, heightOk = false;
    const int width = image.text(nativeWidthKey).toInt(&widthOk);
    const int height = image.text(nativeHeightKey).toInt(&heightOk);
    const QSize size(width,height);
    return widthOk && heightOk && validSize(size) ? size : image.size();
}
inline void setNativeSize(QImage& image, const QSize& size) {
    if (!validSize(size) || image.isNull()) return;
    image.setText(nativeWidthKey,QString::number(size.width()));
    image.setText(nativeHeightKey,QString::number(size.height()));
}
}
