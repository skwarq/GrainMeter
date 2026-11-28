<!-- SPDX-License-Identifier: MIT -->
# Emulator DS18B20 dla STW-100

Samodzielny projekt **ESP-IDF 6.1.1 / ESP32-C6**, FireBeetle 2 ESP32-C6.
Nie korzysta z komponentów GrainMeter. Jedna linia do STW-100 udaje 1–8
czujników: dowolne ROM-y, również `0000000000000000`, nietypowa rodzina
lub celowo niepoprawny CRC. Domyślnie:

| ROM (kolejność bajtów na magistrali) | Temperatura docelowa |
| --- | --- |
| `28 D0 5B CF 05 00 00 A9` | 20,0 °C |
| `28 6B 40 D0 05 00 00 F8` | 21,5 °C |

Po włączeniu scratchpad pokazuje 85 °C do zakończenia pierwszej konwersji.
Można wyłączyć tę funkcję osobno dla każdego wpisu.

## Bezpieczne podłączenie

**Przed podłączeniem zmierz napięcie spoczynkowe DATA względem GND STW-100.
ESP32-C6 nie toleruje napięcia powyżej 3,6 V. Rezystor 100 Ω NIE jest
translatorem poziomów.** Dla wyższego napięcia zastosuj dwukierunkowy
translator dla linii z otwartym drenem, np. BSS138, z poprawnie zasilonymi
stronami 3,3 V / napięcie mastera i odpowiednimi pull-upami. Sprawdź też
oscyloskopem czas narastania oraz przesunięcie zboczy translatora.

Przy bezpiecznym poziomie 3,3 V:

```text
STW-100 GND ------------------ ESP GND
STW-100 DATA ---- 100 ohm ----- ESP GPIO3
USB -------------------------- zasilanie ESP
```

Odłącz sondy od wejścia STW-100. ESP zasilaj osobno przez USB, nie z DATA
ani z przewodu zasilania sondy. Pull-up slave jest w STW-100; emulator
nie włącza wewnętrznych pull-upów. GPIO pracuje jako wejście + wyjście
open-drain: zapis 0 ściąga linię, zapis 1 **tylko zwalnia dren**, nigdy
nie wymusza wysokiego poziomu. Numer GPIO zmienia się w `menuconfig`;
używaj numeru GPIO, nie numeru pinu złącza, i sprawdź schemat płytki.

### Najprostsze podłączenie do magistrali 5 V: jedna dioda

```
STW-100 DATA ──|>|── GPIO3      (anoda przy DATA, katoda przy ESP)
                      └─[4,7..10 kΩ]── 3V3   (zalecane, opcjonalne)
STW-100 GND ──────── GND
```

Domyślnie włączony `CONFIG_OW_INTERNAL_PULLUP` trzyma pin na 3,3 V, gdy DATA
ma 5 V (dioda zaporowo, 5 V nie dochodzi do ESP). Stan niski na DATA ściąga
pin przez diodę. Dioda małosygnałowa o małej pojemności: BAT85 / BAT43 /
BAT46 (Schottky) albo 1N4148. Nie używaj 1N4007 (wolna) ani 1N5819 / SS14
(duża pojemność wydłuża zbocze przy słabym pull-upie wewnętrznym ~45 kΩ).
Zewnętrzny rezystor do 3V3 skraca zbocze narastające po stronie ESP.
Przy dzielniku, magistrali 3,3 V lub module BSS138 wyłącz tę opcję.

## Budowanie i uruchomienie

```bash
cmake -S tools/ow_emulator/test -B /tmp/ow_emulator_test
cmake --build /tmp/ow_emulator_test
ctest --test-dir /tmp/ow_emulator_test --output-on-failure

docker run --rm -v "$PWD:/project" -w /project/tools/ow_emulator \
  -u "$(id -u):$(id -g)" -e HOME=/tmp -e IDF_GIT_SAFE_DIR=/project \
  espressif/idf:v6.1.1 idf.py set-target esp32c6 build
```

