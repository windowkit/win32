{
  "targets": [
    {
      "target_name": "win32",
      "sources": ["src/win32.cc", "src/surface.cc", "src/text.cc"],
      # `.include` and not `.include_dir`: the first is absolute and already
      # quoted, which is what the `<!@()` list form wants. `.include_dir` is
      # relative to the package, and node-gyp generates the project into
      # build/, so it resolves against the wrong directory and napi.h is not
      # found.
      "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")"],
      "defines": [
        "NAPI_VERSION=8",
        "WIN32_LEAN_AND_MEAN",
        "NOMINMAX",
        "UNICODE",
        "_UNICODE"
      ],
      "conditions": [
        ["OS=='win'", {
          "libraries": [
            "-ld3d11.lib",
            "-ld2d1.lib",
            "-ldwrite.lib",
            "-ldcomp.lib",
            "-ldxgi.lib",
            "-lshcore.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              # node-addon-api uses C++ exceptions unless
              # NAPI_DISABLE_CPP_EXCEPTIONS is defined; 1 is /EHsc.
              "ExceptionHandling": 1,
              # No /std: here — node-gyp's default is newer, and overriding it
              # only produces warning D9025.
              "AdditionalOptions": ["/utf-8"]
            }
          }
        }]
      ]
    }
  ]
}
