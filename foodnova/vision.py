"""ID-card barcode reading for FoodNova.

Pipeline:  Image from the ESP32-CAM / Webcam -> YOLOv8 finds the ID card (optional) -> pyzbar / OpenCV reads the 1D BARCODE on it (QR codes are ignored).
YOLOv8 is only used if the `ultralytics` package is installed AND a trained weights file exists
(id_card.pt next to server.py, or the path in the FOODNOVA_YOLO environment variable).
Without it, the barcode scanner scans the whole frame, so scanning still works.
"""
import os
import re
import sys

try:
    import cv2
    import numpy as np
except Exception:          # server still runs; /api/scan then explains what to install
    cv2 = None
    np = None

try:
    from pyzbar import pyzbar
except Exception:
    pyzbar = None

HERE = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else os.path.dirname(os.path.abspath(__file__))
WEIGHTS = os.environ.get("FOODNOVA_YOLO", os.path.join(HERE, "id_card.pt"))
_model, _tried = None, False

# Explicitly exclude 2D QR codes so only 1D Barcodes are scanned
EXCLUDED_TYPES = {"QRCODE", "PDF417", "DATAMATRIX", "AZTEC"}


def _yolo():
    global _model, _tried
    if not _tried:
        _tried = True
        if os.path.exists(WEIGHTS):
            try:
                from ultralytics import YOLO
                _model = YOLO(WEIGHTS)
            except Exception:
                _model = None
    return _model


def status():
    return {
        "opencv": cv2 is not None,
        "pyzbar": pyzbar is not None,
        "yolo": _yolo() is not None,
        "weights": os.path.basename(WEIGHTS)
    }


def find_card(img):
    """YOLOv8 box (x1, y1, x2, y2, confidence) around the ID card, or None."""
    m = _yolo()
    if m is None:
        return None
    r = m.predict(img, imgsz=640, conf=0.4, verbose=False)[0]
    if len(r.boxes) == 0:
        return None
    b = max(r.boxes, key=lambda x: float(x.conf))
    x1, y1, x2, y2 = [int(v) for v in b.xyxy[0].tolist()]
    return x1, y1, x2, y2, float(b.conf)


def _variants(g):
    """ESP32-CAM frames are small, soft and unevenly lit, so try barcode-optimized clean-ups before giving up."""
    yield g
    # 2x & 3x horizontal upscaling helps linear 1D barcode bars separate clearly on 160x120 frames
    yield cv2.resize(g, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_CUBIC)
    yield cv2.resize(g, None, fx=3.0, fy=2.0, interpolation=cv2.INTER_LINEAR)
    # Local contrast enhancement (CLAHE)
    clahe = cv2.createCLAHE(2.5, (8, 8)).apply(g)
    yield clahe
    yield cv2.resize(clahe, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_CUBIC)
    # High-contrast binary thresholding for vertical barcode stripes
    yield cv2.adaptiveThreshold(g, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C, cv2.THRESH_BINARY, 31, 10)
    _, otsu = cv2.threshold(clahe, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    yield otsu
    # Unsharp / sharpening filter
    yield cv2.filter2D(g, -1, np.array([[0, -1, 0], [-1, 5, -1], [0, -1, 0]]))


def read_code(img):
    """Strictly reads 1D Barcodes (Code 128, Code 39, EAN-13, UPC, etc.) and ignores QR codes."""
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    bar = cv2.barcode.BarcodeDetector() if hasattr(cv2, "barcode") else None

    for v in _variants(g):
        # 1. Primary: pyzbar (Reads Code 128, Code 39, EAN, UPC, I25, etc.)
        if pyzbar is not None:
            try:
                decoded = pyzbar.decode(v)
                for obj in decoded:
                    btype = (obj.type or "").upper()
                    if btype not in EXCLUDED_TYPES:
                        txt = obj.data.decode("utf-8", errors="ignore").strip()
                        if txt:
                            return txt, f"barcode ({btype.lower()})"
            except Exception:
                pass

        # 2. Secondary Fallback: OpenCV built-in BarcodeDetector (Reads EAN/UPC)
        if bar is not None:
            try:
                res = bar.detectAndDecode(v)
                txt = (
                    res[0]
                    if isinstance(res[0], str)
                    else (res[1][0] if len(res) > 1 and res[1] is not None and len(res[1]) else "")
                )
                if txt:
                    return txt, "barcode"
            except Exception:
                pass

    return None, None


def clean(code):
    """Card barcodes may say FOODNOVA:FN-2026-101 or FN2026101. Normalize so both match."""
    if not code:
        return None
    code = re.sub(r"^\s*FOODNOVA[:\-\s]*", "", code, flags=re.I)
    return re.sub(r"[^A-Za-z0-9]", "", code).upper()[:20] or None


def read_id(data):
    """Image bytes -> {code, method, card_found, conf}."""
    if cv2 is None:
        raise RuntimeError("OpenCV is not installed")
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise ValueError("not an image")
    box, crop = find_card(img), img
    if box:
        x1, y1, x2, y2, _ = box
        crop = img[max(0, y1 - 12):y2 + 12, max(0, x1 - 12):x2 + 12]
    code, method = read_code(crop)
    if not code and box:
        code, method = read_code(img)      # card box was wrong: try the whole frame
    return {
        "code": clean(code),
        "method": method,
        "card_found": bool(box),
        "conf": box[4] if box else None
    }