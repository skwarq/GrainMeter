# GrainMeter

Monitor temperatury ziarna w silosie. Zastępuje ręczny czytnik BIN STW-100,
czyta jego oryginalną sondę wielopunktową (1-Wire, 8 punktów), pracuje z
akumulatora i panelu słonecznego, co 15 min mierzy, raz na godzinę wysyła
cztery próbki do ThingSpeak przez HTTPS i zasypia (deep sleep). Lokalnie:
diagnostyka przez USB, portal serwisowy na SoftAP z captive portalem (także
w sieci lokalnej, gdy płytka jest podłączona do komputera), wybór sieci Wi-Fi
z listy, logi przez UDP i aktualizacja firmware z przeglądarki (OTA z
rollbackiem).

Ta sama logika aplikacji (C++11, bez wyjątków) działa na PC (symulator z
wirtualną sondą) i na **DFRobot FireBeetle 2 ESP32-C6 (DFR1075)**.

- [Struktura](#struktura)
- [Narzędzia PC: konsola w sieci lokalnej](tools/README.md)
- [PC: budowanie i uruchomienie](#pc-budowanie-i-uruchomienie)
- [Symulacja awarii](#symulacja-awarii)
- [Testy, formatowanie, Valgrind](#testy-formatowanie-valgrind)
- [Firmware ESP32-C6](#firmware-esp32-c6)
- [Sonda STW-100 – podłączenie](#sonda-stw-100--podłączenie)
- [Pierwsza konfiguracja (USB)](#pierwsza-konfiguracja-usb)
- [ThingSpeak](#thingspeak)
- [Tryb serwisowy i lokalne OTA](#tryb-serwisowy-i-lokalne-ota)
- [Energooszczędność](#energooszczędność)
- [Zdalna aktualizacja i podpisywanie obrazów](#zdalna-aktualizacja-i-podpisywanie-obrazów)
- [Wydania i podpisywanie w CI](#wydania-i-podpisywanie-w-ci)
- [Portal (frontend)](#portal-frontend)
- [Dashboard z dowolnego miejsca (GitHub Pages)](#dashboard-z-dowolnego-miejsca-github-pages)
- [Historia lokalna w urządzeniu](#historia-lokalna-w-urządzeniu)
- [CI](#ci)
- [Do potwierdzenia na sprzęcie](#do-potwierdzenia-na-sprzęcie)

## Struktura

| Katalog | Zawartość |
| --- | --- |
| `core/` | wspólna logika (C++11, bez wyjątków/RTTI), podział logiczny: `onewire/` (Search ROM, CRC, DS18B20), `data/` (model pomiaru, mapowanie ROM→punkt, bufor próbek w RTC), `comm/` (ThingSpeak bulk JSON), `app/` (konfiguracja, harmonogram, aplikacja, portal REST, konsola), `ota/` (walidacja obrazu, sesja OTA, multipart), `util/` |
| `pc/` | symulator PC: `sim/` (zegar, sonda 1-Wire na poziomie bitów, sieć, lokalny ThingSpeak, OTA), `platform/` (plik konfiguracji, serwer HTTP na cpp-httplib), `app/` (urządzenie, `main`), `receiver/` (lokalny odbiornik `gm_receiver`) |
| `firmware/` | projekt ESP-IDF v6.1.1 (`esp32c6`): `main/hw` (GPIO 1-Wire, ADC akumulatora, LED/przycisk), `main/platform` (NVS, Wi-Fi, HTTPS, OTA, system), `main/service` (DNS captive, serwer HTTP, konsola USB), `components/gm_core` (te same źródła z `core/`) |
| `frontend/` | osobny projekt portalu (Preact), budowany do `portal/` |
| `portal/` | zbudowany portal: `index.html` (PC) i `index.html.gz` (wbudowany w firmware) – wspólne pliki dla obu platform |
| `tests/` | GoogleTest: `unit/`, `integration/` (portal przez HTTP, scenariusz end-to-end) |
| `docs/API.md` | REST API portalu (wspólne dla PC i ESP32), przykłady w `docs/api-samples/` |
| `scripts/` | `clang-format.sh`, `build-firmware.sh`, `firmware_artifacts.py` |

## PC: budowanie i uruchomienie

Wymagania: CMake ≥ 3.25, kompilator C++17 (GCC/Clang/MSVC), Ninja (dla presetu
`dev`). ESP-IDF nie jest potrzebne. Zależności PC (cpp-httplib v0.67.0,
GoogleTest v1.18.0) pobiera CMake (FetchContent, wersja + SHA-256).

```bash
# Linux / macOS / Windows - te same presety co w CI
cmake --workflow --preset dev        # konfiguracja + build + testy (Debug, Ninja)
cmake --workflow --preset ci         # Release, ostrzeżenia jako błędy, domyślny generator

# uruchomienie symulatora (8 czujników, czas x60)
build/dev/pc/grainmeter_pc --speed 60 --data-dir pc-data
```

Albo przez Makefile – domyślnie wszystko w Dockerze (wymagany tylko Docker):

```bash
make            # build + testy w obrazie docker/dev.Dockerfile
make run        # symulator, portal na http://127.0.0.1:8080/
make run-tls    # to samo z HTTPS (prawdziwy ThingSpeak, aktualizacje z GitHub)
make check      # to co CI na Linuksie: format, testy, Valgrind, firmware
make USE_DOCKER=0 test   # narzędzia z hosta, te same presety
```

**Portal lokalny: <http://127.0.0.1:8080/>** (nasłuch tylko na 127.0.0.1).

Przy pierwszym starcie symulator tworzy `pc-data/grainmeter-pc.conf`
(poza repozytorium, w `.gitignore`) z symulowaną siecią „SimulatedWiFi” i
kanałami 1000001/1000002 kierowanymi do **lokalnego** ThingSpeak (`mock://`).
Hasło serwisowe (wymagane do zmian w portalu) jest losowane i zapisane w tym
pliku jako `svc_pass`; można też podać `--password ...`.

Opcje: `--port`, `--bind`, `--data-dir`, `--config`, `--speed X` (sekundy
symulowane na sekundę), `--sensors N`, `--portal-dir`, `--no-console`.
Interwały zmienia się w portalu lub konsoli (`set measure_s 60`; na PC minimum
5 s, na ESP32 60 s).

Konsola na stdin to ta sama powłoka co na USB ESP32: `help`, `status`,
`diag`, `scan`, `measure [store]`, `map <ROM> <punkt> [cm]`, `unmap`,
`automap`, `config`, `set <klucz> <wartość>`, `upload`, `sim ...`.

Gdzie widać raporty:

- `pc-data/reports.jsonl` – każdy wpis przyjęty przez lokalny ThingSpeak,
- portal → „Wysyłka” (`/api/status` → `last_upload`, `buffer`).

Prawdziwa ścieżka HTTP bez konta ThingSpeak: `build/dev/pc/gm_receiver --port 8090`
i w symulatorze `set report_url http://127.0.0.1:8090`
(odczyt: `curl http://127.0.0.1:8090/channels/1000001/feeds.json`).

**Symulator → prawdziwy kanał ThingSpeak (HTTPS).** Zwykły build nie obsługuje
`https://`, więc domyślnie nic nie wychodzi na zewnątrz. Wariant z TLS
(OpenSSL ≥ 3.0, np. `sudo apt install libssl-dev`; certyfikat i nazwa hosta
są weryfikowane jak na ESP32):

```bash
make run-tls                                  # Docker: build z TLS + symulator na :8080
# albo bez Dockera:
cmake --workflow --preset dev-tls             # (make test-tls = same testy w Dockerze)
build/dev-tls/pc/grainmeter_pc --speed 60 --data-dir pc-data
```

W konsoli symulatora (albo w portalu → Konfiguracja):

```text
set report_url https://api.thingspeak.com
set ts_channel <Channel ID>
set ts_key <Write API Key>
set diag_channel <ID kanału diag>      # opcjonalnie
set diag_key <Write API Key diag>
upload
```

Dane trafiają tylko do pliku `pc-data/grainmeter-pc.conf` (poza
repozytorium). Wracasz do lokalnego mocka poleceniem
`set report_url mock://thingspeak`. Uwaga: przy `--speed 60` symulator robi
pomiar co 15 s rzeczywistych, a wysyłkę co minutę. Darmowy ThingSpeak ma limit
wiadomości, więc do dłuższych testów ustaw `--speed 1` albo dłuższe interwały.

## Symulacja awarii

Z konsoli symulatora (`sim ...`), z portalu (zakładka Symulacja) lub
`POST /api/sim` (szczegóły w `docs/API.md`). `sensor=N` to numer czujnika na
symulowanej sondzie (lista w diagnostyce `sim.probe.devices`), nie punkt pomiarowy.

| Komenda konsoli | Efekt |
| --- | --- |
| `sim crc sensor=3` (`count=-1` stale) | uszkodzony scratchpad → `crc_error`, pole pominięte w wysyłce |
| `sim remove sensor=2` / `sim restore sensor=2` | zniknięcie / powrót czujnika → `missing` |
| `sim temp sensor=4 value=0` | prawdziwe 0,00 °C (odróżnialne od braku) |
| `sim unsupported on` | dodaje DS18S20 (0x10) – widoczny w skanie jako „NOT MEASURED” |
| `sim short on` | DATA zwarte do masy → `bus_error` |
| `sim net down` / `sim net up` | przerwa i powrót sieci (próbki zostają w buforze) |
| `sim lost_response on` | serwer zapisuje, odpowiedź ginie → test braku duplikatów |
| `sim powerloss` | utrata zasilania: bufor RTC tracony, mapowanie zostaje |
| `sim speed value=600`, `sim advance seconds=3600` | przyspieszenie / przeskok czasu |
| `sim battery value=3400` (0 = krzywa) | wymuszone napięcie: tryb oszczędny / krytyczny |
| `sim ota_fail_boot on` | następny nowy obraz „nie wstaje” → rollback i blokada tej wersji |

Przykład awarii łączności: `sim net down` → po godzinie „Wysyłka” pokazuje
błąd `network: AP 'SimulatedWiFi' not found (simulated outage)`, bufor rośnie
→ `sim net up` → przy następnym oknie wysyłki zaległe próbki trafiają do
`reports.jsonl` jednym żądaniem, bufor pustoszeje.

## Testy, formatowanie, Valgrind

- `cmake --workflow --preset dev` – 116 testów GoogleTest (CTest).
  Jednostkowe: CRC/ROM, Search ROM na symulowanej magistrali, dekodowanie
  DS18B20 (tabela z datasheetu, 0,0 °C, brak, CRC, 85 °C power-on, zwarcie,
  rozdzielczości), trwałe mapowanie ROM (zapis + odczyt „po restarcie”),
  bufor (przepełnienie, usuwanie tylko potwierdzonych), ThingSpeak JSON/czas/
  odpowiedzi, harmonogram, walidacja konfiguracji (brak sekretów w JSON),
  OTA (walidacja obrazu, przerwany transfer, błąd zapisu, multipart przy
  każdym podziale na kawałki). Integracyjne: wysyłka z awarią/powrotem sieci,
  zgubiona odpowiedź bez duplikatów, brak czujnika ≠ 0, portal przez HTTP
  (hasło, blokada po 5 próbach, konfiguracja, mapowanie, OTA multipart/raw),
  scenariusz end-to-end, zdalna aktualizacja (instalacja, ta sama wersja,
  zły SHA-256, obcy obraz, rollback bez ponownej instalacji, okno serwisowe),
  tryby zasilania, SNTP i jedna sesja HTTP w każdym oknie.
- `cmake --workflow --preset valgrind` / `make valgrind` – te same testy pod
  Valgrind memcheck (błąd przy każdym błędzie pamięci lub wycieku `definite`).
- `scripts/clang-format.sh` (formatowanie) i `scripts/clang-format.sh --dry-run`
  (kontrola w CI) – styl Google, `.clang-format`, clang-format 18.
- Ostrzeżenia: `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wold-style-cast
  ...` (MSVC `/W4`), `-Werror` w presetach `ci`/`valgrind` i w firmware.

## Firmware ESP32-C6

ESP-IDF **v6.1.1** (przypięte: obraz `espressif/idf:v6.1.1`,
`firmware/main/idf_component.yml`, `firmware/dependencies.lock`), bez Arduino,
bez komponentów z rejestru.

```bash
scripts/build-firmware.sh          # albo: make firmware  (Docker, jak w CI)
# z lokalnym ESP-IDF 6.1.1:  GM_NO_DOCKER=1 scripts/build-firmware.sh
```

Wynik w `firmware/dist/`:

- `grainmeter-<wersja>-esp32c6-ota.bin` – **plik do aktualizacji z portalu**,
- `flash/` – pierwsze flashowanie: `bootloader.bin` (0x0), `partition-table.bin`
  (0x8000), `ota_data_initial.bin` (0xF000), `grainmeter.bin` (0x20000),
  `flash_args.txt`.

Skrypt sprawdza, czy aplikacja mieści się w **obu** slotach OTA (z binarnej
tablicy partycji). Aktualnie: aplikacja 1 246 352 B, slot 1 900 544 B (34 % wolne).

Partycje (`firmware/partitions.csv`, flash 4 MB ESP32-C6FH4): `nvs` 24 KiB,
`otadata`, `phy_init`, `ota_0` i `ota_1` po 1,81 MiB, `coredump` 64 KiB,
`history` 192 KiB (historia lokalna). Tablicy partycji nie zmienia aktualizacja
OTA: urządzenia wgrane wcześniej (v0.1.0/v0.1.1) działają dalej, ale historię
lokalną dostaną dopiero po wgraniu przez USB (`make flash-release`).

### Pierwsze flashowanie przez USB

FireBeetle podłączony kablem USB-C (natywny USB Serial/JTAG, sterownik nie
jest potrzebny). Wystarczy Docker – esptool działa w przypiętym obrazie ESP-IDF:

```bash
make flash-release                  # najnowsza wersja z pliku VERSION z GitHub Releases
make flash-release RELEASE=v0.1.0   # konkretne wydanie
make flash                          # lokalny build (firmware/dist, po: make firmware)
make monitor                        # konsola USB (wyjście: Ctrl+])
```

`make flash-release` pobiera paczkę z wydania i sprawdza `SHA256SUMS`. Potem
sprawdza, czy paczka zawiera ten sam obraz co plik OTA, oraz **podpis obrazu
kluczem `firmware/signing_pubkey.pem`**. Dopiero wtedy czyści flash i wgrywa
bootloader, tablicę partycji, `ota_data_initial.bin` i aplikację.

Opcje: `DEVICE_PORT=/dev/ttyACM1` (domyślnie `/dev/ttyACM0`), `ERASE=0` (bez
czyszczenia – zostaje konfiguracja w NVS). Skrypt bezpośrednio:
`scripts/flash.sh release v0.1.0`, `scripts/flash.sh local`.

Ręcznie (np. Windows, port `COMx`): rozpakuj
`grainmeter-X.Y.Z-esp32c6-flash.zip` i wykonaj

```bash
python -m esptool --chip esp32c6 -p /dev/ttyACM0 erase-flash
python -m esptool --chip esp32c6 -p /dev/ttyACM0 -b 460800 write-flash \
  --flash-mode dio --flash-size keep --flash-freq 80m \
  0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 grainmeter.bin
```

Jeśli port nie odpowiada: przytrzymaj BOOT, naciśnij RST, puść BOOT (tryb
pobierania ROM), po wgraniu naciśnij RST.

### Płytka – użyte piny (schemat DFR1075 V1.0/V1.1)

| Funkcja | GPIO | Na płytce | Uwagi |
| --- | --- | --- | --- |
| 1-Wire DATA | **GPIO3** | złącze P1 pin 8, **„IO3/A2”** | Kconfig `GM_ONEWIRE_GPIO`; nie jest strapping, nie używany na płytce |
| Pomiar akumulatora | GPIO0 (ADC1_CH0) | `ADC_BAT` | dzielnik R15/R16 = 1 MΩ / 1 MΩ (×2), C18 100 nF; obecny w V1.0 i V1.1 |
| Przycisk serwisowy (główny) | **GPIO2** | złącze P1 pin 9, **„IO2/A1”** | przycisk do GND + 10 kΩ do 3V3; budzi z deep sleep (EXT1) bez resetu – Kconfig `GM_SERVICE_WAKE_GPIO` |
| Przycisk serwisowy (zapasowy) | GPIO9 | BOOT | czytany dopiero po starcie aplikacji (patrz niżej) |
| LED | GPIO15 | LED „D13” | miganie w trybie serwisowym |
| Konsola / flash | GPIO12/13 | USB-C | USB Serial/JTAG |

Unikać: GPIO4, 5, 8, 9, 15 (strapping), 12/13 (USB), 16/17 (UART0), 0
(akumulator), 1/6/7/8/14/18–23 (złącze GDI wyświetlacza, wolne gdy brak
wyświetlacza). Ładowanie: CN3165, wejście 5 V / panel 5 V, max 0,5 A.
DFRobot podaje pobór w deep sleep 16 µA (V1.0) i 36 µA (V1.2) – do pomiaru.

## Sonda STW-100 – podłączenie

Sprzęt (tor zasilania, diody, akumulator AKYGA LP475085 i progi, ładowanie
poniżej 0 °C, budżet energii, schematy i noty w `docs/datasheets/`):
**[docs/hardware.md](docs/hardware.md)**.

Pełny schemat (sonda, przycisk serwisowy, panel, akumulator), lista części i
procedura pierwszego uruchomienia: **[docs/wiring.md](docs/wiring.md)**.

![Schemat podłączenia](docs/wiring.svg)

Żeńskie gniazdo panelowe Hirschmann CA 3 GD:

| Pin | Kolor | GrainMeter |
| --- | --- | --- |
| 1 | niebieski | GND (P1 pin 3) |
| 2 | czerwony | DATA → **GPIO3** (P1 pin 8) + **4,7 kΩ do 3V3** |
| 3 | czarny | VCC sondy → **3V3** FireBeetle (P1 pin 2) – pierwsza próba |
| 4 | zielony | niepodłączony (załączanie oryginalnego czytnika) |

- Próba wyłącznie z **odłączonym STW-100**: na DATA oryginalnego czytnika
  zmierzono ok. 4,95 V, a GPIO ESP32-C6 tolerują tylko 3,3 V. Nigdy nie
  łączyć obu urządzeń jednocześnie.
- Wewnętrzny pull-up GPIO jest dodatkowo włączony (`GM_ONEWIRE_INTERNAL_PULLUP`),
  ale zewnętrzny 4,7 kΩ jest wymagany (długi kabel sondy).
- Firmware przy starcie sprawdza, czy linia w spoczynku jest wysoka, i zgłasza
  brak pull-upu lub zwarcie.
- Diagnostyka bez sieci: konsola USB → `scan` wypisuje pełne 64-bitowe ROM,
  kod rodziny i nazwę: 0x28 DS18B20 (mierzony), 0x10 DS18S20/DS1820 i 0x22
  DS1822 („NOT MEASURED”, jawnie w diagnostyce). `measure` czyta wszystkie.
- Pomiar: Convert T dla wszystkich, odpytywanie gotowości do 850 ms (750 ms przy
  12 bitach + zapas), Match ROM + Read Scratchpad, CRC scratchpada.
  Rozróżniane: `ok` (także 0,00 °C), `missing`, `crc_error`,
  `unsupported_family`, `power_on_value` (85 °C), `bus_error`, `out_of_range`.

## Pierwsza konfiguracja (USB)

### Z pliku (zalecane)

```bash
make device-config        # tworzy config/device.conf (prawa 600, poza gitem)
# uzupełnij: svc_pass, wifi_ssid, wifi_pass, ts_channel, ts_key (+ opcjonalne)
make provision            # wgrywa ustawienia do płytki przez USB
make provision AUTOMAP=1 TEST_UPLOAD=1   # + przypisanie czujników i próbna wysyłka
make setup-board          # flash-release + provision jednym poleceniem
make provision-sim        # te same ustawienia dla symulatora PC
```

`make provision` w razie potrzeby resetuje płytkę, żeby dostać się do konsoli.
Potem wysyła wszystkie niepuste pola jedną komendą `apply`, a urządzenie
waliduje je razem i zapisuje atomowo (np. stały IP wymaga bramy i maski).
Puste pole oznacza „bez zmian”. Hasła i klucze nie są wypisywane – urządzenie
też ich nigdy nie zwraca. Na koniec pokazuje konfigurację (bez sekretów).
Port: `DEVICE_PORT=/dev/ttyACM1`. Firmware bez komendy `apply` (v0.1.0)
dostaje pola pojedynczo.

### Ręcznie w konsoli

Konsola: dowolny terminal szeregowy na porcie USB (115200, wartość nieistotna
dla USB CDC), np. `python -m serial.tools.miniterm /dev/ttyACM0`. Konsola
działa bez sieci; urządzenie nie zasypia, dopóki jest używana (60 s od
ostatniej komendy).

```text
set svc_pass <min. 8 znaków>        # hasło zmian w portalu (i SoftAP przy WPA2)
set wifi_ssid <sieć>
set wifi_pass <hasło>
set ts_channel <id kanału>
set ts_key <Write API Key>
set diag_channel <id>               # opcjonalnie, kanał diagnostyczny
set diag_key <Write API Key>
scan
automap                             # albo: map <ROM> <punkt 1..8> [wysokość cm]
measure
upload                              # test wysyłki od razu
set log_udp 1                       # opcjonalnie: logi przez UDP (patrz niżej)
set lan_wake 0                      # opcjonalnie: bez konsoli w sieci lokalnej
set ap_delay_s 60                   # SoftAP po 60 s bez sieci Wi-Fi (0 = od razu)
set ap_enabled 0                    # bez SoftAP i captive portalu
status                              # stan (JSON), diag = pełna diagnostyka
```

Ustawienia są w NVS (przestrzeń `grainmeter`), mapowanie ROM → punkt i wysokość
przetrwa restart i utratę zasilania. Sekrety nie są logowane ani zwracane
przez API (tylko `*_set: true`). Bez `svc_pass` portal działa tylko do
odczytu, a przy SoftAP z WPA2 (`GM_SERVICE_AP_OPEN=n`) AP się nie uruchomi.
Hasło ustawione w trakcie trybu serwisowego działa od razu, bez restartu.
W repozytorium nie ma domyślnego hasła; opcjonalnie lokalnie
`CONFIG_GM_DEFAULT_SERVICE_PASSWORD`.

Wszystko to samo da się zrobić w portalu (tryb serwisowy).

## ThingSpeak

Zweryfikowane w dokumentacji MathWorks (bulk write JSON, kody błędów, cennik):

- `POST https://api.thingspeak.com/channels/<id>/bulk_update.json`,
  `Content-Type: application/json`, body `{"write_api_key": "...", "updates": [...]}`.
- Sukces: **HTTP 200 lub 202 i `{"success":true}`** – tylko wtedy próbki są
  usuwane z bufora. Dokumentacja pokazuje 200, ale działające API odpowiada na
  zapis zbiorczy `202 Accepted` (sprawdzone na prawdziwym kanale).
  429 / 5xx / błąd transportu → ponowienie w następnym oknie; 400/401/402/413/422
  → błąd konfiguracji, próbki zostają (widoczne w portalu).
- Maks. 960 wpisów na żądanie (licencja darmowa); ≥ 15 s między żądaniami
  bulk na kanał. Wysyłamy ≤ 96 wpisów, raz na godzinę, osobne kanały.
- `created_at` w formacie z dokumentacji: `YYYY-MM-DD HH:MM:SS +0000` (UTC).
  „Znacznik musi być unikalny w kanale”: duplikat w jednym żądaniu odrzuca całe
  żądanie, a wpisy ze znacznikiem już istniejącym w kanale są odrzucane.
- TLS: `esp_http_client` + pakiet CA ESP-IDF (`esp_crt_bundle_attach`),
  weryfikacja certyfikatu zawsze włączona, `http://` odrzucane na ESP32.

Kanał główny: `field1..field8` = temperatury punktów 1–8 [°C],
`status` = `seq=<nr> p3:crc_error ...`. **Brakująca temperatura jest pomijana**
(pole nieobecne), nigdy nie jest wysyłana jako 0.

Kanał diagnostyczny (8 pól zajętych przez temperatury, więc osobny kanał):

- **Napięcie akumulatora z każdego pomiaru** (także bez sondy i bez
  zmapowanych czujników) czeka w buforze RTC jak temperatury i trafia
  zbiorczo w każdym oknie wysyłki jako osobne wpisy `field1` [mV] z czasem
  pomiaru – na ThingSpeak powstaje pełny wykres i statystyka baterii.
- **Pełny wiersz** co `diag_every_h` godzin (domyślnie 6, 0 = w każdym
  oknie): `field1` akumulator [mV], `field2` RSSI [dBm], `field3` znalezione
  czujniki, `field4` błędne odczyty, `field5` próbki w buforze, `field6`
  próbki utracone, `field7` licznik nieudanych wysyłek, `field8` licznik
  wybudzeń, `status` = wersja firmware, przyczyna wybudzenia, tryb zasilania.

Wynik w `last_upload.diag` (status, portal → Wysyłka): `sent`, `failed`,
`not_due` (nic do wysłania w tym oknie) albo `not_configured`.

### Bufor, duplikaty, przepełnienie, zasilanie

- Bufor: 96 próbek (24 h przy 15 min) w pamięci RTC, chroniony CRC-32.
  Przeżywa deep sleep i restart programowy (np. po OTA).
- Okno wysyłki: Wi-Fi ≤ 20 s, SNTP ≤ 10 s (w każdym oknie – zegar RTC
  chodzi na oscylatorze RC, bez kwarcu 32 kHz), HTTP ≤ 15 s na żądanie.
  Po porażce następna próba za godzinę (nie co pomiar) – oszczędność baterii.
- **Duplikaty przy ponowieniu**: każda próbka dostaje stały, rosnący znacznik
  UTC zapisany w buforze (próbki sprzed pierwszej synchronizacji czasu są
  przeliczane z licznika RTC raz, po synchronizacji). Ponowienie wysyła
  identyczne `created_at`, więc gdy serwer zapisał dane, a odpowiedź zginęła,
  ThingSpeak odrzuca powtórzone wpisy zamiast je dublować (test
  `Upload.LostResponseDoesNotDuplicate`). `status` zawiera numer `seq`.
- **Przepełnienie**: najstarsza próbka jest usuwana, licznik
  `dropped_overflow` (portal, kanał diagnostyczny) i wpis w błędach.
- **Aktualizacja z 0.1.0**: układ stanu w RTC zmienił się (wersja `GMR2`),
  więc po pierwszym starcie nowej wersji bufor jest raz inicjalizowany od nowa.
- **Utrata zasilania**: pamięć RTC ginie → bufor (maks. niewysłane próbki od
  ostatniej udanej wysyłki) przepada, stan inicjalizowany od nowa
  (`recovered_after_power_loss`). Konfiguracja i mapowanie (NVS) zostają.
  Próbki, którym nie da się odtworzyć czasu (RTC zrestartowany przed
  synchronizacją), są odrzucane i liczone w `dropped_no_time`.

## Tryb serwisowy i lokalne OTA

Wejście:
1. **Przycisk serwisowy na GPIO2** (P1 pin 9, do GND): budzi urządzenie z
   deep sleep bez resetu, więc niewysłane próbki w pamięci RTC zostają.
   Działa też, gdy urządzenie akurat nie śpi.
2. Zapasowo: naciśnij i puść **RST**, potem w ciągu 3 s naciśnij **BOOT**
   (LED miga szybko). BOOT trzymany podczas resetu wybiera tryb pobierania ROM
   (strapping GPIO9), dlatego firmware czyta go dopiero po starcie. Reset
   pinem RST może skasować pamięć RTC (bufor) – do potwierdzenia na płytce.
3. Pierwsze uruchomienie bez konfiguracji Wi-Fi, komenda `service` w konsoli
   USB albo zdalne żądanie z manifestu (`service_id`, patrz niżej).
4. **Komputer podłączony przez USB** (tryb „na biurku”, patrz niżej).

Gdy sieć Wi-Fi jest skonfigurowana, urządzenie najpierw łączy się z nią
(portal pod adresem w sieci lokalnej). SoftAP startuje dopiero wtedy, gdy
przez `ap_delay_s` sekund (domyślnie 60, ustawiane w portalu: Konfiguracja →
Serwis; 0 = od razu) połączenie się nie uda. Bez skonfigurowanej sieci SoftAP
startuje od razu. `set ap_enabled 0` w konsoli USB lub LAN wyłącza SoftAP i
captive portal całkowicie (celowo bez przełącznika w portalu); zostaje
wtedy portal przez sieć Wi-Fi, konsola USB i konsola w sieci lokalnej.

SoftAP: **`GrainMeter-XXXX`**, domyślnie **otwarty, bez hasła** (jak w
WIFI_RS; telefon łączy się od razu i pokazuje okno logowania). Kanał 1,
maks. 4 klientów. Captive portal:

- DNS odpowiada adresem 192.168.4.1 na każde zapytanie A (TTL 0, telefon nie
  zapamiętuje adresu po zakończeniu trybu),
- każdy nieznany adres HTTP (sprawdzanie łączności Androida, iOS, Windows)
  dostaje przekierowanie 302 na `http://192.168.4.1/`,
- DHCP podaje adres portalu w opcji 114 (RFC 8910), z której korzystają
  Android 11+ i iOS 14+.

Ręcznie: **<http://192.168.4.1/>** (z `http://`). Podgląd stanu jest dostępny
bez hasła, a każda zmiana, akcja i aktualizacja firmware wymaga hasła
serwisowego (`svc_pass`). SoftAP z WPA2 i hasłem `svc_pass`: Kconfig
`GM_SERVICE_AP_OPEN=n`.

Pomiary trwają dalej. Wysyłka działa tylko wtedy, gdy płytka ma równolegle
połączenie z siecią Wi-Fi (tryb na biurku albo po „Połącz i zapisz”);
w przeciwnym razie radio pracuje tylko jako AP. Uśpienie jest zablokowane,
LED miga co 1 s. Koniec: przycisk „Zakończ tryb serwisowy” w portalu lub
10 min bez klientów i żądań (nigdy w trakcie OTA i nigdy przy podłączonym
komputerze) → powrót do cyklu pomiar/deep sleep, LED gaśnie.

Gdy płytka łączy się z siecią Wi-Fi w trybie serwisowym (AP + stacja), AP
przechodzi na kanał tej sieci. Telefon podłączony do AP może się na chwilę
rozłączyć – to normalne zachowanie ESP32.

### Tryb „na biurku” (komputer na USB)

Płytka wykrywa komputer na USB (host USB, nie zwykła ładowarka ani panel)
i sama wchodzi w tryb serwisowy:

- nie zasypia i nie kończy trybu serwisowego, dopóki kabel jest w komputerze,
- łączy się dodatkowo z siecią Wi-Fi z konfiguracji (ponawia co 60 s), więc
  portal jest dostępny także w sieci lokalnej, np. `http://192.168.0.193/`;
  adres jest w logu (`portal on the local network: http://…/`), w `status`
  (`wifi.ip`) i w Diagnostyce,
- wysyłka do ThingSpeak działa normalnie przez to połączenie.

`exit-service` w konsoli wyłącza tryb do czasu odłączenia kabla. Po
odłączeniu od komputera urządzenie pracuje jak zwykle. Uwaga: po wybudzeniu
z deep sleep USB potrzebuje ok. 1 s na ponowne zgłoszenie się w komputerze;
jeśli płytka zaśnie wcześniej, naciśnij RST albo przycisk serwisowy.

### Wybór sieci Wi-Fi w portalu

Jak w WIFI_RS: Konfiguracja → Wi-Fi → **Szukaj sieci** pokazuje sieci 2,4 GHz
w zasięgu (siła sygnału, kanał, kłódka = sieć z hasłem; jedna pozycja na SSID,
najsilniejsze na górze). Kliknięcie wpisuje SSID; potem hasło i ewentualnie
stały IP (puste pola = DHCP) i **Połącz i zapisz**. Urządzenie łączy się w
tle, a portal odpytuje o wynik:

- udane połączenie → ustawienia sieci i IP zapisane, widać adres IP i sygnał;
  portal jest od razu dostępny także pod tym adresem,
- złe hasło / brak sieci / brak połączenia → komunikat, nic nie zapisano.

API: `POST /api/wifi/scan`, `POST /api/wifi/join` (pola `wifi_ssid`,
`wifi_pass`, `wifi_pass_clear`, `static_ip`, `gateway`, `netmask`, `dns`;
puste hasło = zapisane), wynik `GET /api/wifi/join`
(`idle|pending|running|ok|failed`). ESP32-C6 obsługuje tylko 2,4 GHz.

### Konsola w sieci lokalnej (wybudzanie z LAN)

Do pracy z urządzeniem na baterii bez kabla: skrypt na komputerze w tej samej
sieci czeka, aż urządzenie się zgłosi, łączy się i dopóki połączenie trwa,
urządzenie nie zasypia (portal w sieci lokalnej, wgrywanie firmware, testy
sondy).

```bash
make lan-console                              # interaktywnie, hasło z config/device.conf
make lan-console LAN_ARGS="--device GrainMeter-0079 --no-logs"
python3 tools/lan_console.py --cmd status     # jedna komenda i koniec
make lan-list                                 # urządzenia zgłaszające się w sieci
```

Przebieg (protokół v1, szczegóły w [tools/README.md](tools/README.md)):

1. Po RST lub włączeniu zasilania (przy skonfigurowanej sieci i baterii
   powyżej `batt_low_mv`) urządzenie od razu łączy się z Wi-Fi, ustawia
   czas i przez **3 s** czeka na skrypt – wystarczy uruchomić
   `make lan-console` i nacisnąć RST.
   Na końcu każdego okna wysyłki (Wi-Fi i tak włączone) urządzenie rozsyła
   UDP `GM-HELLO` na port 5515 i przez **0,5 s** czeka na połączenie TCP na
   porcie 5516. W trybie serwisowym ogłasza się co 5 s i przyjmuje połączenie
   cały czas.
2. Skrypt łączy się od razu i loguje hasłem serwisowym (`auth`; 3 błędy =
   rozłączenie, wspólna blokada z portalem). Urządzenie nie zasypia tylko dla
   zalogowanego klienta; połączenie bez logowania jest zamykane po 10 s.
3. Sesja = konsola USB przez Wi-Fi (te same komendy) + logi na żywo
   (`logs off` wyłącza). Urządzenie pracuje jak w trybie na biurku, bez
   SoftAP.
4. Skrypt co 10 s wysyła `#ping`. Urządzenie kończy sesję po 60 s ciszy, po
   4 h albo po `exit` i wraca do cyklu z deep sleep.
5. Restart w trakcie sesji (komenda `restart`, aktualizacja firmware):
   urządzenie po starcie przez 2 min ogłasza się co 3 s, a skrypt sam łączy
   się ponownie.

Czeka się najwyżej do najbliższego okna wysyłki (`upload_s`, domyślnie
1 h). Okno otwiera się co `upload_s` także wtedy, gdy nie ma czego wysłać
(brak sondy lub zmapowanych czujników): idzie wtedy diagnostyka, sprawdzenie
aktualizacji i hello. Koszt: 0,5 s radia na okno, ok. 0,3 mAh na dobę. Hasło idzie w sieci
lokalnej otwartym tekstem (jak portal przez HTTP). Wyłączenie:
`set lan_wake 0`. Testy skryptu z symulatorem urządzenia: `make tools-test`.

### Logi przez UDP

Jak w WIFI_RS: Konfiguracja → **Logi przez UDP** (albo w konsoli
`set log_udp 1`, `set log_level 1..4`, `set log_host`, `set log_port`).
Każda linia logu trafia dalej na konsolę USB, a dodatkowo jako datagram UDP
na `log_host:log_port` (domyślnie port 5514; puste `log_host` = rozgłoszenie
255.255.255.255 w sieci lokalnej). Linie mają prefiks z nazwą SoftAP, np.
`GrainMeter-0079 I (24880) app: …`, więc da się rozróżnić kilka urządzeń.
Znacznik czasu to lokalny czas `HH:MM:SS.sss` (strefa z Kconfig
`GM_TIMEZONE`, domyślnie Polska; przed pierwszą synchronizacją SNTP liczony
od 00:00). Poziom: 1 błędy, 2 ostrzeżenia, 3 informacje, 4 debug (dotyczy też konsoli
USB, gdy logi UDP są włączone). Odbiór:

```bash
nc -ulk 5514
```

Logi idą tylko wtedy, gdy płytka ma połączenie z siecią Wi-Fi: w trybie na
biurku stale, w normalnym cyklu na baterii przez kilka sekund okna wysyłki.
Wysyłką zajmuje się osobne zadanie z kolejką, więc logowanie z zadań o małym
stosie (np. sterownik Wi-Fi) jest bezpieczne; linie, które nie zmieściły się w
kolejce, są liczone (`udp_log.dropped` w diagnostyce).

### Diagnostyka urządzenia

Strona **Diagnostyka** w portalu i `GET /api/diag` (`esp`), podobnie jak w
WIFI_RS:

| Pole | Opis |
| --- | --- |
| `uptime_s`, `boot_count`, `build`, `idf` | czas od tego startu, liczba uruchomień (NVS, bez wybudzeń), data kompilacji, ESP-IDF |
| `chip_temp_c` | temperatura krzemu ESP32-C6 (nie otoczenia; Wi-Fi i CPU ją podnoszą) |
| `heap` | wolna i zajęta pamięć, minimum od startu, największy wolny blok |
| `station` | połączenie Wi-Fi: SSID, IP, kanał, BSSID, RSSI |
| `udp_log` | wysłane i utracone linie logów UDP |
| `tasks` | zadania FreeRTOS: wolny stos i priorytet |
| `crash` | ostatnia awaria z partycji core dump: zadanie, PC, `mcause`, przyczyna, SHA-256 ELF; przycisk „Usuń zapis awarii” (`POST /api/crash/clear`) |

Aktualizacja (bez Internetu):
1. Zbuduj firmware lub pobierz artefakt z CI: `grainmeter-<wersja>-esp32c6-ota.bin`.
2. Połącz się z `GrainMeter-XXXX`, portal → **Aktualizacja**, wybierz plik
   (dowolna nazwa), podaj hasło serwisowe.
3. Obraz jest strumieniowany (formularz multipart) do nieaktywnego slotu.
   Najpierw sprawdzany jest nagłówek (magic, chip ESP32-C6, projekt
   `grainmeter`, rozmiar slotu) – inne pliki są odrzucane przed zapisem.
   Po zapisie `esp_ota_end()` weryfikuje cały obraz (SHA-256) i dopiero wtedy
   zmienia partycję startową. Restart.
4. Nowy firmware startuje jako `pending_verify`, sprawdza NVS, GPIO 1-Wire i
   sterownik Wi-Fi, i potwierdza się (`esp_ota_mark_app_valid_cancel_rollback`).
   Brak Internetu lub odpięta sonda **nie** powodują rollbacku. Błąd
   samo-testu albo reset/crash przed potwierdzeniem → bootloader wraca do
   poprzedniej wersji. Przerwany transfer → `esp_ota_abort()`, partycja startowa
   bez zmian.

Na PC formularz, odbiór pliku, walidacja i „restart” (zamiana slotów) są
symulowane (`pc-data/ota_*.bin`) i testowane; prawdziwe partycje, rollback,
captive portal i deep sleep wymagają płytki.

## Energooszczędność

Szacunek (do zmierzenia na płytce): deep sleep 16–36 µA wg DFRobot + sonda;
pomiar ~1 s przy ~25 mA co 15 min; okno Wi-Fi ~3–5 s przy ~100 mA raz na
godzinę → rzędu 5 mAh/dobę. Okno Wi-Fi jest najdroższe, dlatego:

| Mechanizm | Działanie |
| --- | --- |
| Zapamiętany AP | kanał i BSSID ostatniego połączenia w pamięci RTC – bez pełnego skanowania; gdy AP zniknie, jedno pełne skanowanie |
| Stały IP (opcja) | `static_ip`, `gateway`, `netmask`, `dns` – bez DHCP |
| Jedna sesja TLS | wszystkie żądania okna (dane, diagnostyka, manifest) przez jedno połączenie keep-alive |
| Diagnostyka co 6 h | `diag_every_h` (0 = przy każdej wysyłce) |
| Czas z nagłówka `Date` | Zegar RTC (oscylator RC, bez kwarcu 32 kHz) dryfuje; każda odpowiedź HTTPS ThingSpeak niesie dokładny czas w nagłówku `Date` i koryguje go za darmo (od 2 s różnicy). SNTP tylko po utracie zasilania albo gdy przez dobę nie było żadnej synchronizacji – oszczędza 0,5–1,6 s radia na okno. Diagnostyka: `clock` (`source`, `sync_age_s`, `last_fix_s`). |
| Tryb oszczędny | bateria < `batt_low_mv` (3650 mV): wysyłka 4× rzadziej, brak zdalnej aktualizacji |
| Tryb krytyczny | bateria < `batt_crit_mv` (3500 mV): bez Wi-Fi, pomiar 2× rzadziej, dane czekają w buforze |
| Przycisk EXT1 | wejście w serwis bez resetu i bez utraty bufora |
| Konsola LAN | tylko 0,5 s nasłuchu na końcu okna wysyłki (`lan_wake 0` wyłącza) |

Tryb zasilania widać w portalu (Przegląd, Diagnostyka) i w kanale
diagnostycznym (`status`: `power=...`). Próg 0 wyłącza dany tryb.

## Zdalna aktualizacja i podpisywanie obrazów

Ustaw `update_url` (manifest JSON przez HTTPS) i `update_check_h`
(domyślnie 24). W oknie wysyłki, w trybie normalnym, urządzenie pobiera:

```json
{"version": "0.2.0",
 "url": "https://example.com/grainmeter-0.2.0-esp32c6-ota.bin",
 "sha256": "<sha256sum pliku .bin>",
 "service_id": 7, "service_min": 15}
```

- Nowsza `version` → strumieniowe pobranie do nieaktywnego slotu tą samą
  ścieżką co upload z portalu (nagłówek, chip, projekt), zgodność SHA-256 z
  manifestem i wersji w obrazie z manifestem, `esp_ota_end` → restart →
  samo-test → potwierdzenie albo rollback.
- Wersja wycofana po nieudanym starcie (`esp_ota_get_last_invalid_partition`)
  nie jest instalowana ponownie – brak pętli aktualizacji.
- `service_id` (opcjonalnie): każde nowe id otwiera portal serwisowy na
  `service_min` minut (1–60) przy najbliższym oknie wysyłki – zdalne
  „przyjdź do silosu” bez wizyty z przyciskiem.
- Ręcznie z portalu (Aktualizacja → Zdalna aktualizacja): **Sprawdź
  dostępność** łączy Wi-Fi i czyta manifest bez pobierania obrazu; gdy jest
  nowsza wersja, pojawia się przycisk **Zainstaluj &lt;wersja&gt;**, który
  pobiera, weryfikuje i instaluje ją od razu, a portal czeka na powrót
  urządzenia. Działa też przy wyłączonym automatycznym sprawdzaniu
  (`update_check_h=0`), wystarczy `update_url`.
- Publikacja: wrzuć plik `grainmeter-<wersja>-esp32c6-ota.bin` z CI i manifest
  na serwer HTTPS; `sha256sum` pliku wpisz do manifestu.

**Podpisywanie (zalecane w produkcji)**: HTTPS chroni transport, ale ktoś z
dostępem do serwera z manifestem mógłby podmienić obraz. Profil
`firmware/sdkconfig.signed` sprawia, że aplikacja przyjmuje (z portalu i
zdalnie) tylko obrazy podpisane kluczem projektu (RSA-3072, format Secure
Boot V2, bez wypalania eFuse):

```bash
mkdir -p firmware/keys     # katalog w .gitignore - klucza NIE commitować
docker run --rm -v "$PWD/firmware/keys:/k" -u $(id -u):$(id -g) espressif/idf:v6.1.1 \
  espsecure generate-signing-key --version 2 --scheme rsa3072 /k/signing_key.pem
GM_SIGNED=1 scripts/build-firmware.sh     # wynik: firmware/dist-signed/
```

Urządzenie z firmware podpisanym przyjmie później tylko obrazy podpisane tym
samym kluczem (utrata klucza = aktualizacja tylko przez USB). Pierwsze
przejście na profil podpisany wymaga wgrania obrazu z `dist-signed`. Zwykłe CI
buduje wersję bez podpisu (bez sekretów). Podpisane wydania robi workflow
Release – patrz [Wydania i podpisywanie w CI](#wydania-i-podpisywanie-w-ci). Sprawdzone lokalnie kluczem testowym: build przechodzi, a
`espsecure verify-signature` potwierdza podpis.

## Wydania i podpisywanie w CI

Wypchnięcie tagu `vX.Y.Z` (zgodnego z plikiem `VERSION`) uruchamia
`.github/workflows/release.yml`. Workflow buduje podpisany firmware, sprawdza
podpis kluczem publicznym z repozytorium i tworzy GitHub Release z plikami:

| Plik | Do czego |
| --- | --- |
| `grainmeter-X.Y.Z-esp32c6-ota.bin` | aktualizacja z portalu albo zdalna |
| `manifest.json` | manifest zdalnej aktualizacji (`version`, `url`, `sha256`) |
| `grainmeter-X.Y.Z-esp32c6-flash.zip` | pierwsze flashowanie USB (profil podpisany) |
| `signing_pubkey.pem`, `SHA256SUMS` | weryfikacja |

Firmware w profilu podpisanym ma domyślnie
`update_url = https://github.com/skwarq/GrainMeter/releases/latest/download/manifest.json`,
więc urządzenia same instalują najnowsze wydanie (domyślnie sprawdzają raz na
dobę). Klient HTTPS urządzenia podąża za przekierowaniami GitHuba (maks. 5,
tylko `https://`, każdy host z weryfikacją certyfikatu).

### Jednorazowa konfiguracja

1. **Klucz podpisujący** – lokalnie, poza repozytorium:

   ```bash
   mkdir -p ~/grainmeter-key && chmod 700 ~/grainmeter-key
   docker run --rm -v ~/grainmeter-key:/k -u $(id -u):$(id -g) espressif/idf:v6.1.1 bash -c \
     'espsecure generate-signing-key --version 2 --scheme rsa3072 /k/signing_key.pem &&
      espsecure extract-public-key --version 2 --keyfile /k/signing_key.pem /k/signing_pubkey.pem'
   cp ~/grainmeter-key/signing_pubkey.pem firmware/signing_pubkey.pem   # publiczny – do commita
   ```

   Zrób kopię zapasową `signing_key.pem` w bezpiecznym miejscu, np. w
   menedżerze haseł albo na zaszyfrowanym nośniku. Utrata klucza oznacza, że
   urządzenia z profilem podpisanym da się aktualizować już tylko przez USB.
2. **Środowisko `release`**: Settings → Environments → *New environment*
   `release`:
   - *Deployment branches and tags* → *Selected* → reguła tagu `v*`;
   - *Required reviewers* → Ty (każde wydanie czeka na Twoje zatwierdzenie);
   - *Environment secrets* → `FIRMWARE_SIGNING_KEY` = cała zawartość
     `signing_key.pem`.
3. **Ochrona tagów**: Settings → Rules → Rulesets → *New tag ruleset* dla
   `v*`: blokada tworzenia, zmiany i usuwania z wyjątkiem administratora.
4. Konto GitHub z **2FA**.
5. Repozytorium publiczne: Settings → General → *Change visibility*.

### Wydanie wersji

```bash
echo 0.2.0 > VERSION
git commit -am "Release 0.2.0"
git tag v0.2.0
git push origin main v0.2.0
# Actions → Release → "Review deployments" → Approve
```

### Dlaczego to bezpieczne

- Klucz prywatny jest tylko sekretem środowiska `release`. Nie dostają go joby
  CI na push/PR ani pull requesty z forków. Każde użycie wymaga tagu `v*` i
  Twojego zatwierdzenia.
- Na runnerze klucz istnieje tylko podczas budowania (plik z prawami 600),
  potem jest usuwany w kroku `always()`. Workflow nigdy go nie wypisuje, a
  GitHub dodatkowo maskuje sekrety w logach.
- Narzędzia, które widzą klucz, są przypięte: obraz ESP-IDF po digeście SHA-256,
  akcje GitHuba po SHA commita.
- Przed publikacją podpis jest sprawdzany kluczem publicznym z repozytorium,
  więc pomyłka z kluczem zatrzymuje wydanie.
- Urządzenie sprawdza niezależnie: podpis RSA-3072 (profil podpisany),
  SHA-256 z manifestu, chip, projekt i wersję. Rollback chroni przed obrazem,
  który nie wystartuje. Publiczne pliki nikomu nie pozwalają wgrać własnego
  firmware bez klucza.
- Ryzyko szczątkowe to przejęcie Twojego konta GitHub (stąd 2FA i
  zatwierdzanie wydań). Rotacja klucza: dołączenie drugiego podpisu do obrazu
  (Secure Boot V2 obsługuje do 3 bloków podpisu) albo przeflashowanie USB.

## Portal (frontend)

Osobny projekt w `frontend/` (Preact 11, esbuild, ESLint, Prettier, testy
`node:test`; szczegóły w `frontend/README.md`). Architektura: SPA + REST, jeden moduł API, upload przez XMLHttpRequest z
postępem, build do jednego pliku osadzanego w firmware. Wygląd i grafiki: wizualizacja silosu z sondą i punktami w skali temperatur, karty
stanu, diagnostyka magistrali/czujników/liczników/błędów, PL/EN, tryb jasny i
ciemny, układ pod telefon. Brak zewnętrznych zasobów (działa na AP bez Internetu).

Wynik jest zapisany w `portal/` i używany bez Node przez oba buildy:
PC serwuje `portal/index.html` (~172 KiB), firmware wbudowuje
`portal/index.html.gz` (~55 KiB, limit 60 KiB sprawdzany przy budowaniu).

```bash
make frontend      # npm ci + verify (format, lint, testy, build) w node:26-alpine
# praca nad UI z hot-reload i proxy /api do symulatora na :8080:
cd frontend && npm run dev   # http://127.0.0.1:5173/
```

## Dashboard z dowolnego miejsca (GitHub Pages)

Strona **<https://skwarq.github.io/GrainMeter/>** czyta dane prosto z
ThingSpeak w przeglądarce, więc działa na telefonie i komputerze z każdego
miejsca, bez własnego serwera. Pokazuje:

- silos z aktualnymi temperaturami punktów i czasem ostatniego pomiaru,
- historię wszystkich punktów (24 h / 7 dni / 30 dni) z wartościami po
  najechaniu, przełączaniem punktów i przerwami tam, gdzie brak danych,
- tabelę: teraz, zmiana w 24 h, min, max,
- ostrzeżenia: temperatura alarmowa, wzrost na dobę (samozagrzewanie), błąd
  czujnika, brak nowych danych,
- stan urządzenia z kanału diagnostycznego (bateria, tryb zasilania, Wi-Fi,
  bufor, nieudane wysyłki).

Gdy w wybranym zakresie nie ma danych, strona nadal działa: widać zakładki
zakresu i historię, a silos pokazuje ostatnie dane spoza zakresu z
informacją, z kiedy pochodzą.

Przy pierwszym wejściu podajesz **Channel ID** i **Read API Key** (klucz
*odczytu* z zakładki API Keys – nie klucz zapisu) oraz opcjonalnie kanał
diagnostyczny i progi ostrzeżeń. Dane zostają **tylko w tej przeglądarce**
(localStorage); klucz trafia wyłącznie do `api.thingspeak.com`. Strona ma
restrykcyjne CSP (skrypt i style dopuszczone tylko po skrócie SHA-256, sieć
tylko do ThingSpeak). Na telefonie: menu przeglądarki → „Dodaj do ekranu
głównego”. Odświeża się co 5 minut.

Jednorazowo: GitHub → Settings → Pages → *Source: GitHub Actions*. Workflow
`.github/workflows/pages.yml` publikuje stronę po każdej zmianie we
`frontend/` na `main`. Lokalnie: `cd frontend && npm run build:dashboard`
(wynik w `build/dashboard/`).

## Historia lokalna w urządzeniu

Przy silosie zwykle nie ma Internetu, więc urządzenie trzyma historię we
własnej pamięci flash (partycja `history`, 192 KiB). Zapisywane są **tylko
zmiany**:

- pomiar zapisuje się, gdy którykolwiek punkt zmienił się o ≥ 0,15 °C (szum
  DS18B20 ±1 krok = 0,0625 °C nie wywołuje zapisu), zmienił się stan czujnika,
  bateria o ≥ 30 mV, sygnał Wi-Fi o ≥ 6 dB, tryb zasilania lub wynik wysyłki,
- przy braku zmian co 3 h zapis kontrolny (odróżnia „silos stabilny” od
  „urządzenie nie działało”),
- przed zmianą po okresie bez zmian zapisywany jest ostatni niezmieniony
  pomiar, więc linia na wykresie nie odbiega od pomiarów o więcej niż 0,15 °C,
- rekordy są różnicowe: pierwszy w sektorze ma pełny stan (34 B), kolejne
  tylko różnice (zwykle 6–14 B), każdy z CRC.

Test z realistycznym miesiącem (8 punktów, dryf 0,3 °C/dobę, wahania dobowe,
szum czujników): 561 z 2880 pomiarów zmienia dane, **~390 B/dobę → ok. 16
miesięcy historii** (wcześniej 3 KB/dobę, 62 dni). Spokojny silos zajmuje
jeszcze mniej.

Zużycie flash: kasowanie sektora następuje raz na pełny obieg bufora
(przy ~390 B/dobę – raz na ~500 dni), a flash ESP32-C6 wytrzymuje min.
100 000 cykli kasowania na sektor; to praktycznie nieograniczony czas pracy
(ograniczeniem jest raczej ~20 lat przechowywania danych przez flash).

Portal (tryb serwisowy lub symulator) ma zakładkę **Historia** z tym samym
widokiem co dashboard: wykres, tabela, ostrzeżenia, silos na żywo oraz karta
*Urządzenie* (bateria, sygnał Wi-Fi, tryby zasilania, nieudane wysyłki,
zajętość pamięci i na ile dni wystarczy). Portal pobiera historię **po
dniach** (`GET /api/history/<od>/<do>`, strony do 256 wierszy) – najpierw
dzisiaj, potem starsze dni; zakończone dni trzyma w pamięci przeglądarki i
przy odświeżaniu pyta tylko o dzisiaj. Opis API: [docs/API.md](docs/API.md).

Pomiary sprzed pierwszej synchronizacji czasu (ESP32 po włączeniu) są
dopisywane po pierwszej wysyłce (czas odtworzony, oznaczony restart po
włączeniu – wykres przerywa wtedy linię). W symulatorze historia jest w
`pc-data/history.bin`; symulator startuje z zegarem komputera, więc historia
działa od razu (`--no-time` = jak świeże ESP32 bez czasu). Historia zapisana
przez 0.2.0 (inny format) jest pomijana i nadpisywana po kolei.

Dashboard (GitHub Pages) pokazuje baterię i sygnał z kanału diagnostycznego
(raport co 6 h).

## CI

`.github/workflows/ci.yml` (push i pull request, `permissions: contents: read`,
bez sekretów, akcje przypięte do SHA):

| Job | Co robi |
| --- | --- |
| PC (ubuntu-24.04, windows-2025, macos-15) | `cmake --workflow --preset ci` |
| Format + Valgrind | `make format-check`, `make valgrind` (obraz `docker/dev.Dockerfile`) |
| Portal UI | `npm ci && npm run verify`, kontrola że `portal/` jest aktualny |
| Firmware esp32c6 | `scripts/build-firmware.sh` w `espressif/idf:v6.1.1`, kontrola mieszczenia w slotach OTA, artefakt `grainmeter-firmware-esp32c6-<sha>` (plik OTA + pliki pierwszego flashowania) |
| Release (tag `v*`) | podpisany firmware, manifest, GitHub Release – patrz wyżej |
| Dashboard | test i build dashboardu, wdrożenie na GitHub Pages (push na `main`) |

## Do potwierdzenia na sprzęcie

1. Odczyt oryginalnej sondy przy **3,3 V** (VCC i pull-up 4,7 kΩ do 3V3):
   typ czujników (kody rodzin w `scan`), stabilność CRC na długim kablu,
   ewentualnie mniejszy pull-up (2,2 kΩ) lub zasilanie sondy z wyższego napięcia
   z konwerterem poziomów.
2. Pobór prądu w deep sleep całego układu (płytka + sonda + pull-up).
3. Captive portal na Android/iOS/Windows z otwartym AP i opcją DHCP 114
   (automatyczne otwarcie, ręcznie 192.168.4.1). Na laptopie z Linuksem
   sprawdzone: DNS, przekierowania 302 i otwarty AP.
4. Rollback: wgranie obrazu, który nie przejdzie samo-testu lub zresetuje się
   przed potwierdzeniem → powrót do poprzedniej wersji; przerwany upload.
5. Kalibracja pomiaru akumulatora (dzielnik 1 MΩ/1 MΩ, wpływ impedancji na ADC).
6. Czasy okna Wi-Fi/TLS przy słabym sygnale w silosie, zysk z zapamiętanego
   AP, stałego IP i jednej sesji TLS (pomiar prądu w oknie).
7. Wybudzenie przyciskiem na GPIO2 (EXT1) i pobór prądu pull-upu w uśpieniu.
8. Zdalna aktualizacja z prawdziwego serwera HTTPS (odczyt manifestu z
   GitHub Releases sprawdzony na płytce; instalacja nowszej wersji jeszcze
   nie), a w profilu podpisanym odrzucenie obrazu niepodpisanego /
   podpisanego innym kluczem.
9. Zapis i odczyt awarii (core dump) po prawdziwym crashu.

### Alarmy temperatury i powiadomienia push

W konfiguracji (portal lub `set <klucz> <wartość>`) ustaw `alarm_temp_c` (0–80 °C,
0 wyłącza) i/lub `alarm_rise_c` (0–20 °C, 0 wyłącza). `alarm_rise_h`
(1–72 h, domyślnie 24) określa czas porównania z historią lokalną. Brak
wartości z tego czasu oznacza pominięcie oceny wzrostu. Wykorzystywana jest
ostatnia wartość sprzed czasu docelowego, nie starsza niż dopuszczalna luka
historii (heartbeat + dwa najdłuższe odstępy pomiarowe + 60 s).
Próg temperatury ma histerezę 1 °C; błędny odczyt nie kasuje gorącego punktu.
Ocena następuje przy każdym pomiarze, więc „od razu” oznacza po wykryciu
podczas pomiaru, a nie ciągłą obserwację w deep sleep.

Nowy alarm otwiera okno Wi-Fi z pominięciem `upload_s` i liczby próbek.
`alarm_repeat_min` (0–1440, domyślnie 180) określa ponawianie; 0 wyłącza
powtórzenia. Okna alarmowe nie przesuwają zwykłego harmonogramu. W trybie
critical automatyczne okno alarmowe czeka na poprawę zasilania.
Dodatkowe wybudzenia i połączenia Wi-Fi/TLS zużywają energię, zwłaszcza
przy krótkim okresie powtarzania lub niedostępnej sieci.

`notify_ntfy_url` to adres z tematem, np. `https://ntfy.sh/moj-silos`:
JSON z tematem jest wysyłany na adres bazowy serwera. Pusty adres wyłącza ntfy.
Telegram wymaga `notify_tg_token` i `notify_tg_chat`; pusty token przy zapisie
zachowuje poprzedni, `notify_tg_token_clear=1` usuwa go. Token nie jest
zwracany w JSON (tylko `notify_tg_token_set`). HTTPS weryfikuje certyfikat.
Powiadomienia po polsku obejmują początek/ustąpienie alarmu i przejście w
oszczędzanie baterii. Limit danego typu alarmowego to jedno powiadomienie
na `alarm_repeat_min`, baterii — jedno na dobę. Kolejka sześciu zdarzeń
przeżywa deep sleep; przy przepełnieniu wypada najstarsze. Nieudany kanał
czeka na kolejne okno, udany nie jest powtarzany dla tego samego zdarzenia.
Stan widoczny jest jako `alarm` w `status`/`diag` i na karcie Przeglądu.

Przycisk testu w Konfiguracji używa zapisanych ustawień; API
`POST /api/notify/test` wymaga hasła serwisowego i zwraca wynik ntfy i Telegrama.
Na PC `mock://notify/temat` zapisuje ostatnie 12 wiadomości do
`/api/diag` → `sim.notifications`; temperaturę ustawia akcja
`sim action=temp sensor=1 value=40` (portal: `POST /api/sim`).

Zmiana układu `RetainedState` powoduje jednorazową reinicjalizację RTC
(kontrola rozmiaru i CRC): oczekujące próbki i liczniki zostaną wyzerowane
przy pierwszym uruchomieniu nowej wersji. Konfiguracja i historia lokalna
pozostają zachowane. Klucz trwałego zapisu `alarm_repeat_m` jest skrócony
ze względu na limit 15 znaków klucza NVS; formularze/JSON używają
`alarm_repeat_min`.
### Prognoza czasu pracy baterii

Portal i diagnostyka udostępniają `battery_forecast`: orientacyjny czas do
progu `batt_crit_mv` (domyślnie 3500 mV), bez nowych ustawień. Prognoza wykorzystuje
ostatnie 7 dni, minimum 4 punkty i dobę danych rozładowania. Pomija ładowanie
(wzrost ponad 20 mV/h lub napięcie ponad 4150 mV) i luki ponad 6 godzin.
Tempo jest średnią nachyleń odcinków ważoną czasem; pominięte przyrosty od panelu
nie przesuwają trendu. Wynik 0–365 dni uwzględnia wiek ostatniego pomiaru.
Przy zmianach poniżej 5 mV/dobę pokazuje stan stabilny, przy przewadze ładowania
w ostatniej dobie — ładowanie. Brak wystarczających lub świeżych danych daje
stan nieznany. Jest to przybliżenie napięciowe, zależne od obciążenia i pogody.

Obliczenia lokalne wykonywane są przy odczycie statusu/diagnostyki, ze stałym
buforem 169 punktów (ostatni rekord każdej godziny). Wymagają odczytu historii,
ale nie dodatkowych pomiarów ani transmisji radiowych. Dashboard używa już
pobranych danych diagnostycznych z wybranego zakresu i domyślnego progu
3500 mV (kanał nie zawiera ustawionego progu); zakres krótszy niż doba wyświetla
„za mało danych w zakresie”.
