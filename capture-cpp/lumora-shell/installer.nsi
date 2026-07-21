; Lumora Native - NSIS-Installer der C++-Shell.
; Zwei Modi (Abfrage beim Start):
;   UMSTIEG  -> installiert ins GLEICHE Verzeichnis wie die Electron-App
;               (%LOCALAPPDATA%\Programs\lumora), loest die Electron-Version ueber
;               deren Uninstaller sauber ab, Verknuepfung heisst "Lumora".
;   PARALLEL -> eigenes Verzeichnis (lumora-native), Beta-Verknuepfung, laesst die
;               Electron-App unangetastet (zum Testen nebenher).
; Autostart-Key + lumora://-Protokoll registriert die Shell selbst beim ersten Start
; (applyAutostart/registerProtocol) - kein Admin noetig.
; Aufruf: makensis /DVERSION=x.y.z /DSRCDIR=<Staging> installer.nsi
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "stage"
!endif

Name "Lumora Native ${VERSION}"
OutFile "Lumora-Native-Setup-${VERSION}.exe"
RequestExecutionLevel user
SetCompressor /SOLID lzma

Var Migrate          ; "1" = Electron ersetzen, "0" = parallel

!define MUI_ICON "app.ico"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "German"

Function .onInit
  MessageBox MB_YESNO|MB_ICONQUESTION \
    "Vorhandene Lumora-Version (Electron) durch die native App ERSETZEN?$\n$\n\
Ja = Umstieg: gleiches Verzeichnis, die Electron-Version wird sauber deinstalliert.$\n\
Nein = parallel zum Testen (eigener Ordner, Electron bleibt unberuehrt)." \
    IDYES lbl_migrate
  ; --- PARALLEL ---
  StrCpy $Migrate "0"
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora-native"
  Goto lbl_done
lbl_migrate:
  StrCpy $Migrate "1"
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora"
lbl_done:
FunctionEnd

Section "Lumora Native"
  ; --- Beim Umstieg zuerst die Electron-App sauber abloesen ---
  ${If} $Migrate == "1"
    DetailPrint "Beende laufende Lumora-Prozesse..."
    nsExec::Exec 'taskkill /F /IM Lumora.exe'
    nsExec::Exec 'taskkill /F /IM lumora-shell.exe'
    Sleep 700
    ${If} ${FileExists} "$INSTDIR\Uninstall Lumora.exe"
      DetailPrint "Deinstalliere die vorhandene Electron-Version..."
      ; _?= laesst den electron-builder-Uninstaller SYNCHRON laufen (kein Selbst-Kopieren);
      ; er entfernt Electron-Dateien + Registry (inkl. GUID-Uninstall-Key + Autostart).
      ExecWait '"$INSTDIR\Uninstall Lumora.exe" /S _?=$INSTDIR'
      Sleep 500
      Delete "$INSTDIR\Uninstall Lumora.exe"
      ; Reste der Electron-Struktur wegraeumen, damit kein Mischbestand bleibt
      RMDir /r "$INSTDIR\resources"
      RMDir /r "$INSTDIR\locales"
      Delete "$INSTDIR\Lumora.exe"
      Delete "$INSTDIR\*.pak"
    ${EndIf}
    ; verwaisten Electron-Autostart-Key entfernen (die Shell setzt spaeter ihren eigenen)
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "com.lumora.app"
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"

  ; WebView2-Runtime pruefen (Evergreen, per-machine ODER per-user); fehlt sie,
  ; installiert der mitgelieferte Microsoft-Bootstrapper sie nach (Win10-Absicherung).
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $0 "" 0 wv2ok
  ReadRegStr $0 HKCU "SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  StrCmp $0 "" 0 wv2ok
  DetailPrint "WebView2-Runtime wird installiert..."
  ExecWait '"$INSTDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install'
wv2ok:

  ; Verknuepfungen + Name je nach Modus
  ${If} $Migrate == "1"
    CreateShortCut "$SMPROGRAMS\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    CreateShortCut "$DESKTOP\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayName" "Lumora"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "Publisher" "Karsten Radermacher"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayIcon" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  ${Else}
    CreateShortCut "$SMPROGRAMS\Lumora Native (Beta).lnk" "$INSTDIR\lumora-shell.exe"
    CreateShortCut "$DESKTOP\Lumora Native (Beta).lnk" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayName" "Lumora Native (Beta)"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "Publisher" "Karsten Radermacher"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayIcon" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Uninstall"
  ; Nutzdaten (%APPDATA%\lumora) bleiben - Einstellungen/Bibliothek
  Delete "$SMPROGRAMS\Lumora.lnk"
  Delete "$DESKTOP\Lumora.lnk"
  Delete "$SMPROGRAMS\Lumora Native (Beta).lnk"
  Delete "$DESKTOP\Lumora Native (Beta).lnk"
  ; eigenen Autostart-Key + lumora://-Protokoll wieder entfernen
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Lumora"
  DeleteRegKey HKCU "Software\Classes\lumora"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native"
SectionEnd
