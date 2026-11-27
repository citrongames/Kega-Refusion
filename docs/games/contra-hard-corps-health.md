# Contra: Hard Corps · Проверка HP / Health verification

Карта относится к **Contra: Hard Corps (USA, Korea)**, серийный номер `GM T-95106-00`, CRC32 `C579F45E`. Все слова читаются в порядке байтов 68000.

Скрипт читает HP из полей, которые использует обработчик конкретного объекта. У одних противников значение уменьшается при попадании. У других растёт счётчик урона, а скрипт показывает остаток до порога текущей фазы. Части составных боссов могут передавать урон контроллеру. Общий счётчик выводится один раз. Одноразовые цели и атаки без такого счётчика учитываются в [журнале проверки](contra-hard-corps-health-ledger.csv).

На оригинальном ROM проверены бои на нескольких маршрутах. У объекта `$033B` на маршруте 1B счётчик урона вырос с 0 до 10 при попадании. На маршруте 2C у `$0295` и `$0296` HP снизилось с 1000 до 990 и с 800 до 790. У `$00A8` подтверждено уменьшение HP с 50 до 40 при состоянии `+$02 = 0`. Остальные записи получены из кода картриджа. Журнал различает проверенный счётчик, передачу урона, одно попадание, атаки и случаи с неполными данными.

## ROM and coverage

The 2 MiB US/Korea cartridge has CRC32 `C579F45E` and SHA-256 `f8feee8e3f2768bec97419c3b278a87827e248fa4d5ae35a1af46a14366c6856`. The dispatcher at `$001F32` covers `$000–$47F`. It has 1,152 entries, of which 290 redirect to the unused return stub at `$001EAC`. The remaining 862 entries point to 791 distinct handlers. The [dispatcher ledger](contra-hard-corps-health-ledger.csv) has one row per value. Its last column records any relevant call-site addresses and the collision mask when the static walk recovered it.

The review followed state dispatch tables, cross-referenced direct writes to health fields, and checked how the game applied damage and crossed death or phase thresholds. The script recognizes 134 meter-bearing IDs. The normal `$00FC` projectile passes `+$36` to the unsigned damage helper at `$046AA4 → $0392A4 → $0392C4`. At zero, the first hit takes the one-hit death path. The review did not establish how the projectile's spawn initializes the word, so the inspector leaves that potential meter unspecified. The older object family dispatches through `A0`; its initialization mask comes from the handler's byte at `+$40`. The later engine uses `A1`, with stage and substate at `+$02/+$04`. Where a boss changes its threshold, the inspector selects the limit from the same phase field as the handler. For example, an unsigned comparison that advances only when damage exceeds 850 requires 851, as with `$033B`.

| Health storage | Evidence |
| --- | --- |
| Decreasing `+$36` words | Standard damage subtract at `$058722`; older unsigned helpers at `$0391E8–$0393FA`; specialized handlers cite initialization and damage PCs in the script. |
| Increasing `+$70` words | Branch-specific thresholds such as `$018E` at `$05CD72/$05CDA4/$05CDCC`, `$0198` at `$0605B4/$061300/$06161E`, and `$0440` at `$0792A6/$079944`. |
| Other actor fields | `$0375` increases `+$5E` and compares against 8,000 at `$074D68/$074D8C`. `$01D9` receives one mechanism strike at `$065CEC` and leaves combat on underflow at `$06612A`. |
| Shared words | `$03A0` controls `FF8000/2/4`, initialized at `$023788`. `$028F/$0290/$0294` use `FFA240`. `$0430/$0433/$0436` use accumulated damage at `FF983C`. The HUD deduplicates the backing address. |
| Forwarders and one-hit targets | For example, `$0197` forwards to `$018E` through `+$6A` at `$05CEE6`; `$033C/$033D` forward to `$033E` at `$0710F0`. `$001D–$0021` enter a different object type on their first hit at `$01601C`. |

The coverage labels describe what was established by the inspection. **Only attack sites recovered** means the inspected shared test uses its attack bit. **No inspected collision or damage path reached** also covers actors whose dispatch is indirect or whose checks use another routine. The ledger does not equate either status with conclusive absence of health. The native playthrough covered selected encounters rather than every route and combat phase.

## Real-core checks

Tested against the original NTSC-U cartridge at 59.922743 Hz. The opening city was exercised in normal play. For selected later-route checks, the probe used the original stage-loading path in an isolated emulator session and applied controller input until a shot landed.

| Encounter | Observed change | Verification |
| --- | --- | --- |
| Route 1B, `$033B` | `+$70` accumulated damage 0 → 10 on frames 3708 → 3709 | The HUD showed 841 remaining out of 851. |
| Route 2C, `$0295/$0296` | `+$36` HP 1000 → 990 and 800 → 790 on frames 1911 → 1912 | The two distinct meters appeared together. |
| Route with `$00A8` | `+$36` HP 50 → 40 on frames 2105 → 2106 while `+$02` remained zero | The inspector showed the initialized object using its `+$40` activation bit. |

Native states also matched between runs with and without the Lua tool under normal presentation and when presenting every 32nd frame.
