; Lumora Native - NSIS-Installer der C++-Shell.
; Modi (automatisch erkannt, siehe .onInit):
;   UPDATE   -> native Version bereits installiert: gleiches Verzeichnis, KEINE Fragen,
;               Verzeichnis-Seite wird uebersprungen (Basis fuer Silent-Auto-Update /S).
;   UMSTIEG  -> Electron-App vorhanden: Abfrage, ob ersetzen (gleiches Verzeichnis,
;               Electron wird ueber ihren Uninstaller sauber abgeloest).
;   FRISCH   -> weder nativ noch Electron: direkt nach lumora-native, keine Frage.
; Deinstallation: fragt (ausser bei /S), ob Einstellungen (%APPDATA%\lumora) mit weg sollen.
; Autostart-Key + lumora://-Protokoll registriert die Shell selbst beim ersten Start.
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

Name "Lumora ${VERSION}"
OutFile "Lumora-Native-Setup-${VERSION}.exe"
RequestExecutionLevel user
SetCompressor /SOLID lzma

Var Migrate          ; "1" = Electron-Namensraum (Verknuepfung "Lumora"), "0" = lumora-native
Var IsUpdate         ; "1" = native Version war schon installiert (Update-Lauf)

; --- Optik: Logo-Bitmaps (aus icon-source.png generiert), Welcome/Finish-Seiten ---
!define MUI_ICON "app.ico"
!define MUI_UNICON "app.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "installer-welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "installer-welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "installer-header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "$(S_WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(S_WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_PRE dirPre
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\lumora-shell.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(S_RunLumora)"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_LANGDLL

LangString S_WelcomeTitle ${LANG_GERMAN} "Willkommen bei Lumora"
LangString S_WelcomeTitle ${LANG_ENGLISH} "Welcome to Lumora"
LangString S_WelcomeText ${LANG_GERMAN} "Dein Spiele-Launcher mit Streaming, Gaming-OSD und HDR-Automatik.$\r$\n$\r$\nLumora ist Open Source (AGPL v3) und besteht vollstaendig aus eigenem, signiertem Code.$\r$\n$\r$\nKlicke auf Weiter, um fortzufahren."
LangString S_WelcomeText ${LANG_ENGLISH} "Your game launcher with streaming, gaming OSD and automatic HDR.$\r$\n$\r$\nLumora is open source (AGPL v3) and consists entirely of our own signed code.$\r$\n$\r$\nClick Next to continue."
LangString S_RunLumora ${LANG_GERMAN} "Lumora jetzt starten"
LangString S_RunLumora ${LANG_ENGLISH} "Launch Lumora now"
LangString S_MigrateAsk ${LANG_GERMAN} "Vorhandene Lumora-Version (Electron) durch die native App ERSETZEN?$\n$\nJa = Umstieg: gleiches Verzeichnis, die Electron-Version wird sauber deinstalliert.$\nNein = parallel zum Testen (eigener Ordner, Electron bleibt unberuehrt)."
LangString S_MigrateAsk ${LANG_ENGLISH} "REPLACE the existing (Electron) Lumora version with the native app?$\n$\nYes = migrate: same folder, the Electron version is uninstalled cleanly.$\nNo = install side by side for testing (own folder, Electron untouched)."
LangString S_RemoveData ${LANG_GERMAN} "Sollen auch deine Einstellungen, Spielzeiten und die Spiele-Bibliothek entfernt werden?$\n$\nJa = alles restlos entfernen ($APPDATA\lumora)$\nNein = Einstellungen behalten (empfohlen, falls du Lumora spaeter erneut installierst)"
LangString S_RemoveData ${LANG_ENGLISH} "Also remove your settings, playtimes and game library?$\n$\nYes = remove everything ($APPDATA\lumora)$\nNo = keep settings (recommended if you might reinstall Lumora later)"

; Verzeichnis-Seite bei Updates ueberspringen (Ziel steht fest)
Function dirPre
  ${If} $IsUpdate == "1"
    Abort
  ${EndIf}
FunctionEnd

