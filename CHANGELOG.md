# Changelog

## [0.3.0](https://github.com/PychronLabsLLC/pychron-cpp/compare/v0.2.0...v0.3.0) (2026-10-07)


### Features

* **ideogram:** ages alone, and shaded spans ([63d5547](https://github.com/PychronLabsLLC/pychron-cpp/commit/63d554776343dc1cacbaad7d3abbed92148ace7a))
* **persistence:** a sample's location is one geometry column ([d42f2bf](https://github.com/PychronLabsLLC/pychron-cpp/commit/d42f2bf53c0e69d577d00990271d8a4080f71f9d))
* **processing:** one-click export after the Schaen et al. 2021 reporting standard ([58ba97b](https://github.com/PychronLabsLLC/pychron-cpp/commit/58ba97bd585835262866c719a3e3d60631880db0))


### Bug Fixes

* **build:** develop builds and passes on every CI compiler again ([aa1a029](https://github.com/PychronLabsLLC/pychron-cpp/commit/aa1a029699411e8da3f1d3826082c9855e98046d))
* **build:** one portable number parser, LF checkouts, Windows-safe tests ([05995fd](https://github.com/PychronLabsLLC/pychron-cpp/commit/05995fd01fd2efec42f7aaebc7935b7a4b931733))
* **build:** parse_double refuses hex, and the setup test writes its input in binary ([e02a4b8](https://github.com/PychronLabsLLC/pychron-cpp/commit/e02a4b8619ffaf3aa9b47b1e303ad570c6645619))
* **build:** the last two CI compile errors on develop ([b0a9390](https://github.com/PychronLabsLLC/pychron-cpp/commit/b0a93901c840e4b4068f8d010ee3a70fc270eb03))
* **ci:** PostGIS in the CI database, and the golden reader off from_chars ([ef9021c](https://github.com/PychronLabsLLC/pychron-cpp/commit/ef9021ccc739bdb299e3866176e41f67af3ecb51))
* **core:** the clock pump advances in fixed steps again ([93bdafc](https://github.com/PychronLabsLLC/pychron-cpp/commit/93bdafc0c503b9632817709a633baa9996632583))
* **core:** the clock pump keeps its speed however long a sleep takes ([a27f89c](https://github.com/PychronLabsLLC/pychron-cpp/commit/a27f89c3500ca8b036c025b86a3704c15e988d33))
* **export:** files written for the user are not quarantined on macOS ([72dccb5](https://github.com/PychronLabsLLC/pychron-cpp/commit/72dccb530f1e2ad29d95a672308d3a9536541c7c))
* **ui:** a popup handed activation leaves the window's commands live ([b2ecfdd](https://github.com/PychronLabsLLC/pychron-cpp/commit/b2ecfddf14976926e849228ce369c690fce5b3be))
* **ui:** the laser window's video thread asks macOS to wake it on time ([f756f9e](https://github.com/PychronLabsLLC/pychron-cpp/commit/f756f9e818ed2bf3370f1c636bc4f1bb7d9d3954))
* **ui:** the macOS UI tests under the offscreen platform ([178242f](https://github.com/PychronLabsLLC/pychron-cpp/commit/178242f68a4bcb984739dec9d64577fa36944810))
* **ui:** the options editor's sections are all in sight at any width ([e4b550f](https://github.com/PychronLabsLLC/pychron-cpp/commit/e4b550f9622ac2cf9d77776139a808cb5b27288d))


### Performance Improvements

* **laser:** the simulated camera draws the tray's other holes in place ([d575982](https://github.com/PychronLabsLLC/pychron-cpp/commit/d5759825575db439857fc57c6529d1d2a1771656))
