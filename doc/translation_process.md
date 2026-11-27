Translations
============

DeFCoN Core can show its graphical interface in several languages. The translations are Qt Linguist
files in `src/qt/locale/`. Their names, `dash_xx.ts` and `dash_xx_YY.ts`, are inherited from Dash Core
and are kept so that the build and the translation tools keep working.

Translators work in the [DeFCoN Qt Wallet project on Transifex](https://app.transifex.com/defcon/defcon/). The resource `o:defcon:p:defcon:r:defcon-qt-wallet` and the English source file are configured in [`.tx/config`](../.tx/config). Submit reviewed language changes through a pull request. The inherited `contrib/devtools/update-translations.py` post-processes the local catalogs in `src/qt/locale/`; its Transifex download step is disabled in this tree. Use a clean checkout if you need to run it.

### Writing code with translations
We use automated scripts to help extract translations in both Qt, and non-Qt source files. It is rarely necessary to manually edit the files in `src/qt/locale/`. The translation source files must adhere to the following format:
`dash_xx_YY.ts or dash_xx.ts`

`src/qt/locale/dash_en.ts` is treated in a special way. It is used as the source for all other translations. Whenever a string in the source code is changed, this file must be updated to reflect those changes. A custom script is used to extract strings from the non-Qt parts. This script makes use of `gettext`, so make sure that utility is installed (ie, `apt-get install gettext` on Ubuntu/Debian). Once this has been updated, `lupdate` (included in the Qt SDK) is used to update `dash_en.ts`.

To automatically regenerate the `dash_en.ts` file, run the following commands:
```sh
cd src/
make translate
```

**Example Qt translation**
```cpp
QToolBar *toolbar = addToolBar(tr("Tabs toolbar"));
```

### Updating the source file
For general PRs, you shouldn’t include any updates to the translation source files. They are updated in a separate pull request, preferably well before a release, so that new phrases can be translated in time. This also avoids translation related merge conflicts.

To create that pull request, use the following commands:
```
git add src/qt/dashstrings.cpp src/qt/locale/dash_en.ts
git commit
```

### Bringing new strings into a language file
`make translate` updates only the English source file. A language file gets the new and changed strings, with its existing translations kept, from the same `lupdate` command with the language file as the target:

```sh
cd src/
make translate V=1     # prints the lupdate command it runs
# run that lupdate command again, with -ts ./qt/locale/dash_de.ts (for example) in place of -ts ./qt/locale/dash_en.ts
```

New strings arrive unfinished, ready to translate in Qt Linguist; with `-no-obsolete`, strings that are no longer used are dropped. Commit such an update in its own pull request, not together with code changes.

### Adding or improving a translation
1. If the language file is missing newer strings, bring them in first (see above). Then open the file, for example
   `src/qt/locale/dash_de.ts`, in Qt Linguist (included in the Qt SDK).
2. Translate the unfinished entries, or correct existing ones, and mark them as done.
3. Save the file and open a pull request with it. Do not include unrelated changes.

### Handling Plurals (in source files)
When new plurals are added to the source file, it's important to do the following steps:

1. Open `dash_en.ts` in Qt Linguist (included in the Qt SDK)
2. Search for `%n`, which will take you to the parts in the translation that use plurals
3. Look for empty `English Translation (Singular)` and `English Translation (Plural)` fields
4. Add the appropriate strings for the singular and plural form of the base string
5. Mark the item as done (via the green arrow symbol in the toolbar)
6. Repeat from step 2, until all singular and plural forms are in the source file
7. Save the source file

### Translating a new language
1. Create the new file, for example by copying `src/qt/locale/dash_en.ts` to `src/qt/locale/dash_xx.ts` and setting its `language` attribute, then translate it in Qt Linguist.
2. Add the language to the manifest file `src/qt/dash_locale.qrc`, manually or via
   `ls src/qt/locale/*ts|xargs -n1 basename|sed 's/\(dash_\(.*\)\).ts/<file alias="\2">locale\/\1.qm<\/file>/'`.
   Below is an example of the English language entry.
3. Add it to `src/Makefile.qt_locale.include`, manually or via
   `ls src/qt/locale/*ts|xargs -n1 basename|sed 's/\(dash_\(.*\)\).ts/  qt\/locale\/\1.ts \\/'`.

```xml
<qresource prefix="/translations">
    <file alias="en">locale/dash_en.qm</file>
    ...
</qresource>
```

Qt Linguist sources use the `.ts` extension. The resource manifest references the compiled `.qm` file.

### Questions and general assistance
Ask on the [DeFCoN Discord](https://discord.gg/UpUq7V7Fg7).
