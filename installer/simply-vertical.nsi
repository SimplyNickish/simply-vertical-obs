; Simply Vertical for OBS: the Windows installer.
;
; Puts the plugin into the streamer's OBS folder (found from the registry, the
; folder OBS's own installer records) and adds an uninstaller to Windows'
; Installed apps list. Build it from this folder after a public build:
;
;   makensis simply-vertical.nsi            the real installer
;   makensis /DTEST simply-vertical.nsi     a test copy: no admin, Installed apps
;                                           entry under the current user only
;
; Output: out\SimplyVertical-OBS-Setup.exe (or ...-TEST.exe).
; The plugin's saved sign-in and settings live in the user's OBS config folder,
; not here, so installing a new version keeps them.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Var ObsRoot

!define NAME "Simply Vertical for OBS"
!define VERSION "0.1.0"
!define PUBLISHER "SimplyNickish"
!define SITE "https://vertical.simplynickish.com"
!define KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SimplyVerticalOBS"
!define BUILD "..\build_release\RelWithDebInfo"
!define DATA "data\obs-plugins\simply-vertical"

!ifdef TEST
	!define ROOT HKCU
	!define OUTFILE "out\SimplyVertical-OBS-Setup-TEST.exe"
	RequestExecutionLevel user
!else
	!define ROOT HKLM
	!define OUTFILE "out\SimplyVertical-OBS-Setup.exe"
	RequestExecutionLevel admin
!endif

Name "${NAME}"
OutFile "${OUTFILE}"
BrandingText "${NAME} ${VERSION}"
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${NAME} installer"
VIAddVersionKey "LegalCopyright" "GPL 2.0 or later"

!define MUI_ABORTWARNING
; Pictures made from the Simply Vertical app icon.
!define MUI_ICON "icon.ico"
!define MUI_UNICON "icon.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "side.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "side.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_WELCOMEPAGE_TITLE "Install ${NAME}"
!define MUI_WELCOMEPAGE_TEXT "This adds the Simply Vertical panel and source to OBS Studio, so your vertical stream runs straight from OBS.$\r$\n$\r$\nClose OBS before you carry on. Your saved sign-in and settings are kept.$\r$\n$\r$\nVersion ${VERSION}."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!define MUI_DIRECTORYPAGE_TEXT_TOP "This is the folder OBS Studio is installed in. It was found for you, so only change it if you put OBS somewhere else."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "OBS Studio folder"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckFolder
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "Simply Vertical is installed"
!define MUI_FINISHPAGE_TEXT "Open OBS. The Simply Vertical panel is under Docks. Press Link my account in it to bring in your layout.$\r$\n$\r$\nIf OBS was open, close it and open it again first."
!define MUI_FINISHPAGE_LINK "Open the Simply Vertical page"
!define MUI_FINISHPAGE_LINK_LOCATION "${SITE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "Simply Vertical needs 64 bit Windows." /SD IDOK
		SetErrorLevel 3
		Abort
	${EndIf}
	; /D=<folder> on the command line wins. Otherwise use the folder OBS's own
	; installer saved, and Program Files if OBS never recorded one.
	${If} $INSTDIR == ""
		SetRegView 64
		ReadRegStr $INSTDIR HKLM "SOFTWARE\OBS Studio" ""
		SetRegView lastused
	${EndIf}
	${If} $INSTDIR == ""
		StrCpy $INSTDIR "$PROGRAMFILES64\obs-studio"
	${EndIf}
FunctionEnd

; The folder has to really be OBS, or the plugin would land somewhere OBS never looks.
Function CheckFolder
	${IfNot} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
		MessageBox MB_ICONEXCLAMATION "OBS Studio is not in that folder.$\r$\n$\r$\nPick the folder OBS is installed in. It has a bin folder inside it, and is usually C:\Program Files\obs-studio." /SD IDOK
		Abort
	${EndIf}
