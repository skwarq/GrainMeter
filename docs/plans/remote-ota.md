# Plan: wgrywanie dowolnego pliku `.bin` przez tunel MQTT

Stan na 2026-11-26. Rozszerzenie [remote-access.md](remote-access.md);
kontrakt tunelu: [../remote-protocol.md](../remote-protocol.md).

## Cel

Ze strony zdalnej (`remote.html`) wybieram plik `grainmeter-*.bin` z dysku
i płytka przy silosie instaluje go przez MQTT, tak jak dziś przez `POST
/api/ota` w sieci lokalnej. Dziś przez tunel działa tylko aktualizacja
z manifestu (`/api/update/*`), a `POST /api/ota` zwraca `501`.

## Co już jest i z czego korzystamy

- `OtaSession` (`core/ota/ota_session.*`): strumieniowy zapis
  `start(len, exact=true) → feed()* → finish()`, sprawdza nagłówek obrazu
  (magic, ESP32-C6, projekt `grainmeter`, rozmiar slotu) **zanim** cokolwiek
  zapisze, wybiera slot do startu dopiero po pełnej weryfikacji. Ta sama
  instancja obsługuje HTTP, więc wzajemne wykluczanie (`ota().busy()`) jest
  za darmo.
- `EspOtaTarget` (`firmware/main/platform/esp_ota_target.cpp`) i plikowy
  target w symulatorze PC.
- Restart po odpowiedzi z `restartAfter` + `op` w statusie po starcie
  (`remote-protocol.md`, „Remote update and restart”) i rollback obrazu,
  który nie wystartował (`ota_fail_boot` w symulatorze).
- `core/util/sha256.*`.

## Decyzje

| # | Decyzja | Wybór |
| --- | --- | --- |
| O1 | Transport kawałków | zwykłe żądania tunelu (`POST`, podpis HMAC jak każde), bez nowego tematu MQTT i bez zmiany limitów: treść ≤ `kPortalMaxBody` (2048) |
| O2 | Kodowanie | `off=<dziesiętnie>&d=<base64url bez =>`; kawałek **1440 B** (1920 znaków) |
| O3 | Integralność | SHA-256 całego pliku podany w `begin`, liczony na płytce przyrostowo, sprawdzany w `finish` przed `OtaSession::finish()`; plus własna weryfikacja obrazu ESP-IDF |
| O4 | Idempotencja | `off == received` → zapis; kawałek już zapisany (`off + len ≤ received`) → `200` bez zapisu (zgubiona odpowiedź); `off > received` → `409 offset` z `next` |
| O5 | Wznawianie | `GET /api/ota` (istnieje) zwraca `received`/`expected`; front wznawia od `received` po zerwaniu połączenia w tej samej sesji |
| O6 | Porzucenie | brak kawałka przez 120 s albo koniec sesji zdalnej → `abort()`; slot startowy bez zmian |
| O7 | Ograniczenia | `begin` odrzucony w trybie Critical (`503 low_battery`), gdy trwa inne OTA (`409 busy`), gdy do końca sesji < 8 min (`409 session_short`); `begin` przedłuża sesję do min. 15 min w ramach budżetu |
| O8 | Czas `begin` | `esp_ota_begin(size)` kasuje cały obszar (kilka sekund): front czeka na `begin` 60 s jak na `/api/update/*` |
| O9 | Gdzie działa | nowe ścieżki obsługuje `Portal::handle()`, więc działają też po HTTP (łatwy test na PC i w sieci lokalnej); stare `POST /api/ota` (jednym strumieniem) zostaje tylko lokalnie |

Szacunek: obraz 1,35 MB to ok. 940 kawałków; przy ~140 ms na żądanie
(zmierzone na płytce: 110–170 ms) około **2,5 min**, ok. 1,9 MB ruchu
(darmowy limit EMQX: 1 GB/mies.).

## API (dopisać do `docs/API.md` i `docs/remote-protocol.md`)

| Metoda, ścieżka | Treść | Odpowiedź |
| --- | --- | --- |
| `POST /api/ota/begin` | `size=<B>&sha256=<64 hex>&name=<plik>` | `{ok, chunk:1440, next:0}`; błędy `409 busy`, `409 session_short`, `413`, `415`, `503 low_battery` |
| `POST /api/ota/chunk` | `off=<B>&d=<base64url>` | `{ok, next}`; `409 {error:"offset", next}`; błędy obrazu jak w `/api/ota` (`415`, `422`, `500`) przerywają sesję |
| `POST /api/ota/finish` | `sha256=<64 hex>` | `{ok, version, bytes, restarting:true}` + `restartAfter`; `422 sha256` → abort |
| `POST /api/ota/abort` | – | `{ok}` |
| `GET /api/ota` | – | jak dziś + `received`, `expected`, `chunk` |

