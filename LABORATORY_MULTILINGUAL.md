# OpenHantek Laboratory — multilingual source branch

Branch: `openhantek-laboratory-multilingual-20261027` (base: `openhantek-laboratory-clean`).

The Laboratory C++/Qt6 source snapshot contains the analysis workspace, media library, and UI translation assets for 10 languages.

## Documents and examples

- English PDF documentation: `docs/*.pdf`
- Russian PDF documentation: `docs/ru/*.pdf`
- Screenshots: [Oscilloscope](docs/images/laboratory-scope.png), [Analysis](docs/images/laboratory-analysis.png), [Settings](docs/images/laboratory-settings.png)

The Windows portable package uses the matching `documents/` and `documents/ru/` layout. No PDF text was modified.

## Windows binary

See the matching GitHub release for the verified portable program. `OpenHantek-Lab.exe` SHA-256: `922beb3ca68d66817c54b6794a9a82f2c2ff33bd235b64a674e9fef1527a5daa`. Some newer source fixes concerning live language switching are not independently verified in that executable.

License and upstream attribution are unchanged; see `LICENSE` and [OpenHantek/OpenHantek6022](https://github.com/OpenHantek/OpenHantek6022). The `main` branch is unaffected.
