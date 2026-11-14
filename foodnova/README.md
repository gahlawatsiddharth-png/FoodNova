# FoodNova — Food-waste monitoring and feedback

FoodNova is a local web app/API prototype aligned with the project synopsis: timestamped food-waste records, category/meal analytics, student feedback, and an ESP32 station endpoint.

## Run on Windows
1. Install Python 3.10+ from https://www.python.org/downloads/ (enable **Add Python to PATH**).
2. Extract the `foodnova` folder from this ZIP.
3. Open the extracted `foodnova` folder and double-click `run_server.bat`, or run:
   ```bat
   python server.py
   ```
4. Open **http://localhost:8000** in your browser.
5. Demo chef account: username `chef`, password `chef123`. Change this before sharing the server with others.

## Student feedback
Students create an account, log in, and submit feedback for each assigned waste event (reason, estimated percentage left, optional note). The chef dashboard shows feedback alongside recent disposal records and category/meal analytics. Students should avoid entering sensitive personal information in feedback notes.

## Use from phones on the same Wi-Fi
- Find the PC's local IPv4 address with `ipconfig`.
- On a phone connected to the same trusted Wi-Fi, open `http://PC-IP-ADDRESS:8000`.
- If Windows Firewall prompts, allow Python only on a trusted Private network. Do not expose this demo server directly to the public internet.

## ESP32
Edit Wi-Fi details and `SERVER` in `firmware/foodnova_esp32/foodnova_esp32.ino`. `SERVER` must use the PC's LAN IP, not `localhost`. Keep `DEVICE_KEY` identical in firmware and server environment. For a custom key, set `FOODNOVA_DEVICE_KEY` before starting the server. Example PowerShell:
```powershell
$env:FOODNOVA_DEVICE_KEY="replace-with-a-long-random-key"
python server.py
```
The camera classifier is currently a placeholder returning `Other`; connect your trained classifier before describing food recognition as operational. Calibrate the load cell with a known mass and verify motor/servo wiring before using real hardware.

## Important prototype notes
- This is a school-project prototype server, not hardened for public internet deployment. It uses basic in-memory sessions and HTTP; use a trusted local network only.
- Data is stored in `~/foodnova.db` on the computer running the server.
- Student accounts are self-sign-up accounts; this demo does not verify school identity.
- Dashboard data is calculated from recorded events; empty charts are normal until records are added.

## How the system follows the improved synopsis
- **Stable reading + one event per disposal:** the ESP32 waits for the weight to settle, then will not record again until the scale is empty. The server also drops repeated events (same event ID, or same weight within 10 s).
- **Unknown / Mixed:** low-confidence camera results are saved as Unknown / Mixed and appear in Admin > AI Analysis > Manual Review Queue.
- **Waste routing:** each record shows a Next Step: Redistribute (if safe), Compost, Treatment facility, or Needs review. Only Vegetables and Fruits go to Compost (see COMPOSTABLE in web/index.html).
- **Evidence-based:** no carbon, tree or "food saved" claims. Compost output is labelled an estimate. Admin > Testing records real test results and shows "To be measured" until you enter some.
- **Privacy:** the chef's screens and the wall dashboard do not show student names. Only the manager sees names (Canteen Access and Users & Logins).
- **Safety:** the motor/servo processing module is OFF in the firmware (ENABLE_PROCESSING) until you decide to use it.

## Screens
- **Login:** Student / Admin toggle. Students create their own account; the admin is `chef` / `chef123`.
- **Student:** Dashboard, Waste Monitoring (+ Log Waste), AI Analysis, Feedback, Compost.
- **Admin:** Analytics, Waste Monitoring (bin simulator), Compost, AI Analysis, Feedback, Testing. "Live Wall Dashboard" opens /dashboard.

## Manager account
- Log in with the **Admin** toggle as `manager` / `manager123` (change this password before real use).
- **Canteen Access:** Block or Allow any student, or Reset to Automatic (the 24-hour feedback rule). A manager decision overrides the rule, and the student sees why they are blocked.
- **Users & Logins:** everyone who has logged in, their role, last login, number of logins and whether they are online now, plus recent login activity.
- **Reason for blocking:** when the manager blocks a student a reason is required. The student sees it on their dashboard, and the manager can edit it with Edit reason.
- **Notes:** a student blocked by the manager sees a box to leave a note. Notes appear on the manager's Canteen Access page (the bell shows unread ones) with Mark read and Allow access buttons.
- The chef can still see analytics and feedback but cannot change canteen access.

