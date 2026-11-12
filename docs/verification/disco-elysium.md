# disco elysium, stage 1

Date: 2026-11-11. Done with Chris at the keyboard, playing a real save.

## setup

- Game: Disco Elysium (`disco.exe`, 64-bit, Unity 2020.3.12f1, save build `5f7b5419`), in the CrossOver 26.3 bottle `Disco Elysium`.
- Machine: Apple M5, 24 GB, macOS 27.0.1.
- moth: branch `stage-1-engine` at `adc46cd`, default `MOTH_HOME`, launched and attached with `moth attach --bottle "Disco Elysium" disco.exe`.
- Two game sessions. The game was restarted once in the middle for an unrelated audio problem, so addresses differ between the two.

## result

| check | bar | measured | verdict |
| --- | --- | --- | --- |
| first scan speed | 3.0 s/GiB or better | 0.87 to 1.29 s/GiB over eight full scans | met |
| values changed and seen on screen | 3 | 3 (experience, money, skill points) | met |
| values found from the typed numbers alone | 3 | 1 (experience). Money and skill points were in moth's list and set with `moth set`, but the row was picked by checking what sits next to it in memory | not met |

Stage 1 works for an ordinary number that changes a couple of times. It does not yet single out a value that is 0 or 1, and it cannot change a number the game recalculates.

## first scans

| command | bytes scanned | time | s/GiB | matches (main) | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `find 5` | 3,908,050,944 | 3,884 ms | 1.07 | 5,076,252 | 0 | 0 |
| `find 6` | 3,979,694,080 | 3,646 ms | 0.98 | 3,369,252 | 0 | 0 |
| `find 5` | 4,015,349,760 | 4,107 ms | 1.10 | 5,077,981 | 0 | 0 |
| `find 5.32` | 4,016,201,728 | 3,639 ms | 0.97 | 7,965 | 0 | 0 |
| `find 5` | 4,016,201,728 | 3,992 ms | 1.07 | 5,077,533 | 0 | 0 |
| `find 5.32` | 3,906,543,616 | 3,273 ms | 0.90 | 7,911 | 0 | 0 |
| `find 5` | 3,907,067,904 | 3,159 ms | 0.87 | 5,113,817 | 0 | 0 |
| `find 1` | 4,049,899,520 | 4,863 ms | 1.29 | 635,702,555 | 0 | 0 |

## the values

### experience (stored as a 4-byte whole number)

Found from the screen numbers alone.

| step | time | whole number | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- |
| `find 5` | 3,159 ms | 878,758 | 5,113,817 | 0 | 0 |
| +5 XP in game, `next 10` | 2,378 ms | 850 | 2,002 | 7,160 | 0 |
| +5 XP in game, `next 15` | 27 ms | 1 | 2 | 0 | 0 |

- One address left (`0x6c551068`), shown under two tags: `whole number` and `whole number (2-byte)`.
- `moth set #1 95` (was 15). The next +5 XP in game produced a level-up and a skill point, so the game used the written value.
- Cheat Engine value type needed: 4 Bytes.
- Before each `next`, the number was checked with a read of the address found earlier, because a wrong number would have triggered the went-up fallback and Chris had not read the sheet.

### money (shown as 5.32, stored as the 4-byte whole number 532)

| step | time | matches | lost | unreadable |
| --- | --- | --- | --- | --- |
| `find 5.32` | 3,639 ms | 7,965: 5,093 `whole number ×100`, 304 `whole number ×100 (8-byte)`, 2,415 `decimal`, 153 `decimal (8-byte)` | 0 | 0 |

- No in-game change was made. A throwaway script (not in the repository) read the words around every 4-byte 532 and found exactly one with experience 5, level 2 and skill points just before it. That address was row 18 of moth's own `results`, tagged `whole number ×100 (8-byte)`.
- `moth set #18 99.99` (was 5.32). The game's money display showed 99.99. A save made afterwards holds `Money: 9999`.
- Repeated after the restart: `find 5.32` 3,273 ms, 7,911 matches, the same pattern picked one address, again row 18, set to 99.99 again.
- Cheat Engine: 4 Bytes, and you would have to know to type 532.

### skill points (4-byte whole number, 0 or 1)

