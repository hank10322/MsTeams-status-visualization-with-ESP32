import os
import paho.mqtt.client as mqtt
import time
import glob

# =======================
BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC = "Your MQTT topic name"

client = mqtt.Client()
client.connect(BROKER, PORT, 60)
client.loop_start()
# =======================

# Teams 日誌路徑
LOG_DIR = r"Your MsTeams local fold"

# 檢查頻率 (秒)
CHECK_INTERVAL = .5

def get_teams_status():
    try:
        search_path = os.path.join(LOG_DIR, "**", "*")
        files = glob.glob(search_path, recursive=True)
        files = [f for f in files if os.path.isfile(f)]
        if not files:
            return None

        latest_file = max(files, key=os.path.getmtime)
        with open(latest_file, 'rb') as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - 5000))
            content = f.read().decode('utf-8', errors='ignore')

            if "Presenting" in content or "InAMeeting" in content:
                return "InAMeeting"
            elif "DoNotDisturb" in content:
                return "DoNotDisturb"
            elif "Busy" in content:
                return "Busy"
            elif "Available" in content:
                return "Available"
            elif "Away" in content:
                return "Away"
            elif  "Back" in content:
                return "BeRightBack"
            elif  "Offline" in content:
                return "Offline"
            return None
    except Exception as e:
        print(f"\n >>> ⚠️ 讀取 Teams 狀態失敗: {e}")
        return None

# --- 主程式 ---
print("--- Teams 狀態監控啟動（MQTT版）---")
print(f"Broker: {BROKER}:{PORT}")
print(f"Topic : {TOPIC}")

last_status = "Offline"
current_status = get_teams_status()
result = client.publish(TOPIC, current_status)
result.wait_for_publish()
try:
    while True:
        # 1. 抓取目前狀態
        current_status = get_teams_status() or last_status
        now_time = time.strftime('%H:%M:%S')

        # 2. 控制台即時顯示
        print(f"[{now_time}] 目前偵測狀態: {current_status}      ", end="\r")

        # 3. 只有「狀態改變」時，才發送到 MQTT
        if current_status != last_status:
            print(f"\n[{now_time}] 偵測到狀態改變！準備發送...")
            result = client.publish(TOPIC, current_status)
            result.wait_for_publish()
            
            print(f" >>> ✅ 已發送到 MQTT: {current_status}")

            last_status = current_status

        # 4. 等待下一輪
        time.sleep(CHECK_INTERVAL)

except KeyboardInterrupt:
    print("\n監控終止")

finally:
    client.loop_stop()
    client.disconnect()