# Podłączenie GrainMeter (FireBeetle 2 ESP32-C6)

![Schemat podłączenia](wiring.svg)

Numeracja pinów złącza **P1** pochodzi ze schematu DFRobot DFR1075 (V1.0 i
V1.1). Przed lutowaniem porównaj ją z opisem na płytce.

## Połączenia

| Od | Do (FireBeetle) | Uwagi |
| --- | --- | --- |
| Sonda pin 3 (czarny) – VCC | P1-2 **3V3** | pierwsza próba z 3,3 V |
| Sonda pin 1 (niebieski) – GND | P1-3 **GND** | |
| Sonda pin 2 (czerwony) – DATA | P1-8 **IO3/A2** (GPIO3) | opcjonalnie przez 100 Ω |
| Rezystor 4,7 kΩ | między **DATA (GPIO3)** a **3V3** | pull-up 1-Wire, wymagany |
| Sonda pin 4 (zielony) | – | niepodłączony (załączanie STW-100) |
| Przycisk serwisowy (NO) | P1-9 **IO2/A1** (GPIO2) ↔ **GND** | budzi z deep sleep bez resetu |
| Rezystor 10 kΩ | między **GPIO2** a **3V3** | pull-up przycisku |
| Panel słoneczny 5 V: + / − | P1-1 **VCC** / **GND** | wejście ładowarki CN3165 (5 V), nie 3V3 |
| Akumulator Li-ion 1S 3,7 V | złącze **BAT** | sprawdź polaryzację wtyku |

Już na płytce (nic nie trzeba dolutowywać):

| Funkcja | GPIO | Element |
| --- | --- | --- |
| Pomiar akumulatora | GPIO0 (ADC1_CH0) | dzielnik R15/R16 1 MΩ/1 MΩ + 100 nF |
| Przycisk BOOT (zapasowe wejście w serwis) | GPIO9 | S1 |
| LED statusu | GPIO15 | D1 |
| USB (flash, konsola) | GPIO12/13 | USB-C |
| Ładowarka Li-ion (solar, MPPT) | – | CN3165, max 0,5 A |

Wersja tekstowa:

```text
  Gniazdo CA 3 GD                         FireBeetle 2 ESP32-C6, złącze P1
  3 czarny  VCC  ───────────────────────── 2  3V3 ──┬───────────┬──────
  1 niebieski GND ───────────────────────── 3  GND   │           │
  2 czerwony DATA ──[100 Ω opc.]──┬──────── 8  IO3   │         [10 kΩ]
                                  └─[4,7 kΩ]─────────┘           │
  4 zielony  ─ NC                            9  IO2 ──────────────┴──o/ o── GND
                                                                  przycisk
  Panel 5 V  + ──────────────────────────── 1  VCC (5 V wejście)
             − ──────────────────────────── GND
  Akumulator Li-ion 3,7 V ───────────────── złącze BAT
```

## Pierwsze uruchomienie z sondą

1. **Odłącz oryginalny czytnik STW-100.** Na jego linii DATA zmierzono ok.
   4,95 V, a GPIO ESP32-C6 tolerują tylko 3,3 V.
2. Bez zasilania sprawdź miernikiem, czy między DATA, VCC i GND nie ma zwarć.
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
| Akumulator Li-ion 1S (18650 z zabezpieczeniem lub Li-Po) | 1 | z wtykiem pasującym do złącza BAT |
| Panel słoneczny 5–6 V, 1–3 W | 1 | wyjście 5 V na VCC |
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
- **Panel na pinie VCC.** Według DFRobot pin VCC (VIN) przyjmuje 5 V z
  zasilacza lub panelu. Przy panelu > 6 V w jałowym biegu potrzebny jest
  regulator.
- **Pobór prądu w deep sleep całego układu.** Pull-up przycisku nie pobiera
  prądu, dopóki przycisk nie jest wciśnięty.
