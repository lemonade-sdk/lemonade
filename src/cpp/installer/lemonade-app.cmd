@echo off
:: Lemonade App Launcher
:: Launches the Lemonade Desktop App from the bin directory
:: Forwards all command-line arguments (e.g., --base-url)

"%~dp0..\app\Lemonade.exe" %*

