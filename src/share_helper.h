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
QJsonObject buildShareData(const QJsonArray& collections, const QString& scope);

// Decodes base64 PNG data and writes <dir>/<sanitized name>.png atomically.
// Validates the PNG magic — QML's grabToImage must have produced a real image.
// Returns {ok:true,path} or {ok:false,error}.
QJsonObject savePngBase64(const QString& dir, const QString& name, const QString& pngBase64);

// "card-2026-06-10" stays; path separators, dots and anything exotic is stripped.
QString sanitizeName(const QString& name);

} // namespace ShareHelper

#endif // SHARE_HELPER_H