Tunel: usunąć `POST /api/ota/*` z listy `501` (zostaje tylko `POST /api/ota`
bez podścieżki); limit czasu w kontrakcie: 60 s dla `begin`/`finish`.

## Zadania

Kolejność wykonawców wg skilla `delegowanie`: najpierw Copilot CLI
(`gpt-6.1-sol`), przy limicie Codex. Każde zadanie w osobnym worktree,
bez commitów; builda firmware i testów na płytce pilnuje Claude.

| # | Kto | Zakres | Gałąź / worktree | Stan |
| --- | --- | --- | --- | --- |
| R0 | Claude | ten plan + dopisanie API do kontraktu | main | plan gotowy |
| R1 | Copilot → Codex | `core/util/base64url.*` (dekoder, testy), `OtaUpload` w `core/ota/` na `OtaSession` (O3–O7), ścieżki w `Portal::handle()`, zmiana listy `501` w `portal_tunnel.cpp`, testy jednostkowe (kawałki, duplikat, luka, porzucenie 120 s, zły SHA, abort, wyłączność z HTTP OTA, Critical) i integracyjne w symulatorze (cały obraz przez `PortalTunnel`, restart + `op`, rollback z `ota_fail_boot`), `docs/API.md`, `docs/remote-protocol.md` | `copilot/remote-ota-core` → `../GrainMeter-copilot-ota-core` | scalone do main 2026-11-26 (202 testy); różnice: kasowanie slotu przy pierwszym kawałku z nagłówkiem, nie w `begin`; `409 no_upload` |
| R2 | Copilot → Codex (równolegle z R1, kontrakt z tabeli API) | `frontend/src/lib/ota-upload.js` (wstępna kontrola nagłówka 0xE9, SHA-256 w Web Crypto, pętla kawałków, 3 ponowienia na kawałek, wznowienie z `GET /api/ota`, anulowanie → `abort`), `Update.jsx`: wybór pliku także w trybie zdalnym, pasek postępu, czas do końca, oczekiwanie na restart jak przy `/api/update/install`; i18n pl/en; testy (mock transportu: zgubiona odpowiedź, 409 offset, zerwanie i wznowienie); budżet portalu gz ≤ 72 KiB | `copilot/remote-ota-front` → `../GrainMeter-copilot-ota-front` | scalone do main 2026-11-26 (84 testy); portal gz 61 435 B = 5 B pod limitem |
| R3 | Claude | firmware: aktywność sesji przy kawałkach (bez wylogowania po 10 min bezczynności w trakcie), abort przy końcu sesji, sprawdzenie czasu `esp_ota_begin` pod blokadą urządzenia, sterta (dziś min. 7,5 KB wolne w sesji) | main | zrobione 2026-11-26: `tick()` w `remotePoll` i pętli serwisowej, abort przy końcu sesji, `canBegin`: przedłużenie do 15 min w budżecie, inaczej `409 session_short`; firmware buduje się (28,7 % wolne) |
| R4 | Claude + użytkownik | test na płytce: wgranie bieżącego `.bin` przez `remote.html`, zerwanie Wi-Fi w połowie i wznowienie, zły plik (415), przerwanie, rollback | – | 2026-11-26 przez EMQX (skrypt z kodem `remote.html`): pełne wgranie 1,36 MB w 140 s, restart, `op.result=ok`; zły obraz → 415 bez zapisu; przerwanie → stara wersja; porzucenie → abort po 120 s; kasowanie przy 1. kawałku 2,1 s. Poprawka: docelowa wersja z nagłówka obrazu (dawniej ta sama wersja → `failed`). Nie testowane: zerwanie Wi-Fi i rollback na płytce |

## Kryteria odbioru

- `make check` zielone, `npm run verify` zielone, portal gz ≤ 72 KiB.
- Na płytce: pełne wgranie przez EMQX, płytka wraca `online` z nową wersją
  i `op.result = ok`; przerwane wgranie zostawia działającą starą wersję.
- Nic nie zapisuje się do flash przed sprawdzeniem nagłówka obrazu.
