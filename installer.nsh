; Lumora NSIS-Hooks (via electron-builder "nsis.include").
;
; Bei der ECHTEN Deinstallation (nicht beim Auto-Update, das intern auch
; deinstalliert) die von Lumora angelegten geplanten Aufgaben entfernen –
; das sind die elevated Broker fuer FPS (PresentMon) und CPU-Sensorik (PawnIO).
; Ohne diesen Hook blieben verwaiste Aufgaben zurueck, die auf eine geloeschte
; Exe zeigen.
;
; PawnIO selbst bleibt bewusst installiert: eigenstaendiges Produkt mit eigenem
; Deinstaller ("Apps & Features"), das auch von anderen Tools (LibreHardwareMonitor,
; FanControl, OpenRGB ...) mitbenutzt sein kann.
!macro customUnInstall
  ${ifNot} ${isUpdated}
    ExecWait 'schtasks /End /TN "LumoraOSD-FPS"'
    ExecWait 'schtasks /Delete /TN "LumoraOSD-FPS" /F'
    ExecWait 'schtasks /End /TN "LumoraOSD-Sensors"'
    ExecWait 'schtasks /Delete /TN "LumoraOSD-Sensors" /F'
  ${endIf}
!macroend
