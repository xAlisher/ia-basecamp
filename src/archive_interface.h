#ifndef ARCHIVE_INTERFACE_H
#define ARCHIVE_INTERFACE_H

#include <QObject>
#include <QString>
#include "interface.h"

/**
 * @brief archive — follow curated LEZ channels + preserve their collections to Storage.
 *
 * ui_qml module with a C++ backend (runs in ui-host). Pure HTTP/JSON-RPC client: reads the LEZ
 * indexer and a Storage endpoint behind a trusted gateway. No platform-module dependencies, so it
 * avoids the core-module crash (delivery-module#31) and builds cross-platform. See SPEC.md.
 */
class ArchiveInterface : public PluginInterface
{
public:
    virtual ~ArchiveInterface() = default;
};

#define ArchiveInterface_iid "org.logos.ArchiveInterface"
Q_DECLARE_INTERFACE(ArchiveInterface, ArchiveInterface_iid)

#endif // ARCHIVE_INTERFACE_H
