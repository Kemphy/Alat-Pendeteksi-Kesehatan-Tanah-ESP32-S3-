# 🌱 Sistem Pemantauan Kesehatan Tanaman v3.6

Sistem IoT berbasis **ESP32-S3** untuk memantau kondisi tanah secara real-time menggunakan sensor NPK RS-485, GPS, dan mengirim data ke **Firebase Realtime Database**. Dikembangkan sebagai bagian dari Tugas Akhir / Skripsi.

---

## 📋 Daftar Isi

- [Fitur](#fitur)
- [Komponen Hardware](#komponen-hardware)
- [Skema Koneksi Pin](#skema-koneksi-pin)
- [Instalasi Library](#instalasi-library)
- [Konfigurasi Awal](#konfigurasi-awal)
- [Upload Kode ke ESP32-S3](#upload-kode-ke-esp32-s3)
- [Cara Penggunaan](#cara-penggunaan)
- [Navigasi Menu](#navigasi-menu)
- [Struktur Data Firebase](#struktur-data-firebase)
- [Troubleshooting](#troubleshooting)

---

## ✨ Fitur

- 📊 Membaca **7 parameter tanah**: pH, Nitrogen, Fosfor, Kalium, Suhu, Kelembaban, Konduktivitas
- 📍 **GPS** untuk koordinat lokasi pengukuran
- 🕐 Sinkronisasi waktu via **GPS** atau **NTP (WIB/UTC+7)**
- 💾 Penyimpanan data lokal ke **LittleFS** (hingga 10 pengukuran per ID tanaman)
- ☁️ Upload data ke **Firebase Realtime Database** secara batch
- 📶 Manajemen WiFi: simpan hingga 4 jaringan, scan & input password via keypad
- 🖥️ Antarmuka **OLED SH1106 128×64** dengan navigasi keypad 4×4

---

## 🔧 Komponen Hardware

| No  | Komponen         | Spesifikasi                            |
| --- | ---------------- | -------------------------------------- |
| 1   | Mikrokontroler   | ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM) |
| 2   | Display          | OLED SH1106 128×64 (I2C)               |
| 3   | Keypad           | Membran 4×4                            |
| 4   | Sensor Tanah NPK | Sensor RS-485 Modbus RTU (7-in-1)      |
| 5   | Modul RS-485     | Konverter UART–RS485 (DE/RE pin)       |
| 6   | Modul GPS        | UART GPS (NMEA output, 9600 baud)      |
| 7   | Catu Daya        | 3.3V / 5V sesuai ESP32-S3              |

---

## 🔌 Skema Koneksi Pin

### OLED SH1106 (I2C)

| OLED | ESP32-S3                 |
| ---- | ------------------------ |
| VCC  | 3.3V                     |
| GND  | GND                      |
| SCL  | GPIO22 (default I2C SCL) |
| SDA  | GPIO21 (default I2C SDA) |

### Keypad 4×4

| Keypad | ESP32-S3 |
| ------ | -------- |
| R1     | GPIO38   |
| R2     | GPIO37   |
| R3     | GPIO36   |
| R4     | GPIO35   |
| C1     | GPIO42   |
| C2     | GPIO41   |
| C3     | GPIO40   |
| C4     | GPIO39   |

### Modul GPS (UART1)

| GPS | ESP32-S3         |
| --- | ---------------- |
| TX  | GPIO17 (RX1 ESP) |
| RX  | GPIO18 (TX1 ESP) |
| VCC | 3.3V / 5V        |
| GND | GND              |

### Sensor NPK RS-485 (UART2)

| RS-485 Module | ESP32-S3 |
| ------------- | -------- |
| RO (RX)       | GPIO16   |
| DI (TX)       | GPIO15   |
| DE + RE       | GPIO7    |
| VCC           | 5V       |
| GND           | GND      |

> **Catatan:** Pin DE dan RE modul RS-485 dihubungkan bersama ke GPIO7.

---

## 📚 Instalasi Library

Buka **Arduino IDE**, masuk ke **Sketch → Include Library → Manage Libraries**, lalu install library berikut:

| Library                                       | Versi yang Direkomendasikan |
| --------------------------------------------- | --------------------------- |
| `Firebase ESP Client` oleh Mobizt             | ≥ 4.x                       |
| `Adafruit GFX Library`                        | ≥ 1.11                      |
| `Adafruit SH110X`                             | ≥ 2.1                       |
| `TinyGPSPlus` oleh Mikal Hart                 | ≥ 1.0.3                     |
| `ArduinoJson` oleh Benoit Blanchon            | ≥ 6.x                       |
| `Keypad` oleh Mark Stanley & Alexander Brevig | ≥ 3.1                       |

> Library **WiFi**, **HardwareSerial**, **LittleFS**, dan **time.h** sudah bawaan ESP32 Arduino Core.

### Menambahkan ESP32-S3 Board

1. Buka **File → Preferences**
2. Tambahkan URL berikut ke _Additional Board Manager URLs_:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Buka **Tools → Board → Board Manager**, cari `esp32`, install oleh Espressif Systems (≥ 2.0)
4. Pilih board: **Tools → Board → ESP32 Arduino → ESP32S3 Dev Module**

---

## ⚙️ Konfigurasi Awal

Sebelum upload, sesuaikan bagian berikut di awal file `.ino`:

```cpp
// === KREDENSIAL DEFAULT ===
#define DEFAULT_WIFI_SSID "NamaWiFiAnda"
#define DEFAULT_WIFI_PASS "PasswordWiFiAnda"

// === FIREBASE ===
#define API_KEY       "API_KEY_DARI_FIREBASE_CONSOLE"
#define DATABASE_URL  "https://nama-project-default-rtdb.asia-southeast1.firebasedatabase.app/"
```

### Cara mendapatkan API Key & URL Firebase:

1. Buka [Firebase Console](https://console.firebase.google.com/)
2. Buat atau buka project → **Project Settings → General**
3. Scroll ke bawah, salin **Web API Key** sebagai `API_KEY`
4. Buka **Realtime Database → Data**, salin URL database sebagai `DATABASE_URL`
5. Pastikan **Authentication → Sign-in method → Anonymous** diaktifkan

---

## 🚀 Upload Kode ke ESP32-S3

1. Hubungkan ESP32-S3 ke komputer via USB
2. Pilih port COM yang sesuai: **Tools → Port**
3. Atur pengaturan board:
   - **Board:** ESP32S3 Dev Module
   - **Flash Size:** 16MB (128Mbit)
   - **Partition Scheme:** Custom — arahkan ke file `partitions.csv` di folder sketch
   - **PSRAM:** OPI PSRAM _(untuk N16R8)_
   - **Upload Speed:** 921600
4. Klik **Upload** (▶)
5. Buka **Serial Monitor** (baud rate: **115200**) untuk melihat log sistem

> **Struktur partisi custom (`partitions.csv`):**
>
> ```
> nvs,      data, nvs,      0x9000,   0x5000     # 20KB  – config WiFi dll
> coredump, data, coredump, 0xE000,   0x2000     #  8KB  – log crash/debug
> app0,     app,  factory,  0x10000,  0x700000   #  7MB  – firmware
> spiffs,   data, spiffs,   0x710000, 0x8F0000   # ~8.94MB – LittleFS data
> ```

---

## 📱 Cara Penggunaan

### Pertama Kali Menyalakan

Sistem akan menampilkan **splash screen** selama beberapa detik. Tekan tombol mana saja untuk masuk ke **Menu Utama**.

### Menu Utama

```
MENU UTAMA
1. Periksa Tanaman  ← Ukur & simpan data baru
2. Data Tanaman     ← Lihat & hapus data tersimpan
3. Kirim Data       ← Upload ke Firebase via WiFi
```

---

## 🗺️ Navigasi Menu

### Tombol Keypad

| Tombol | Fungsi Umum                      |
| ------ | -------------------------------- |
| `1–9`  | Pilih menu / input angka         |
| `#`    | Konfirmasi / OK / Simpan         |
| `*`    | Batal / Kembali                  |
| `D`    | Hapus karakter / Hapus data      |
| `A`    | Navigasi atas / Mode tampilan A  |
| `B`    | Navigasi bawah / Mode tampilan B |
| `C`    | Scan WiFi / Mode tampilan C      |

### Alur Menu 1 — Periksa Tanaman

```
Menu Utama
  └─[1]→ Input ID Tanaman (angka 1–999)
           └─[#]→ Mengukur Sensor (~7 detik)
                   └─ Tampilkan Hasil
                       ├─[A] pH, N, P, K
                       ├─[B] Suhu, Kelembaban, Konduktivitas
                       ├─[C] Koordinat GPS & Waktu
                       └─[#] Simpan ke LittleFS
```

### Alur Menu 2 — Data Tanaman

```
Menu Utama
  └─[2]→ Daftar ID Tanaman
           ├─[A/B] Navigasi atas/bawah
           ├─[#]→ List pengukuran ID terpilih
           │       ├─[A/B] Navigasi
           │       ├─[#]→ Detail pengukuran (A/B/C untuk detail)
           │       └─[D]→ Konfirmasi hapus pengukuran
           └─[D]→ Konfirmasi hapus semua data ID
```

### Alur Menu 3 — Kirim Data

```
Menu Utama
  └─[3]→ Menu Kirim Data (auto-connect WiFi terakhir)
           ├─[1]→ Pilih WiFi
           │       ├─[A/B] Navigasi daftar WiFi tersimpan
           │       ├─[#]→ Gunakan WiFi terpilih
           │       ├─[C]→ Scan WiFi baru → input password
           │       └─[D]→ Hapus WiFi dari daftar
           └─[2]→ Kirim semua data ke Firebase
                   (muncul setelah WiFi & Firebase siap)
```

### Menambah WiFi via Serial Monitor

Selain via keypad, WiFi dapat ditambahkan melalui Serial Monitor:

```
ADD_WIFI:NamaSSID,PasswordWiFi
```

---

## 🗄️ Struktur Data Firebase

Data dikirim ke path `/sensor/{datatanaman_id}/pengukuran/{waktu_pengambilan}`:

```json
{
  "sensor": {
    "1": {
      "datatanaman_id": 1,
      "pengukuran": {
        "26-02-2026T10:30:00": {
          "datatanaman_id": 1,
          "ph": 6.5,
          "nitrogen": 45,
          "fosfor": 30,
          "kalium": 60,
          "suhu": 28.3,
          "kelembaban": 55.2,
          "konduktivitas": 320,
          "koordinat": "-6.917464, 107.619123"
        }
      }
    }
  }
}
```

---

## 🗃️ Struktur File Lokal (LittleFS)

| File                | Isi                                               |
| ------------------- | ------------------------------------------------- |
| `/wifi_config.json` | Daftar WiFi tersimpan dan SSID terakhir digunakan |
| `/plant_{ID}.json`  | Data pengukuran per ID tanaman (maks 10 entri)    |

File lokal **otomatis dihapus** setelah berhasil diupload ke Firebase.

---

## 🔍 Troubleshooting

| Masalah               | Kemungkinan Penyebab             | Solusi                                            |
| --------------------- | -------------------------------- | ------------------------------------------------- |
| OLED tidak menyala    | Salah alamat I2C                 | Ganti `i2c_Address` ke `0x3D`                     |
| Sensor NPK selalu 0   | Koneksi RS-485 salah / baud rate | Cek kabel DE/RE dan koneksi GPIO                  |
| GPS tidak valid       | Belum mendapat fix satelit       | Tunggu 1–3 menit di area terbuka                  |
| WiFi gagal connect    | Password salah / sinyal lemah    | Cek password, gunakan menu Scan                   |
| Firebase signup error | API Key / URL salah              | Pastikan Anonymous Auth aktif di Firebase         |
| Data tidak tersimpan  | LittleFS penuh / JSON overflow   | Hapus data lama, atau turunkan `MAX_MEASUREMENTS` |
| Upload gagal terus    | Koneksi internet tidak stabil    | Coba lagi, sistem otomatis retry 3x               |

### Melihat Log Debug

Buka **Serial Monitor** dengan baud rate **115200** untuk melihat log lengkap seperti:

- Status koneksi WiFi dan IP address
- Hasil pembacaan semua sensor
- Progress upload per-pengukuran ke Firebase

---

## 📌 Catatan Penting

- **Kredensial WiFi dan API Key Firebase** jangan dibagikan secara publik. Jika kode akan diunggah ke GitHub, gunakan file terpisah yang masuk ke `.gitignore`.
- Maksimum **50 pengukuran per ID tanaman** (dikontrol oleh `#define MAX_MEASUREMENTS 50` di kode). Nilai ini dapat diubah sesuai kebutuhan — LittleFS tersedia ~8.94MB sehingga masih sangat lapang.
- Waktu pengukuran diambil dari GPS jika sinyal valid, atau dari NTP jika terhubung WiFi. Jika keduanya tidak tersedia, waktu default `01-01-2025T00:00:00` digunakan.

---

## 🛠️ Versi

**v3.6** — ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM) | Real Hardware Sensor Mode | Custom Partition
