# Plan: zdalny dostęp do portalu (MQTT + EMQX)

Stan na 2026-11-26. Plan po przeglądzie `/plan-eng-review` z drugą opinią
Codexa. Kontrakt techniczny: [../remote-protocol.md](../remote-protocol.md).

## Cel

Portal serwisowy (konfiguracja, status, diagnostyka, aktualizacja) dostępny
z dowolnego miejsca na świecie, bez otwierania portów w sieci przy silosie.
Wzorzec: WIFI_RS_ESP32S2, czyli żądania HTTP portalu tunelowane przez MQTT.
Logika portalu zostaje bez zmian (`Portal::handle`).

## Decyzje

| # | Decyzja | Wybór |
| --- | --- | --- |
| D1 | Zakres | pełny portal (odczyt i zapis), bez historii binarnej |
| D2 | Front zdalny | ta sama aplikacja Preact, wymienny transport w `api.js` |
| D3, D4 | Zakres prac | tunel + wybudzanie + front, bez własnego serwera |
| D5 | Wybudzanie z deep sleep | TalkBack ThingSpeak: komenda `REMOTE`, sprawdzana w oknie wysyłki |
| D6 | Broker | ~~HiveMQ Cloud Serverless~~ – unieważnione (koniec usługi 2026-12-31, ACL tylko `#`) |
| D9 | Broker | **EMQX Cloud Serverless** (mqtts 8883 płytka, wss 8084 przeglądarka, ACL per login) |
| D7 | Współbieżność | kolejka głębokości 1, nadmiar → `503 busy` |

Poza zakresem: historia binarna przez tunel (później), własny broker/VPS,
powiadomienia push z aplikacji, wgrywanie pliku `.bin` przez tunel
(aktualizacja zdalna tylko z manifestu GitHub: `/api/update/*`).

## Wymagania z drugiej opinii (Codex), wpisane w kontrakt

1. Hasło serwisowe nie przechodzi przez broker: podpis HMAC-SHA256,
   `nonce` sesji, rosnący `seq`, odrzucanie wiadomości retained.
2. Osobny licznik błędów dla tunelu (zdalne złe podpisy nie blokują LAN).
3. Wspólny moduł `session_hold` (dzierżawy LAN i zdalna), Wi-Fi trzymane,
   dopóki trwa którakolwiek.
4. Publikacja odpowiedzi po zwolnieniu `DeviceLock`; heartbeat poza blokadą.
5. Aktualizacja zdalna: odpowiedź QoS 1 przed restartem, wznowienie sesji,
   wynik w `status`.
6. Energia: TalkBack z limitem 3 s, komenda z terminem ważności, dobowy
   budżet minut sesji, ochrona baterii.
7. Bufor MQTT ≥ 24 KiB, odrzucanie za dużych wiadomości, testy błędów.

## Zadania

| # | Kto | Zakres | Gałąź / worktree | Stan |
| --- | --- | --- | --- | --- |
| T0 | Claude | kontrakt `docs/remote-protocol.md` | main | gotowe |
| T1 | Codex | `core/util/hmac_sha256.*`, `core/app/portal_tunnel.*`, testy | `codex/remote-tunnel` → `../GrainMeter-codex-tunnel` | scalone do main (niezacommitowane); poprawka: puste hasło → 403 no_password |
| T2 | Codex | `core/comm/talkback.*`, konfiguracja `remote_*`/`talkback_*`, budżet, mock w symulatorze, testy | `codex/remote-talkback` → `../GrainMeter-codex-talkback` | scalone do main (niezacommitowane) |
| T4 | Codex | transport MQTT (własny mini-klient WebSocket), HMAC w Web Crypto, ekran połączenia, „Wybudź”, CSP, testy | `codex/remote-frontend` → `../GrainMeter-codex-frontend` | scalone do main (niezacommitowane); portal gz 58,3 KiB |
| T3 | Codex (po T1+T2) | firmware: `session_hold` (+ migracja konsoli LAN), `remote_session` na esp-mqtt (espressif/mqtt 1.1.0), wznowienie po aktualizacji | `codex/remote-firmware` → `../GrainMeter-codex-firmware` | scalone do main (niezacommitowane); firmware buduje się (1,35 MB, 29 % wolne); przetestowane na płytce 2026-11-26 (poprawki: stos konsoli 8 KB, TalkBack `{}` = pusta kolejka, kolejność `remote`/`ota` w diag) |
| T5 | Claude + użytkownik | scalenie, testy, konto EMQX + ACL, test na płytce, README, wydanie | – | test na płytce (gm2) zaliczony 2026-11-26 10/10; wydanie 0.4.0 |