Lub po aktywowaniu lokalnego IDF 6.1.1:

```bash
cd tools/ow_emulator
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`menuconfig → OW emulator`: GPIO slave (3), hasło AP (puste lub 8–63
znaki), opcjonalny master i jego GPIO (6). CPU musi pracować stale przy
160 MHz; projekt odrzuca dynamiczne skalowanie częstotliwości.
Duża standardowa partycja aplikacji mieści także opcjonalny master.
IDF 6 przeniósł cJSON do rejestru komponentów: manifest przypina
`espressif/cjson` 1.7.19; pierwsze budowanie wymaga dostępu do rejestru.
Komponent emulatora jest budowany z `-Wall -Wextra -Werror`, bez wyjątków
i RTTI. Nie trzeba budować żadnego innego katalogu repozytorium.

## Strona i konfiguracja

Połącz się z **OW-Emu-XXXX** (dwa ostatnie bajty MAC AP).
Otwórz `http://192.168.4.1/`, jeśli system nie pokaże captive portalu.
DNS odpowiada adresem AP na zapytania A. Android `/generate_204`,
iOS `/hotspot-detect.html` i Windows `/ncsi.txt` otrzymują przekierowanie
302 na stronę. HTTPS nie jest przechwytywany. Wszystko działa lokalnie,
bez CDN ani połączenia z Internetem. Domyślna sieć jest otwarta;
ustaw hasło przy pracy poza izolowanym stanowiskiem.

Na stronie dodaj/usuń wpisy, włącz/wyłącz czujnik, ustaw ROM, temperaturę
oraz źródło. **7 bajtów ROM**: CRC Maxim zostanie dopisany automatycznie.
**8 bajtów ROM**: adres jest zachowany dokładnie, także z błędnym CRC;
strona wyświetla ostrzeżenie. Spacje, `:` i `-` są dozwolonymi separatorami.
Nie jest wymagana rodzina `28`. Powtarzające się ROM-y są dozwolone, lecz
Search nie potrafi ich rozróżnić; odpowiedzi wybranych urządzeń to AND.

Opcje rozwijane: czas konwersji 0–60000 ms (domyślnie 750), zasilanie
zewnętrzne/pasożytnicze dla B4, stan startowy 85 °C, TH, TL, config,
bajty zarezerwowane 5–7 i surowy scratchpad. TH/TL są surowymi bajtami
0–255 interpretowanymi jako int8 przy porównywaniu alarmu. Config
`31/63/95/127` odpowiada 9/10/11/12 bitom. Temperatura jest zaokrąglana
do 1/16 °C, a nieważne bity przy niższej rozdzielczości są zerowane
(również dla temperatur ujemnych).

Niepusty surowy scratchpad ma **8 bajtów hex**; nadpisuje wyliczaną
temperaturę, stan 85 °C i układ bajtów. Dziewiąty bajt CRC jest zawsze
liczony automatycznie. Write Scratchpad zmienia jego bajty 2–4, a
następna konwersja przywraca skonfigurowane 8 surowych bajtów.

**Zapisz do NVS** utrwala cały zestaw. Jest stosowany dopiero podczas
długiego impulsu następnego resetu magistrali; do tego czasu
`pending_config=1`. Kolejny zapis zastępuje jeszcze niezastosowany zestaw.
Zastosowanie konfiguracji restartuje wirtualne czujniki i ich wirtualne
EEPROM-y. Nie restartuje Wi-Fi ani liczników. Błąd NVS jest zgłaszany,
bez udawania poprawnego zapisu lub automatycznego kasowania partycji.

API:

* `GET /config.json` – zapisana/docelowa konfiguracja (nie stan scratchpada
  zmieniony komendami mastera); pole `rom_crc_valid` informacyjne.
