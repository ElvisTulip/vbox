/* $Id$ */
/** @file
 * VBoxStub - VirtualBox's Windows installer stub.
 */

/*
 * Copyright (C) 2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <windows.h>
#include <msiquery.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strsafe.h>

#include <VBox/version.h>

#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/file.h>
#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/thread.h>

#include "VBoxStub.h"
#include "../StubBld/VBoxStubBld.h"
#include "resource.h"

#ifndef  _UNICODE
#define  _UNICODE
#endif


/**
 * Shows a message box with a printf() style formatted string.
 *
 * @returns Message box result (IDOK, IDCANCEL, ...).
 *
 * @param   uType               Type of the message box (see MSDN).
 * @param   pszFmt              Printf-style format string to show in the message box body.
 *
 */
static int ShowInfo(const char *pszFmt, ...)
{
    char       *pszMsg;
    va_list     va;

    va_start(va, pszFmt);
    RTStrAPrintfV(&pszMsg, pszFmt, va);
    va_end(va);

    int rc;
    if (pszMsg)
        rc = MessageBox(GetDesktopWindow(), pszMsg, VBOX_STUB_TITLE, MB_ICONINFORMATION);
    else
        rc = MessageBox(GetDesktopWindow(), pszFmt, VBOX_STUB_TITLE, MB_ICONINFORMATION);
    RTStrFree(pszMsg);
    return rc;
}


/**
 * Shows an error message box with a printf() style formatted string.
 *
 * @returns Message box result (IDOK, IDCANCEL, ...).
 *
 * @param   pszFmt              Printf-style format string to show in the message box body.
 *
 */
static int ShowError(const char *pszFmt, ...)
{
    char       *pszMsg;
    va_list     va;

    va_start(va, pszFmt);
    RTStrAPrintfV(&pszMsg, pszFmt, va);
    va_end(va);

    int rc;
    if (pszMsg)
        rc = MessageBox(GetDesktopWindow(), pszMsg, VBOX_STUB_TITLE, MB_ICONERROR);
    else
        rc = MessageBox(GetDesktopWindow(), pszFmt, VBOX_STUB_TITLE, MB_ICONERROR);
    RTStrFree(pszMsg);
    return rc;
}


/**
 * Reads data from a built-in resource.
 *
 * @returns iprt status code.
 *
 * @param   hInst               Instance to read the data from.
 * @param   pszDataName         Name of resource to read.
 * @param   ppvResource         Pointer to buffer which holds the read resource data.
 * @param   pdwSize             Pointer which holds the read data size.
 *
 */