Function .onInit
  ; --- 1) UPDATE-Erkennung: native Version schon da? (lumora-shell.exe = Marker) ---
  StrCpy $IsUpdate "0"
  ${If} ${FileExists} "$LOCALAPPDATA\Programs\lumora-native\lumora-shell.exe"
    StrCpy $IsUpdate "1"
    StrCpy $Migrate "0"
    StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora-native"
    Goto lbl_done
  ${EndIf}
  ${If} ${FileExists} "$LOCALAPPDATA\Programs\lumora\lumora-shell.exe"
    StrCpy $IsUpdate "1"
    StrCpy $Migrate "1"
    StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora"
    Goto lbl_done
  ${EndIf}

  ; --- 2) Frische Installation: Sprachauswahl (entfaellt bei /S automatisch) ---
  !insertmacro MUI_LANGDLL_DISPLAY

  ; --- 3) Electron-Bestand? Nur DANN die Umstiegs-Frage - und nie im Silent-Modus ---
  ${If} ${FileExists} "$LOCALAPPDATA\Programs\lumora\Uninstall Lumora.exe"
  ${AndIfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION "$(S_MigrateAsk)" IDYES lbl_migrate
  ${EndIf}
  ; Standard: eigener Ordner (auch Silent-Frischinstallation landet hier)
  StrCpy $Migrate "0"
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora-native"
  Goto lbl_done
lbl_migrate:
  StrCpy $Migrate "1"
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\lumora"
lbl_done:
FunctionEnd

Section "Lumora"
  ; Laufende native App IMMER beenden (sonst sperrt lumora-shell.exe das Kopieren)
  DetailPrint "Beende laufende Lumora-Prozesse..."
  nsExec::Exec 'taskkill /F /IM lumora-shell.exe'
  nsExec::Exec 'taskkill /F /IM lumora-capture-native.exe'
  nsExec::Exec 'taskkill /F /IM lumora-media-relay.exe'

  ; --- Beim Electron-Umstieg (kein Update-Lauf) zuerst die Electron-App abloesen ---
  ${If} $Migrate == "1"
  ${AndIf} $IsUpdate == "0"
    nsExec::Exec 'taskkill /F /IM Lumora.exe'
    Sleep 700
    ${If} ${FileExists} "$INSTDIR\Uninstall Lumora.exe"
      DetailPrint "Deinstalliere die vorhandene Electron-Version..."
      ; _?= laesst den electron-builder-Uninstaller SYNCHRON laufen (kein Selbst-Kopieren);
      ; er entfernt Electron-Dateien + Registry (inkl. GUID-Uninstall-Key + Autostart).
      ExecWait '"$INSTDIR\Uninstall Lumora.exe" /S _?=$INSTDIR'
      Sleep 500
      Delete "$INSTDIR\Uninstall Lumora.exe"
      RMDir /r "$INSTDIR\resources"
      RMDir /r "$INSTDIR\locales"
      Delete "$INSTDIR\Lumora.exe"
      Delete "$INSTDIR\*.pak"
    ${EndIf}
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "com.lumora.app"
  ${Else}
    Sleep 400
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

  ; Verknuepfungen + Uninstall-Eintrag je nach Namensraum
  ${If} $Migrate == "1"
    CreateShortCut "$SMPROGRAMS\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    CreateShortCut "$DESKTOP\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayName" "Lumora"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "Publisher" "Karsten Radermacher"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "DisplayIcon" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora" "InstallLocation" "$INSTDIR"
  ${Else}
    CreateShortCut "$SMPROGRAMS\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    CreateShortCut "$DESKTOP\Lumora.lnk" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayName" "Lumora"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "Publisher" "Karsten Radermacher"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayIcon" "$INSTDIR\lumora-shell.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "InstallLocation" "$INSTDIR"
    ; Alt-Verknuepfungen aus der Beta-Phase aufraeumen
    Delete "$SMPROGRAMS\Lumora Native (Beta).lnk"
    Delete "$DESKTOP\Lumora Native (Beta).lnk"
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Update-Lauf im Silent-Modus (Auto-Update): App direkt wieder starten
  ${If} ${Silent}
  ${AndIf} $IsUpdate == "1"
    Exec '"$INSTDIR\lumora-shell.exe"'
  ${EndIf}
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM lumora-shell.exe'
  nsExec::Exec 'taskkill /F /IM lumora-capture-native.exe'
  nsExec::Exec 'taskkill /F /IM lumora-media-relay.exe'
  Sleep 400
  Delete "$SMPROGRAMS\Lumora.lnk"
  Delete "$DESKTOP\Lumora.lnk"
  Delete "$SMPROGRAMS\Lumora Native (Beta).lnk"
  Delete "$DESKTOP\Lumora Native (Beta).lnk"
  ; eigenen Autostart-Key + lumora://-Protokoll wieder entfernen
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Lumora"
  DeleteRegKey HKCU "Software\Classes\lumora"
  ; von Lumora angelegte geplante Aufgaben (OSD-Broker) entfernen
  nsExec::Exec 'schtasks /Delete /F /TN LumoraOSD-FPS'
  nsExec::Exec 'schtasks /Delete /F /TN LumoraOSD-Sensors'
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native"

  ; Einstellungen: fragen (ausser Silent - dort IMMER behalten)
  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(S_RemoveData)" IDNO keepData
    RMDir /r "$APPDATA\lumora"
keepData:
  ${EndIf}
SectionEnd