* `POST /config.json` – pełny zestaw `{"devices":[...]}`; 1–8 wpisów,
  do 12288 bajtów. Odpowiedź `{"ok":true,"apply":"next_reset","saved_to_nvs":true}`.
  Można wyłączyć wszystkie wpisy, wtedy nie ma presence.
* `GET /log.txt` – ostatnie do 200 zakończonych segmentów transakcji oraz
  liczniki. Strona pobiera log co 2 s.

Przykład celowego adresu zerowego i wyłączenia stanu startowego:

```json
{"devices":[{"enabled":true,"rom":"0000000000000000",
 "temperature":23.5,"source":"fixed","conversion_ms":700,
 "power_on_85":false,"powered":true,"th":75,"tl":70,
 "config":127,"reserved":"FF0C10","raw":""}]}
```

Log zawiera czas w µs od uruchomienia ESP, komendę ROM, adres Match/Search,
komendę funkcji, maskę wybranych wpisów (bit 0 = pierwszy), pełne bajty i
liczbę bitów oddanych masterowi. Search liczy 128 bitów odpowiedzi
arbitrażu, nie dane scratchpada. Reset lub bezczynność >2 ms zamyka segment;
stan protokołu i konwersji przetrwa bezczynność. Częściowe bajty widać
w `bits`. Bezadresowe Skip/Read ROM mają pole adresu zerowe – **nie oznacza
to odebrania Match ROM z adresem zerowym**. Trwająca transakcja pojawi się
dopiero po zakończeniu segmentu. Przy zmianie pierścienia podczas pobierania
API może zwrócić krótszy, ale spójny prefiks logu.

## Protokół

`main/ow_slave.{h,cpp}` to C++17 bez nagłówków IDF, alokacji i wyjątków.
Zdarzenia `lowStarted(nowUs)` / `lowEnded(widthUs, nowUs)` oraz
`bitToSend(nowUs)` rozdzielają protokół od elektrycznej realizacji.
Reset ≥400 µs przerywa dowolny stan. Szerokość <30 µs oznacza zapis 1,
więc impuls STW-100 17 µs nie jest błędnie traktowany jak zapis 0.
Dla odczytu zero warstwa GPIO widzi impuls łączny mastera i slave,
trwający do około 30 µs; rdzeń zna kierunek slotu i nie interpretuje go
jako zapisu. Presence: około 30 µs po zwolnieniu resetu, low 117 µs.

Obsługiwane: F0 Search, EC Alarm Search, 33 Read ROM, 55 Match ROM,
CC Skip ROM, 44 Convert T, BE Read Scratchpad (9 bajtów z CRC),
4E Write Scratchpad, 48 Copy Scratchpad, B8 Recall E2 i B4 Read Power.
Arbitraż śledzi maskę aktywnych urządzeń przy każdym wybranym bicie ROM.
Alarmy są aktualizowane po konwersji, przez porównanie całkowitej,
signed temperatury z TH/TL. Po starcie nie ma alarmów do konwersji.
Konwersja przeżywa reset; odczyt statusu zwraca AND: 0, dopóki choć jeden
wybrany czujnik jest zajęty, potem 1. Nieznany Match ROM nigdy nie
ściąga linii, ale funkcja jest dekodowana do logu: odczyt daje FF.
Copy/Recall działają natychmiast i tylko w RAM; status 1, zapis NVS
dotyczy wyłącznie konfiguracji strony. Emulacja pasożytniczego zasilania
jest logiczna (B4=0), nie wymaga ani nie symuluje poboru strong pull-up.

## Timing i jego granice

