# Changelog

## [0.0.9](https://github.com/windowkit/win32/compare/v0.0.8...v0.0.9) (2026-10-04)


### Features

* **text:** `justify: true` justifies a layout's lines as DirectWrite does, `layoutMetrics(handle, { minWidth: false })` leaves the min-content width to `layoutMinWidth`, and `textFeatures()` says what a layout takes — where a caller justified each paragraph by spacing its words itself, in three layouts, and every layout ran the line breaker a second time for a width nobody asked for ([#16](https://github.com/windowkit/win32/issues/16)) ([686defa](https://github.com/windowkit/win32/commit/686defa9f2351a4e9687fe0230a83622257765a5))

## [0.0.8](https://github.com/windowkit/win32/compare/v0.0.7...v0.0.8) (2026-10-04)


### Features

* setCursor, GDI face names (fontFamilyOf), font width, letter spacing, OpenType features and x-height for react-x11's Windows text engine ([#14](https://github.com/windowkit/win32/issues/14)) ([59c7862](https://github.com/windowkit/win32/commit/59c786242528b9b1fc0f1ea0b7b6524290f8f12d))

## [0.0.7](https://github.com/windowkit/win32/compare/v0.0.6...v0.0.7) (2026-10-03)


### Features

* ctxClip takes the fill rule ctxFill takes, so a ring clipped evenodd cuts a ring where it cut the square around it ([#12](https://github.com/windowkit/win32/issues/12)) ([9380558](https://github.com/windowkit/win32/commit/938055880bef49725265c6edc2f6339a648b51c6))

## [0.0.6](https://github.com/windowkit/win32/compare/v0.0.5...v0.0.6) (2026-09-29)


### Features

* ctxRoundRectXY and op 9, a rounded rect whose corners are elliptical ([#10](https://github.com/windowkit/win32/issues/10)) ([bb4cb30](https://github.com/windowkit/win32/commit/bb4cb300e05907ab7eafa7150cbe3d218af1d2e2))

## [0.0.5](https://github.com/windowkit/win32/compare/v0.0.4...v0.0.5) (2026-09-24)


### Bug fixes

* a press on a popup leaves activation where it was ([#8](https://github.com/windowkit/win32/issues/8)) ([e79d663](https://github.com/windowkit/win32/commit/e79d663bfda8ffed66a5bd5fa2720679a62e633b))

## [0.0.4](https://github.com/windowkit/win32/compare/v0.0.3...v0.0.4) (2026-09-23)


### Features

* **text:** layoutCoverage — a layout's coverage without a surface ([#6](https://github.com/windowkit/win32/issues/6)) ([89fe937](https://github.com/windowkit/win32/commit/89fe93776bd62dbffb9c992a8e9417fd2f047de5))

## [0.0.3](https://github.com/windowkit/win32/compare/v0.0.2...v0.0.3) (2026-09-23)


### Bug fixes

* a dash offset is pixels, as canvas states it ([#4](https://github.com/windowkit/win32/issues/4)) ([77b0315](https://github.com/windowkit/win32/commit/77b03154c60673e1859b83c01ddff24f96728531))

## [0.0.2](https://github.com/windowkit/win32/compare/v0.0.1...v0.0.2) (2026-09-23)


### Features

* layers over GL swap chains; perf for fills, strokes and the UIA mirror ([#2](https://github.com/windowkit/win32/issues/2)) ([8e9f458](https://github.com/windowkit/win32/commit/8e9f458e0b7d0743017d7831836e3ffb66cee74c))