FunctionEnd

; While OBS is open the plugin file is in use and can't be replaced.
Function WaitForObsClosed
	${DoUntil} 0 = 1
		ClearErrors
		${IfNot} ${FileExists} "$INSTDIR\obs-plugins\64bit\simply-vertical.dll"
			Return
		${EndIf}
		FileOpen $0 "$INSTDIR\obs-plugins\64bit\simply-vertical.dll" a
		${IfNot} ${Errors}
			FileClose $0
			Return
		${EndIf}
		MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "OBS is still open, so Simply Vertical can't be updated.$\r$\n$\r$\nClose OBS (File, Exit), then press Retry." /SD IDCANCEL IDRETRY retry
		SetErrorLevel 2
		Quit
		retry:
	${Loop}
FunctionEnd

Section "Simply Vertical"
	SectionIn RO
	${IfNot} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
		DetailPrint "OBS Studio is not in $INSTDIR."
		MessageBox MB_ICONSTOP "OBS Studio is not in $INSTDIR, so nothing was installed. Run the installer again and pick the OBS folder." /SD IDOK
		SetErrorLevel 4
		Quit
	${EndIf}
	Call WaitForObsClosed

	SetOutPath "$INSTDIR\obs-plugins\64bit"
	File "${BUILD}\simply-vertical.dll"
	; A debug file from a test copy would only take up space.
	Delete "$INSTDIR\obs-plugins\64bit\simply-vertical.pdb"

	; The plugin's own files are replaced whole, so nothing from an older version is left over.
	RMDir /r "$INSTDIR\${DATA}"
	SetOutPath "$INSTDIR\${DATA}"
	File /r "..\data\*.*"
	WriteUninstaller "$INSTDIR\${DATA}\uninstall.exe"

	WriteRegStr ${ROOT} "${KEY}" "DisplayName" "${NAME}"
	WriteRegStr ${ROOT} "${KEY}" "DisplayVersion" "${VERSION}"
	WriteRegStr ${ROOT} "${KEY}" "Publisher" "${PUBLISHER}"
	WriteRegStr ${ROOT} "${KEY}" "URLInfoAbout" "${SITE}"
	WriteRegStr ${ROOT} "${KEY}" "InstallLocation" "$INSTDIR"
	WriteRegStr ${ROOT} "${KEY}" "UninstallString" '"$INSTDIR\${DATA}\uninstall.exe"'
	WriteRegStr ${ROOT} "${KEY}" "QuietUninstallString" '"$INSTDIR\${DATA}\uninstall.exe" /S'
	WriteRegStr ${ROOT} "${KEY}" "DisplayIcon" "$INSTDIR\bin\64bit\obs64.exe"
	WriteRegDWORD ${ROOT} "${KEY}" "NoModify" 1
	WriteRegDWORD ${ROOT} "${KEY}" "NoRepair" 1
	WriteRegDWORD ${ROOT} "${KEY}" "EstimatedSize" 6000
SectionEnd

; The uninstaller sits in OBS\data\obs-plugins\simply-vertical, three folders below OBS.
Function un.onInit
	GetFullPathName $ObsRoot "$INSTDIR\..\..\.."
FunctionEnd

Section "Uninstall"
	ClearErrors
	Delete "$ObsRoot\obs-plugins\64bit\simply-vertical.dll"
	${If} ${Errors}
	${AndIf} ${FileExists} "$ObsRoot\obs-plugins\64bit\simply-vertical.dll"
		MessageBox MB_ICONSTOP "OBS is still open, so Simply Vertical can't be removed. Close OBS and run the uninstaller again." /SD IDOK
		SetErrorLevel 2
		Quit
	${EndIf}
	Delete "$ObsRoot\obs-plugins\64bit\simply-vertical.pdb"
	RMDir /r "$ObsRoot\${DATA}"
	DeleteRegKey ${ROOT} "${KEY}"
SectionEnd
