; Lumora Native (Beta) - NSIS-Installer der C++-Shell (Phase 4, Schritt 1).
; BEWUSST kollisionsfrei zum Parallelbetrieb mit der Electron-App:
; eigener Ordner (lumora-native), eigener Uninstall-Eintrag, KEINE Uebernahme
; von Autostart/lumora://-Protokoll (das macht erst der finale Umstiegs-Installer).
; Aufruf: makensis /DVERSION=x.y.z /DSRCDIR=<Staging> installer.nsi
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef SRCDIR
  !define SRCDIR "stage"
!endif

Name "Lumora Native ${VERSION} (Beta)"
OutFile "Lumora-Native-Setup-${VERSION}.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\lumora-native"
SetCompressor /SOLID lzma

!define MUI_ICON "app.ico"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "German"

Section "Lumora Native"
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

  CreateShortCut "$SMPROGRAMS\Lumora Native (Beta).lnk" "$INSTDIR\lumora-shell.exe"
  CreateShortCut "$DESKTOP\Lumora Native (Beta).lnk" "$INSTDIR\lumora-shell.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayName" "Lumora Native (Beta)"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "Publisher" "Karsten Radermacher"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "DisplayIcon" "$INSTDIR\lumora-shell.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
SectionEnd

Section "Uninstall"
  ; Nutzdaten (%APPDATA%\lumora) bleiben - sie gehoeren der Haupt-App (Parallelbetrieb)
  Delete "$SMPROGRAMS\Lumora Native (Beta).lnk"
  Delete "$DESKTOP\Lumora Native (Beta).lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\lumora-native"
SectionEnd
