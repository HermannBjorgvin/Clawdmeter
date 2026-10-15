#pragma once

// Build identity reported by the `info` serial command.
//   CLAWD_ENV      PlatformIO env name; set per env in platformio.ini.
//   CLAWD_VERSION  release tag; CI sets it through PLATFORMIO_BUILD_FLAGS,
//                  e.g. PLATFORMIO_BUILD_FLAGS='-DCLAWD_VERSION=\"v1.9.0\"'.
#ifndef CLAWD_ENV
#define CLAWD_ENV "unknown"
#endif
#ifndef CLAWD_VERSION
#define CLAWD_VERSION "dev"
#endif