## ID card scanning (ESP32-CAM + OpenCV + YOLOv8)
1. `python -m pip install -r requirements.txt` (OpenCV and NumPy). Without them the server still runs, but scanning is switched off.
2. Student ID cards need a **QR code** containing the ID number (for example `123456`, or `FOODNOVA:123456`). QR codes are the tested, recommended option. OpenCV's barcode reader only understands EAN-13, EAN-8 and UPC barcodes, so Code 128 or Code 39 school barcodes will not read, and even EAN reading was inconsistent in my tests. Students type the same number when they sign up, or link it later on their dashboard.
3. Flash `firmware/foodnova_idcam/foodnova_idcam.ino` to an AI-Thinker ESP32-CAM. A button (or IR sensor) on GPIO 13 takes the photo. One flash blink = student found, three = try again.
4. When the ID is read, the next food drop on the bin within 90 seconds is linked to that student, and they get a notification such as "You wasted 120 g of Rice-based. Your total today is 320 g." (Student > Notifications, plus a browser alert if they turned it on.)
5. **YOLOv8 (optional):** YOLOv8 finds the card in the photo before OpenCV reads it. It needs a model you train on photos taken by your own ESP32-CAM, because no ready-made "student ID card" model exists:
   - label about 200 photos with one class, `id_card` (for example in Roboflow or LabelImg)
   - `pip install ultralytics`, then `yolo detect train data=id.yaml model=yolov8n.pt imgsz=640 epochs=50`
   - copy `runs/detect/train/weights/best.pt` next to `server.py` and rename it `id_card.pt`
   Admin > ID Scanner shows whether the model is loaded and the measured read and match rates.
6. Photos are never saved. Only the time, read method and whether a student matched are logged.
7. The Windows EXE includes OpenCV scanning if OpenCV was installed when you built it. For YOLOv8, run `python server.py` instead, because bundling PyTorch makes the EXE very large.

## Using your laptop camera until the ESP32-CAM arrives
- Run the server on the laptop and open **http://localhost:8000** (browsers only allow the camera on localhost). Log in as the manager or chef, then go to **ID Scanner > Laptop Camera Scanner > Start camera**.
- Hold the QR code on a student ID card steady, about 20 cm from the lens. Frames go to the same YOLOv8 + OpenCV scanner as the ESP32-CAM, and are not saved.
- When the student is matched, type the grams and food type and press **Record food drop** (this replaces the load cell). The student gets the same notification as with the real hardware. You have 90 seconds after the scan.
- The chef sees only "student matched". The manager also sees the student's name.

## ESP32-S3-CAM station (one board does everything)
Use `firmware/foodnova_s3cam/foodnova_s3cam.ino`. It replaces the two older sketches: the S3 camera reads the ID card, the load cell weighs the food, and the board sends both to the server.
1. Arduino IDE: board **ESP32S3 Dev Module**, Tools > **PSRAM** = OPI (N8R8 modules) or QSPI (N8R2 modules). Libraries: HX711, LiquidCrystal_I2C.
2. Check the **camera pin list** at the top of the sketch against your board's documentation. The default is the common ESP32-S3-EYE / Freenove WROOM CAM layout, and boards differ.
3. Default wiring on pins the camera does not use: HX711 DT=1, SCK=2. IR sensor=14. LCD I2C SDA=47, SCL=21 (address 0x27). Change them in the sketch if your board exposes other pins.
4. Fill in Wi-Fi, the server IP and the device key, then upload.
5. Flow: a student steps up and the IR sensor wakes the station. The camera reads their ID QR code (LCD shows "ID OK"), they drop their food, the weight settles, and the LCD shows "Alert sent!". The student gets the notification instantly.
6. If the ID could not be read, the drop is still saved but with no student ("Saved (no ID)"), and nobody is notified.

## How the notification reaches the student
- Alerts are pushed to the student's open FoodNova page the moment the event is saved (no waiting). They also get a browser alert if they enabled it.
- A closed browser or locked phone cannot be reached this way. For that you would need SMS, WhatsApp or email, which need an outside service and are not included.

## Scan Station on the laptop (no ESP32 needed yet)
- Run `python server.py` on the laptop, log in as the chef or manager at http://localhost:8000, then open **http://localhost:8000/station** (or ID Scanner > Open full-screen Scan Station).
- Press **Start camera** and allow it. A student holds their ID card QR code up. On a match you hear a beep and see "ID accepted" (the screen never shows the student's name).
- Pick the food type, type or tap the grams, and press **Send alert to the student**. They have 90 seconds after the scan. The student's page shows the alert instantly. The station then goes back to scanning.
- When the ESP32-S3-CAM and load cell arrive, the same server and the same student alerts work with no changes. Only the weight stops being typed by hand.
