# Pomysł (na później): dodawanie nowego silosu jednym poleceniem

Stan na 2026-11-26: odłożone (jest jeden silos). Rozmowa i rozpoznanie API.

## Dziś ręcznie

ThingSpeak: kanał główny + diagnostyczny + TalkBack; EMQX: konta `<id>-dev`,
`<id>-web` i reguły ACL; `config/device.conf`; `scripts/provision.py`.

## Rozpoznanie

| Krok | API | Uwagi |
| --- | --- | --- |
| Kanały ThingSpeak | tak: `POST https://api.thingspeak.com/channels.json` z User API Key (Account → My Profile) | zwraca ID i klucze |
| TalkBack | brak udokumentowanego API do zakładania (tylko Apps → TalkBack → New) | ominąć wspólnym TalkBackiem |
| EMQX Cloud Serverless: konta, ACL | nie znaleziono dokumentacji API dla Serverless | sprawdzić z kluczem API z konsoli |
| Limit ThingSpeak (darmowe konto) | 4 kanały | 2 kanały na silos → 2 silosy |

## Proponowany projekt

1. Wspólny TalkBack: komenda `REMOTE <id> <min> <valid_until>`; płytka czyta
   listę komend (`GET /talkbacks/<tb>/commands.json`), wykonuje tylko swoją
   i usuwa ją (`DELETE .../commands/<cmd>`).
2. EMQX: reguły z placeholderem `gm/${username}/...` — nowy silos = jedno konto.
3. `tools/new_silo.py` (kreator): klucze kont z pliku poza repo, zakłada kanały
   i konto MQTT (albo wypisuje linię do konsoli EMQX), losuje hasła, zapisuje
   `config/silos/<id>.conf`, opcjonalnie `provision.py`, wypisuje link
   `remote.html` i dashboard. Później ewentualnie lokalny formularz WWW
   uruchamiany przez narzędzie (klucze kont nie trafiają do przeglądarki).
4. Przy > 2 silosach: płatny ThingSpeak, osobne konta albo wspólny kanał
   diagnostyczny (zmiana firmware i dashboardu).
