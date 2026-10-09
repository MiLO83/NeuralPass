@echo off
rem Start once, detached, so no console can steal focus while the game enters exclusive fullscreen.
wsl.exe -d Ubuntu -- bash -lc "pgrep -f '[t]ools/stream_bridge.py' >/dev/null || (cd /home/topnotch/github/MiLO83/NeuralPass && nohup external/stream-venv/bin/python -u tools/stream_bridge.py >/tmp/neuralpass-stream.log 2>&1 </dev/null &)"
exit /b
