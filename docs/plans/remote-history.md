# Plan: historia binarna przez tunel MQTT

Stan na 2026-11-26. Rozszerzenie [remote-access.md](remote-access.md)
(tam historia binarna była poza zakresem, D1); kontrakt:
[../remote-protocol.md](../remote-protocol.md).

## Cel

Zakładka Historia na stronie zdalnej działa jak w sieci lokalnej: wykres
z pamięci płytki, strona po stronie (`/api/history/<from>/<to>`). Przed H1 tunel
zwracał na te ścieżki `501`, a `getHistoryPage` w trybie zdalnym
`unsupported`. H1 usuwa te ograniczenia, zachowując format stron HTTP.

## Decyzje

| # | Decyzja | Wybór |
| --- | --- | --- |
| H-D1 | Kodowanie | odpowiedź `application/octet-stream` przez tunel: `body` = base64 (standardowe, z `=`), w kopercie `"enc":"base64"`; bez `enc` jak dziś (tekst) |
| H-D2 | MAC | liczony nad **przesłanym** `body` (tekst base64), więc przeglądarka weryfikuje przed dekodowaniem, bez zmian w `macText` |
| H-D3 | Rozmiar | strona 256 wierszy × 24 B + 16 B = 6160 B → 8216 znaków base64; koperta < 20 KiB (`kMaxResponse`) bez zmiany limitów |
| H-D4 | Pamięć płytki | base64 pisany strumieniowo do `output_` (bez dodatkowego bufora 8 KB) i HMAC liczony przyrostowo nad tymi samymi znakami; pamięć podręczna duplikatu (`cache_`) bez zmian |
| H-D5 | Czas | jedna strona na żądanie (jak HTTP: per dzień, dalej od `next`); limit czasu w przeglądarce 15 s |

## Zadanie H1 (Copilot → Codex)

Uruchomione 2026-11-26 po scaleniu R1 i R2 z [remote-ota.md](remote-ota.md): te
same pliki (`portal_tunnel.cpp`, `mqtt.js`, `api.js`).

| # | Kto | Zakres | Gałąź / worktree | Stan |
| --- | --- | --- | --- | --- |
| H1 | Copilot → Codex | core: kodowanie base64 + `enc` w `PortalTunnel::respond`, usunięcie `501` dla `/api/history/`; front: dekodowanie w `mqtt.js`, `getHistoryPage` przez transport zdalny; testy C++ i JS; dokumentacja | `copilot/remote-history` → `../GrainMeter-copilot-history` | scalone do main 2026-11-26: C++ 216/216, JS 96; strona przez tunel = HTTP bajt w bajt; test płytki w H2 |
| H2 | Claude + użytkownik | firmware build, test na płytce: zakładka Historia zdalnie = lokalnie, sterta w trakcie | – | czeka na H1 |

## Kryteria odbioru

- Strona historii pobrana przez tunel jest bajt w bajt taka sama jak przez
  HTTP (test integracyjny w symulatorze).
- `make check`, `npm run verify` zielone; portal gz ≤ 72 KiB.
- Na płytce: wykres w `remote.html` = wykres lokalny; minimum wolnej sterty
  w sesji nie spada poniżej dzisiejszego (7,5 KB).
