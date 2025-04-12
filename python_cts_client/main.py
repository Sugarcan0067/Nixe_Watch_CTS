import asyncio
import json
import os
import struct
from datetime import datetime
from bleak import BleakScanner, BleakClient

# CTS 服務與特徵值 UUID（依照 BLE CTS 定義）
CTS_SERVICE_UUID = "00001805-0000-1000-8000-00805f9b34fb"
CURRENT_TIME_CHAR_UUID = "00002a2b-0000-1000-8000-00805f9b34fb"

# 設定檔檔名與預設內容
CONFIG_FILE = "config.json"
DEFAULT_CONFIG = {
    "last_device": None,    # 格式：{"name": <裝置名稱>, "address": <位址>}
    "scan_interval": 300,     # 掃描間隔秒數 (預設 300 秒 = 5 分鐘)
    "sync_interval": 1800     # 校時間隔秒數 (預設 1800 秒 = 30 分鐘)
}

def load_config():
    """讀取設定檔，若不存在則建立預設設定檔。"""
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                config = json.load(f)
                print("載入 config.json 成功：", config)
                return config
        except Exception as e:
            print("讀取 config.json 失敗，使用預設設定：", e)
    save_config(DEFAULT_CONFIG)
    return DEFAULT_CONFIG

def save_config(config):
    """寫入設定檔到 config.json。"""
    try:
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(config, f, indent=4, ensure_ascii=False)
        print("設定已儲存至 config.json。")
    except Exception as e:
        print("寫入 config.json 失敗：", e)

def build_current_time_bytes(dt: datetime) -> bytes:
    """
    將 datetime 依照 BLE CTS 格式打包成 10 個位元組：
      - Year: 2 個位元組（小端序）
      - Month: 1 個位元組
      - Day: 1 個位元組
      - Hour: 1 個位元組
      - Minute: 1 個位元組
      - Second: 1 個位元組
      - Day of Week: 1 個位元組（1: 星期一 ~ 7: 星期日）
      - Fractions256: 1 個位元組（此處固定為 0）
      - Adjust Reason: 1 個位元組（手動更新設為 1）
    """
    time_bytes = struct.pack(
        "<HBBBBBBB",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        dt.isoweekday(),
        0  # fraction256 固定為 0
    ) + bytes([1])  # adjust_reason 設為 1 (Manual time update)
    return time_bytes

def parse_current_time_bytes(data: bytes) -> dict:
    """
    解析 CTS 讀回資料（預期為 10 個位元組），回傳各欄位組成的字典。
    """
    if len(data) < 10:
        print("回傳資料長度不足：", len(data))
        return {}
    values = struct.unpack("<HBBBBBBB", data[:9])
    adjust_reason = data[9]
    return {
        "year": values[0],
        "month": values[1],
        "day": values[2],
        "hour": values[3],
        "minute": values[4],
        "second": values[5],
        "day_of_week": values[6],
        "fraction256": values[7],
        "adjust_reason": adjust_reason,
    }

def choose_device(devices) -> object:
    """
    顯示掃描到的裝置清單，讓使用者自行輸入編號選擇目標裝置。
    """
    if not devices:
        return None
    print("掃描到以下裝置：")
    for index, device in enumerate(devices):
        print(f"[{index}] {device.name} ({device.address})")
    try:
        choice = int(input("請輸入目標裝置編號："))
        if 0 <= choice < len(devices):
            return devices[choice]
        else:
            print("輸入範圍錯誤。")
    except Exception as e:
        print("輸入錯誤：", e)
    return None

async def scan_for_device(target_address: str = None) -> object:
    """
    掃描附近的 BLE 裝置。
      - 若指定 target_address，則只回傳該目標裝置（掃描超時 10 秒）。
      - 若未指定，則回傳掃描到的裝置清單。
    """
    print("開始掃描 BLE 裝置...")
    try:
        devices = await BleakScanner.discover(timeout=10.0)
    except Exception as e:
        print("掃描失敗：", e)
        return None

    if target_address:
        # 尋找與 target_address 匹配的裝置
        for device in devices:
            if device.address.lower() == target_address.lower():
                print("找到目標裝置：", device)
                return device
        print("目標裝置不在掃描範圍內。")
        return None
    else:
        # 未指定 target_address，回傳掃描到的裝置清單
        if devices:
            return devices
        print("未發現任何 BLE 裝置。")
        return None

