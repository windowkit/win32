# Changelog

## [0.0.2](https://github.com/windowkit/win32/compare/v0.0.1...v0.0.2) (2026-09-20)


### Features

* a &lt;Frame&gt; pane's pixels, over a shared composition surface ([99b7a7b](https://github.com/windowkit/win32/commit/99b7a7ba5f7ddf7b38964ef3e52ea230b5b65469))
* a line's direction runs, from HitTestTextRange ([6ec4277](https://github.com/windowkit/win32/commit/6ec4277183cb09028277d05257ced8d6eb4bf8e0))
* a UI Automation provider, served from a mirror of the tree ([186d3ca](https://github.com/windowkit/win32/commit/186d3ca886725edb9b39dd7ff8d91a8f74e172b5))
* a UI thread of the bridge's own, and a DirectComposition surface ([bba080a](https://github.com/windowkit/win32/commit/bba080adc681b8882bd6cb66c9750c10cafe8b00))
* drag and drop over OLE ([fa20ba8](https://github.com/windowkit/win32/commit/fa20ba8a08937c36bc0a22fb875d1bc9df301d2d))
* glProbe(), which asks what OpenGL this machine can give a window ([29373c0](https://github.com/windowkit/win32/commit/29373c02cec47ecfafcb5db143ebf404ba9818ea))
* notifications, staying awake, idle time and window states ([57d123f](https://github.com/windowkit/win32/commit/57d123f77d0bf7348e45575916210cc4bca3d7f0))
* pace frames on the compositor clock, not a JS timer ([3b257e1](https://github.com/windowkit/win32/commit/3b257e160559d883654f9c22d02ad0333a45337e))
* relaunch properties, so a pinned tile starts the app ([e0f953e](https://github.com/windowkit/win32/commit/e0f953eb963124726c700f81b424b8b1e1b64af9))
* the ctx verb table over Direct2D, and DirectWrite behind it ([fd480b2](https://github.com/windowkit/win32/commit/fd480b2019e71f79c8c41e456d9f53d84c48e133))
* the desktop around the app — appearance, clipboard, bezels, popups ([32256cd](https://github.com/windowkit/win32/commit/32256cdcd9d5a2e11ae6c194410c4a1457db269a))
* the input method, through IMM32 ([e1fc44f](https://github.com/windowkit/win32/commit/e1fc44f24245bea9f66c7c732c65ff0606960c2c))
* the taskbar's own surfaces ([a2ec327](https://github.com/windowkit/win32/commit/a2ec3271ce6d2a023ec556c85a9dc82f4d1f5ebb))
* the tray, the taskbar button, file dialogs and global hotkeys ([2e29de2](https://github.com/windowkit/win32/commit/2e29de2f5a37a80e3db13e18c35ae8ea01b7543f))
* the wheel, mouse leave, synthetic input, and the second teardown door ([412114e](https://github.com/windowkit/win32/commit/412114e2e17a211ecf374f90d458c44f756d5e62))
* the window's AppUserModelID, and a jump list attached to it ([5da2d16](https://github.com/windowkit/win32/commit/5da2d168295ee56bb7bd4de0b275b3f21c5ac4fc))
* version() and probe() ([69764f8](https://github.com/windowkit/win32/commit/69764f83ffd988881c81eff12751180b6e8964cd))
* window verbs, listScreens, and undefined-safe option reads ([dc57ae6](https://github.com/windowkit/win32/commit/dc57ae61cef9d0871715ab34813047bff77c5bdb))


### Bug fixes

* a drag preview must not be the window the shell drops on ([54a36aa](https://github.com/windowkit/win32/commit/54a36aa668eea736f2fa81c96ed58750ef8d708e))
* a uri-list offered as CF_HDROP has to be one ([82ee38b](https://github.com/windowkit/win32/commit/82ee38be568c5715888bcb451eec10c913848d60))
* **build:** rename the prebuild script, which npm ran as a build hook ([0c624f8](https://github.com/windowkit/win32/commit/0c624f852191c1b6042073a927a7ac3e11cfc329))
* clip a pane to its rect, and pin down how stale its buffer is ([ac01575](https://github.com/windowkit/win32/commit/ac01575d175ce97a81ac6e77aa32859dcb3c02f5))
* popups are shaped, unactivated and where they were placed ([b7df967](https://github.com/windowkit/win32/commit/b7df96742989259ccafabd8778d59e2d35a0aac4))
* pump the apartment while a drop waits ([ed03d0c](https://github.com/windowkit/win32/commit/ed03d0c70866af50577e479dd16a4d21fea55597))
* push the tree when a client attaches, and convert to screen coordinates ([7db4703](https://github.com/windowkit/win32/commit/7db4703293f413fcd5a84443b84c624489bc506e))
* report a drag's motion from the callback that actually gets it ([2b55794](https://github.com/windowkit/win32/commit/2b55794d865f8e550efd53e074dcb2db6cb7d6c1))


### Documentation

* record the thread split, and what is missing ([a49568b](https://github.com/windowkit/win32/commit/a49568b91444b6a956eca75434fba1c48611def7))
* the README described a two-verb spike ([dc528a1](https://github.com/windowkit/win32/commit/dc528a1a6d1f8c06c086248181b7c45eb3c842bc))
* the README listed as missing what has since been built ([3964188](https://github.com/windowkit/win32/commit/3964188aed7653fd1b96b8066ae557889115a54e))
* two ways a first release goes wrong without anyone noticing ([89c88e4](https://github.com/windowkit/win32/commit/89c88e472eb612d07e43168345ca708ee705cdb0))