| S1 | Copilot | przyciski sesji w `remote.html`: „Przedłuż o N min” (`POST /api/remote/extend`, ≤ 60 min od teraz, w ramach budżetu) i „Zakończ sesję” (`/api/service/exit`), odliczanie czasu i budżetu; pole `remote_idle_min` (1..60, dziś stałe 10 min) | `copilot/remote-session-controls` → `../GrainMeter-copilot-session` | scalone do main 2026-11-26 (207 testów C++, 87 JS; firmware buduje się); czeka na test na płytce: retained status po przedłużeniu, odpowiedź → PUBACK → offline → sen, limit bezczynności |
| L1 | Copilot | logi przez MQTT: `log_dest` udp/mqtt + poziom w portalu, temat `gm/<id>/log` (QoS 0, paczki ≤ 1 KB, bufor sprzed połączenia, licznik strat), panel „Logi” w `remote.html`, `tools/mqtt_log.py` (paho-mqtt); ACL: `<id>-dev` Publish i `<id>-web` Subscribe `gm/<id>/log` | `copilot/mqtt-log` → `../GrainMeter-copilot-mqttlog` | scalone do main 2026-11-26 (C++ 218, JS 106, portal 60,8 KiB, firmware 28,1 % wolne); test na płytce w toku |
| L2 | Copilot | podgląd logów w portalu (Diagnostyka → Logi): `GET /api/logs/<since>` (lokalnie i przez tunel), pierścień RAM ~4,3 KiB przydzielany przy pierwszym odczycie, zwalniany po 5 min; odczyt lokalny bez hasła jak `/api/diag` | `copilot/portal-log` → `../GrainMeter-copilot-portallog` | scalone do main 2026-11-26 (C++ 225, JS 110, portal 63 KiB, firmware 27,9 % wolne); test na płytce czeka |
| S2 | Copilot | gm2: PBKDF2-HMAC-SHA256 (10000), osobne klucze MAC/szyfrowania, szyfrowane treści, podpis restart/status, niezależne seq i LRU cid; front i symulator; review #1–#3 | `GrainMeter-copilot-s2` | wdrożone w worktree (bez commitu); C++ 238/238, JS 123/123, format/lint/build OK, portal gz 64587 B (63,1 KiB); build firmware i pomiar PBKDF2 ≤ ~1 s oraz sterty na ESP32-C6 pozostają do wykonania |
| P1 | Copilot | jedno żądanie statusu na cykl (wspólny polling, bez nakładania, pauza w tle) | `copilot/poll` | scalone 2026-11-26 |
| C1 | Copilot | zegar zachowany przez restart programowy / awarię (zapis RTC z CRC), synchronizacja przed sesją zdalną | `copilot/clock` | scalone 2026-11-26; na płytce: zegar ważny od razu po OTA |
| UI | Claude | zwijany panel połączenia z paskiem sesji, logi tylko w Diagnostyce (MQTT albo `/api/logs`), wybór „Wysyłanie logów” (portal/UDP/MQTT), budżet widoczny tylko < 10 min, aktywność = działania użytkownika (automatyczne odczyty nie podtrzymują sesji), link w dashboardzie | main | 2026-11-26 |

Czasy pomiaru i wysyłki (`measure_s`, `upload_s`) są już w portalu
(Konfiguracja) i działają też zdalnie; od `upload_s` zależy, jak szybko
płytka odbierze komendę `REMOTE` z TalkBack.

### Test na płytce 2026-11-26 (EMQX, `remote.html` + skrypt e2e)

Zaliczone: wybudzenie przez TalkBack, status `online`/`offline` (retained,
LWT), GET status/diag/config z poprawnym MAC (110–170 ms), duplikat →
odpowiedź z pamięci, stary `seq` → `409 stale_seq`, zły MAC → `401` bez
podpisu, `POST /api/ota` → `501`, nieznana ścieżka → `404`, diag 5,3 KB
poprawny JSON, `/api/update/check` przez tunel.

