# check_log.py - is this device run as good as the accepted one?

After a change to the host you run the game again on the phone and compare the new **full** log with the game's accepted
`baseline.log`. `check_log.py` does that comparison and prints one PASS or FAIL line per check. It reads two files and writes
nothing.

```
python tools\check_log.py C:\logs\MyGame_new.log games\mygame\baseline.log
python tools\check_log.py C:\logs\MyGame_new.log --game mygame          same, baseline looked up in games\
```
Exit codes: **0** every check passed, **1** at least one failed, **2** cannot run (usage error, a file is missing).

## The checks
| | check | fails when |
|---|---|---|
| a | link counts equal the baseline | the `link: direct= shim= trap=` line differs: the host or the map changed what is bound |
| b | no `STOPPED at import` | the game hit an import the host cannot even stub |
| c | `SUMMARY` says `exit=normal` and `frames` > 0 | the run crashed, hung, or never drew a frame |
| d | no new unresolved import reached | a `hits #N ...` line appears that the baseline does not have |
| e | the game thread ended by itself | no `ended by themselves` line, or the thread had to be killed |
| f | none of the known failure lines | the focus handlers of the old code left (`foreground: old code left with`, `foreground: old AppUi::HandleForegroundEventL`, `... has no valid vtable`), or the audio fell back (`REAL STREAM -> silent fake`). Ordinary `foreground: gain=...` lines are normal and ignored |
| g | audio facades balanced | an audio facade was created and never freed, or freed twice |
| h | frame rate within ±30% of the baseline | `gfx: total frames N in M ms` gives a rate outside 0.7–1.3 of the baseline. A different phone can do this legitimately: look at the number before you suspect the host |
| i | the host closed normally | no `SymBridge GUI exit` line (older builds wrote `legacyhost GUI exit`; both count), or `CRASHWATCH ... type=` is not 0 |
| j | same game | the `SUMMARY uid3=` of the log differs from the baseline's |

## Notes
- Use a **full** device log. The baseline is a reduced log (see `accept.md`) and has no `GUI exit` line, so checking a
  baseline against itself fails check i and nothing else.
- Check a also fails when the host build or the map is newer than the baseline: that is the signal to run the game again and
  `accept.py --update`.
- Logs from older host builds show differences by design (other link counts, `legacyhost` names); only logs of the current
  build are expected to pass.
- This script is not part of `check_all.py` because it needs a device log.

## Tests
`tools\tests\test_check_log.py` builds a full log from a real baseline plus the lines a full log has, and checks that it
passes, that the old host name is accepted, and that each failure above is reported by exactly its check, plus the usage and
missing-file exits.
