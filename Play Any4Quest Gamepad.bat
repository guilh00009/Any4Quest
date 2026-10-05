@echo off
rem Any4Quest compatibility entry for the existing Astro gamepad profile.
rem For the multi-game chooser use Play Any4Quest VR.bat.
title Any4Quest Gamepad
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc-vr\launch.ps1" %*
