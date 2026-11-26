# Konsola LAN GrainMeter

Narzędzie podtrzymuje wybudzenie urządzenia z sieci lokalnej: po odebraniu
ogłoszenia natychmiast łączy się z GrainMeter. Urządzenie nie zasypia, dopóki
sesja trwa. Komendy działają tak jak w konsoli USB. Zakończenie przez `exit`,
EOF lub Ctrl+C pozwala urządzeniu zasnąć. Sam komputer nie budzi śpiącego
urządzenia — czeka na jego ogłoszenie i okno połączenia.

Wymagania: Python 3.8 lub nowszy (tylko biblioteka standardowa), Linux,
macOS lub Windows, ta sama sieć lokalna. Firewall musi zezwalać na broadcast
UDP 5515 i połączenia TCP 5516 (lub port podany w hello). Cele Makefile
uruchamiają narzędzia bez Dockera, ponieważ broadcast UDP wymaga sieci hosta.

**Hasło jest przesyłane otwartym tekstem w sieci lokalnej.**

## Użycie

```sh
python3 tools/lan_console.py --list
python3 tools/lan_console.py --list 20 --bind-ip 192.168.1.10
python3 tools/lan_console.py --device GrainMeter-0079
python3 tools/lan_console.py --device 'Moje urządzenie' --no-logs
python3 tools/lan_console.py --cmd status --cmd diag --timeout 30
python3 tools/lan_console.py --config config/device.conf --once
make lan-console LAN_ARGS='--device GrainMeter-0079'
make lan-list
make tools-test
```

Hasło jest pobierane kolejno z `--password`, zmiennej środowiskowej
`GM_SVC_PASS`, wpisu `svc_pass=` w `config/device.conf` albo przez `getpass`.
Domyślna ścieżka konfiguracji odnosi się do katalogu repozytorium, niezależnie
od bieżącego katalogu; `--config` pozwala wskazać inny plik. Pytanie o hasło
następuje przed nasłuchem, aby nie stracić krótkiego okna połączenia.
`--list` nie wymaga hasła. Unikaj podawania hasła w argumentach, jeśli nie
chcesz go w historii powłoki ani na liście procesów.

`--device` dopasowuje dokładny SSID lub nazwę bez rozróżniania wielkości liter.
Bez filtra klient wybiera pierwsze urządzenie, z którym się połączy, i przy
kolejnych połączeniach pozostaje przy jego SSID. Ogłoszenia powtórzone przez
różne interfejsy są deduplikowane. Lista pokazuje każdy SSID/adres/port raz.

Domyślnie klient ponawia połączenie po utracie sesji i po restarcie.
`--once` kończy pracę po jednej sesji. `--cmd` można powtarzać: klient czeka
na prompt po każdej komendzie (maksymalnie 10 s), następnie wysyła `exit`.
Ten tryb wykonuje jedną sesję. Limit oczekiwania na hello wynosi w nim
3600 s, a interaktywnie jest nieograniczony; zmienia go `--timeout`.
Kody zakończenia: 0 — zakończenie sesji, EOF lub Ctrl+C; 1 — błąd lokalny
lub połączenia w trybie `--once`; 2 — błąd uwierzytelniania; 3 — brak hello
przed upływem limitu. Statusy i BYE trafiają na stderr z czasem HH:MM:SS.
Wyjście komend, logi i prompt trafiają na stdout, a `#pong` jest ukrywany.

## Symulator i testy

W dwóch terminalach (klienta można uruchomić jako pierwszego):

```sh
python3 tools/lan_console.py --password test --cmd status --hello-port 15515
python3 tools/fake_device.py --password test --hello-addr 127.0.0.1 --hello-port 15515 --tcp-port 15516
```

```sh
python3 tools/fake_device.py --hello-addr 127.0.0.1 --once --window-ms 500
python3 tools/fake_device.py --hello-addr 127.0.0.1 --hello-every 3 --idle-s 60
python3 -m unittest discover -s tools -p 'test_*.py' -v
python3 tools/lan_console.py --help
```

Symulator obsługuje `help`, `status` (JSON), `echo TEKST`, `restart`, `exit`,
`logs on/off`, heartbeat i okresowe logi. `restart` rozłącza i po 2 s wznawia
ogłoszenia. `--once` wysyła jedno hello i udostępnia jedno okno połączenia.
`--hello-addr` domyślnie wskazuje broadcast; reklamowany adres IP domyślnie
wynosi `127.0.0.1`, więc do testu z innego komputera należy podać `--ip`
z adresem LAN hosta. Dodatkowo dostępne są `--name`, `--bind-ip` i
`--log-every`. Testy używają wyłącznie loopback i wybierają wolne porty.
Test heartbeat trwa około 12 sekund.

## Protokół v1

### Ogłoszenie (UDP)
Urządzenie rozsyła broadcast na UDP port 5515 (255.255.255.255) jedną linię ASCII zakończoną "\n":
  GM-HELLO v1 ssid=<ap_ssid> name=<nazwa> ip=<a.b.c.d> port=<tcp_port> fw=<wersja> window_ms=<ms>
- pola rozdzielone spacją, wartości URL-encoded (percent-encoding, spacja = %20), kolejność pól dowolna, nieznane pola ignorować; ssid np. "GrainMeter-0079" identyfikuje urządzenie.
- window_ms: jak długo serwer TCP czeka na połączenie po tym hello (np. 500 w normalnym cyklu na baterii, 5000 w trybie serwisowym). Hello może przychodzić wielokrotnie (np. co 3–5 s).
- Skrypt musi połączyć się NATYCHMIAST po odebraniu hello (okno bywa 500 ms): nie rób DNS, nie pytaj o hasło w tym momencie – hasło ma być znane wcześniej.

### Sesja (TCP, port z hello, domyślnie 5516), tekst liniowy
Urządzenie kończy linie "\r\n", klient wysyła "\n" (urządzenie akceptuje też "\r\n").
1. Urządzenie: `GM-CONSOLE v1 ssid=<ap_ssid> fw=<wersja>` potem `AUTH?`
2. Klient: `auth <hasło>`
3. Urządzenie: `AUTH OK` albo `AUTH FAIL` (po 3 błędach rozłącza z `BYE auth`).
4. Po AUTH OK: klient wysyła linie komend (te same co konsola USB: help, status, diag, scan, measure, set k v, config, upload, restart, ...). Urządzenie odsyła wyjście komend oraz na bieżąco linie logów (np. "I (1234) app: ...") – wszystko trafia na stdout skryptu tak jak przyszło. Po każdej komendzie urządzenie wysyła prompt "> " (bez nowej linii) – wypisz go jak jest.
5. Heartbeat: klient wysyła `#ping` co 10 s; urządzenie odpowiada `#pong`. Linii `#pong` NIE wypisuj. Urządzenie zamyka sesję po 60 s bez żadnej odebranej linii.
6. `logs off` / `logs on` – klient może wyłączyć strumień logów (urządzenie odpowiada zwykłym tekstem).
7. `exit` – urządzenie wysyła `BYE exit` i zamyka; urządzenie potem zasypia.
8. Przed zamknięciem z innych powodów urządzenie wysyła `BYE <powód>` (idle, timeout, battery, restart, auth). Linię BYE wypisz na stderr jako informację.
9. Po restarcie urządzenia (komenda restart, aktualizacja firmware) urządzenie przez ok. 120 s powtarza hello co 3 s – skrypt ma się sam ponownie połączyć i zalogować (tryb domyślny), chyba że ostatni BYE to `exit` albo użyto --once.
