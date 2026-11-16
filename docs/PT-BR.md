# Brazilian Portuguese fan translation

Super Mario Odyssey has no native Brazilian Portuguese language slot. The
**Português (Brasil)** setting uses the **US English (`USen`)** slot with the
separately installed **JumpManClub Brasil v1.1** RomFS translation.
This setting changes the game's language over the save's language at startup;
it does not translate the PC port's own F1 menu.

The F1 menu and the configuration screen have their own language, chosen in
**F1 > Language > Language** (Auto / English / Korean / **Português (Brasil)**). It
is independent of the game's text language; with Auto, the menu follows the game's
language when it has a translation for it, so picking Português (Brasil) for the game
also puts the menu in Portuguese. It applies at the next start. The translation is
`runtime/ui/lang/pt-BR.txt`.

## Obtain and install

Obtain the **Atmosphere** package from the
[JumpManClub Brasil translation page](https://www.jumpmanclubbrasil.com.br/post/mario-odyssey).
Do not download or install a copy of the game from a translation mirror.
Extract the archive and copy its `romfs` folder into the **game-data folder**:

```text
mods/
  JumpManClub Brasil/
    romfs/
      LocalizedData/
        USen/
          LayoutData/
            FontData.szs
          MessageData/
            LayoutMessage.szs
            StageMessage.szs
            SystemMessage.szs
```

Keeping the package's Atmosphere directories also works:
`mods/JumpManClub Brasil/atmosphere/contents/0100000000010000/romfs/...`.
Use F1 > Mods > Open mods folder to locate the correct folder, including on
Linux installations with a separate data directory. Leave the original game
files and saves in place. The supplied font archive is required for accents.

Restart the application after installing files. In **Controls > System language**,
select **Português (Brasil)** and ensure the translation is enabled in **Mods**.
The description reports detection, disabled files, incomplete/modified packages,
unsupported code patches, or a conflicting mod. Restart again if the game was
already running. The preference is saved as `language = pt-BR` in `powermoon.ini`.

## Detection and limitations

Detection checks the sizes and SHA-256 fingerprints of all four files in the
official Atmosphere v1.1 package. A folder name alone never identifies a
translation. Fingerprints are checked once during the existing mod scan, with
at most 580 KB of temporary file data; no checks run per frame. Detection uses
the same case-insensitive paths and alphabetical priority as the RomFS loader.
It reports the package's identity and effective mod priority, not a guarantee
that every message or scene works with every game version.

The inspected package contains three Yaz0/SARC message archives with MSBT files
and one font archive, without ExeFS patches. Its archive entry names cover those
in the supplied 1.4.1 files. Other distributions or revised translation assets
must be inspected before their fingerprints can be recognized. Generic RomFS
mods continue to load as before, even when not recognized by this setting.

There is no native `pt-BR` game code: system-language queries and the existing
save-language override receive `en-US` / `USen`. If the translation is missing,
the preference remains saved and the description explains that the English slot
is being used. Auto retains the save's language as before; it does not detect
Portuguese from the host locale.

Game 1.0.0, Linux gameplay and full-game translation coverage require separate
testing. Do not infer compatibility from the translation's version number or
from successful file detection. The current validation record accompanies the
contribution; unexecuted checks must remain explicitly unverified.

## Current validation

Windows 11 x64, NVIDIA RTX 5060 Ti, game 1.4.1: the original and changed source
builds completed. Bounded runs exited successfully with inspected screenshots:
original English, missing translation, installed PT-BR, the actual Controls
menu/detection message, the opening, an existing save copied into a private
folder, original English restored with the mod disabled, Japanese with the mod
still installed, and PT-BR restored on restart. Cap Kingdom's tutorial text
rendered in Portuguese. Original save hashes remained unchanged.

All 12 localization tests passed with the separately obtained package. Without
private assets, five tests passed and seven asset-dependent tests were skipped.
The existing camera-settings (4), Steam shortcut (8), generated-wrapper (1) and
AES/SHA-256 known-answer checks passed. The existing standalone Vulkan pipeline
test failed to link due to missing `vk_device_fault_report` and `smaa_hlsl_src`
references in its harness; it was not changed as part of this issue. Actual
Vulkan game execution succeeded.

This is bounded 1.4.1 validation, not a complete playthrough. Individual visual
checks for every accented character, all dialogues, long sessions and a
controlled performance comparison are still pending. The translation's message
archives contain the requested accented characters; that is distinct from
visually checking each glyph. No 1.0.0 game files were available. Linux gameplay
and compilation have not been verified in this contribution.

## Troubleshooting and removal

- **English text:** verify the four files are under `romfs/LocalizedData/USen`,
  enable the mod, select Português (Brasil), and restart. Existing saves require
  an explicit language choice rather than Auto.
- **Unrecognized or incomplete:** obtain the original v1.1 Atmosphere package
  again and include `FontData.szs`. Do not mix packages or game files into it.
- **Conflicts:** mods higher in the alphabetical Mods list win for each file.
  Disable a conflicting text/font mod and restart. Other non-conflicting mods
  remain enabled; this feature never rewrites their configuration.
- **Missing accents or broken text:** check the font archive and conflicts,
  then report the game version, port version, log and a screenshot. Package
  detection alone does not establish correct font rendering.
- **Return to original English:** disable this mod in Mods, select English and
  restart. Selecting English alone still uses translated USen files while the
  RomFS mod is enabled. Other original languages can be selected normally.
- **Uninstall:** close the application, remove only this translation's mod
  directory, select an original language, and restart. Saves, original RomFS
  files and other mods need no removal.

## Credits and redistribution

Translation: **JumpManClub Brasil** (the included tutorial also credits Red Wolf
Traduções). Obtain their files separately. No redistribution licence was found
in the package, translation page or copyright page. Neither the translation's
assets nor Nintendo assets are included here. Do not commit these archives,
fonts, extracted MSBT files, generated game code or game libraries. The runtime
does not download the translation or alter the original game files.

## Automated checks

From the build environment, with SDL3 installed:

```sh
clang -O2 -D_FILE_OFFSET_BITS=64 -Iruntime tools/test_localization.c \
  runtime/settings.c runtime/mods.c runtime/localization.c runtime/nxcrypto.c \
  -lSDL3 -o build/test_localization
tools/venv/Scripts/python.exe tools/test_localization.py build/test_localization.exe
```

On Linux, use `tools/venv/bin/python` and `build/test_localization` instead.
Optional private package checks use `PTBR_TEST_ROMFS=/path/to/translation/romfs`.
Without it, asset-dependent checks are skipped; no proprietary test fixtures
are supplied. All checks use temporary settings/mod directories and new
processes to verify persistence across restarts.
