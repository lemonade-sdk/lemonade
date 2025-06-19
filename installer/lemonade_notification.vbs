' Lemonade Server Loading Notification
' Shows a notification that can be manually controlled
' Usage: wscript lemonade_notification.vbs [title] [message]

Dim objShell, objFSO, signalFile, windowTitle, messageText
Set objShell = CreateObject("WScript.Shell")
Set objFSO = CreateObject("Scripting.FileSystemObject")

' Get command line arguments or use defaults
If WScript.Arguments.Count >= 1 Then
    windowTitle = WScript.Arguments(0)
Else
    windowTitle = "Lemonade Server"
End If

If WScript.Arguments.Count >= 2 Then
    messageText = WScript.Arguments(1)
    ' Replace pipe characters with line breaks for multi-line notifications
    messageText = Replace(messageText, "\n", vbCrLf)
Else
    messageText = "Starting Lemonade Server..."
End If

' Signal file path for manual control
signalFile = objFSO.GetSpecialFolder(2) & "\lemonade_notification_signal.txt"

' Create signal file to indicate the notification is active
objFSO.CreateTextFile(signalFile, True).Close

' Show notification (no timeout - stays open until manually closed)
result = objShell.Popup(messageText, 0, windowTitle, 0)

' Clean up signal file
If objFSO.FileExists(signalFile) Then
    objFSO.DeleteFile signalFile
End If

Set objShell = Nothing
Set objFSO = Nothing 