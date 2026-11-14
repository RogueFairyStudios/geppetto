# Geppetto

Geppetto is [Rogue Fairy Studios](https://roguefairy.com)' fork of [Krita](https://krita.org), the free and open source digital painting application. The studio uses it for concept art, texture painting (including textures for 3D models) and 2D frame-by-frame animation.

Geppetto is not Krita. It is not affiliated with, or endorsed by, the Krita project, KDE or the Krita Foundation. "Krita" and the Krita logo are trademarks of Stichting Krita Foundation. Almost all of the code in this repository was written by the Krita community; Geppetto adds a small set of changes on top of it.

## What Geppetto Adds

- **Asset versioning inside the application.** A Projects panel lets an artist choose the folders to work on, see which files changed, and synchronize with one button and optional notes, without leaving the application or learning a version control tool.
- **Pluggable storage.** A driver interface keeps the panel independent of where versions are kept. Two drivers exist: one that keeps versions in a local folder, with no server, and one for [Lore](https://github.com/EpicGames/lore), built only when the Lore library is available.
- **Nobody's work is overwritten silently.** When two people change the same file, both versions are kept side by side and flagged for everyone.

Planned next: versions shown in the edit history, and layer-level merge of `.kra` files.

## Status

| | |
|---|---|
| Based on | Krita `master`: `5.4.0-prealpha` with Qt 5 (the default build), `6.1.0-prealpha` with Qt 6 |
| Maturity | Early development. The versioning features have automated tests and have not been used in production yet. |
| Name in the application | The application built from this repository still shows Krita's name and artwork. Renaming it is planned. |
| Platforms | Built and tested on Windows so far. On Android the versioning features are left out of the build. |

Builds made from this repository are not Krita releases. Please do not report problems with them to the Krita project.

## Building

Geppetto builds the same way as Krita. Follow Krita's [build instructions](https://docs.krita.org/en/untranslatable_pages/building_krita.html).

The Lore driver is optional. To build it, point CMake at a Lore `0.14.1-nightly` library with `-DLore_ROOT=<path>`. Without it everything else builds and works, and the local folder driver is still available.

## Documentation

Krita's documentation applies to everything Geppetto inherits from it:

- [User manual](https://docs.krita.org/en/user_manual.html)
- [Developer guides and notes](https://docs.krita.org/en/untranslatable_pages.html)
- [API documentation](https://api.kde.org/legacy/krita/html/index.html)

The files `HACKING`, `README.android.md` and `README.packagers.md` in this repository come from Krita unchanged.

## Reporting Problems and Contributing

- A problem you can reproduce in an official Krita release belongs in [Krita's bug tracker](https://bugs.kde.org).
- Krita itself is developed on KDE's infrastructure, at [invent.kde.org/graphics/krita](https://invent.kde.org/graphics/krita). Changes that are useful to Krita as a whole should be proposed there.
- Changes specific to Geppetto can be proposed as pull requests on this repository.

## Supporting Krita

Geppetto exists because Krita does. If Krita is useful to you, please [support its development](https://krita.org/en/support-us/donations/).

## License

Like Krita, Geppetto as a whole is licensed under the GNU General Public License, version 3. Individual files may have a different, but compatible license; see `COPYING` and the `LICENSES` folder. Lore is a separate project under the MIT license and is not part of this repository.
