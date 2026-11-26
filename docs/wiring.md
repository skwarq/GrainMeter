# Podłączenie GrainMeter (FireBeetle 2 ESP32-C6)

Analiza płytki, akumulatora i zasilania: [hardware.md](hardware.md).

![Schemat podłączenia](wiring.svg)

Nazwy pinów jak w **nadruku na płytce** (pinout DFRobot:
[datasheets/DFR1075-pinout.png](datasheets/DFR1075-pinout.png)). Na wierzchu
(strona z elementami) jest krótka nazwa („VIN”, „3”, „2”), na spodzie pełna
(„3/A2”, „2/A1”). „Poz.” = pozycja na listwie liczona od złącza baterii
(na schemacie DFRobot to złącze P1, pin 1–14).

### Listwa przy złączu baterii (używana przez GrainMeter)

| Poz. | Nadruk wierzch | Nadruk spód | GPIO | GrainMeter |
| --- | --- | --- | --- | --- |
| 1 | **VIN** | VIN | – | **+5 V panelu** (wejście 5 V / panel) |
| 2 | **3V3** | 3V3 | – | **VCC sondy**, pull-upy 4,7 kΩ i 10 kΩ |
| 3 | **GND** | GND | – | **GND sondy, − panelu**, przycisk |
| 4 | SCL | SCL | 20 | – |
| 5 | SDA | SDA | 19 | – |
| 6 | 5 | 5/A4/LP_TX | 5 | – (strapping/JTAG, unikać) |
| 7 | 4 | 4/A3/LP_RX | 4 | – (strapping/JTAG, unikać) |
| 8 | **3** | **3/A2** | **3** | **DATA sondy (1-Wire)** |
| 9 | **2** | **2/A1** | **2** | **przycisk serwisowy** (budzi z deep sleep) |
| 10 | NC | NC | – | – |
| 11 | 15 | 15/D13 | 15 | (LED na płytce) |
| 12 | 6 | 6/D12/LP_SDA | 6 | wolny (np. wykrywanie ładowania – CHRG#) |
| 13 | 7 | 7/D11/LP_SCL | 7 | wolny (np. odcinanie panelu przy mrozie) |
| 14 | NC | NC | – | – |

**Uwaga:** GND jest **trzeci** – drugi to 3V3. Panel: + na **VIN**, − na
**GND**; nigdy na 3V3.

### Druga listwa (od strony przycisku RST)

RST, 3V3, NC, GND, NC, 23/SCK, 22/MO, 21/MI, 20/SCL, 19/SDA, 9/D9, 18/D7,
1/D6, NC, 14/D3, 8/D2, 16/TX, 17/RX (spód). Są na niej dodatkowe **3V3**
i **GND** – można z nich korzystać zamiast pinów 2–3 pierwszej listwy.
GrainMeter nie używa jej sygnałów (GPIO8 i GPIO9 to piny strapping, 16/17
to UART0).

Złącze akumulatora („Li-ion”) – biegunowość nadrukowana na spodzie (+ / −);
sprawdź przed podłączeniem.

## Połączenia

| Od | Do (nadruk na płytce) | Uwagi |
| --- | --- | --- |
| Sonda pin 3 (czarny) – VCC | **3V3** (poz. 2) | pierwsza próba z 3,3 V |
| Sonda pin 1 (niebieski) – GND | **GND** (poz. 3) | |
| Sonda pin 2 (czerwony) – DATA | **3** (spód: 3/A2) = GPIO3 (poz. 8) | opcjonalnie przez 100 Ω |
| Rezystor 4,7 kΩ | między **3** (DATA) a **3V3** | pull-up 1-Wire, wymagany |
| Sonda pin 4 (zielony) | – | niepodłączony (załączanie STW-100) |
| Przycisk serwisowy (NO) | **2** (spód: 2/A1) = GPIO2 (poz. 9) ↔ **GND** | budzi z deep sleep bez resetu |
| Rezystor 10 kΩ | między **2** a **3V3** | pull-up przycisku (zalecany) |
| Panel słoneczny 5 V (DFRobot FIT0601, wyjście USB-A ze stabilizatorem): + / − | **VIN** (poz. 1) / **GND** (poz. 3), albo USB-C płytki | wejście ładowarki CN3165 (5 V), nie 3V3; szczegóły: [hardware.md](hardware.md#panel-słoneczny-dfrobot-fit0601-monocrystalline-solar-panel-5v-1a) |
| Akumulator LiPo 1S 3,7 V (AKYGA LP475085) | złącze **Li-ion** (BAT) | sprawdź polaryzację wtyku |

Już na płytce (nic nie trzeba dolutowywać):

| Funkcja | GPIO | Element / nadruk |
| --- | --- | --- |
| Pomiar akumulatora | GPIO0 (ADC1_CH0) | dzielnik R15/R16 1 MΩ/1 MΩ + 100 nF (bez wyprowadzenia) |
| Przycisk BOOT (zapasowe wejście w serwis) | GPIO9 | przycisk **9/BOOT** |
| Przycisk resetu | – | przycisk **RST** |
| LED statusu (zielona) | GPIO15 | dioda przy nadruku **15/D13** |
| Dioda ładowania (czerwona) | – | sterowana przez CN3165 |
| USB (flash, konsola) | GPIO12/13 | USB-C |
| Ładowarka Li-ion (solar) | – | CN3165, ok. 0,5 A |

Wersja tekstowa:

```text
  Gniazdo CA 3 GD                    FireBeetle 2 ESP32-C6 (nadruk), poz. od złącza baterii
  3 czarny  VCC  ──────────────────── 2  3V3 ──┬───────────┬──────
  1 niebieski GND ──────────────────── 3  GND   │           │
  2 czerwony DATA ──[100 Ω opc.]──┬─── 8  3     │         [10 kΩ]
                                  └─[4,7 kΩ]────┘           │
  4 zielony  ─ NC                       9  2  ───────────────┴──o/ o── GND
                                                            przycisk
  Panel 5 V (USB)  + ───────────────── 1  VIN
                   − ───────────────── 3  GND
  Akumulator LiPo 3,7 V ────────────── złącze Li-ion
```

## Pierwsze uruchomienie z sondą

1. **Odłącz oryginalny czytnik STW-100.** Na jego linii DATA zmierzono ok.
   4,95 V, a GPIO ESP32-C6 tolerują tylko 3,3 V.
2. Bez zasilania sprawdź miernikiem, czy między DATA (3), 3V3 i GND nie ma zwarć.
   Rezystor 4,7 kΩ powinien być widoczny między DATA a 3V3.
3. Podłącz USB-C, wgraj firmware (`make flash-release`) i otwórz konsolę
   (`make monitor`).
4. Przy starcie firmware sprawdza, czy linia DATA w spoczynku jest wysoka. Jeśli
   nie jest, w logu pojawi się „1-Wire DATA (GPIO3) is low while idle”: brak
   pull-upu albo zwarcie.
5. `scan`: lista czujników z pełnym ROM i kodem rodziny (0x28 = DS18B20,
   0x10 i 0x22 są wypisane, ale nie mierzone).
6. `measure`: odczyt wszystkich czujników. Kilka razy pod rząd, żeby ocenić
   stabilność. Błędy `crc_error` na długim kablu to sygnał do zmiany pull-upu.
7. `automap` albo `map <ROM> <punkt> <cm>` – przypisanie punktów i wysokości.

## Lista części

| Element | Ilość | Uwagi |
| --- | --- | --- |
| DFRobot FireBeetle 2 ESP32-C6 (DFR1075) | 1 | |
| Gniazdo panelowe Hirschmann CA 3 GD | 1 | pasuje do wtyku sondy STW-100 |
| Rezystor 4,7 kΩ 0,25 W | 1 | pull-up DATA |
| Rezystor 10 kΩ 0,25 W | 1 | pull-up przycisku |
| Przycisk chwilowy NO, wodoszczelny (np. IP67, M12/M16) | 1 | przycisk serwisowy na obudowie |
| Akumulator AKYGA LP475085 LiPo 3,7 V 2500 mAh (PCM) | 1 | wtyk pasujący do złącza Li-ion płytki (sprawdź typ i biegunowość) |
| Panel słoneczny DFRobot FIT0601 / SYSP-M0606W (5 V 1 A, 6 W, USB-A ze stabilizatorem) | 1 | + na VIN, − na GND, albo USB-C |
| Obudowa IP65 + dławiki kablowe | 1 | |
| Opcjonalnie: rezystor 100 Ω, dioda TVS/ESD 3,3 V | 1+1 | ochrona DATA na długim kablu |

## Do sprawdzenia na sprzęcie

- **Stabilność 1-Wire przy 3,3 V na długim kablu sondy.** Jeśli odczyty
  zawodzą:
  - mniejszy pull-up (2,2 kΩ);
  - rezystor szeregowy 100 Ω i dioda TVS;
  - zasilanie sondy z 5 V z konwerterem poziomów na DATA.
- **Wartości opcjonalnych elementów** (100 Ω, TVS) są propozycją i nie
  zostały zweryfikowane.
- **Panel na pinie VIN.** Według DFRobot VIN przyjmuje 5 V z zasilacza lub
  panelu (nasz FIT0601 ma stabilizator 5 V). Panel bez stabilizatora
  (> 6 V bez obciążenia) wymaga przetwornicy 5 V.
- **Pobór prądu w deep sleep całego układu.** Pull-up przycisku nie pobiera
  prądu, dopóki przycisk nie jest wciśnięty.
