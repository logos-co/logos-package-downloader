# Test fixtures

`signed-with-backend.lgx` — a minimal `ui_qml` package whose manifest (0.7.0)
has `"backend": "fixture_app_backend"`, signed with a throwaway key by `lgx`
from logos-package 65f6707e. `ManifestWithBackendVerifies` checks that the
logos-package this downloader links keeps `backend` when it verifies.

Regenerate: `lgx create fixture_app`, `lgx add … --main Main.qml --icon
<256x256 png>`, add `type: ui_qml`, `view`, `backend` to `manifest.json`,
`lgx keygen`, `lgx sign`.
