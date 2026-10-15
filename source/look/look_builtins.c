#include "look_builtins.h"
#include "math_util.h"

// Own recipes in the spirit of well-known film and creative looks, without any maker's names or LUTs.
// They are tuned for the colours after the camera's colour matrix.
// An empty looks folder gets them as files, so they can be edited like any other look.
const BuiltinLook BUILTIN_LOOKS[] = {
        {"01-standard.look", "name: Standard\n"
                             "short: STD\n"
                             "hotpixel 48\n"
                             "saturation 1.00\n"
                             "tone 0.35 0 0.25\n"},
        {"02-vivid.look", "name: Vivid\n"
                          "short: VIVID\n"
                          "hotpixel 48\n"
                          "saturation 1.30\n"
                          "tone 0.60 0 0.15\n"
                          "split 0 0 0.010 / 0.010 0 0\n"},
        {"03-soft.look", "name: Soft\n"
                         "short: SOFT\n"
                         "hotpixel 48\n"
                         "saturation 1.12\n"
                         "tone 0.15 0.01 0.45\n"
                         "split 0 0 0 / 0.008 0.004 0\n"},
        {"04-chrome.look", "name: Chrome\n"
                           "short: CHROME\n"
                           "hotpixel 48\n"
                           "saturation 0.72\n"
                           "tone 0.45 0.03 0.35\n"
                           "split 0 0.0075 0.0225 / 0.012 0.006 -0.008\n"},
        {"05-negative.look", "name: Negative\n"
                             "short: NEG\n"
                             "hotpixel 48\n"
                             "saturation 0.88\n"
                             "tone 0.70 0.02 0.20\n"
                             "split -0.0125 0.015 0.025 / 0.025 0.010 -0.020\n"},
        {"06-cinema.look", "name: Cinema\n"
                           "short: CINEMA\n"
                           "hotpixel 48\n"
                           "saturation 0.70\n"
                           "tone -0.10 0.05 0.55\n"
                           "split 0 0.008 0.012 / 0.006 0.004 0\n"},
        {"07-portrait-neg.look", "name: Portrait Neg\n"
                                 "short: PORT.NEG\n"
                                 "hotpixel 48\n"
                                 "saturation 0.88\n"
                                 "tone 0.05 0.02 0.45\n"
                                 "split 0 0 0.006 / 0.006 0.003 0\n"},
        {"08-nostalgia.look", "name: Nostalgia\n"
                              "short: NOSTALG\n"
                              "hotpixel 48\n"
                              "matrix 1 0 0 0.10 0.90 0 -0.08 0.08 1\n"
                              "saturation 0.90\n"
                              "tone 0.25 0.03 0.40\n"
                              "split 0.010 0.004 -0.006 / 0.035 0.018 -0.025\n"},
        {"09-mono-green.look", "name: Mono Green\n"
                               "short: MONO G\n"
                               "hotpixel 48\n"
                               "mono 0.24 0.59 0.17\n"
                               "tone 0.55 0 0.25\n"
                               "grain 0.035\n"},
        {"10-mono.look", "name: Mono\n"
                         "short: MONO\n"
                         "hotpixel 48\n"
                         "mono 0.29 0.53 0.18\n"
                         "tone 0.30 0 0.30\n"},
        {"11-sepia.look", "name: Sepia\n"
                          "short: SEPIA\n"
                          "hotpixel 48\n"
                          "mono 0.29 0.53 0.18\n"
                          "tone 0.25 0.02 0.35\n"
                          "split 0.030 0.012 -0.020 / 0.045 0.020 -0.030\n"},
        {"12-natural.look", "name: Natural\n"
                            "short: NATURAL\n"
                            "hotpixel 48\n"
                            "saturation 0.86\n"
                            "tone 0.15 0.01 0.35\n"
                            "split 0 0 0 / 0.006 0.003 0\n"},
        {"13-portrait.look", "name: Portrait\n"
                             "short: PORTRAIT\n"
                             "hotpixel 48\n"
                             "matrix 1 0 0 0.05 0.95 0 -0.04 0.04 1\n"
                             "saturation 0.96\n"
                             "tone 0.05 0.015 0.50\n"
                             "split 0 0 0 / 0.012 0.005 -0.004\n"},
        {"14-deep.look", "name: Deep\n"
                         "short: DEEP\n"
                         "hotpixel 48\n"
                         "exposure -0.15\n"
                         "saturation 1.34\n"
                         "tone 0.55 0 0.20\n"},
        {"15-faded-film.look", "name: Faded Film\n"
                               "short: FADED\n"
                               "hotpixel 48\n"
                               "saturation 0.82\n"
                               "tone 0.35 0.06 0.35\n"
                               "split -0.006 0.008 0.016 / 0.014 0.008 -0.004\n"},
        {"16-instant.look", "name: Instant\n"
                            "short: INSTANT\n"
                            "hotpixel 48\n"
                            "saturation 0.74\n"
                            "tone 0.25 0.08 0.40\n"
                            "split 0 0.004 0.010 / 0.020 0.016 -0.010\n"},
        {"17-high-key.look", "name: High-Key\n"
                             "short: HIGHKEY\n"
                             "hotpixel 48\n"
                             "exposure 0.30\n"
                             "saturation 0.90\n"
                             "tone -0.10 0.04 0.60 2.3\n"},
        {"18-cine-tone.look", "name: Cine Tone\n"
                              "short: CINETONE\n"
                              "hotpixel 48\n"
                              "saturation 0.92\n"
                              "tone 0.10 0.01 0.60\n"
                              "split 0 0.002 0.004 / 0.008 0.004 0\n"},
        {"19-hard-mono.look", "name: Hard Mono\n"
                              "short: HARDMONO\n"
                              "hotpixel 48\n"
                              "mono 0.29 0.53 0.18\n"
                              "tone 0.80 0 0.15\n"},
};

_Static_assert(ARRAY_COUNT(BUILTIN_LOOKS) == BUILTIN_LOOK_COUNT, "one recipe per built-in look");
