@echo off
rem Experimental multi-game PC VR launcher. Settings: pc-vr\settings.txt
rem Local licensed game dumps only; selecting a title does not establish compatibility.
title Any4Quest VR
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc-vr\launch.ps1" -LauncherProfile any %*
