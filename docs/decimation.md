# Optional integer-rate decimation

`Sink::conversionRate()` defaults to zero. Queue-configured hosts opt into
48000 Hz output and advertise 192000 Hz input. Decoding precedes the original
gain/transition path. The converter maps 88200/176400 to 44100 and 96000/192000
to 48000. Lower rates bypass it without altering PCM. Unsupported rates fail.

The independently implemented double-precision Kaiser FIR uses beta 18.5,
unity DC, and a cutoff midway between 20 kHz and output Nyquist. Lengths are
641/1281 taps for the 44.1 kHz family and 513/1025 for the 48 kHz family.
Compensated group delay is 3.628118/2.666667 ms. Only the retained phase is
evaluated. The intermediate keeps eight guard bits until existing 24-bit
packing; saturation prevents wrapping. Full-scale intersample overshoot is
counted and reported rather than silently wrapped.

Exact frame count is ceil(source count / factor). A shared DecimationChain
carries source history, global phase and successor lookahead across same-rate
gapless boundaries. Per-item ceil budgets determine ownership; odd-length
fixtures compare concatenated output against one FIR stream. Final or tiny
items pad; missing successors pad after five seconds following their request.
The bounded delayed tail permits STMd after speaker-confirmed STMs and a
committed prefix, with the actual route still waiting for produced bytes.
Unknown source counts remain unknown until EOF.

`tests/decimator_unit.cpp` checks exact counts/continuity. The external test-only
SoX comparison is `tests/decimator_quality.py --sox <binary> --report <file>`
after building `decimator-fixture`; it needs NumPy. No SoX/libsoxr code is used.
`tests/decimator_passthrough.py --before <old decoder-test>` compares bypass
PCM and packed WAV bytes with a separately built pre-change core.
