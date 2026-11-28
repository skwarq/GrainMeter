<!-- SPDX-License-Identifier: MIT -->
# Pasywny sniffer 1-Wire — FireBeetle 2 ESP32-C6

Samodzielny projekt **ESP-IDF v6.1.1**, target **esp32c6**. Nie używa
komponentów aplikacji GrainMeter. Nasłuchuje równolegle między STW-100
a oryginalną sondą; **nigdy nie steruje DATA**, nie wysyła resetów ani komend.
SoftAP i captive portal działają bez routera, Internetu oraz zewnętrznych CDN.

## Podłączenie

Najpierw **zmierz napięcie DATA w spoczynku względem GND STW-100**,
bez podłączania GPIO. ESP32-C6 **nie toleruje 5 V**; sam rezystor szeregowy
nie obniża napięcia w stanie wysokim i nie chroni przed 5 V.

- Zasil ESP z powerbanku USB, bez połączenia USB z uziemionym komputerem.
- Połącz GND sniffera z GND STW-100.
- Przy poziomie DATA nieprzekraczającym 3,6 V: DATA → **1 kΩ** → GPIO3.
  Sprawdź również poziom wysoki przy rzeczywistym obciążeniu; nominalnie
  powinien być bliski 3,3 V, nie granicy 3,6 V.
- Jeżeli DATA > 3,6 V, zastosuj przed GPIO dzielnik lub wejściowy
  tranzystor/translator dopasowany do zmierzonego napięcia. Dla około 5 V
  przykładowy dzielnik: DATA → **10 kΩ** → punkt pomiarowy → **20 kΩ** → GND,
  a punkt pomiarowy → **1 kΩ** → GPIO3. Translator nie może aktywnie
  wymuszać stanów na magistrali; jego wyjście do ESP powinno być nieodwracające.
  Nie korzystaj z dodatkowego pull-upu po stronie magistrali.

Dzielnik obciąża DATA rezystancją 30 kΩ (około 0,17 mA przy 5 V).
Wraz z istniejącym pull-upem obniża poziom magistrali i zmienia stałą czasową
zbocza. Przy słabym pull-upie, długim przewodzie albo większym napięciu
podany dzielnik może być niewłaściwy: sprawdź napięcia po podłączeniu,
kształt zboczy i poprawną pracę oryginalnej sondy; wtedy preferuj
wysokoimpedancyjny translator. Przewody sniffera powinny być krótkie.

Podłączaj się wyłącznie do dostępnej niskonapięciowej części sterownika,
przy bezpiecznie odłączonym zasilaniu STW-100. Nie pracuj przy odsłoniętych
zaciskach sieciowych. Wspólna masa z powerbankiem nie zapewnia separacji
galwanicznej.

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

## Uruchomienie

1. Zbuduj i wgraj projekt, włącz sniffer z powerbanku.
2. Po pomiarze napięcia podłącz GND oraz DATA zgodnie z opisem powyżej.
3. Zrestartuj STW-100, aby zarejestrować początkowe **Search ROM**.
4. Połącz telefon/laptop z **OW-Sniffer-XXXX**, gdzie XXXX to końcówka MAC.
   Pozostań w tej sieci mimo komunikatu „brak Internetu”.
5. Otwórz captive portal lub ręcznie **http://192.168.4.1/**.
   Pobierz `/frames.txt` oraz `/raw.csv`. GPIO15 miga przy aktywności DATA.

AP używa kanału 1. `idf.py menuconfig` → **OW Sniffer** pozwala zmienić
wejście DATA (domyślnie GPIO3) i hasło AP (domyślnie puste; niepuste
musi mieć 8–63 bajty, WPA2-PSK). Wybieraj wyłącznie wolne piny wyprowadzone
na płytce, nie piny USB/flash/strapping; GPIO15 jest zarezerwowane dla LED.
DATA ma wyłączone oba wewnętrzne rezystory i zawsze tryb **INPUT**.
LED jest jedynym wyjściem GPIO projektu.

## Akwizycja i ograniczenia

