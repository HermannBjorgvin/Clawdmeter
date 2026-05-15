#pragma once

// Board dispatcher. The build system supplies exactly one -DBOARD_* flag.
#if defined(BOARD_LILYGO_T4S3)
  #include "board_lilygo_t4s3.h"
#elif defined(BOARD_WAVESHARE_AMOLED_216)
  #include "board_waveshare_amoled_216.h"
#else
  #error "No board selected. Set -DBOARD_WAVESHARE_AMOLED_216 or -DBOARD_LILYGO_T4S3"
#endif
