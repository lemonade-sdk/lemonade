' This script detects wheter we are in headless mode and launches lemonade-server
' either in headless mode or with a system tray icon.

Set wshShell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)

' Declare headless variable
Dim HEADLESS

' Simple GUI detection: check if system tray is available
On Error Resume Next
Set shell = CreateObject("Shell.Application")
If Err.Number = 0 Then
    ' Try to access the system tray area
    Set trayWnd = shell.Windows()
    If Err.Number = 0 Then
        ' GUI mode: show tray
        Set trayWnd = Nothing
        Set shell = Nothing
        On Error GoTo 0
        HEADLESS = False
    Else
        ' Headless mode: no GUI
        Set shell = Nothing
        On Error GoTo 0
        HEADLESS = True
    End If
Else
    ' Headless mode: no GUI
    On Error GoTo 0
    HEADLESS = True
End If

If HEADLESS = True Then
    ' Headless mode: open a terminal and run the server without the tray
    wshShell.Run """" & scriptDir & "\lemonade-server.bat"" serve --no-tray", 1, True
Else
    ' GUI mode: Run the server on a hidden window with the tray
    wshShell.Run """" & scriptDir & "\lemonade-server.bat"" serve", 0, False
End If
