# PROJECT_KNOWLEDGE — ia-basecamp

See SPEC.md for the design. Key facts:

- **Shape:** `ui_qml` module with a C++ backend (radio `ui-qml-backend` / logos-delivery-demo). Pure
  `QtNetwork` HTTP/JSON-RPC client — `dependencies: []`, `nix.runtime: []`. This is what makes it
  cross-platform and dodges delivery#31.
- **Contract:** `src/archive.rep` (QRO). PROP = synced state, SLOT = QML-callable (returns JSON).
- **Read path:** LEZ indexer JSON-RPC `getTransactionsByAccount` → decode `Op::ChannelInscribe`.
  Finalized data only. Gated on LEZ#519 (indexer sync).
