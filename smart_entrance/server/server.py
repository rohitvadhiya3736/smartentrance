from flask import Flask, request, jsonify, render_template_string
from deepface import DeepFace
import numpy as np
import cv2
import os
import base64
import time
import requests
from datetime import datetime

app = Flask(__name__)

# ─── USER CONFIG ──────────────────────────────────────────────
TELEGRAM_BOT_TOKEN = "8590597390:AAEfNFercZpgk4HIpGR2D8Upjt2EyaatH3I"    # <── paste your bot token
TELEGRAM_CHAT_ID   = "5995143601"     # <── paste your chat ID
# ─────────────────────────────────────────────────────────────

# ─── Paths ────────────────────────────────────────────────────
BASE_DIR        = os.path.dirname(os.path.abspath(__file__))
KNOWN_FACES_DIR = os.path.join(BASE_DIR, "known_faces")
TEMP_IMAGE      = os.path.join(BASE_DIR, "temp_capture.jpg")
UNKNOWN_IMAGE   = os.path.join(BASE_DIR, "unknown_capture.jpg")

# ─── Storage ──────────────────────────────────────────────────
detection_log  = []
stats          = {"total": 0, "known": 0, "unknown": 0, "no_face": 0}
last_update_id = None  # tracks last processed Telegram message globally

os.makedirs(KNOWN_FACES_DIR, exist_ok=True)

# ─── Dashboard ────────────────────────────────────────────────
DASHBOARD_HTML = open(os.path.join(BASE_DIR, "dashboard.html"), encoding="utf-8").read()

@app.route("/")
def dashboard():
    return render_template_string(DASHBOARD_HTML)

@app.route("/status", methods=["GET"])
def status():
    known_people = [
        os.path.splitext(f)[0].replace("_", " ").title()
        for f in os.listdir(KNOWN_FACES_DIR)
        if f.lower().endswith((".jpg", ".jpeg", ".png"))
    ]
    return jsonify({"status": "online", "known_people": known_people})

@app.route("/logs", methods=["GET"])
def get_logs():
    return jsonify({"logs": detection_log[-20:], "stats": stats})

# ─── Telegram Functions ───────────────────────────────────────
def send_telegram_photo(image_path, caption):
    try:
        url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendPhoto"
        with open(image_path, "rb") as photo:
            requests.post(url, data={
                "chat_id": TELEGRAM_CHAT_ID,
                "caption": caption
            }, files={"photo": photo}, timeout=10)
        print("[TELEGRAM] Photo sent!")
    except Exception as e:
        print(f"[TELEGRAM] Error: {e}")

def send_telegram_message(message):
    try:
        url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
        requests.post(url, data={
            "chat_id": TELEGRAM_CHAT_ID,
            "text": message
        }, timeout=10)
        print(f"[TELEGRAM] Sent: {message}")
    except Exception as e:
        print(f"[TELEGRAM] Error: {e}")

def init_update_id():
    """Get current latest update_id so we ignore all old messages"""
    global last_update_id
    try:
        url  = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/getUpdates"
        resp = requests.get(url, params={"offset": -1}, timeout=10)
        data = resp.json()
        if data.get("ok") and data.get("result"):
            last_update_id = data["result"][-1].get("update_id")
            print(f"[TELEGRAM] Initialized update_id: {last_update_id}")
    except Exception as e:
        print(f"[TELEGRAM] Init error: {e}")

def wait_for_owner_decision(timeout=30):
    """Wait up to 30 seconds for owner to reply yes/no — ignores old messages"""
    global last_update_id
    print(f"[TELEGRAM] Waiting for owner reply ({timeout}s)...")
    print(f"[TELEGRAM] Ignoring messages before update_id: {last_update_id}")
    start = time.time()

    while time.time() - start < timeout:
        try:
            url    = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/getUpdates"
            # offset = last_update_id + 1 means only fetch NEW messages after last known
            offset = (last_update_id + 1) if last_update_id else 0
            params = {"timeout": 5, "offset": offset}
            resp   = requests.get(url, params=params, timeout=10)
            data   = resp.json()

            if data.get("ok") and data.get("result"):
                for update in data["result"]:
                    uid = update.get("update_id")
                    msg = update.get("message", {}).get("text", "").strip().lower()
                    if msg in ("yes", "no", "y", "n"):
                        last_update_id = uid  # mark as processed globally
                        decision = "yes" if msg in ("yes", "y") else "no"
                        print(f"[TELEGRAM] Owner replied: {decision} (uid:{uid})")
                        return decision
        except Exception as e:
            print(f"[TELEGRAM] Poll error: {e}")
        time.sleep(2)

    print("[TELEGRAM] Timeout — defaulting to DENY")
    return "no"