| step | time | whole number | 8-byte | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `find 1` | 4,863 ms | 22,200,043 | 3,771,345 | 635,702,555 | 0 | 0 |
| point spent, `next 0` | 30,010 ms | 346,029 | 22,503 | 573,360,162 | 0 | 0 |
| level-up, `next 1` | 26,427 ms | 68,558 | 1,006 | 559,876,126 | 448,232 | 0 |
| point spent, `next 0` | 25,700 ms | 5,424 | 95 | 558,913,859 | 0 | 0 |
| `next 0` | 26,025 ms | 4,704 | 74 | 558,706,753 | 0 | 0 |
| `next 0` | 25,795 ms | 4,391 | 70 | 558,616,326 | 0 | 0 |

- Not unique after four in-game changes. The address known from the money pattern (`0x6c551070`) was row 1 of `results`, tagged `whole number (8-byte)`.
- `moth set #1 5` (was 0). The character sheet showed five skill points.
- Cheat Engine: 4 Bytes.
- Experience had to be put back to 95 between rounds with the throwaway script, because the hunt for skill points had replaced the hunt for experience.

## values that could not be changed

### conceptualization (shown as 6)

| step | time | whole number | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- |
| `find 6` | 3,646 ms | 339,392 | 3,369,252 | 0 | 0 |
| shirt off, `next 5` | 2,324 ms | 65 | 123,802 | 31 | 0 |
| shirt on, `next 6` | 547 ms | 5 | 119,999 | 0 | 0 |

- Five addresses. 7, 8, 9, 10 and 11 were written to them; the screen still showed 6, and the game had put four of the five back to 6. All five writes were undone with `moth undo`.
- The game builds this number from a base, a learned count and item bonuses each time. Changing it needs the parts, which is stage 3 work.

### logic (5, then 6 after spending a skill point)

- `find 5` then `next 6` (2,499 ms): 167 whole number matches; two more `next 6` (542 ms, 506 ms): 151. No second change was possible, so it stayed at 151. Lost 0, unreadable 0.

## other things seen

- The game closing: `moth status` still said attached; the next step said `the game closed. attach again once it's running.` and status then said `not attached`.
- Nothing was unreadable in any step. Candidates in memory the game had freed were reported as lost (31, 7,160 and 448,232 in three steps), never dropped silently.

## problems found

1. `moth results` shows at most 20 rows and cannot page, filter or sort, so a value that is not in the first rows of its group cannot be reached.
2. There is no way to set a value by address.
3. One hunt at a time. Starting a second value throws the first away.
4. A decimal that lies between two typed numbers passes both steps without having changed. After 5 then 6 then 5, about 110,000 unchanged decimals stayed in the main count. A step with a new number should also require the value to have changed.
5. For a small typed number the decimal reading matches every tiny float, which includes every small whole number read as a float: 355 million 4-byte and 223 million 8-byte matches for `find 1`. These made each later step take about 26 seconds, against 0.03 to 2.5 seconds for other hunts.
6. A 0 or 1 value does not converge: each change cut the whole-number list by only 5 to 65 times.
7. A number the game recalculates is found but cannot be changed, and moth does not say why the write did not stick.
8. `moth status` does not notice that the game closed until the next step.

---

# disco elysium, stage 1b

Date: 2026-11-11 into 2026-11-12. Done with Chris at the keyboard, in the same game session as stage 1's second run (so addresses match that run).

## setup

- Game and machine as above. moth: branch `stage-1b-disco-gaps` at `efffb86`, default `MOTH_HOME`.
- Every read and write of the game in this section went through `moth` commands. No script outside moth touched the game's memory. The game's save files were read from disk three times to learn how a value is stored (see "thought cabinet").

## result

| check | bar | measured | verdict |
| --- | --- | --- | --- |
| first scan speed | 3.0 s/GiB or better | 0.75 to 1.30 s/GiB over five scans with recorded sizes | met |
| each step of the skill-point hunt | 8 seconds or less | `find 1` 5.4 s, `next 0` 10.6 s (stage 1: 4.9 s and 30.0 s) | `find 1` met, `next 0` missed |
| experience, money and skill points found, changed and confirmed with moth commands only | all three | all three | met |
| a write to a recalculated number reported as not held | said in plain words | moth said it held; the game put it back later | missed |

## first scans

