# IPC-Inventar - Checkliste fuer den Electron-Ausstieg (Phase 2)

Stand: 2026-07-19. Quelle: index.html/osd.html/doorman.html (UI) + main.js (Handler).
Jeder Kanal wird beim Portieren abgehakt; bis dahin antwortet die Shell mit null.

## UI -> Shell (invoke, 53)

**Fenster (Win32, trivial):** close-window, minimize-window, toggle-maximize

**Settings/Persistenz:** get-app-settings, set-app-settings, save-games, save-prefs

**Spiele-Bibliothek:** scan-games, launch-game, browse-game, browse-icon, browse-scan-folder, open-game-folder, get-file-icon, list-gpus

**Artwork/Medien (WinHTTP + Cache):** fetch-cover, fetch-hero, fetch-game-info, fetch-image-url, search-sgdb, search-steam, search-msstore, store-media, delete-media

**Streaming (Orchestrierung ist schon nativ!):** start-broadcast, stop-broadcast, broadcast-status, list-sources, list-viewers, kick-viewer, preview-whep, preview-whep-stop, test-connectivity, set-hdr-tonemap

**Gruppe:** group-start, group-join, group-leave, group-status

**Tuersteher:** doorman-decide, doorman-lists, doorman-remove

**OSD:** setup-osd, osd-sources, osd-edit-corner, osd-edit-done, osd-edit-fields, osd-edit-opacity, osd-edit-scale, osd-edit-theme

**Updates (eigener Updater, Phase 4):** check-for-updates, download-update, install-update

**Sonstiges:** set-hotkey, shell-open-external (nur im Shim)

## Shell -> UI (Push, 31)

- broadcast-status
- copy-stream-link
- deep-join
- doorman-list
- external-running
- group-autojoin
- group-status
- hdr-status
- lan-groups
- launch-status
- osd-config
- osd-data
- osd-edit
- osd-fps-off
- osd-setup-status
- play-session
- show-forward-help
- sources-updated
- stream-error
- stream-source-changed
- stream-summary
- stream-toggle-sound
- switch-freeze
- update-available
- update-error
- update-none
- update-progress
- update-ready
- viewer-joined
- window-maximized
- window-unmaximized