Wybrano **bezpośrednie przerwanie GPIO (IRAM, poziom 3)** na zbocze
opadające, następnie polling rejestrów GPIO z licznikiem cykli CPU w
sekcji krytycznej. HP core C6 jest pojedynczy; zadania HTTP, Wi-Fi i
mastera prawdziwej sondy nie wykonują się w środku tej sekcji.
Wyjściowy bit jest obliczony wcześniej podczas wysokiego poziomu;
na kolejnym zboczu dren jest ściągany **przed** księgowaniem protokołu.
CRC ma tablicę w DRAM; kod rdzenia/timingu jest w IRAM, dane statyczne
w wewnętrznym RAM. Nie ma logowania, NVS, cJSON ani alokacji w ISR.
Konfigurację i temperaturę źródła real przyjmuje się w reset-low ≥400 µs,
nie na krótkim zboczu odczytu.

Sekcja kończy się przy stanie wysokim >2 ms albo po 30 ms. Twardy limit
zwalnia DATA i zwiększa `overruns`; przy niskiej linii także `stuck_low`.
Nie jest to pełny zamiennik slave dla mastera prowadzącego nieprzerwane
transakcje >30 ms. Zapis 0 do 117 µs i reset 490 µs mieszczą się w modelu.
Przerwania/ticki Wi-Fi są opóźnione o długość transakcji; testuj z
rzeczywistym telefonem i obserwuj stabilność AP. Zapis NVS może dodatkowo
zwiększać opóźnienia początku transakcji, choć kod ISR nie wymaga cache.

`late_reactions`: odpowiedzi zero z oszacowaniem ≥8 µs oraz presence
późniejsze niż 60 µs. `max_reaction_cycles / 160` daje czas w µs.
W pollingu mierzony jest **konserwatywny górny limit**
od ostatniej obserwacji high do zapisu pull-low. Dla pierwszego zbocza
po bezczynności mierzy się tylko pracę od wejścia ISR – sprzętowa
latencja wejścia jest niewidoczna. `first_edges` liczy te przypadki.
Zwykle pierwsze zbocze to długi reset, a krótkie odczyty są już obsługiwane
w pollingu. Izolowane sloty statusu po >2 ms wymagają osobnego pomiaru
oscyloskopem. Liczniki programowe **nie dowodzą** opóźnienia <8 µs
na pierwszym zboczu. Ostrzeżenia zbiorcze są drukowane z zadania co 1 s,
nigdy w sekcji krytycznej.

Nie przeprowadzono pomiaru elektrycznego ani próby Wi-Fi/STW-100 na
fizycznej płytce w ramach implementacji. Build i symulacja hosta nie
zastępują tej weryfikacji. Sprawdź w szczególności reakcję <8 µs,
moment próbkowania mastera, start/długość presence i czas narastania
przy translatorze oraz ośmiu wpisach.

LP core C6 mógłby uniezależnić slave od HP/Wi-Fi, ale wymaga własnego
programu LP, współdzielonej konfiguracji i potwierdzenia mapowania
LP GPIO3 na tej płytce. RMT RX dostarcza szerokości po zakończeniu
impulsu – za późno dla zera przed zwolnieniem mastera. Sprzętowa
kombinacja ETM/timera z zatrzaskiem zbocza może dać lepszą latencję,
ale sama nie realizuje arbitrażu i dynamicznego wyboru bitu.
Nie twierdzimy, że te alternatywy zostały wdrożone lub sprawdzone.

## Opcjonalny prawdziwy DS18B20

Włącz `CONFIG_OW_REAL_SENSOR` w menuconfig. Na oddzielnej magistrali:
DS18B20 VDD do **3,3 V**, GND wspólne, DATA do GPIO6 oraz **zewnętrzny
pull-up około 4,7 kΩ do 3,3 V**. Nie łącz GPIO6 z DATA STW-100.
Obsługiwany jest jeden prawdziwy DS18B20 z zasilaniem zewnętrznym;
nie używaj trybu pasożytniczego ani wielu sond na tym wejściu.

Alternatywnie z katalogu projektu zbuduj osobny wariant bez menu:

```bash
idf.py -B build/real -D SDKCONFIG=sdkconfig.real \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.real.defaults' build
```

