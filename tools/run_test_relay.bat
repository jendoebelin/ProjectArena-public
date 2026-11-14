@echo off
rem A private test server on this PC for headset tests with a real delay (never the community server; nobody else
rem sees its rooms). 50 ms each way + up to 5 ms: about a 100 ms round trip, a player in another part of the country.
rem Its stats, reports and sounds stay in build\tests\private_relay. Close this window (or Ctrl+C) to stop it.
rem Steps: docs/TEST_CHECKLIST_CATCHES.md. Optional: run_test_relay.bat 125 10  (ms each way, jitter)
setlocal
set ROOT=%~dp0..
set WORK=%ROOT%\build\tests\private_relay
set LAG=%1
set JIT=%2
if "%LAG%"=="" set LAG=50
if "%JIT%"=="" set JIT=5
if not exist "%WORK%" mkdir "%WORK%"
if not exist "%WORK%\sounds" mkdir "%WORK%\sounds"
echo 0.3.18> "%WORK%\latest.txt"
echo Private test server on port 47790: %LAG% ms each way + up to %JIT% ms. Close this window to stop it.
python -I "%ROOT%\server\projectarena_relay.py" --port 47790 --bind 0.0.0.0 --lag %LAG% --jitter %JIT% ^
  --stats "%WORK%\stats.db" --latest "%WORK%\latest.txt" --bans "%WORK%\bans.txt" --reports "%WORK%\reports" ^
  --sounds "%WORK%\sounds"