Wybrano przerwanie GPIO na obu zboczach, **priorytet sprzętowy 3**,
`ESP_INTR_FLAG_IRAM`, `esp_timer_get_time()` z rozdzielczością **1 µs**.
ISR i funkcje sterowania GPIO działają z IRAM, dane są w SRAM; ISR nie
alokuje pamięci, nie loguje i nie wywołuje sieci. Rzeczywiste szerokości
obejmują różnicę opóźnień obsługi dwóch przerwań. Sprzętowy **flex glitch
filter 3000 ns** usuwa krótsze szpilki przed przerwaniem, także szpilki
stanu wysokiego w impulsie low. Dekoder dodatkowo scala impulsy < 3 µs
z otaczającym stanem (fixture ma próbki co 0,5 µs).

ESP32-C6 w IDF 6.1.1 obsługuje RMT RX ping-pong / partial RX, **bez DMA**.
RMT 1 MHz ma ograniczony licznik stanu i próg zakończenia bezczynności;
`rmt_receive()` trzeba ponowić po zakończeniu, co tworzy okno bez nasłuchu.
Sam RMT nie odtworzy wielosekundowego idle. GPIO wybrano, aby nie restartować
akwizycji i zachować pełny czas długich stanów, kosztem obsługi każdego
odfiltrowanego zbocza w ISR. Priorytet 3 ogranicza wpływ przerwań Wi-Fi,
wyłączono power management i zegar CPU ma 160 MHz. Sekcje krytyczne
czytelników kopiują tylko pojedynczy rekord, nigdy cały bufor.

**Nie jest to sprzętowa gwarancja braku strat**: dowolnie długie maskowanie
przerwań może zgubić zbocza, nawet przy ISR w IRAM. `anomalies` liczy
przerwania bez zmiany poziomu (wykrywalne utracone pary); niewykrywalne
straty nie są w tym liczniku. Przed pomiarami wymagającymi pewności porównaj
przechwycenie z analizatorem logicznym przy równoczesnym, wielokrotnym
pobieraniu CSV/ramek przez Wi-Fi. Test hosta i build nie zastępują tego
testu na fizycznym ESP32-C6.

Pierścień mieści **20 000 statycznych rekordów** po 9 bajtów (180 kB).
Czas trwania jest `uint64_t` w µs, więc nie przepełnia się po 65 ms ani
po wielosekundowym idle. Rekordy powstają po zmianie poziomu; snapshot
eksportu dodaje też aktualny, nadal trwający stan. Status pokazuje liczbę
zakończonych rekordów, nadpisania i anomalie ISR.
Po zapełnieniu najstarsze rekordy są nadpisywane — to bufor ostatnich
20 000 stanów, **nie archiwum całej wielogodzinnej sesji**.

Dekodowanie odbywa się na żądanie z ograniczoną pamięcią: maksymalnie
256 bajtów na transakcję, 20 ostatnich linii dla UI, 2 kB bufora wysyłki.
Eksport nie zatrzymuje ISR. Jeśli zapis nadpisze jeszcze nieodczytany
rekord lub nagranie zostanie wyczyszczone, plik zawiera jawny komentarz
`# ERROR`; takie pobranie trzeba powtórzyć, nie traktować go jako kompletne.
Nie ma pełnej kopii pierścienia ani rosnącego dziennika w RAM/flash.

## Dekodowanie i HTTP

Low < 24 µs oznacza 1, low 24–300 µs oznacza 0, low > 400 µs reset.
Pierwszy low 50–300 µs zaczynający się < 70 µs po końcu resetu oznacza
presence. Impuls 300–400 µs jest nieprawidłowy (`invalid=Y`).
Bajty są LSB-first. `F0` dekoduje 64 trójki Search ROM i bierze
**kierunek mastera**, nie pierwszy bit trójki.

Transakcję zamyka **następny reset**. Ostatnia otwarta transakcja pozostaje
w CSV, a pojawi się w `/frames.txt` dopiero po kolejnym resecie.
To pozwala nie przedstawiać uciętej transakcji jako zakończonej.
Fixture kończy się w 3,0000 s otwartą transakcją `55 00 … 44`
od 2,9574 s; dziewięć wcześniejszych, zamkniętych transakcji odpowiada
wzorowi zlecenia. Niepełne bajty w zamkniętej ramce mają `partial=Nbits`,
a przekroczenie limitu 256 bajtów `truncated=Y`.

