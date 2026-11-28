# STW-100 i czujniki DS18B20 – wyniki dochodzenia

Dochodzenie z 2026-11-27/28: dlaczego STW-100 wykrywał nowe czujniki DS18B20, ale
pokazywał na nich 0 °C. Ramki 1-Wire nagrane analizatorem logicznym (Saleae, sigrok),
a protokół sprawdzony emulatorem czujników na ML-12, który udawał 1–32 czujniki
o dowolnych adresach, temperaturach i bajtach scratchpada.

## Najważniejsze: numer punktu siedzi w czujniku

**STW-100 przypisuje czujnik do punktu Pn na podstawie bajtów TH i TL scratchpada:
TH = TL = n.** Sprawdzone dla n = 1…23.

- Nowe DS18B20 mają fabrycznie TH = 0x4B, TL = 0x46. STW-100 je wykrywa (liczy w
  „sonda posiada N czujników”), ale nie przypisuje do żadnego punktu.
- Nieprzypisany punkt STW-100 odpytuje pod adresem `00 00 00 00 00 00 00 00`. Nikt nie
  odpowiada, przychodzi `FF…`, a punkt pokazuje **0 °C**.
- STW-100 nigdy nie zapisuje TH/TL (w ramkach nie ma `4E` ani `48`). W oryginalnej
  sondzie numery są zaprogramowane fabrycznie. Czujnik zastępczy trzeba zaprogramować
  samemu (niżej).
- Wartość temperatury przy pierwszym odczycie (np. 85 °C po włączeniu) nie ma wpływu na
  przypisanie. Liczą się tylko TH/TL.

GrainMeter korzysta z tej samej konwencji: czujnik z TH = TL = n (1…8) przypisuje
automatycznie do punktu n (`docs/plans/thtl-automap.md`).

## Jak zaprogramować czujnik dla STW-100

Wystarczy dowolny master 1-Wire. Programuj **jeden czujnik naraz** (sam na magistrali),
wtedy można użyć Skip ROM (`CC`) zamiast adresu. Przykład dla punktu 3:

| Krok | Bajty | Uwagi |
|---|---|---|
| 1. Zapis do scratchpada | reset, `CC 4E 03 03 7F` | TH = 3, TL = 3, konfiguracja 0x7F (12 bit) |
| 2. Sprawdzenie | reset, `CC BE`, odczyt 9 bajtów | bajty 2 i 3 = `03 03`, CRC poprawne |
| 3. Utrwalenie w EEPROM | reset, `CC 48`, potem ≥ 10 ms linii w stanie wysokim | przy zasilaniu pasożytniczym silny pull-up na te 10 ms |
| 4. Kontrola | odłącz zasilanie albo reset, `CC B8` (Recall), `CC BE` | bajty 2 i 3 nadal `03 03` |

Z kilkoma czujnikami na raz: zamiast `CC` użyj `55` + 8 bajtów adresu ROM danego czujnika.
Numery w sondzie muszą być unikalne. Kolejność punktów wynika z numerów, nie z adresów
ani kolejności wykrycia.

Arduino (biblioteka OneWire), jeden czujnik na magistrali:

```cpp
ow.reset(); ow.skip(); ow.write(0x4E); ow.write(n); ow.write(n); ow.write(0x7F);
ow.reset(); ow.skip(); ow.write(0x48, 1);   // 1 = zostaw linię zasiloną
delay(20);
```

## Zachowanie STW-100 na magistrali

- **Start:** dwa przebiegi Search ROM (każdy znajduje wszystkie czujniki), potem dla
  każdego czujnika `55 <ROM> BE` i odczyt 8 bajtów (bez CRC). Na tym odczycie przypisuje
  punkty.
- **Cykl:** dla kolejnych punktów `55 <ROM> 44` (Convert T), po około 709 ms
  `55 <ROM> BE` i 8 bajtów. Jeden punkt mniej więcej co 4,8 s (pierwsze trzy szybciej).
- **Timing:** reset około 490 µs. Zapis „1” trwa **15–19 µs**, czyli poza specyfikacją
  (master powinien zwolnić linię przed 15 µs). Prawdziwe DS18B20 to tolerują, ale dekoder
  sigrok `onewire_link` próbkuje w 15 µs i czyta takie jedynki jako zera. Do dekodowania
  używaj progu szerokości impulsu: < 24 µs → 1, 24…300 µs → 0, > 400 µs → reset.
  Zapis „0” trwa 75–80 µs, slot odczytu 10–15 µs.
- **Czas konwersji:** odczyt około 709 ms po `44`, czyli mniej niż 750 ms z dokumentacji
  dla 12 bitów. Oryginalne czujniki się mieszczą (typowo około 600 ms).
- **Brak sprawdzania CRC:** czyta 8 bajtów, dziewiątego (CRC) nie. Brak odpowiedzi
  przypisanego czujnika (same `FF`) pokazuje jako **−0,0625 °C**, zamiast alarmu.
- **Limit: 23 punkty.** Przy 24 czujnikach jeden ginie, a P23 powiela wartość z P22. Przy
  32 STW-100 przepełnia własną tablicę adresów i odpytuje adresy złożone ze śmieci
  (kawałki ROM przesunięte o 2 bajty), a wiele punktów pokazuje to samo.
- Po zmianach konfiguracji emulatora w trakcie pracy zdarzało się, że pojedyncze punkty
  pokazywały wartość innego czujnika aż do restartu STW-100. Nie badane dalej.

## Podłączanie przyrządów do linii STW-100

- Linia DATA jest podciągnięta w STW-100 do 5 V. Wejścia 3,3 V (ESP32) tylko przez diodę
  (anoda przy DATA, katoda przy GPIO, pull-up po stronie ESP) albo konwerter z otwartym
  drenem (BSS138).
- Wejścia z rezystorem szeregowym, filtrem RC albo transoptorem (wejścia dwustanowe i NTC
  sterowników, wejścia KC868-A4) zniekształcają albo zjadają impulsy 10–15 µs. Do 1-Wire
  trzeba wpiąć się bezpośrednio w pin mikrokontrolera.
- Saleae (klon FX2, `fx2lafw`) można podłączyć bezpośrednio do linii 5 V. 2 MHz wystarcza.

## Narzędzia z dochodzenia

- Sniffer i emulator na ESP32-C6 (FireBeetle) z Wi-Fi i captive portalem: gałęzie
  `copilot/ow-sniffer` i `copilot/ow-emulator` (worktree, niezmergowane).
- Build diagnostyczny ML-12 (listwaTFF, gałąź `feature/onewire-diag`, niezacommitowany):
  komendy CLI `ow` (sniffer na TIM1_CH3, PE13, JP2 pin 11) i `owe` (emulator do 32
  czujników: `preset`, `rom`, `temp`, `thtl`, `raw`, `matchany`, `timing`, `log`).
