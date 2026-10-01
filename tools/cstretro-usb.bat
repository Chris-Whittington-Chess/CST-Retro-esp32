@echo off
rem CST Retro on the CoreS3 as a UCI engine over USB (see tools\uci_bridge.py).
rem Uses the PlatformIO venv's Python (it has pyserial). Extra args go to the
rem bridge, e.g. --port COM3 --log bridge.log
"%~dp0..\..\m5-llm\.venv\Scripts\python.exe" -u "%~dp0uci_bridge.py" %*
