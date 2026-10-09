The MP3 fixtures are original, generated one-second 440 Hz sine waves (amplitude 10000
in signed 16-bit PCM), encoded at 64 kbit/s using FFmpeg's libmp3lame encoder.
`gapless-44100.mp3` is stereo at 44100 Hz; `gapless-48000.mp3` and `gapless-22050.mp3`
are mono at 48000 Hz and 22050 Hz. Their expected decoded lengths are exactly one second
at each respective rate. The Xing/LAME-compatible
tags exercise removal of the metadata frame, encoder/decoder delay, and trailing padding.
No external music or copyrighted recording is included.
