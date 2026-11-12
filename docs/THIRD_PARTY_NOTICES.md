# Third-party notices

The MIT license covers original ni_GHT_si code. Existing third-party notices retain their original terms.

## Qt

The application dynamically links the host's unmodified Qt libraries and loads host QML/platform plugins. The binary package does not bundle Qt. Qt Core, Gui, DBus, QML, Quick and Quick Controls are used; tests additionally use Qt Test. These components are available under Qt's applicable open-source licenses, including LGPLv3. See the included LGPLv3 and GPLv3 license texts in `licenses/` and the Qt sources and notices supplied by your system package provider.

Users may replace the shared libraries with compatible modified builds and rebuild the application from its source. No restriction on debugging such modifications is imposed.

Official references: [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [Qt Quick Controls licensing](https://doc.qt.io/qt-6/qtquickcontrols-index.html#license-and-attributions), [LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations).

## NVIDIA

Optional NVML integration uses installed SDK declarations and dynamically loads the separately installed NVIDIA library. No NVIDIA library or header is redistributed in the archive. Its mandatory SDK notice is included in [THIRD_PARTY_NVML.md](THIRD_PARTY_NVML.md) and retained in the source. See NVIDIA's terms for the installed SDK and driver; MIT does not relicense them.
