# GrainMeter

Monitor temperatury ziarna w silosie. Zastępuje ręczny czytnik BIN STW-100,
czyta jego oryginalną sondę wielopunktową (1-Wire, 8 punktów), pracuje z
akumulatora i panelu słonecznego, co 15 min mierzy, raz na godzinę wysyła
cztery próbki do ThingSpeak przez HTTPS i zasypia (deep sleep). Lokalnie:
diagnostyka przez USB, portal serwisowy na SoftAP z captive portalem i
aktualizacja firmware z przeglądarki (OTA z rollbackiem).

Ta sama logika aplikacji (C++11, bez wyjątków) działa na PC (symulator z
wirtualną sondą) i na **DFRobot FireBeetle 2 ESP32-C6 (DFR1075)**.

- [Struktura](#struktura)
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
Na PC `https://` jest celowo odrzucane – domyślnie nic nie trafia na prawdziwe konto.

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

- `cmake --workflow --preset dev` – 106 testów GoogleTest (CTest).
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
tablicy partycji). Aktualnie: aplikacja 1 233 552 B, slot 1 966 080 B (37 % wolne).

Partycje (`firmware/partitions.csv`, flash 4 MB ESP32-C6FH4): `nvs` 24 KiB,
`otadata`, `phy_init`, `ota_0` i `ota_1` po 1,875 MiB, `coredump` 64 KiB.

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

Konsola: dowolny terminal szeregowy na porcie USB (115200, wartość nieistotna
dla USB CDC), np. `python -m serial.tools.miniterm /dev/ttyACM0`. Konsola
działa bez sieci; urządzenie nie zasypia, dopóki jest używana (60 s od
ostatniej komendy).

```text
set svc_pass <min. 8 znaków>        # hasło SoftAP (WPA2) i zmian w portalu
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
```

Ustawienia są w NVS (przestrzeń `grainmeter`), mapowanie ROM → punkt i wysokość
przetrwa restart i utratę zasilania. Sekrety nie są logowane ani zwracane
przez API (tylko `*_set: true`). Bez `svc_pass` SoftAP się nie uruchomi
(nie ma domyślnego hasła w repozytorium; opcjonalnie lokalnie
`CONFIG_GM_DEFAULT_SERVICE_PASSWORD`).

Wszystko to samo da się zrobić w portalu (tryb serwisowy).

## ThingSpeak

Zweryfikowane w dokumentacji MathWorks (bulk write JSON, kody błędów, cennik):

- `POST https://api.thingspeak.com/channels/<id>/bulk_update.json`,
  `Content-Type: application/json`, body `{"write_api_key": "...", "updates": [...]}`.
- Sukces: **HTTP 200 i `{"success":true}`** – tylko wtedy próbki są usuwane z bufora.
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

Kanał diagnostyczny (8 pól zajętych przez temperatury, więc osobny kanał),
wysyłany co `diag_every_h` godzin (domyślnie 6, 0 = przy każdej wysyłce):
`field1` akumulator [mV], `field2` RSSI [dBm], `field3` znalezione czujniki,
`field4` błędne odczyty, `field5` próbki w buforze, `field6` próbki utracone,
`field7` licznik nieudanych wysyłek, `field8` licznik wybudzeń,
`status` = wersja firmware i przyczyna wybudzenia.

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

Wtedy: SoftAP **`GrainMeter-XXXX`** (WPA2, hasło = `svc_pass`, kanał 1,
maks. 2 klientów), captive DNS (każde zapytanie A → 192.168.4.1), telefon
sam otwiera portal; ręcznie: **<http://192.168.4.1/>**. Pomiary trwają dalej,
wysyłka jest wstrzymana (radio pracuje jako AP). Uśpienie jest zablokowane.
Koniec: przycisk „Zakończ tryb serwisowy” w portalu lub 10 min bez klientów
i żądań (nigdy w trakcie OTA) → powrót do cyklu pomiar/deep sleep.

„Wyślij teraz” w trybie serwisowym łączy się dodatkowo jako stacja (APSTA).
Jeśli sieć Wi-Fi pracuje na innym kanale niż AP (kanał 1), radio przełączy
kanał i telefon może się na chwilę rozłączyć – to normalne zachowanie ESP32.

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
| SNTP w każdym oknie | koryguje dryft zegara RC (~0,2 s), znaczniki czasu się nie rozjeżdżają |
| Tryb oszczędny | bateria < `batt_low_mv` (3450 mV): wysyłka 4× rzadziej, brak zdalnej aktualizacji |
| Tryb krytyczny | bateria < `batt_crit_mv` (3300 mV): bez Wi-Fi, pomiar 2× rzadziej, dane czekają w buforze |
| Przycisk EXT1 | wejście w serwis bez resetu i bez utraty bufora |

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
PC serwuje `portal/index.html` (~130 KiB), firmware wbudowuje
`portal/index.html.gz` (40,3 KiB, limit 60 KiB sprawdzany przy budowaniu).

```bash
make frontend      # npm ci + verify (format, lint, testy, build) w node:26-alpine
# praca nad UI z hot-reload i proxy /api do symulatora na :8080:
cd frontend && npm run dev   # http://127.0.0.1:5173/
```

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

## Do potwierdzenia na sprzęcie

1. Odczyt oryginalnej sondy przy **3,3 V** (VCC i pull-up 4,7 kΩ do 3V3):
   typ czujników (kody rodzin w `scan`), stabilność CRC na długim kablu,
   ewentualnie mniejszy pull-up (2,2 kΩ) lub zasilanie sondy z wyższego napięcia
   z konwerterem poziomów.
2. Pobór prądu w deep sleep całego układu (płytka + sonda + pull-up).
3. Captive portal na Android/iOS/Windows (automatyczne otwarcie, ręcznie 192.168.4.1).
4. Rollback: wgranie obrazu, który nie przejdzie samo-testu lub zresetuje się
   przed potwierdzeniem → powrót do poprzedniej wersji; przerwany upload.
5. Kalibracja pomiaru akumulatora (dzielnik 1 MΩ/1 MΩ, wpływ impedancji na ADC).
6. Czasy okna Wi-Fi/TLS przy słabym sygnale w silosie, zysk z zapamiętanego
   AP, stałego IP i jednej sesji TLS (pomiar prądu w oknie).
7. Wybudzenie przyciskiem na GPIO2 (EXT1) i pobór prądu pull-upu w uśpieniu.
8. Zdalna aktualizacja z prawdziwego serwera HTTPS, a w profilu podpisanym
   odrzucenie obrazu niepodpisanego / podpisanego innym kluczem.
