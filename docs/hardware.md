# Sprzęt GrainMeter – płytka, zasilanie, akumulator

Notatki z analizy schematów i not katalogowych. Podłączenie przewodów:
[wiring.md](wiring.md).

## Dokumenty źródłowe

| Dokument | Kopia lokalna | Źródło |
| --- | --- | --- |
| Pinout FireBeetle 2 ESP32-C6 (DFRobot, V1.0) | [datasheets/DFR1075-pinout.png](datasheets/DFR1075-pinout.png) | [DFRobot wiki](https://wiki.dfrobot.com/SKU_DFR1075_FireBeetle_2_Board_ESP32_C6) |
| Schemat FireBeetle 2 ESP32-C6 (DFR1075) V1.1 i V1.0 | [datasheets/](datasheets/) | [DFRobot wiki](https://wiki.dfrobot.com/SKU_DFR1075_FireBeetle_2_Board_ESP32_C6), [ZIP ze schematami](https://dfimg.dfrobot.com/wiki/20576/DFR1075_firebeetle-esp32-c6-microcontroller_schematics_v1.zip) |
| Ładowarka CN3165 (EN, CN) | [datasheets/](datasheets/) | [Consonance – CN3165](http://www.consonance-elec.com/79.html) |
| Akumulator AKYGA LP475085 3,7 V 2500 mAh | [datasheets/](datasheets/) | [AKYGA AKY0156](https://www.akyga.com/products/355-li-po-battery-lp475085-akyga-aky0156-3-7v-2500mah-pcm-jst-2-pin-connector-150mm.html) |

Kopie PDF są własnością producentów; aktualne wersje zawsze pod linkami.

## Płytka DFR1075 (schemat V1.1)

### Tor zasilania

```text
USB-C VBUS ──D2 (SS34)──┐
                        ├── VCC1 ── CN3165 (ładowarka liniowa 1-ogniwowa) ── VBAT ── akumulator (złącze BAT)
VIN (panel, 1. pin przy złączu baterii) ──Q1 (AO3407)──┘                                        │
                                                                              ├─ D4/Q2 ── TPS62A02 (buck) ── 3V3
                                                                              └─ R15/R16 1 MΩ/1 MΩ ── GPIO0 (ADC_BAT)
```

- **CN3165** – prąd ładowania z R9 = 2,2 kΩ: I = 1188 V / R9 ≈ **540 mA**.
- **TEMP (pin 1) zwarty do GND** – czujnik temperatury ogniwa wyłączony: płytka
  **ładuje w każdej temperaturze** (patrz „Ładowanie poniżej 0 °C”).
- **Brak wejścia włączającego** – ładowania nie da się wyłączyć programowo.
- `CHRG#` (pin 7) steruje tylko czerwoną diodą D3, `DONE#` (pin 6)
  niepodłączony – ESP nie wie, czy trwa ładowanie (możliwa przeróbka: przewód
  od katody D3 do pinu „6” (GPIO6) z pull-upem; stan niski = ładowanie).
- 3V3 (TPS62A02) zasila ESP **i sondę cały czas**, także w deep sleep.

### Diody

| Dioda | Kolor | Sterowanie |
| --- | --- | --- |
| D1 | zielona („G”) | GPIO15 → D1 → R5 2 kΩ → GND; firmware: miga w trybie serwisowym, w uśpieniu zgaszona |
| D3 | czerwona („R”) | `CHRG#` CN3165 z VCC1 przez R8 2 kΩ; świeci podczas ładowania, na samej baterii nie może świecić |

### Piny i wybudzanie

- Z deep sleep budzą tylko **LP GPIO0–7** (EXT1). BOOT (GPIO9) nie może
  wybudzić – przycisk serwisowy jest na **GPIO2** (nadruk „2”, spód „2/A1”). Pin musi być
  skonfigurowany jako LP IO przed uśpieniem (`rtc_gpio_init`), inaczej
  niepodłączony GPIO2 „pływa” i od razu budzi płytkę.
- Wolne: GPIO6 (nadruk „6”, spód „6/D12/LP_SDA”), GPIO7 („7”,
  „7/D11/LP_SCL”) – oba LP, mogą budzić (na schemacie sygnały wyświetlacza
  TCS/INT, wolne bez wyświetlacza).
- Kwarcu 32 kHz nie da się dodać: ESP32-C6 podłącza go do GPIO0/1, a GPIO0
  mierzy baterię. Zegar RTC chodzi z oscylatora RC i dryfuje – firmware
  koryguje go z nagłówka `Date` odpowiedzi ThingSpeak.

## Akumulator AKYGA LP475085 (karta producenta, wyd. 2024-10-22)

| Parametr | Wartość |
| --- | --- |
| Pojemność | 2500 mAh @ 0,2C, odcięcie 3,0 V |
| Napięcie ładowania / nominalne / odcięcia | 4,2 V / 3,7 V / 3,0 V |
| Maks. prąd ładowania / rozładowania | 1C (2,5 A) / 1C – płytka ładuje 540 mA (0,22C) |
| **Temperatura ładowania** | **0 … +45 °C** |
| Temperatura rozładowania / przechowywania | −20 … +60 °C / −20 … +45 °C |
| Samorozładowanie | do 10 % / 30 dni (wartość graniczna) |
| Przy −20 °C | > 45 % pojemności (0,1C do 3,0 V) |
| Żywotność | 500 cykli do 80 % pojemności |
| Zabezpieczenie | PCM (przeładowanie, nadmierne rozładowanie, zwarcie; progi nie podane) |
| Wymiary, masa, złącze | 85 × 50 × 4,7 mm, 44 g, JST 2,54 mm 2-pin |

### Procenty i progi (wartości domyślne od 0.4.0)

Karta nie podaje krzywej napięcie–pojemność; dla ogniwa LiPo (LiCoO₂) przy
małym obciążeniu typowo: 4,20 V = 100 %, 4,02 V ≈ 80 %, 3,87 V ≈ 60 %,
3,84 V ≈ 50 %, 3,80 V ≈ 40 %, 3,75 V ≈ 25 %, 3,69 V ≈ 10 %, 3,6 V ≈ 5 %,
3,0 V = 0 %.

| Próg | Wartość | Uzasadnienie |
| --- | --- | --- |
| `batt_low_mv` (tryb oszczędny) | **3650 mV** (~8 %) | ~2 miesiące zapasu w trybie oszczędnym |
| `batt_crit_mv` (bez Wi-Fi) | **3500 mV** (~4 %) | Wi-Fi pobiera chwilowo ~300 mA; na mrozie przy niskim napięciu groźba resetu (brownout) |

### Ładowanie poniżej 0 °C

Ładowanie zmrożonego ogniwa LiPo je uszkadza (osadzanie litu) i jest
ryzykiem bezpieczeństwa, a płytka ładuje z panelu w każdej temperaturze.

1. **NTC na wejściu TEMP CN3165** (zalecane, działa bez firmware): odciąć TEMP
   od masy i podłączyć dzielnik wg noty (progi 45 % i 80 % VIN). Dla NTC
   10 kΩ B3950 na ogniwie i zakresu 0–45 °C:
   ```text
   VIN ──[R1 4,7 kΩ]──┬── TEMP (pin 1)
                      ├──[R2 47 kΩ]── GND
                      └──[NTC 10 kΩ B3950 na ogniwie]── GND
   ```
   (wyliczone R1 ≈ 4,86 kΩ, R2 ≈ 46 kΩ; tolerancja progów ±2,5 % → odcięcie
   ok. 0 °C ± 2–3 °C). Wykonalność zależy od PCB (CN3165 w obudowie DFN-8).
2. **ESP odcina panel**: P-MOSFET (np. AO3401A) w przewodzie panelu,
   bramka do pinu „7” (GPIO7) z rezystorem 100 kΩ do GND (domyślnie panel podłączony),
   DS18B20 na ogniwie na tej samej magistrali 1-Wire; firmware odcina panel
   poniżej ~+2 °C i trzyma stan w deep sleep (wymaga dopisania w firmware).
3. Bez przeróbek: zimą ogniwo w miejscu, które nie zamarza, albo panel odłączony.

**Ostrzeżenie programowe (bez przeróbek, w firmware):** przy każdym pomiarze
firmware zgaduje ładowanie po wzroście napięcia (≥ 8 mV i ≥ 20 mV/h między
pomiarami) i czyta temperaturę układu ESP zaraz po wybudzeniu (bliską
temperaturze w obudowie). Ładowanie przy ≤ +2 °C (wyłączenie ostrzeżenia od
+4 °C) → wpis w błędach, `power.frost_charging` w statusie, ostrzeżenie w
portalu i powiadomienie push (najwyżej raz na dobę). Nie zatrzymuje ładowania.
Bez panelu (zima na samym akumulatorze) ostrzeżenie się nie pojawi.

## Panel słoneczny (DFRobot FIT0601 „Monocrystalline Solar Panel 5V 1A”)

Kupiony na Allegro (oferta 15600774466, tytuł zgodny z DFRobot FIT0601),
oznaczenie na panelu: **SYSP-M0606W** – monokrystaliczny panel 6 V / 6 W
(typowo Vmp 6 V, Imp ~1 A, Voc 7,2 V, Isc 1,1 A, np.
[Melopero SN-EP6W](https://melopero.com/wp-content/uploads/2024/08/6w-6v-mini-solar-panel.pdf)).
FIT0601 to ten panel z modułem stabilizatora 5 V i wyjściem USB-A.

Nasz egzemplarz **ma moduł USB-A z diodą** (stabilizowane 5 V) –
bezpieczny do podłączenia do płytki.

> **Inny egzemplarz:** jeśli z panelu wychodzą **gołe przewody**
> (bez modułu USB z diodą), panel daje 6 V pod obciążeniem i do 7,2 V bez
> obciążenia – **nie wolno** go podłączyć bezpośrednio do płytki; potrzebny
> stabilizator/przetwornica step-down 5 V.

Dane z modułem stabilizatora
([DFRobot FIT0601](https://www.dfrobot.com/product-1774.html)):

| Parametr | Wartość |
| --- | --- |
| Napięcie pracy (wyjście) | 5 V ze stabilizatorem (moduł na przewodzie) |
| Napięcie jałowe ogniw (Voc) | 7,2 V – przed stabilizatorem |
| Prąd pracy / zwarciowy | 1 A maks. / 1,1 A |
| Moc maks. | 6 W (ogniwa) |
| Budowa | monokrystaliczny, laminat PET, giętki, 275 × 170 × 2,5 mm, 90 g |
| Wyjście | gniazdo USB-A, przewód 1 m |
| Temperatura pracy | −30 … +70 °C |
| Szczelność | nie podana (laminat PET; moduł USB chronić przed wodą) |

**Dioda LED na panelu** to wskaźnik pracy modułu stabilizatora („working
indicator”): świeci, gdy panel daje napięcie wyjściowe (jest dość światła).
Zasilana z panelu, nie z akumulatora – nie zużywa baterii. Producent nie
opisuje kolorów ani progów.

**Napięcia i współpraca z płytką:**

- Wyjście jest stabilizowane na 5 V; w słabym świetle stabilizator nie
  utrzyma 5 V i napięcie spada razem z nasłonecznieniem (aż do zera).
- **Minimum do ładowania ok. 4,4 V** – zakres wejścia CN3165 to 4,4–6 V, a
  przy VIN niższym niż napięcie akumulatora + ~10 mV ładowarka przechodzi w
  uśpienie. CN3165 dopasowuje prąd ładowania do możliwości źródła, więc w
  słabym świetle ładuje mniejszym prądem zamiast „dusić” panel.
- **Podłączenie:**
  - przez USB-C płytki (kabel USB-A → USB-C): najprościej, ale przez diodę
    D2 (SS34, spadek ~0,3–0,45 V) – z 5 V zostaje ~4,6 V, w słabym świetle
    ładowanie skończy się wcześniej;
  - lepiej na **VIN** (1. pin przy złączu baterii) **i GND** (3. pin) (np. przez wtyk USB-A z wyprowadzonymi
    przewodami, bez cięcia kabla panelu): wejście przez tranzystor Q1 o
    małym spadku napięcia.
  - Płytka na USB-C z samym panelem **nie** wchodzi w tryb „na biurku” –
    wykrywany jest tylko komputer (host USB).
- **Nigdy nie podłączać gołych ogniw z pominięciem stabilizatora**: 7,2 V
  przekracza dopuszczalne wejście przetwornicy TPS62A02 (zasilanej z VCC1
  przez D4) i jest blisko granicy CN3165 (6,5 V).
- W nocy akumulator nie rozładowuje się do panelu: CN3165 w uśpieniu pobiera
  z akumulatora < 3 µA i nie zasila wejścia wstecz.
- Prąd ładowania ogranicza płytka: ~540 mA (R9 = 2,2 kΩ), poniżej 1 A panelu.

**Bilans:** zużycie GrainMeter to ~3 mAh/dobę; nawet w grudniu (w Polsce
~0,5–1 kWh/m² dziennie) panel 6 W przy dobrej orientacji daje rzędu
setek mAh na dobę – nadwyżka wielokrotna. **Zimą jednak ładuje także przy
mrozie** (TEMP CN3165 wyłączony, patrz wyżej): na zimę panel odłączyć albo
zastosować jedną z przeróbek; firmware ostrzega (`power.frost_charging`).

## Sonda 1-Wire – zasilanie

Sonda jest zasilana z 3V3 na stałe. 8 × DS18B20 w spoczynku: 0,75 µA typ.
(maks. 1 µA) każdy → 6–8 µA, ok. 0,15–0,2 mAh/dobę (~5 % budżetu) –
odłączanie zasilania się nie opłaca. Zasilanie sondy wprost z GPIO
niezalecane (do 12 mA przy konwersji, spadek napięcia, minimum DS18B20 3,0 V).
Do sprawdzenia: prąd sondy STW-100 w spoczynku (przewód „załączanie”
sugeruje dodatkową elektronikę) – pomiar µA w przewodzie VCC przy uśpionej
płytce.

## Budżet energii (pomiary na płytce, wersja 0.3.x)

| Faza | Czas |
| --- | --- |
| Start po wybudzeniu (ROM + bootloader + sprawdzenie obrazu) | ~300 ms |
| Wybudzenie pomiarowe (bez sondy; z sondą + ~750 ms konwersji) | ~94 ms |
| Okno Wi-Fi z SNTP (łączenie + DHCP 1,45–1,58 s, SNTP 0,34–1,30 s) | 2,7–2,85 s |

Szacunek dla pomiaru co 15 min i wysyłki co godzinę: ~3,5 mAh/dobę (z SNTP),
~2,8–3,0 mAh/dobę (korekta czasu z `Date`). Okna Wi-Fi ~50 %, uśpienie ~27 %
(szacunek – do zmierzenia), pomiary ~16 %, start ~5 %. Ogniwo LP475085:
realnie 12–15 miesięcy bez słońca (samorozładowanie 2–3 %/mies.), w
najgorszym przypadku z karty (10 %/mies.) ok. 7–8 miesięcy.
