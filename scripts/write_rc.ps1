$rc = @'
// Microsoft Visual C++ generated resource script.
//
#include "resource.h"

#define APSTUDIO_READONLY_SYMBOLS
/////////////////////////////////////////////////////////////////////////////
//
// Generated from the TEXTINCLUDE 2 resource.
//
#include "winres.h"

/////////////////////////////////////////////////////////////////////////////
#undef APSTUDIO_READONLY_SYMBOLS

/////////////////////////////////////////////////////////////////////////////
// English (United States) resources

#if !defined(AFX_RESOURCE_DLL) || defined(AFX_TARG_ENU)
LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US

#ifdef APSTUDIO_INVOKED
/////////////////////////////////////////////////////////////////////////////
//
// TEXTINCLUDE
//

1 TEXTINCLUDE
BEGIN
    "resource.h\0"
END

2 TEXTINCLUDE
BEGIN
    "#include ""winres.h""\r\n"
    "\0"
END

3 TEXTINCLUDE
BEGIN
    "\r\n"
    "\0"
END

#endif    // APSTUDIO_INVOKED


/////////////////////////////////////////////////////////////////////////////
//
// Icon
//

IDI_APPICON             ICON                    "app.ico"


/////////////////////////////////////////////////////////////////////////////
//
// Version
//

#include "version.h"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION MDD_VERSION_MAJOR,MDD_VERSION_MINOR,MDD_VERSION_PATCH,0
 PRODUCTVERSION MDD_VERSION_MAJOR,MDD_VERSION_MINOR,MDD_VERSION_PATCH,0
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS 0x1L
#else
 FILEFLAGS 0x0L
#endif
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "MuteDiscordDevice"
            VALUE "FileDescription", "Mutes Discord on selected audio output devices"
            VALUE "FileVersion", MDD_VERSION_STRING
            VALUE "InternalName", "MuteDiscordDevice_Config.exe"
            VALUE "LegalCopyright", "Copyright (C) 2026"
            VALUE "OriginalFilename", "MuteDiscordDevice_Config.exe"
            VALUE "ProductName", "MuteDiscordDevice"
            VALUE "ProductVersion", MDD_VERSION_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END

#endif    // English (United States) resources
/////////////////////////////////////////////////////////////////////////////



#ifndef APSTUDIO_INVOKED
/////////////////////////////////////////////////////////////////////////////
//
// Generated from the TEXTINCLUDE 3 resource.
//


/////////////////////////////////////////////////////////////////////////////
#endif    // not APSTUDIO_INVOKED
'@

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$outPath   = Join-Path $repoRoot 'MuteDiscordDevice_Config.rc'

# Write as UTF-16 LE with BOM so rc.exe detects it reliably.
[System.IO.File]::WriteAllText($outPath, $rc, [System.Text.Encoding]::Unicode)
Write-Host "Wrote $outPath"
