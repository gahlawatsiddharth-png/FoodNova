@echo off
python -m pip install pyinstaller
python -m PyInstaller --onefile --name FoodNova --add-data "web;web" --add-data "schema.sql;." server.py
echo.
echo Done! Your app is dist\FoodNova.exe
pause