Osobne zadanie wykonuje Skip ROM + konwersję, czeka 750 ms bez aktywnego
pollingu, czyta 9 bajtów i sprawdza CRC, mniej więcej raz na sekundę.
Nie wyłącza przerwań ani nie blokuje slave. Preempcja przez slave może
wydłużyć slot mastera; taki pomiar jest odrzucany, błąd logowany i
zwiększa `real_errors`. Odczyt jest ponawiany w następnym cyklu.
Wybierz źródło „Prawdziwy DS18B20” dla dowolnych wpisów.
Nowy pomiar przechodzi do temperatury docelowej przy następnym resecie,
a do scratchpada po kolejnej zakończonej wirtualnej konwersji.
Niepusty raw ma pierwszeństwo nad tym źródłem.

`real_valid` i `real_temp` w logu informują o poprawności ostatniej próby.
Po błędzie pozostaje ostatnia przyjęta temperatura wirtualnego czujnika;
przed pierwszym poprawnym pomiarem używa skonfigurowanej temperatury.
Master STW-100 nie otrzymuje dodatkowego kodu błędu: **nie traktuj trybu
przekazywania jako systemu pomiarowego ani zabezpieczenia termicznego**.
Bez opcji Kconfig żądanie `source:"real"` jest jawnie odrzucane przez API.

## Procedura diagnostyczna z STW-100

1. Sprawdź napięcie, podłącz DATA/GND i analizator logiczny, uruchom ESP,
   sprawdź AP i domyślne ROM-y, dopiero potem włącz STW-100.
2. Oczekuj dwóch wyszukiwań F0, następnie 55 + ROM + BE. W logu odczyt
   STW-100 powinien mieć `bytes=8 bits=64` (master nie czyta CRC).
   Pierwsze dane powinny wynosić `50 05 4B 46 7F FF 0C 10`.
3. Obserwuj 44 i późniejsze BE. Domyślna konwersja 750 ms jest dłuższa
   niż zmierzone 709 ms przerwy STW; dla natychmiastowej zmiany pierwszego
   pomiaru ustaw np. 700 ms lub wyłącz startowe 85 °C. To celowa,
   konfigurowalna różnica, nie stały timeout w sterowniku.
4. Zapisz jeden wpis z ROM `0000000000000000`, drugie urządzenie wyłącz.
   Porównaj zachowanie STW oraz `selected` i `unmatched_roms`. Przy
   obecnych tylko niezerowych ROM-ach Match z zerami powinien dać FF,
   nie temperaturę żadnego przypadkowego czujnika.
5. Sprawdź 7-bajtowe auto-CRC, 8-bajtowy błędny CRC, konfigurację 9 bitów,
   raw, B4 i EC (alarm po konwersji). Reset w połowie BE musi natychmiast
   przywrócić obsługę komend ROM.
6. Zapisz `/log.txt`, powtórz próbę przy otwartej stronie/pobieraniu
   HTTP i 8 wpisach. Sprawdź oscyloskopem reakcję zera przed zwolnieniem
   mastera oraz liczniki `late_reactions`, `overruns`, `stuck_low`.
   Restart ESP powinien zachować ustawienia, ale odtworzyć stan startowy.

## Weryfikacja implementacji

Testy hosta obejmują wielokrotne Search i Alarm Search, 8 dowolnych ROM-ów
z jednym wyłączonym i adresem zerowym, Read ROM z AND, Match/Skip + BE,
znany wektor CRC startowego scratchpada, konwersję/status, reset w połowie
bajtu i po 8 bajtach, brak odpowiedzi na niewłaściwy ROM, Copy/Recall,
Write Scratchpad, zasilanie, temperaturę ujemną w 9 bitach, raw i pierścień
200 wpisów. Symulowany master zapisuje 1 przez 17 µs i 0 przez 78/117 µs,
czyta slotami low 12 µs, resetuje low 490 µs. To test protokołu, nie ISR.
