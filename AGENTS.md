# Working on ios-cheeseburger (OnePlus 5 port of ios-topaz / ios-sweet)

- Read `PORT-OP5.md` and the cheeseburger handoff at the top of `NOTES.md`
  first. The rest of `NOTES.md` and `README-topaz.md` are the sweet and
  topaz logs: their hardware facts are for SM7150 / SM6225, not MSM8998.
- Update the cheeseburger handoff in `NOTES.md` after each milestone and
  before stopping: commits, CI run IDs, exact build/boot results, unfinished
  work. Distinguish observations (probe photos, logs) from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds.
  Before a CI run, check that `xnu/patches/` still applies to
  xnu-8796.141.3 and that patches keep their hunk line counts.
- Device bring-up uses temporary `fastboot boot` images. Flashing the iOS
  root erases `userdata`: confirm every destructive step with the user
  first. Never write boot partitions, recovery, modem, persist, fsg, fsc
  or modemst, and keep the partition backups.
- Do not touch secure TLMM GPIOs 0-3 and 81-84 or carve-outs below 4 GB.
  Do not apply patches from `xnu/patches-topaz/` without porting their MMIO.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