async def calibrate_device(device):
    """
    連線到指定裝置，透過 CTS 寫入目前系統時間後讀回驗證，
    校時完成後即斷開連線。
    """
    print(f"嘗試連線校時：{device.name} ({device.address})")
    try:
        async with BleakClient(device.address) as client:
            if not client.is_connected:
                print("連線失敗。")
                return
            print("已連線到裝置。")
            now = datetime.now()
            time_data = build_current_time_bytes(now)
            print("寫入時間：", now.strftime("%Y-%m-%d %H:%M:%S"))
            await client.write_gatt_char(CURRENT_TIME_CHAR_UUID, time_data)
            print("時間寫入完成。")
            await asyncio.sleep(1)  # 等待裝置更新
            read_data = await client.read_gatt_char(CURRENT_TIME_CHAR_UUID)
            device_time = parse_current_time_bytes(read_data)
            print("讀回裝置時間：", device_time)
    except Exception as e:
        print("校時過程中發生錯誤：", e)
    print("斷開連線。")

async def scanning_loop(config: dict):
    """
    定時掃描作業，每隔 config 中設定的 scan_interval 秒執行一次：
      - 若 config 中已有目標裝置，則先嘗試以 target_address 掃描確認是否在範圍內。
      - 若找不到或尚未設定目標裝置，則列出掃描清單供使用者選擇；
        並於首次選擇後立即進行校時。
    """
    while True:
        if config.get("last_device") is not None:
            # 嘗試以儲存的 target_address 進行掃描
            device = await scan_for_device(config["last_device"].get("address"))
            if device:
                config["last_device"] = {"name": device.name, "address": device.address}
                save_config(config)
                print(f"使用現有目標裝置：{device.name} ({device.address})")
            else:
                print("未找到儲存的目標裝置，請選擇新的目標裝置。")
                devices = await scan_for_device()  # 取得全部掃描清單
                if devices:
                    device = choose_device(devices)
                    if device:
                        config["last_device"] = {"name": device.name, "address": device.address}
                        save_config(config)
                        # 如果之前沒有設備（首次設定），則立即校時
                        await calibrate_device(device)
                    else:
                        print("未選擇任何裝置。")
                else:
                    print("無裝置可供選擇。")
        else:
            # 尚未設定目標裝置，執行掃描並讓使用者選擇
            print("尚未設定目標裝置，請選擇掃描清單中的裝置。")
            devices = await scan_for_device()
            if devices:
                device = choose_device(devices)
                if device:
                    config["last_device"] = {"name": device.name, "address": device.address}
                    save_config(config)
                    # 首次設定後立即進行校時
                    await calibrate_device(device)
                else:
                    print("未選擇任何裝置。")
            else:
                print("無裝置可供選擇。")
        await asyncio.sleep(config.get("scan_interval", 300))

async def calibration_loop(config: dict):
    """
    定時校時作業，每隔 config 中設定的 sync_interval 秒執行一次：
      - 若 config 中有目標裝置資訊，則嘗試掃描並對該裝置進行校時作業。
    """
    while True:
        if config.get("last_device"):
            device_info = config["last_device"]
            print(f"準備對 {device_info.get('name')} 進行校時...")
            # 先確認目標裝置在掃描中是否出現
            device = await scan_for_device(device_info.get("address"))
            if device:
                await calibrate_device(device)
            else:
                print("目前掃描中無法找到目標裝置，請透過掃描選擇更新目標裝置。")
        else:
            print("尚未設定目標裝置，請先透過掃描選擇裝置。")
        await asyncio.sleep(config.get("sync_interval", 1800))

async def main():
    """
    主流程：
      - 載入設定檔後，同時啟動掃描與校時背景工作，
        使程式自動檢查裝置並依設定頻率進行連線校時作業。
    """
    config = load_config()
    await asyncio.gather(
        scanning_loop(config),
        calibration_loop(config)
    )

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("程式手動中斷。")

