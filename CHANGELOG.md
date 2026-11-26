# Zmiany

Co wnosi każda wersja oprogramowania GrainMeter. Urządzenie samo tylko
informuje o nowej wersji; instalacja jest zawsze decyzją użytkownika
(portal → Aktualizacja → Zainstaluj).

## 0.4.2

2026-11-26

- Więcej wolnej pamięci: kod sterownika Wi-Fi działa z flash zamiast z IRAM
  (+33 KB sterty; w trakcie TLS minimum 62 KB zamiast 29 KB, największy wolny
  blok ~2×). Obiekty tworzone przy pierwszym użyciu (przerwania MPI i AES,
  semafor lwIP) powstają przy starcie, nie w środku wolnej pamięci.
- Dashboard: bateria i sygnał Wi-Fi na dużym wykresie historii (przełącznik
  widoku Temperatury / Bateria / Sygnał Wi-Fi), ciągłe linie zamiast kropek,
  tabela „Zmiany” (temperatury, bateria, urządzenie).

## 0.4.1

2026-11-26

- Dashboard i strona zdalna mają wspólne ustawienia: zdalny dostęp (broker,
  ID urządzenia, TalkBack) wpisujesz raz – w Ustawieniach dashboardu albo w
  `remote.html`; hasła zapisują się tylko na życzenie.
- `remote.html`: przy uśpionej płytce link „Dane z ThingSpeak”.
- Link z `scripts/remote_link.py` wypełnia też ID kanałów w dashboardzie.
- Szybsze wybudzenie zdalne: po resecie jedno połączenie Wi-Fi zamiast dwóch,
  TalkBack sprawdzany od razu po synchronizacji zegara, a sprawdzanie
  aktualizacji odkładane, gdy czeka sesja – „Wybudź” + reset: ~17 s do
  połączenia zamiast ~30 s (zwykłe wybudzenie w oknie wysyłki: ok. 6 s szybciej).
- EMQX: reguły uprawnień przy loginach (Username), nie przy Client ID;
  opis zarządzania nimi przez API. Linki do dashboardu na górze README.

## 0.4.0

2026-11-26

**Zdalny dostęp przez internet (MQTT, EMQX Cloud)**

- Portal serwisowy przez MQTT: `remote.html` (także na GitHub Pages, link
  „Połączenie zdalne” w dashboardzie) – konfiguracja, status, diagnostyka,
  historia, logi i aktualizacja z dowolnego miejsca, bez otwierania portów.
- Protokół gm2: klucz z hasła serwisowego (PBKDF2), szyfrowana i podpisana
  treść żądań i odpowiedzi, podpisany status urządzenia, osobna numeracja dla
  każdej karty przeglądarki, blokada po błędnych podpisach.
- Wybudzanie przez ThingSpeak TalkBack („Wybudź”), dzienny budżet minut sesji
  (bezpiecznik baterii), przyciski „Przedłuż” i „Zakończ sesję”, limit
  bezczynności `remote_idle_min` – liczą się tylko działania użytkownika.
- Wgrywanie dowolnego pliku `.bin` przez MQTT (kawałki, SHA-256, wznawianie,
  anulowanie, raport wyniku po restarcie z wykrywaniem rollbacku).
- Historia z pamięci płytki przez MQTT (identyczna z lokalną).
- `scripts/remote_link.py`: link do strony zdalnej z wypełnionym formularzem.

**Logi**

- Podgląd logów w portalu (Diagnostyka → Logi), lokalnie i zdalnie.
- Wybór „Wysyłanie logów”: tylko podgląd w portalu (domyślnie), UDP albo
  MQTT, oraz poziom logów; `tools/mqtt_log.py` do odbioru na PC.

**Aktualizacje**

- Zaplanowane sprawdzanie manifestu **tylko informuje** o nowej wersji
  (portal; raz na wersję także ntfy/Telegram) – urządzenie nie instaluje
  się samo. Link „Co nowego” prowadzi do tego pliku.

**Bateria, czas, alarmy**

- Przewidywany czas pracy na baterii z mierzonego zużycia (mAh/dobę) w
  portalu i na dashboardzie; pojemność `batt_capacity_mah`.
- Alarmy temperatury i jej wzrostu, powiadomienia ntfy/Telegram, prognoza i
  statystyki baterii, czas z nagłówka HTTP Date.
- Zegar zachowany przez restart programowy i awarię.

**Inne**

- Konsola LAN na PC (`tools/lan_console.py`) i wybudzanie przez LAN,
  opóźniony SoftAP, okna heartbeat.
- Dynamiczne bufory TLS: sprawdzanie aktualizacji działa w trakcie sesji
  MQTT (więcej wolnej pamięci).
- Poprawki: stos konsoli LAN/USB, poprawny JSON diagnostyki, pusta kolejka
  TalkBack, jedno odpytywanie statusu w portalu, `provision.py` dla
  symulatora.

## 0.3.0

2026-11-25

- Wybór sieci Wi-Fi w portalu („Połącz i zapisz”), tryb na biurku (portal w
  sieci lokalnej, komputer na USB), captive portal w stylu WIFI_RS.
- Logi przez UDP, rozszerzona diagnostyka (sterta, zadania, ostatnia awaria).
- Dashboard bez danych w zakresie nie znika; poprawka pobierania manifestu z
  GitHuba, wydruk konsoli USB, wersja w buildzie.

## 0.2.2

2026-11-24

- Przyciski zdalnej aktualizacji w portalu (sprawdź, potem zainstaluj).
- Dashboard oznacza znaczniki czasu z przyszłości; poprawka wyjścia z
  symulatora.

## 0.2.1

2026-11-24

- Te same zmiany co 0.2.0 (tag v0.2.0 pozostał na wcześniejszym buildzie).

## 0.2.0

2026-11-24

- Lokalna historia pomiarów (tylko zmiany, stan baterii/Wi-Fi) i zakładka
  Historia w portalu; dashboard na GitHub Pages.
- Plik ustawień urządzenia i konfiguracja przez USB (`provision.py`).
- Nowa tabela partycji (partycja historii): wgraj przez USB.

## 0.1.0

2026-11-23

- Pierwsza wersja: pomiar temperatury silosu (DS18B20), wysyłka do
  ThingSpeak, portal serwisowy, symulator PC, podpisane wydania na GitHubie.