# ─── Main Detection Endpoint ──────────────────────────────────
@app.route("/detect-face", methods=["POST"])
def detect_face():
    start_time = time.time()
    stats["total"] += 1
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    try:
        # ── Decode image ─────────────────────────────────────
        img_bytes = np.frombuffer(request.data, np.uint8)
        img = cv2.imdecode(img_bytes, cv2.IMREAD_COLOR)
        if img is None:
            return jsonify({"status": "error", "message": "Invalid image"}), 400

        img = cv2.resize(img, (320, 240))
        cv2.imwrite(TEMP_IMAGE, img)

        _, buffer = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 70])
        img_b64 = base64.b64encode(buffer).decode("utf-8")

        # ── Detect face ──────────────────────────────────────
        face_detected = False
        for backend in ["opencv", "ssd", "mtcnn"]:
            try:
                faces = DeepFace.extract_faces(
                    img_path=TEMP_IMAGE,
                    detector_backend=backend,
                    enforce_detection=True,
                    align=True
                )
                if faces:
                    face_detected = True
                    break
            except:
                continue

        if not face_detected:
            elapsed = round(time.time() - start_time, 2)
            stats["no_face"] += 1
            response = {
                "status":              "no_face",
                "person":              None,
                "processing_time_sec": elapsed,
                "timestamp":           timestamp
            }
            detection_log.append({**response, "image": img_b64})
            print(f"[{timestamp}] No face | {elapsed}s")
            return jsonify(response)

        # ── Recognize person ─────────────────────────────────
        person_name = "Unknown"
        confidence  = 0.0
        known_files = [f for f in os.listdir(KNOWN_FACES_DIR)
                       if f.lower().endswith((".jpg", ".jpeg", ".png"))]

        if known_files:
            try:
                results = DeepFace.find(
                    img_path=TEMP_IMAGE,
                    db_path=KNOWN_FACES_DIR,
                    model_name="SFace",
                    detector_backend="opencv",
                    enforce_detection=False,
                    silent=True
                )
                if results and len(results[0]) > 0:
                    top_match    = results[0].iloc[0]
                    matched_file = os.path.basename(top_match["identity"])
                    person_name  = os.path.splitext(matched_file)[0].replace("_", " ").title()
                    distance     = top_match.get("distance", 1.0)
                    confidence   = round((1 - distance) * 100, 1)
                    if confidence < 30:
                        person_name = "Unknown"
            except Exception as e:
                print(f"[RECOG] Error: {e}")

        elapsed  = round(time.time() - start_time, 2)
        is_known = person_name not in ("Unknown", "No known faces loaded")

        # ── Known person → grant immediately ─────────────────
        if is_known:
            stats["known"] += 1
            response = {
                "status":              "face_detected",
                "person":              person_name,
                "is_known":            True,
                "access":              "granted",
                "confidence":          confidence,
                "processing_time_sec": elapsed,
                "timestamp":           timestamp
            }
            detection_log.append({**response, "image": img_b64})
            print(f"[{timestamp}] ✅ GRANTED: {person_name} | {confidence}%")

            send_telegram_message(
                f"✅ ACCESS GRANTED\n"
                f"👤 Person: {person_name}\n"
                f"📊 Confidence: {confidence}%\n"
                f"🕐 Time: {timestamp}"
            )
            return jsonify(response)

        # ── Unknown person → ask owner ────────────────────────
        else:
            stats["unknown"] += 1
            print(f"[{timestamp}] ❓ Unknown person — asking owner...")

            cv2.imwrite(UNKNOWN_IMAGE, img)

            send_telegram_photo(
                UNKNOWN_IMAGE,
                f"🚨 UNKNOWN PERSON AT DOOR!\n"
                f"🕐 Time: {timestamp}\n\n"
                f"Reply YES to unlock\n"
                f"Reply NO to deny"
            )

            decision = wait_for_owner_decision(timeout=30)
            elapsed  = round(time.time() - start_time, 2)

            if decision == "yes":
                response = {
                    "status":              "face_detected",
                    "person":              "Guest (Owner Approved)",
                    "is_known":            True,
                    "access":              "granted_by_owner",
                    "confidence":          0,
                    "processing_time_sec": elapsed,
                    "timestamp":           timestamp
                }
                send_telegram_message("✅ Door UNLOCKED for guest!")
                print(f"[{timestamp}] ✅ Owner approved guest")
            else:
                response = {
                    "status":              "face_detected",
                    "person":              "Unknown",
                    "is_known":            False,
                    "access":              "denied",
                    "confidence":          0,
                    "processing_time_sec": elapsed,
                    "timestamp":           timestamp
                }
                send_telegram_message("🔒 Access DENIED. Door locked.")
                print(f"[{timestamp}] ❌ Owner denied guest")

            detection_log.append({**response, "image": img_b64})
            return jsonify(response)

    except Exception as e:
        print(f"[ERROR] {str(e)}")
        return jsonify({"status": "no_face", "person": None,
                        "processing_time_sec": 0, "timestamp": timestamp})


if __name__ == "__main__":
    known = [os.path.splitext(f)[0] for f in os.listdir(KNOWN_FACES_DIR)
             if f.lower().endswith((".jpg", ".jpeg", ".png"))]
    print("=" * 55)
    print("  SMART ENTRANCE - Face Recognition Server")
    print("  With Telegram Owner Notifications")
    print("=" * 55)
    print(f"  Known people ({len(known)}): {', '.join(known) if known else 'None'}")
    print("  Dashboard  -> http://localhost:5000")
    print("  API        -> http://localhost:5000/detect-face")
    print("=" * 55)
    init_update_id()  # ignore all old Telegram messages on startup
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)