# Translating MFO

You can translate the Field Orders menu without rebuilding anything. You write one text file. That is all.

## The quick version

1. Copy `Data/Interface/Translations/MFO_ENGLISH.txt` to `MFO_<LANGUAGE>.txt` in the same folder. The language is the game's own name for it, the `sLanguage` line in `Skyrim.ini`. For example `MFO_FRENCH.txt`, `MFO_GERMAN.txt`, `MFO_JAPANESE.txt`.
2. Save it as **UTF-16 LE with a BOM**. This is the same format MCM Helper and the game use. Notepad calls it "UTF-16 LE". Notepad++ calls it "UCS-2 LE BOM".
3. Each line is `$MFO_SomeKey`, then one TAB, then your text. Change the text. Never touch the key.
4. Start the game. MFO picks the file that matches the game's language.

A line you leave out stays English. You do not have to translate everything in one go.

## The rules for a line

- Keep the key exactly as it is. It is what MFO looks up. Renaming it means your line is ignored.
- Lines that start with `;` are notes for you. The game skips them.
- Markers like `{1}` and `{2}` are filled in by MFO with numbers or names. Keep every marker your line needs. You can move them around, so `{2} of {1}` is fine.
- Do not add a marker the English line does not have. Do not leave a stray `{` or `}`. MFO throws that line away and uses the English one. It says so in `MFO.log` with a `[i18n]` line that names the key.
- A `%` is just a percent sign. It is not special.
- For a line break write `\n`. For a real backslash write `\\`.
- Keep the short ones short. The `[C]` `[L]` `[T]` markers, `up`, `dn`, `del` and the column headers sit in tight spaces. The wide columns grow to fit your words, but a shorter word looks better.

## Words the game already has

For a few plain words MFO uses the game's own translation when you do not give one. Today that is Health, Magicka, Stamina and Cancel. If your game is not English and your file has no line for them, MFO asks the game for its word and logs what it found as `[i18n] game term ...`. Your own line always wins. English never uses this, so English stays as written.

Skill names, perk names, spell names and item names are the game's data. They are already in your language and MFO shows them as they are.

## Checking your work

Open `MFO.log` after you start the game. Look for the `[i18n]` lines.

- `reload (...): language=FRENCH keys=216 mfoOverrides=170 rejected=0` means your file was read. `mfoOverrides` is how many lines MFO used.
- `rejected=N` means N of your lines broke the marker rules. The next lines name them.
- `mfoOverrides=0` means MFO did not see your file. Check the name, the folder and the encoding.

## Fonts

MFO draws with EB Garamond for text and Cinzel for headers. Noto Sans JP is merged in behind both for Japanese. Menu text is looked up at runtime and the font draws whatever glyphs it has, so there is no list to update. What each font covers:

| Script | Covered |
|---|---|
| Latin, with accents | Yes |
| Cyrillic | Yes in the body font. The header font falls back to the Japanese font, which has only part of it |
| Greek | Mostly |
| Japanese kana and kanji | Yes |
| Simplified Chinese | Partly. Characters the Japanese font lacks draw as a box |
| Korean Hangul | No |
| Thai, Arabic, Hebrew | No |

For Chinese or Korean, replace `Data/SKSE/Plugins/MFO/fonts/cjk.otf` with a font that has your script. Keep the file name. MFO merges whatever is there. Noto Sans is free and has a version for each.

## The MCM

The MCM pages are a separate thing. Today `MCM/Config/MFO/config.json` is plain English text, so the MCM is not translated yet. MCM Helper can translate it. You write `$MFO_SomeKey` in place of a `text` or `help` value in `config.json` and put that key in the same `MFO_<LANGUAGE>.txt` file. It reads the same format. Moving the MCM text over is a later job.

## What does not translate yet

- Reasons that come from other parts of MFO, like why a spell cast failed or why a perk is locked. They show in English.
- The name of the Progression tab. It comes from the Progression add-on.
- The MCM, see above.
- `MFO.log`. The log stays English on purpose.

## For people adding text to MFO

Every new word on screen is a new line in `native/i18n/Strings_keys.h`. One line, one string literal, a stable id. Then run `python3 tools/i18n/gen_template.py` and commit the new template. `release.sh` refuses to ship a stale one. Never rename an id once it has shipped. Opcode strings like `cond.self_hp_below` are frozen and are not keys. The editor shows a label for them and the opcode is what gets saved.