`55 + ROM + BE` oraz `CC + BE` z co najmniej dwoma bajtami danych
otrzymują temperaturę ze signed int16 LE: rodzina 0x10 / 2,
0x28 / 16. Dla Skip ROM (brak informacji o rodzinie) i zerowego ROM
zakładana jest interpretacja DS18B20. Inne rodziny nie otrzymują temperatury.
Dziewięć bajtów scratchpada daje `crc=ok|BAD` (CRC8 Maxim);
Search ROM otrzymuje CRC po dokładnie 192 bitach.
Wartość T jest surową interpretacją danych, **nie potwierdzonym pomiarem**:
85°C może być wartością startową, a same FF mogą oznaczać brak odpowiedzi.
Nie stosuje się korekcji count-remain DS18S20.

| Adres | Działanie |
| --- | --- |
| `/` | Status co 2 s, ostatnie 20 zamkniętych ramek, czyszczenie i pliki |
| `/status.json` | Uptime, czas nagrania, rekordy, nadpisania, anomalie, DATA |
| `/frames.txt` | Wszystkie zamknięte ramki z zachowanego pierścienia |
| `/frames.txt?recent=1` | Ostatnie 20 ramek dla UI |
| `/raw.csv` | Strumieniowy CSV `level,duration_us`, bez pełnego pliku w RAM |
| `POST /clear` | Atomowe wyzerowanie pierścienia, czasu i liczników |

Czas ramek liczy się od uruchomienia akwizycji / ostatniego `/clear`,
także po nadpisaniu najstarszych rekordów. CSV zawiera komentarz
`# recording_offset_us=...` oznaczający początek zachowanego fragmentu;
przy dekodowaniu takiego pliku poza ESP należy dodać ten offset.
Dekoder ignoruje fragment przed pierwszym zachowanym resetem.
DNS odpowiada IP AP na dowolną nazwę w zapytaniu A; dla AAAA zwraca pustą
odpowiedź, aby klient użył IPv4. Nieznane URL, m.in. Android `/generate_204`
i `/gen_204`, iOS `/hotspot-detect.html`, Windows `/ncsi.txt` i
`/connecttest.txt`, otrzymują przekierowanie 302 na portal.
HTTPS nie jest przechwytywany. Implementacje DNS i przekierowania są
lokalnymi kopiami/adaptacjami wzorców GrainMeter, bez linkowania do nich.

Otwarte AP umożliwia każdemu w zasięgu odczyt i wyczyszczenie nagrania.
Hasło AP ogranicza ten dostęp; HTTP nie ma osobnego uwierzytelniania.

## Build i test

Z katalogu głównego repozytorium, g++ / C++17:

```sh
cmake -S tools/ow_sniffer/test -B /tmp/ow_sniffer_test && cmake --build /tmp/ow_sniffer_test && ctest --test-dir /tmp/ow_sniffer_test --output-on-failure
```

Test porównuje dokładnie dziewięć zamkniętych ramek prawdziwego nagrania,
z tolerancją czasu ±0,1 ms i z rozszerzeniami T/CRC. Sprawdza też szpilki
obu polaryzacji, kierunki Search ROM, błędne CRC, temperatury ujemne
obu rodzin, długi idle, progi, niepełny bajt i lokalną odpowiedź DNS.

Wymagany build firmware:

```sh
docker run --rm -v "$PWD:/project" -w /project/tools/ow_sniffer -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e IDF_GIT_SAFE_DIR=/project espressif/idf:v6.1.1 idf.py set-target esp32c6 build
```

W środowisku z aktywnym ESP-IDF 6.1.1:

```sh
cd tools/ow_sniffer
idf.py set-target esp32c6
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Projekt używa domyślnej tablicy IDF **Single factory app, no OTA**
z partycją aplikacji 1 MiB i flash 4 MiB FireBeetle 2 C6. Pierścień jest
w zerowanej sekcji BSS wewnętrznej SRAM, nie w inicjalizowanym obrazie flash;
optymalizacja rozmiaru pozwala zmieścić firmware w domyślnej partycji.
`build/`, `sdkconfig` oraz
`sdkconfig.old` są lokalne i ignorowane; wersjonowany jest tylko
`sdkconfig.defaults`. Źródła C++ mają ostrzeżenia traktowane jako błędy
i nie używają wyjątków.
