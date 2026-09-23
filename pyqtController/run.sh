#!/bin/zsh
# Launch the FX30 Multi-Camera Controller GUI
cd "$(dirname "$0")"
exec ./venv/bin/python fx30_controller.py