EMQX: reguła **All Users → `#` → Deny blokuje też reguły per login** (każda
subskrypcja odrzucana). Zamiast niej: `#` Deny na końcu listy każdego
loginu. Front pokazuje teraz przyczynę (login / ACL z nazwą tematu / sieć).

Uwagi z review (do decyzji):

1. **Poprawione 2026-11-26:** kolejka pełna → niepodpisane `503 busy` z wątku MQTT, front ponawia do 3 razy. Dawniej: żądanie porzucane bez odpowiedzi.
   Dwie karty strony naraz: co drugie żądanie `409`, ruch podwójny.
2. **Poprawione 2026-11-26:** wiadomość > 4096 B → niepodpisane `400 bad_request`. Treść > 2048 B front odrzuca sam (`413`) bez wysyłania.
3. Ścieżka z `?` → `400` (zgodnie z kontraktem; front jej nie używa).
4. Retained przy żywej subskrypcji przychodzi bez flagi retain (MQTT 3.1.1),
   więc płytka obsługuje je normalnie; ochrona działa tylko przy ponownym
   połączeniu. Zabezpiecza `seq`.
5. **Poprawione 2026-11-26:** dwa klienty naraz (np. dwie karty) mogły dać dwa `409 stale_seq` z rzędu → błąd; front wyrównuje `seq` do 3 razy.
6. **Poprawione 2026-11-26:** `ESP_ERR_HTTP_CONNECT` przy manifeście w trakcie sesji MQTT: dwie sesje TLS ze statycznymi buforami (16+4 KB każda). `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`: wolna sterta w sesji 68 KB (min 27–32 KB) zamiast 22 KB (min 7,5 KB).
7. `POST /api/config` z nieznanym polem → „configuration saved” (zapis bez
   zmian); błąd sprzed tunelu.


Zlecenia dla Codexa (prompty i raporty): `.git/delegowanie/remote-*.md`
oraz `.git/delegowanie/codex-remote-*.txt`.

## Do zrobienia przez użytkownika (T5)

- Konto EMQX Cloud Serverless, deployment, dwa loginy (płytka, przeglądarka)
  i reguły ACL z kontraktu (sekcja „ACL”).
- TalkBack w ThingSpeak (ID i klucz API).

## Inne otwarte sprawy

- Płytka ma testowe interwały `measure_s=180`, `upload_s=540`; w polu
  przywrócić 900 / 3600.
- Wydanie 0.4.0 po scaleniu (push tylko na wyraźne polecenie).

### Review Copilota 2026-11-26 (całość zmian, 104 pliki)

Poprawione od razu (zweryfikowane w kodzie):

- #4 link `#cfg=` sam zaznaczał „zapamiętaj” → zapamiętywanie tylko, gdy formularz był już zapisany w tej przeglądarce.
- #5 chwilowy `offline` odmontowywał portal i przerywał wgrywanie → portal zostaje zamontowany przez całą sesję.
- #6 anulowanie odrzuconego `begin` (np. `409 busy`) przerywało cudze wgrywanie → `abort` tylko po przyjętym `begin`.
- #7 wynik `op` po restarcie z porównania wersji (zwykły `/api/restart` → `failed`, rollback tej samej wersji → `ok`) → porównanie partycji startowej zapisanej przed restartem.
- #8 sesja nie wznawiała się po rollbacku z powodu panic/WDT → wznowienie także po `PANIC`/`*_WDT`.
- #9 `provision.py sim`: klucze > 15 znaków (`remote_budget_min`, `alarm_repeat_min`) zapisywane pod złą nazwą → mapowanie jak `batt_capacity`.

Do decyzji (zmiana protokołu, osobne zadanie S2):

- #1 treść żądań przechodzi przez broker jawnie (TLS tylko do brokera): zdalna zmiana `svc_pass`, haseł Wi-Fi, kluczy API jest widoczna dla operatora brokera.
- #2 klucz HMAC = hasło serwisowe: z jednego podsłuchanego żądania broker może zgadywać słabe hasło offline (blokada 5 prób tego nie zatrzymuje).
- #3 pole `restart` w odpowiedzi i status `gm/<id>/status` (z `op.result`) nie są podpisane: broker może ukryć rollback albo skrócić oczekiwanie na restart.