| command | bytes scanned | time | s/GiB | matches (main) | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `find 55` | 4,499,763,200 | 4,547 ms | 1.09 | 1,650,763 | 0 | 0 |
| `find 55` | 4,516,605,952 | 4,139 ms | 0.98 | 1,702,709 | 0 | 0 |
| `find 106.98` | 4,516,605,952 | 3,145 ms | 0.75 | 201 | 0 | 0 |
| `find 1` | 4,524,994,560 | 5,426 ms | 1.29 | 108,976,905 | 0 | 0 |
| `find 13` | 4,559,269,888 | 5,505 ms | 1.30 | 2,115,579 | 0 | 0 |

## the values

### experience (numbers only)

| step | time | whole number | decimal | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `find 55` | 4,139 ms | 58,523 | 25,474 | 1,702,709 | 0 | 0 |
| +5 XP in game, `next 60` | 2,734 ms | 1 | 1 | 63 | 2 | 0 |

- One whole-number match after one change (`0x6c551068`); stage 1 needed two changes and had 55 decimals left after the first.
- `keep 1 xp`, `set xp 95` (held). The next +5 XP levelled Chris up.

### money (through its neighbour)

- `find 106.98`: 3,145 ms, 201 matches (99 decimal, 5 eight-byte decimal, 81 whole number ×100, 16 ×100 eight-byte).
- `results --near xp`: 4 rows; the first is `0x6c551078`, 16 bytes after experience, tagged `whole number ×100`.
- `set 1 200.00` (held), `keep 1 money`. The game showed 200.60 a few minutes later (200.00 plus 0.60 earned).

### skill points (0 and 1)

| step | time | whole number | decimal | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `find 1` | 5,426 ms | 28,392,975 | 31,429,682 | 108,976,905 | 0 | 0 |
| point spent, `next 0` | 10,619 ms | 156,231 | 25,374,211 | 32,632,295 | 0 | 0 |

- Stage 1's `find 1` had 635,702,555 main matches, 355 million of them decimals.
- `results --near xp`: 50 rows; rows 1 to 3 are `0x6c551070`, 8 bytes after experience.
- `set 1 5` (held), `keep 1 points`. Chris saw five skill points and spent them.

### conceptualization (a recalculated number)

| step | time | whole number | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- |
| `find 13` | 5,505 ms | 226,468 | 2,115,579 | 0 | 0 |
| shirt off, `next 12` | 3,263 ms | 604 | 1,312 | 0 | 0 |
| shirt on, `next 13` | 17 ms | 3 | 37 | 0 | 0 |

- `set 3 20` answered `set #3 to 20 (was 13)` with `held` true: a quarter of a second later the value still read 20.
- Kept as `conc` and set to 20 again. Chris reopened the character sheet: it showed 13, and `moth kept` then read 13.
- The game recalculates this number when the sheet is opened or clothing changes, not continuously, so a check a moment after the write cannot see it.

## thought cabinet (asked for by Chris during the session)

- The save file shows each thought has a `timeLeft` in minutes. Two were being researched: Advanced Race Theory (100) and Mazovian Socio-Economics (174 at the save).
- Advanced Race Theory: `find 100` 4,141 ms, main 3,195,734; two game minutes later `next 98` 2,938 ms: 5 whole number; one more minute, `next 97` 29 ms: 1 whole number. `keep 1 art`, `set art 1` (held). The thought completed on the next game minute.
- Mazovian Socio-Economics: `find 171` 4,578 ms, main 501,429; `results --near art`: nothing within 4 kb; one minute later `next 170` 2,286 ms: 5 whole number. The one in the same region of memory as `art` was kept as `maz` and set to 1 (held). The thought completed on the next game minute.
- An attempt to work out the addresses from evenly spaced neighbours (an anchor found with `find 1200` and `results --near`) held for two neighbours and no further.

## problems found

1. `next 0` after `find 1` takes 10.6 seconds. The decimal groups are too large to keep their values, so the rule that a value must have moved cannot apply to them, and about 25 million unchanged decimals between 0 and 1 stay in the hunt.
2. A number the game recalculates only on an event is reported as held. Only a later `moth kept` shows the game put it back; `kept` does not say so.
3. A value whose number is not on screen (minutes left on a thought, shown only as a percentage) needed the save file for its starting number. Hunting by change alone would cover it.
4. `results --near` reaches 4 kb. Records of the same kind that the game allocates further apart are not found by it.
5. `keep` right after a `set` on the same row reports the value from the listing, not the one just written (`kept #1 as money (106.98, ...)` after setting 200.00).
