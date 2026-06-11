#ifndef IA_FILES_H
#define IA_FILES_H

#include <QByteArray>
#include <QList>
#include <QString>

// Campaign preserve (#14, docs/campaign-brief.md): an ia_item entry names an IA
// item; the bytes come from archive.org and are verified against IA's own
// per-file manifest — https://archive.org/download/{id}/{id}_files.xml — which
// carries md5/sha1 for every file. Pure functions so the parser and the verify
// policy are deterministic-testable without network.

struct IaFileEntry {
    QString name;
    QString md5;    // hex, may be empty (the files.xml cannot checksum itself)
    QString sha1;   // hex, may be empty
    qint64  size = 0;
};

// Parses {id}_files.xml. Returns every <file> entry with a name; entries
// without checksums are included (verification policy decides, not the parser).
QList<IaFileEntry> parseIaFilesXml(const QByteArray& xml);

enum class IaVerify {
    Verified,     // a published checksum matched
    Unverified,   // IA published no checksum for this file — uploaded, but counted
    Mismatch,     // checksum differs — the file must be discarded
};

// Streams the file through md5 (preferred) or sha1. Never reads it into memory.
IaVerify verifyIaFile(const QString& path, const IaFileEntry& entry);

#endif // IA_FILES_H
