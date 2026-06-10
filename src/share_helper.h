#ifndef SHARE_HELPER_H
#define SHARE_HELPER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Share-card data + PNG export (SPEC §12). Pure functions — no I/O besides the explicit
// write — so the whole share path is unit-testable without the plugin.
namespace ShareHelper {

// scope "me": everything the user mirrors (the contribution card).
// scope <collectionId>: that one collection (the collection card).
// Returns {ok:false,error} for an unknown collection id or an empty "me" scope.
// Thumbnails are chain-inscribed (untrusted): a value is only emitted when it is a
// bare CID-shaped token, resolved against `thumbnailBase` (the gateway's storage
// endpoint) — full URLs from manifests are never passed through to the UI.
// fallbackUsedBytes: real stored bytes (storage quotaUsedBytes) — cid_pin manifests
// carry no sizeBytes, so the manifest sum is often 0
QJsonObject buildShareData(const QJsonArray& collections, const QString& scope,
                           const QString& thumbnailBase = QString(),
                           qint64 fallbackUsedBytes = 0);

// CID-shaped (letters/digits/:/-/_/.), no scheme, no slashes — else empty.
QString resolveThumbnail(const QString& thumbnail, const QString& base);

// Validates the PNG at tmpPath (magic + size caps) and moves it to
// <dir>/<sanitized name>.png atomically; removes tmpPath. {ok,path}|{ok:false,error}.
QJsonObject saveFromFile(const QString& dir, const QString& name, const QString& tmpPath);

// Decodes base64 PNG data and writes <dir>/<sanitized name>.png atomically.
// Validates the PNG magic — QML's grabToImage must have produced a real image.
// Returns {ok:true,path} or {ok:false,error}.
QJsonObject savePngBase64(const QString& dir, const QString& name, const QString& pngBase64);

// "card-2026-06-10" stays; path separators, dots and anything exotic is stripped.
QString sanitizeName(const QString& name);

} // namespace ShareHelper

#endif // SHARE_HELPER_H
