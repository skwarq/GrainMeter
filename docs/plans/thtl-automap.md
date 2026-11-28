# Automatyczne przypisanie czujników po TH/TL (konwencja STW-100)

Stan: projekt do akceptacji, 2026-11-28.

## Po co

Sondy do sterownika STW-100 mają w każdym DS18B20 zapisany numer punktu w bajtach
TH i TL scratchpada (TH = TL = n → punkt Pn). Ustalone emulatorem na ML-12
(dochodzenie: `docs/stw100-onewire.md`). GrainMeter dziś przypisuje nowe czujniki
w kolejności adresów ROM, co nie ma związku z położeniem czujnika w sondzie, więc
każdą sondę trzeba mapować ręcznie. Cel: po podłączeniu takiej sondy GrainMeter sam
przypisuje czujniki do właściwych punktów.

## Cele

- Czujnik nieprzypisany, z TH = TL = n, n w 1..kMaxPoints (8), trafia do punktu n
  automatycznie przy pomiarze i zostaje w zapisanej mapie (NVS).
- Wymiana czujnika: nowy czujnik z numerem n przejmuje punkt n, jeśli czujnika,
  który go dotąd zajmował, nie ma na magistrali w tym skanie.
- `automap` (portal, CLI) najpierw przypisuje po TH/TL, potem resztę jak dziś
  (kolejność ROM, wolne punkty).
- Notatka z dochodzenia w `docs/stw100-onewire.md`: zachowanie STW-100 i jak
  zaprogramować TH/TL w czujniku.

## Poza zakresem

- Zapis TH/TL do czujników przez GrainMeter (tylko odczyt; zapis opisany w notatce).
- Punkty powyżej 8 (STW-100 obsługuje do 23): czujnik z n > 8 traktowany jak bez numeru.
- Zmiany UI portalu poza tym, że automatyczne przypisania widać jak każde inne.

## Czego nie wolno zepsuć

- Ręczne przypisania i wysokości czujników zostają; TH/TL nie odbiera punktu
  czujnikowi, który jest obecny na magistrali.
- Brak automatycznego przypisania przy wątpliwościach: dwa obecne czujniki z tym
  samym numerem → żaden nie dostaje punktu, ostrzeżenie w logu/błędach.
- Bez dodatkowej komunikacji 1-Wire w zwykłym pomiarze (scratchpad i tak jest czytany).

## Podejścia

- **A (wybrane): automatycznie przy pomiarze, trwała mapa.** Wykorzystuje istniejący
  `RomMap` i odczyt scratchpada w `measure()`.
- B: tylko przy `automap`. Mniej niespodzianek, ale nie automatyczne. Odrzucone.
- C: bez mapy, przypisanie liczone przy każdym pomiarze. Gubi wysokości i ręczne
  przypisania. Odrzucone.

## Projekt

- Numer z scratchpada: tylko przy poprawnym CRC (statusy Ok, PowerOnValue,
  OutOfRange) i TH == TL w 1..8. Fabryczne 0x4B/0x46 → brak numeru.
- Logika w `RomMap` (czysta, testowalna): przyjmuje listę (ROM, numer) dla
  nieprzypisanych czujników i listę ROM obecnych na magistrali; zwraca liczbę
  przypisań oraz konflikty. Reguły jak w „Cele" i „Czego nie wolno zepsuć".
- `measure()`: `readSensor` zwraca surowy scratchpad (parametr już istnieje); po
  odczytach przebieg TH/TL dla nieprzypisanych, zapis mapy (`romMapSave`) tylko gdy
  coś się zmieniło; przypisane czujniki dostają punkt już w tym pomiarze. Log:
  `map: ROM <hex> -> point <n> (TH/TL)`; konflikt przez `recordError`.
- `mapAuto()`: czyta scratchpad nieprzypisanych czujników (bez konwersji), potem
  ten sam przebieg TH/TL, potem dotychczasowe `autoAssign`.
- Symulator PC (`pc/sim/sim_probe`): ustawialne TH/TL czujnika, żeby to sprawdzić
  w teście integracyjnym i na żywo.
- Testy: jednostkowe dla reguł `RomMap` (przypisanie, wymiana, konflikt, n > 8,
  fabryczne bajty, obecny właściciel), integracyjny pomiar na symulatorze.

## Do decyzji wykonawcy

- Dokładna sygnatura funkcji w `RomMap` i nazwy.
- Gdzie w symulatorze ustawia się TH/TL (konfiguracja sondy).
