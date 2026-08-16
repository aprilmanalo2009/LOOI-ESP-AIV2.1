# Replit run notes

## Start the server

```bash
npm install
npm start
```

The HTTP and WebSocket server listens on port `5000`. The WebSocket endpoints
are `/ws/gemini` and `/ws/esp32`.

## Gemini configuration

The server reads `GEMINI_API_KEY` through `GEMINI_API_KEY10` from Replit
Secrets. The default Live model is `models/gemini-3.1-flash-live-preview`; set
`GEMINI_LIVE_MODEL` only if you need to switch to another model supported by
your Gemini project.

## Browser voice tester

Open `/app` in the Replit preview, click **Start microphone**, speak, then click
**Stop & Send**. The dashboard shows the mic level and logs the complete
`START_STREAM` / `END_STREAM` sequence. Gemini's PCM voice response is played in
the browser, so this can verify the server and Gemini flow before flashing the
ESP32 firmware. The bridge queues audio and the end-of-stream marker while
Gemini is still completing its session setup, so short first utterances are not
lost.

## ESP32 connection stability

The ESP32 firmware uses `WebSocketsClient`'s built-in reconnect loop. Its
15-second JSON keepalive is handled by the server as a local pong and is not
forwarded to Gemini Live. After changing `firmware.ino`, flash the sketch to
the ESP32 and monitor the serial log for `[WS] Connected ✓` and
`[WS] Server hello/keepalive ack`, AI audio frames, and `[AUDIO] PLAYBACK_END`.
The server splits Gemini's larger PCM responses into 2048-byte binary frames
for compatibility with ESP32 WebSocketsClient audio callbacks. The firmware
places those frames in a separate 64-block playback queue (about 2.7 seconds
of 24 kHz mono audio) before writing to the PCM5102. This keeps the WebSocket
receive loop responsive and absorbs normal mobile-hotspot jitter. The browser
tester uses a 350 ms jitter buffer and waits for its queued audio to finish
before closing the session.
The firmware plays those 24 kHz, 16-bit mono PCM frames through the PCM5102
using standard stereo I2S: BCK GPIO 4, WS/LRCK GPIO 5, and DIN GPIO 6. Connect
the PCM5102 analog output to the amplifier/speaker and keep its power and
ground common with the ESP32. Keep `WS_HOST` in `firmware.ino` aligned with
the current Replit preview host before flashing.
The ESP32 does not receive firmware changes automatically, so re-flash the
sketch after changing `WS_HOST`, the firmware playback queue, or the server
audio bridge.
The firmware VAD uses a short four-frame silence endpoint (~128 ms) so the
reply starts closer to the browser tester's immediate Stop & Send behavior.
The current firmware also plays a one-second 440 Hz PCM5102 diagnostic tone at
boot; set `PCM5102_TONE_TEST` to `false` after the DAC wiring is confirmed.