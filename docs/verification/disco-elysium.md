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

# disco elysium, stage 1c

Date: 2026-11-12. Done with Chris at the keyboard, on Day 2 of the same playthrough.

## setup

- Game and machine as above. The game's writable memory has grown to between 5.87 and 5.93 GiB (4.2 to 4.5 GiB in stage 1b). Experience, money and skill points are at the same addresses as in stage 1b.
- moth: branch `stage-1c-change-hunts` at `0abd138`, default `MOTH_HOME`, default snapshot limit (8192 MiB).
- Every read and write of the game in this section went through `moth` commands, and no save file was read. A script was used on moth's own output twice: to time commands, and to shortlist rows from `moth results` (see "the thought").
- Earlier in the same game session, before this section's work, the skill records had been located for an unrelated request with the stage 1b build. So the address Conceptualization ended at was already known; the hunt below was still done from the typed numbers alone.
- The Mac was short of memory throughout: 37% free before the first snapshot, and 9.9 of 10.2 GB of swap in use.

## result

| check | bar | measured | verdict |
| --- | --- | --- | --- |
| snapshot speed | 3.0 s/GiB or better | 1.81 s/GiB (first, timed end to end) and 1.50 s/GiB (second, helper's own time) | met |
| each step taken while the snapshot is held | 3.0 s/GiB or better | `next same` as a first step 3.08, a second `next same` 3.05, `next down` as a first step 1.70 | missed for `next same`, met for `next down` |
| a thought's remaining time found and changed with no number typed until it was on moth's screen, and no save file read | done | found after four changes, set to 1, the thought completed on the next game minute | met |
| `find 1` and `next 0` | 8 seconds or less each | `find 1` 9.0 s, `next 0` 6.4 s (stage 1b: 5.4 s and 10.6 s) | `find 1` missed, `next 0` met |
| `moth kept` says a recalculated number went back | said in plain words | `conc    13    0x50d4a6cd4  whole number  you set it to 20; it is back to what it was before.` | met |
| the game stayed playable while the snapshot was held | Chris's word | Chris played through the second hold without reporting a problem; asked about the first, he said he had not been paying attention | not confirmed |

## memory

| moment | helper's memory (top) | of which compressed | system free |
| --- | --- | --- | --- |
| attached, before any hunt | 18 MB | | 37% |
| after the first snapshot (5.87 GiB) | 6,992 MB | 6,045 MB | 29% |
| peak during the first `next same` | 8,112 MB | 6,094 MB | 30% |
| after a `find 92` replaced the hunt | 768 MB | 641 MB | 57% |
| after the second snapshot (5.92 GiB) | 7,121 MB | 4,525 MB | 33% |
| after the first `next down` of that hunt | 860 MB | 434 MB | |

- The plan expected about 4.2 GB held and 7.2 GB at the first step's peak; the game is larger now, and the numbers are about 7.0 and 8.1 GB.
- macOS had moved most of the first snapshot into compressed memory within seconds of it being taken.

## speed of the first steps

| command | bytes | time | s/GiB | matches left (main) | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- |
| `snapshot` (first) | 6,300,262,400 | 10.6 s end to end | 1.81 | 7,875,328,000 | | 0 |
| `next same` | 6,300,262,400 | 18,071 ms | 3.08 | 7,758,033,648 | 0 | 0 |
| `next same` | 6,300,057,240 | 17,918 ms | 3.05 | 7,756,445,639 | 0 | 0 |
| `find 92` | 5.868 GiB | 7,760 ms | 1.32 | 1,301,541 | 0 | 0 |
| `snapshot` (second) | 6,350,856,192 | 8,889 ms | 1.50 | | | 0 |
| `next down` | 6,350,856,192 | 10,055 ms | 1.70 | 23,167,556 | 0 | 0 |

- `next same` right after a snapshot keeps almost everything (98.5% here), so the snapshot stays held and nearly every value is stored. `next down` kept 0.3% and the helper let the snapshot go at once.
- The bottle scenario measures `next same` at 1.94 to 2.45 s/GiB on 1 GiB with memory to spare.

## the thought (by change alone)

Ace's Low, research time 2h 45m, started at 09:54 game time. Nothing about it was typed into moth.

| game time | what Chris did | command | time | matches left (main) |
| --- | --- | --- | --- | --- |
| 09:54 | started the research | `snapshot` | 8,889 ms | everything |
| 09:57 | talked to someone | `next down` | 10,055 ms | 23,167,556 |
| 09:58 | talked to someone | `next down` | 675 ms | 2,943,254 |
| 09:58 | nothing | `next same` eight times | 126 to 179 ms | 1,074,037 |
| 09:58 | walked around, no dialogue | `next same` twice | 88 to 126 ms | 432,385 |
| 09:59 | talked to someone | `next down` | 132 ms | 33,191 |
| 09:59 | nothing | `next same` three times | 43 to 57 ms | 25,835 |
| 10:00 | walked, then talked | `next down` | 35 ms | 3,841 (1 lost) |
| 10:00 | nothing | `next same` twice | 16 to 17 ms | 3,751 |

- At 3,751 rows (790 whole number, 747 decimal, 1,112 two-byte, 510 eight-byte, 592 eight-byte decimal) all of them were listed with `moth results --all` in 19 pages, and a script kept the rows whose value was the size of a timer. One fitted: row 1,850, `159  0x7000e78c  whole number`, which is 165 minutes less the six that had passed.
- `keep 1850 thought`, `set thought 1` (was 159, held). At 10:01 Chris reported the thought finished. `moth kept` then read 0 for it.

## zero and one

Skill points were set from 5 to 1 with `moth set points 1` first.

| step | time | whole number | decimal | all main | wide | lost | unreadable |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `find 1` | 8,987 ms | 40,167,952 | 43,981,098 | 157,040,720 | 202,006,807 | 0 | 0 |
| point spent on a thought slot, `next 0` | 6,413 ms | 333,243 | 649,974 | 12,564,648 | 72,666,134 | 506 | 0 |

- `find 1` scanned 5.93 GiB at 1.52 s/GiB; in stage 1b it scanned 4.2 GiB at 1.29.
- Decimals fell from 44.0 million to 0.65 million (stage 1b: 31.4 to 25.4 million). The eight-byte decimals did not: 11,337,824 to 11,106,428. That group had no stored values after `find 1` (`has_values` false), so the rule that a value must have moved could not apply to it.
- `results --near points`: 43 rows, the first three at `0x6c551070`. Points were set back to 5.

## put back

| step | time | whole number | all main | lost | unreadable |
| --- | --- | --- | --- | --- | --- |
| `find 13` | 4,890 ms | 285,059 | 2,269,960 | 0 | 0 |
| shirt off, `next 12` | 3,714 ms | 84 | 220 | 0 | 0 |
| shirt on, `next 13` | 22 ms | 31 | 66 | 0 | 0 |

- 28 of the 31 whole numbers sit at evenly repeating addresses from `0xe080d4d0` up; three are together at `0x50d4a6cd4`, `0x50d4a6cd8` and `0x50d4a6cec`.
- `keep 33 conc` (`0x50d4a6cd4`), `set conc 20`: `set conc to 20 (was 13)`, `held` true.
- Chris reopened the character sheet: it showed 13. `moth kept` printed the line in the result table, and `kept --json` gave `"shown":"13","set_to":"20","put_back":true`.

## problems found

1. `next same` as the first step of a snapshot hunt took 3.05 to 3.08 s/GiB here, just over the bar, with the Mac short of memory and the snapshot already compressed by macOS.
2. `find 1` took 9.0 seconds. Its speed per GiB is as before; the game is 1.4 times the size it was when the 8-second bar was written.
3. Holding a snapshot of this game costs about 7 GB in the helper and 8.1 GB at the first step's peak, on a 24 GB Mac that was already swapping. Whether the game stutters during that was not established.
4. After four changes a blind hunt still had 3,751 rows. `results` cannot filter or sort by value or by form, so picking the timer out meant paging through all of them. Something to hide the rows that cannot be a small number would have made the last step one screen.
5. The eight-byte decimals keep no values after a large `find`, so `next 0` cannot thin them (11.1 million left).

## also seen in this session (not part of this proof)

- With the stage 1b build, before the work above: writing a skill's own numbers (value, maximum, rank, in both copies of the character sheet) held, but a dialogue check's tooltip ignored them. Raising the attribute the skill is built from (Psyche) changed the check from 3% to 97%. All fourteen values were put back afterwards.

# disco elysium, stage 2a

Date: 2026-11-13. Done with Chris at the keyboard, on Day 2 of the same playthrough.

## setup

- Game and machine as above. The game had been running since the evening before (Windows pid 204).
- moth: branch `stage-2a-window` at `e59d0ff`, default `MOTH_HOME`. The service had been attached from a terminal the night before; `bin/moth-app` was opened at 15:56 and came up already attached.
- Chris worked the window. Commands from "the terminal" below were typed by the assistant in a second terminal on the same service.
- How things were watched: `moth log` for what was sent and when; from step 3 on, `moth watch --json` with a script that also photographed the moth window (by window id) while a command ran; for the last step a script that waited for the game's macOS process to go and then photographed the window every third of a second. Chris was not asked to report numbers after the first hunt.

## result

| check | bar | seen | verdict |
| --- | --- | --- | --- |
| a hunt by clicking, no command typed | done | experience found (32, then 42), kept, typed over to 90, and the game showed 90 | met |
| a hunt with no number, no command typed | done | six steps by button took 99,365,278 rows to 105,361 on the game clock; Chris stopped there, so no value was found | not finished |
| during a first scan the window can be moved and the kept panel scrolled | Chris's word | "windows moves while dragging in scan yeh". Scrolling the kept panel was not tried (nothing was kept yet) | moved: met. scrolled: not tried |
| a command from the terminal shows in the window | within one second | a photograph 1.0 s after `moth set xp 97` showed 97 in the list; one 1.0 s after `moth set 1 98` showed 98 in the list and in the kept panel and the line `set #1 to 98 (was 97)` | met, with the window in view (see problem 1) |
| while a scan runs, a click starts nothing and the window says what is running | both | during `moth find 1` from the terminal (6.5 s) every button was disabled and the line read `working: find 1 (user)…` in all eight photographs. No click was made | met as far as it was tried |
| after the game quits, the window says so and nothing can be written | within two seconds | photograph at 0.33 s: unchanged. At 0.66 s: `no game attached`, both lists empty, line `not attached. disco.exe closed.`, every button disabled except `pick a game` and `undo last change` | met (see problem 5) |
| idle, with a list in view | under 5% of one core | `ps -o %cpu` 0.0, 0.0, 0.0 five seconds apart; `top` 0.0 four times. 48 rows listed, one value kept | met |

## the steps

1. **Pick the game.** `detach` at 15:57:11, then `pick a game`, Disco Elysium, disco.exe: attached at 15:57:22.
2. **Experience.** `find 32` answered at 15:57:56 with 6,986,637 rows. After a dialogue gave experience, `next 42` left 48 (2 whole number, 8 decimal, 37 two-byte, 1 eight-byte). It was sent three times (16:01:35, 16:02:09, 16:02:11), each with the same answer. Chris selected row 2 (`0x6c55dfb8`, whole number) and pressed `keep`: `kept #2 as xp (42, whole number)`. He typed 90 over it in the kept list: `set xp to 90 (was 42)`. The character sheet showed 90.
3. **No number.** `no number` at 18:00:17: `snapshot of 4.0 gb taken`. Chris then detached, picked the game again and pressed `no number` again (18:00:42). The steps that followed, on the clock in the corner of the screen (talk until it moves, `went up`; walk without talking, `same`):

| step | time to answer | rows left |
| --- | --- | --- |
| `went up` | not timed | 99,365,278 |
| `same` | 1.72 s | 85,749,676 |
| `went up` | 0.89 s | 1,799,838 |
| `same` | 0.27 s | 1,402,630 |
| `went up` | 0.20 s | 107,678 |
| `same` | 0.04 s | 105,361 |

   The two photographs taken during the first `same` show every button disabled and the line `working: next same (user)…`.
4. **Both hands.** From the terminal: `moth set xp 95`, `96`, `95` while the window was covered by another app (see problem 1). With the window in view: `moth set xp 97`, in the window a second later. Then the case the review found: `moth results --near xp` listed `#1  97  0x6c55dfb8`; three seconds later (the window polls every second and its own row 1 is `0x7b746e28`) `moth set 1 98` answered `set #1 to 98 (was 97)`. `moth kept`: `xp  98  0x6c55dfb8`. The window's row 1 still read 32. `moth find 1` from the terminal: 103,551,059 rows in 6.5 s. `moth bottles` was not run.
5. **The game goes away.** xp had been kept again from the terminal (`moth keep 0x6c55dfb8 98 xp`), because detaching in step 3 had emptied the kept list. Chris quit the game from its menu. The game's process was gone at 18:19:44.22; the service's `not attached. disco.exe closed.` came 0.50 s later. From the terminal afterwards, `moth undo`, `moth set xp 5` and `moth kept` each answered `not attached. run: moth attach --bottle <bottle> <game.exe>`.

## problems found

1. A window that is completely covered by another app could not be shown to follow. Its picture still read 90 half a minute after three changes from the terminal. Whether the window was behind or macOS simply had not redrawn it is not known; when Chris brought it forward it read 95, the current value.
2. `moth log` typed in a terminal puts its first line of output, a raw history row, in the window's line. Four seconds after the game closed this replaced `not attached. disco.exe closed.` with `95323  2026-11-13 18:19:48  user  status  not attached`.
3. The line after a row write from the terminal says `set #1 to 98`, and that row is #2 in the window's own list.
4. The match count is shown twice after a step (under the buttons with its breakdown, and above the list), and the `kind` column cuts off every label longer than "whole number" (`whole numb…`, `decimal (8-…`).
5. With no game attached `undo last change` stays enabled. The service refuses it, so nothing is written.
6. Pressing Return again in the number box sends the same `next` again. Three were sent for 42.
7. After a short no-number hunt the list opens on 200 of 105,361 rows that read 0, as in stage 1c's problem 4: nothing in the window sorts or hides rows by value.
