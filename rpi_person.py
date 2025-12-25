import cv2
import numpy as np
import requests
import time
import ssl
import paho.mqtt.client as mqtt

# =========================
# CONFIG
# =========================
ESP_IP = ""  
STREAM_URL = f"http://{ESP_IP}:8080/stream"

MQTT_HOST = "a1326901dd494c3bb89746fcd9d4bf98.s1.eu.hivemq.cloud"
MQTT_PORT = 8883  # MQTT over TLS
MQTT_USER = ""
MQTT_PASS = ""

TOPIC_EVENT = "gd/cam01/event"

DETECT_EVERY_N_FRAMES = 3
COOLDOWN_SEC = 2.0
# =========================


def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"[MQTT] connected reason_code={reason_code}")


def on_disconnect(client, userdata, reason_code, properties=None):
    print(f"[MQTT] disconnected reason_code={reason_code}")


# OpenCV HOG person detector
hog = cv2.HOGDescriptor()
hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

# MQTT client
mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pi-cam01-hog")
mqttc.on_connect = on_connect
mqttc.on_disconnect = on_disconnect
mqttc.username_pw_set(MQTT_USER, MQTT_PASS)

# TLS encryption
mqttc.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)
mqttc.reconnect_delay_set(min_delay=1, max_delay=10)
mqttc.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
mqttc.loop_start()

# Connect to ESP32 MJPEG stream
r = requests.get(STREAM_URL, stream=True, timeout=10)
r.raise_for_status()

buf = b""
frame_i = 0
last_event = 0.0

try:
    while True:
        for chunk in r.iter_content(chunk_size=4096):
            if not chunk:
                continue

            buf += chunk

            a = buf.find(b"\xff\xd8")  # JPEG start
            b = buf.find(b"\xff\xd9")  # JPEG end

            # Ensure valid JPEG boundaries
            if a == -1 or b == -1 or b <= a:
                continue

            jpg = buf[a:b + 2]
            buf = buf[b + 2:]

            # Guard against corrupted frames
            if len(jpg) < 100:
                continue

            frame = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                continue

            frame_i += 1
            if frame_i % DETECT_EVERY_N_FRAMES != 0:
                continue

            small = cv2.resize(frame, (320, 240))

            rects, _ = hog.detectMultiScale(
                small,
                winStride=(8, 8),
                padding=(8, 8),
                scale=1.05
            )

            now = time.time()
            if len(rects) > 0 and (now - last_event) > COOLDOWN_SEC:
                payload = f'{{"type":"person","count":{len(rects)},"ts":{int(now * 1000)}}}'
                mqttc.publish(TOPIC_EVENT, payload, qos=1)
                print(f"[EVENT] published: {payload}")
                last_event = now

            for (x, y, w, h) in rects:
                cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

            cv2.imshow("HOG Person Detection", frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                raise KeyboardInterrupt

except KeyboardInterrupt:
    print("\nExiting...")

finally:
    try:
        mqttc.loop_stop()
        mqttc.disconnect()
    except Exception:
        pass
    cv2.destroyAllWindows()