static int ReadData(HINSTANCE   hInst,
                    const char *pszDataName,
                    PVOID      *ppvResource,
                    DWORD      *pdwSize)
{
    do
    {
        AssertBreakStmt(pszDataName, "Resource name is empty!");

        /* Find our resource. */
        HRSRC hRsrc = FindResourceEx(hInst, RT_RCDATA, pszDataName, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
        AssertBreakStmt(hRsrc, "Could not find resource!");

        /* Get resource size. */
        *pdwSize = SizeofResource(hInst, hRsrc);
        AssertBreakStmt(*pdwSize > 0, "Size of resource is invalid!");

        /* Get pointer to resource. */
        HGLOBAL hData = LoadResource(hInst, hRsrc);
        AssertBreakStmt(hData, "Could not load resource!");

        /* Lock resource. */
        *ppvResource = LockResource(hData);
        AssertBreakStmt(*ppvResource, "Could not lock resource!");
    } while (0);
    return *ppvResource ? VINF_SUCCESS : VERR_IO_GEN_FAILURE;
}


/**
 * Constructs a full temporary file path from the given parameters.
 *
 * @returns iprt status code.
 *
 * @param   pszTempPath         The pure path to use for construction.
 * @param   pszTargetFileName   The pure file name to use for construction.
 * @param   ppszTempFile        Pointer to the constructed string.  Must be freed
 *                              using RTStrFree().
 */
static int GetTempFileAlloc(const char  *pszTempPath,
                            const char  *pszTargetFileName,
                            char       **ppszTempFile)
{
    return RTStrAPrintf(ppszTempFile, "%s\\%s", pszTempPath, pszTargetFileName);
}


/**
 * Extracts a built-in resource to disk.
 *
 * @returns iprt status code.
 *
 * @param   pszResourceName     The resource name to extract.
 * @param   pszTempFile         The full file path + name to extract the resource to.
 *
 */
static int ExtractFile(const char *pszResourceName,
                       const char *pszTempFile)
{
    int rc;
    RTFILE fh;
    BOOL bCreatedFile = FALSE;

    do
    {
        AssertBreakStmt(pszResourceName, "Resource pointer invalid!");
        AssertBreakStmt(pszTempFile, "Temp file pointer invalid!");

        /* Read the data of the built-in resource. */
        PVOID pvData = NULL;
        DWORD dwDataSize = 0;
        rc = ReadData(NULL, pszResourceName, &pvData, &dwDataSize);
        AssertRCBreakStmt(rc, "Could not read resource data!");

        /* Create new (and replace an old) file. */
        rc = RTFileOpen(&fh, pszTempFile,
                          RTFILE_O_CREATE_REPLACE
                        | RTFILE_O_WRITE
                        | RTFILE_O_DENY_NOT_DELETE
                        | RTFILE_O_DENY_WRITE);
        AssertRCBreakStmt(rc, "Could not open file for writing!");
        bCreatedFile = TRUE;

        /* Write contents to new file. */
        size_t cbWritten = 0;
        rc = RTFileWrite(fh, pvData, dwDataSize, &cbWritten);
        AssertRCBreakStmt(rc, "Could not open file for writing!");
        AssertBreakStmt((dwDataSize == cbWritten), "File was not extracted completely! Disk full?");

    } while (0);

    if (RTFileIsValid(fh))
        RTFileClose(fh);

    if (RT_FAILURE(rc))
    {
        if (bCreatedFile)
            RTFileDelete(pszTempFile);
    }
    return rc;
}


/**
 * Extracts a built-in resource to disk.
 *
 * @returns iprt status code.
 *
 * @param   pPackage            Pointer to a VBOXSTUBPKG struct that contains the resource.
 * @param   pszTempFile         The full file path + name to extract the resource to.
 *
 */
static int Extract(const PVBOXSTUBPKG  pPackage,
                   const char         *pszTempFile)
{
    return ExtractFile(pPackage->szResourceName,
                       pszTempFile);
}


/**
 * Detects whether we're running on a 32- or 64-bit platform and returns the result.
 *
 * @returns TRUE if we're running on a 64-bit OS, FALSE if not.
 *
 */
static BOOL IsWow64(void)
{
    BOOL bIsWow64 = TRUE;
    fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(GetModuleHandle(TEXT("kernel32")), "IsWow64Process");
    if (NULL != fnIsWow64Process)
    {
        if (!fnIsWow64Process(GetCurrentProcess(), &bIsWow64))
        {
            /* Error in retrieving process type - assume that we're running on 32bit. */
            return FALSE;
        }
    }
    return bIsWow64;
}


/**
 * Decides whether we need a specified package to handle or not.
 *
 * @returns TRUE if we need to handle the specified package, FALSE if not.
 *
 * @param   pPackage            Pointer to a VBOXSTUBPKG struct that contains the resource.
 *
 */
static BOOL PackageIsNeeded(PVBOXSTUBPKG pPackage)
{
    BOOL bIsWow64 = IsWow64();
    if ((bIsWow64 && pPackage->byArch == VBOXSTUBPKGARCH_AMD64)) /* 64bit Windows. */
    {
        return TRUE;
    }
    else if ((!bIsWow64 && pPackage->byArch == VBOXSTUBPKGARCH_X86)) /* 32bit. */
    {
        return TRUE;
    }
    else if (pPackage->byArch == VBOXSTUBPKGARCH_ALL)
    {
        return TRUE;
    }
    return FALSE;
}


/**
 * Recursivly copies a directory to another location.
 *
 * @returns iprt status code.
 *
 * @param   pszDestDir          Location to copy the source directory to.
 * @param   pszSourceDir        The source directory to copy.
 *
 */
int CopyDir(const char *pszDestDir, const char *pszSourceDir)
{
    char szDest[_MAX_PATH + 1];
    char szSource[_MAX_PATH + 1];

    AssertStmt(pszDestDir, "Destination directory invalid!");
    AssertStmt(pszSourceDir, "Source directory invalid!");

    SHFILEOPSTRUCT s = {0};
    if (   RTStrPrintf(szDest, _MAX_PATH, "%s%c", pszDestDir, '\0') > 0
        && RTStrPrintf(szSource, _MAX_PATH, "%s%c", pszSourceDir, '\0') > 0)
    {
        s.hwnd = NULL;
        s.wFunc = FO_COPY;
        s.pTo = szDest;
        s.pFrom = szSource;
        s.fFlags = FOF_SILENT |
                   FOF_NOCONFIRMATION |
                   FOF_NOCONFIRMMKDIR |
                   FOF_NOERRORUI;
    }
    return RTErrConvertFromWin32(SHFileOperation(&s));
}


int WINAPI WinMain(HINSTANCE  hInstance,
                   HINSTANCE  hPrevInstance,
                   char      *lpCmdLine,
                   int        nCmdShow)
{
    char **pArgV = __argv;
    int iArgC = __argc;

    /* Check if we're already running and jump out if so. */
    /* Do not use a global namespace ("Global\\") for mutex name here, will blow up NT4 compatibility! */
    HANDLE hMutexAppRunning = CreateMutex (NULL, FALSE, "VBoxStubInstaller");
    if (   (hMutexAppRunning != NULL)
        && (GetLastError() == ERROR_ALREADY_EXISTS))
    {
        /* Close the mutex for this application instance. */
        CloseHandle(hMutexAppRunning);
        hMutexAppRunning = NULL;
        return 1;
    }

    /* Init IPRT. */
    int vrc = RTR3Init();
    if (RT_FAILURE(vrc))
        return vrc;

    BOOL fExtractOnly = FALSE;
    BOOL fSilent = FALSE;
    BOOL fEnableLogging = FALSE;
    BOOL bExit = FALSE;
    char *pszTempPathFull = NULL; /* Contains the final extraction directory later. */

    /* Temp variables for arguments. */
    char szExtractPath[1024] = {0};
    char szMSIArgs[1024] = {0};

    /* Process arguments. */
    for (int i = 0; i < iArgC; i++)
    {
        if (   (0 == RTStrICmp(pArgV[i], "-x"))
            || (0 == RTStrICmp(pArgV[i], "-extract"))
            || (0 == RTStrICmp(pArgV[i], "/extract")))
        {
            fExtractOnly = TRUE;
        }

        else if (   (0 == RTStrICmp(pArgV[i], "-s"))
                 || (0 == RTStrICmp(pArgV[i], "-silent"))
                 || (0 == RTStrICmp(pArgV[i], "/silent")))
        {
            fSilent = TRUE;
        }

        else if (   (0 == RTStrICmp(pArgV[i], "-l"))
                 || (0 == RTStrICmp(pArgV[i], "-logging"))
                 || (0 == RTStrICmp(pArgV[i], "/logging")))
        {
            fEnableLogging = TRUE;
        }

        else if ((  (0 == RTStrICmp(pArgV[i], "-p"))
                 || (0 == RTStrICmp(pArgV[i], "-path"))
                 || (0 == RTStrICmp(pArgV[i], "/path")))
                 && (iArgC > i))
        {
            vrc = ::StringCbCat(szExtractPath, sizeof(szExtractPath), pArgV[i+1]);
            pszTempPathFull = szExtractPath; /* Point to the new path. */
            i++; /* Avoid the specify path from being parsed */
        }

        else if ((  (0 == RTStrICmp(pArgV[i], "-msiparams"))
                 || (0 == RTStrICmp(pArgV[i], "/msiparams")))
                 && (iArgC > i))
        {
            for (int a=i+1; a<iArgC; a++)
            {
                if (a > i+1) /* Insert a space. */
                    vrc = ::StringCbCat(szMSIArgs, sizeof(szMSIArgs), " ");

                vrc = ::StringCbCat(szMSIArgs, sizeof(szMSIArgs), pArgV[a]);
            }
        }

        else if (   (0 == RTStrICmp(pArgV[i], "-v"))
                 || (0 == RTStrICmp(pArgV[i], "-version"))
                 || (0 == RTStrICmp(pArgV[i], "/version")))
        {
            ShowInfo("Version: %d.%d.%d.%d",
                     VBOX_VERSION_MAJOR, VBOX_VERSION_MINOR, VBOX_VERSION_BUILD, VBOX_SVN_REV);
            bExit = TRUE;
        }

        else if (   (0 == RTStrICmp(pArgV[i], "-help"))
                 || (0 == RTStrICmp(pArgV[i], "/help"))
                 || (0 == RTStrICmp(pArgV[i], "/?")))
        {
            ShowInfo("-- %s v%d.%d.%d.%d --\n"
                         "Command Line Parameters:\n\n"
                         "-extract | -x           - Extract file contents to temporary directory\n"
                         "-silent | -s            - Enables silent mode installation\n"
                         "-path | -p              - Sets the path of the extraction directory\n"
                         "-help | /?              - Print this help and exit\n"
                         "-msiparams <parameters> - Specifies extra parameters for the MSI installers\n"
                         "-logging | -l           - Enables installer logging\n"
                         "-version | -v           - Print version number and exit\n\n"
                         "Examples:\n"
                         "%s -msiparams INSTALLDIR=C:\\VBox\n"
                         "%s -extract -path C:\\VBox\n",
                         VBOX_STUB_TITLE, VBOX_VERSION_MAJOR, VBOX_VERSION_MINOR, VBOX_VERSION_BUILD, VBOX_SVN_REV,
                         pArgV[0], pArgV[0]);
            bExit = TRUE;
        }
        else
        {
            if (i > 0)
            {
                ShowInfo("Unknown option \"%s\"!\n"
                         "Please refer to the command line help by specifying \"/?\"\n"
                         "to get more information.", pArgV[i]);
                bExit = TRUE;
            }
        }
    }

    if (bExit)
        return 0;

    HRESULT hr = S_OK;

    do
    {
        /* Get/create our temp path (only if not already set). */
        if (pszTempPathFull == NULL)
        {
            char szTemp[_MAX_PATH] = {0};
            vrc = RTPathTemp(szTemp, sizeof(szTemp));
            AssertRCBreakStmt(vrc, "Could not retrieve temp directory!");
            vrc = RTStrAPrintf(&pszTempPathFull, "%s\\VirtualBox", szTemp);
            AssertRCBreakStmt(vrc, "Could not construct temp directory!");
        }

        if (!RTDirExists(pszTempPathFull))
        {
            vrc = RTDirCreate(pszTempPathFull, 0700);
            AssertRCBreakStmt(vrc, "Could not create temp directory!");
        }

        /* Get our executable path */
        char szPathExe[_MAX_PATH];
        vrc = RTPathExecDir(szPathExe, sizeof(szPathExe));
        /** @todo error checking */

        /* Read our manifest. */
        PVBOXSTUBPKGHEADER pHeader = NULL;
        DWORD cbHeader = 0;
        vrc = ReadData(NULL, "MANIFEST", (LPVOID*)&pHeader, &cbHeader);
        AssertRCBreakStmt(vrc, "Manifest not found!");

        /* Extract files. */
        for (BYTE k = 0; k < pHeader->byCntPkgs; k++)
        {
            PVBOXSTUBPKG pPackage = NULL;
            DWORD cbPackage = 0;
            char szHeaderName[_MAX_PATH] = {0};

            hr = ::StringCchPrintf(szHeaderName, _MAX_PATH, "HDR_%02d", k);
            vrc = ReadData(NULL, szHeaderName, (LPVOID*)&pPackage, &cbPackage);
            AssertRCBreakStmt(vrc, "Header not found!"); /** @todo include header name, how? */

            if (PackageIsNeeded(pPackage) || fExtractOnly)
            {
                char *pszTempFile = NULL;
                vrc = GetTempFileAlloc(pszTempPathFull, pPackage->szFileName, &pszTempFile);
                AssertRCBreakStmt(vrc, "Could not create name for temporary extracted file!");
                vrc = Extract(pPackage, pszTempFile);
                AssertRCBreakStmt(vrc, "Could not extract file!");
                RTStrFree(pszTempFile);
            }
        }

        if (FALSE == fExtractOnly && !RT_FAILURE(vrc))
        {
            /*
             * Copy ".custom" directory into temp directory so that the extracted .MSI
             * file(s) can use it.
             */
            char *pszPathCustomDir;
            vrc = RTStrAPrintf(&pszPathCustomDir, "%s\\.custom", szPathExe);
            if (RT_SUCCESS(vrc) && RTDirExists(pszPathCustomDir))
            {
                vrc = CopyDir(pszTempPathFull, pszPathCustomDir);
                if (RT_FAILURE(vrc)) /* Don't fail if it's missing! */
                    vrc = VINF_SUCCESS;

                RTStrFree(pszPathCustomDir);
            }

            /* Do actions on files. */
            for (BYTE k = 0; k < pHeader->byCntPkgs; k++)
            {
                PVBOXSTUBPKG pPackage = NULL;
                DWORD cbPackage = 0;
                char szHeaderName[_MAX_PATH] = {0};

                hr = StringCchPrintf(szHeaderName, _MAX_PATH, "HDR_%02d", k);
                vrc = ReadData(NULL, szHeaderName, (LPVOID*)&pPackage, &cbPackage);
                AssertRCBreakStmt(vrc, "Package not found!");

                if (PackageIsNeeded(pPackage))
                {
                    char *pszTempFile = NULL;

                    vrc = GetTempFileAlloc(pszTempPathFull, pPackage->szFileName, &pszTempFile);
                    AssertRCBreakStmt(vrc, "Could not create name for temporary action file!");

                    /* Handle MSI files. */
                    if (RTStrICmp(RTPathExt(pszTempFile), ".msi") == 0)
                    {
                        /* Set UI level. */
                        INSTALLUILEVEL UILevel = MsiSetInternalUI(  fSilent
                                                                  ? INSTALLUILEVEL_NONE
                                                                  : INSTALLUILEVEL_FULL,
                                                                    NULL);
                        AssertBreakStmt((UILevel != INSTALLUILEVEL_NOCHANGE), "Could not set installer UI level!");

                        /* Enable logging? */
                        if (fEnableLogging)
                        {
                            char *pszLog = NULL;
                            vrc = RTStrAPrintf(&pszLog, "%s\\VBoxInstallLog.txt", pszTempPathFull);
                            char *pszMSILog = NULL;
                            AssertRCBreakStmt(vrc, "Could not convert MSI log string to current codepage!");
                            UINT uLogLevel = MsiEnableLog(INSTALLLOGMODE_VERBOSE,
                                                          pszLog, INSTALLLOGATTRIBUTES_FLUSHEACHLINE);
                            RTStrFree(pszLog);
                            AssertBreakStmt((uLogLevel == ERROR_SUCCESS), "Could not set installer logging level!");
                        }

                        UINT uStatus = ::MsiInstallProductA(pszTempFile, szMSIArgs);
                        if (   (uStatus != ERROR_SUCCESS)
                            && (uStatus != ERROR_SUCCESS_REBOOT_REQUIRED)
                            && (uStatus != ERROR_INSTALL_USEREXIT))
                        {
                            if (!fSilent)
                            {
                                switch (uStatus)
                                {
                                    case ERROR_INSTALL_PACKAGE_VERSION:

                                        ShowError("This installation package cannot be installed by the Windows Installer service.\n"
                                                  "You must install a Windows service pack that contains a newer version of the Windows Installer service.");
                                        break;

                                    case ERROR_INSTALL_PLATFORM_UNSUPPORTED:

                                        ShowError("This installation package is not supported on this platform.\n");
                                        break;

                                    default:

                                        /** @todo Use FormatMessage here! */
                                        ShowError("Installation failed! ERROR: %u", uStatus);
                                        break;
                                }
                            }

                            vrc = VERR_NO_CHANGE; /* No change done to the system. */
                        }
                    }
                    RTStrFree(pszTempFile);
                } /* Package needed? */
            } /* For all packages */
        }

        /* Clean up (only on success - prevent deleting the log). */
        if (   !fExtractOnly
            && RT_SUCCESS(vrc))
        {
            for (int i=0; i<5; i++)
            {
                vrc = RTDirRemoveRecursive(pszTempPathFull, 0 /*fFlags*/);
                if (RT_SUCCESS(vrc))
                    break;
                RTThreadSleep(3000 /* Wait 3 seconds.*/);
            }
        }

    } while (0);

    if (RT_SUCCESS(vrc))
    {
        if (    fExtractOnly
            && !fSilent
            )
        {
            ShowInfo("Files were extracted to: %s", pszTempPathFull);
        }

        /** @todo Add more post installation stuff here if required. */
    }

    if (strlen(szExtractPath) <= 0)
        RTStrFree(pszTempPathFull);

    /* Release instance mutex. */
    if (hMutexAppRunning != NULL)
    {
        CloseHandle(hMutexAppRunning);
        hMutexAppRunning = NULL;
    }

    /* Set final exit (return) code (error level). */
    if (RT_FAILURE(vrc))
    {
        switch(vrc)
        {
            case VERR_NO_CHANGE:
            default:
                vrc = 1;
        }
    }
    else /* Always set to (VINF_SUCCESS), even if we got something else (like a VWRN etc). */
        vrc = VINF_SUCCESS;
    return vrc;
}

