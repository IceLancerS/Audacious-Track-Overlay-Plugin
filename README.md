# Audacious Qt Plugins: Track Overlay

A small plugins for [Audacious](https://audacious-media-player.org/) 4.x with the Qt interface, built and tested on Windows (MSYS2, Qt 6). They also use only Audacious's cross-platform plugin API, so they should build on Linux with the Qt interface too, though that is untested.

| Plugin | Type | What it does |
|---|---|---|
| **Track Overlay** (`track-overlay-qt`) | General | On-screen info overlay when a new track starts |

## Preview
<img width="361" height="166" alt="Screenshot 2026-11-22 192104" src="https://github.com/user-attachments/assets/59c8d755-c45b-43fa-90b0-044b9471e180" />

https://github.com/user-attachments/assets/0efdb9cb-5048-4a9f-89da-323590b3363a

## Track Overlay

A small, click-through overlay shown when a new track starts.

- **Card 1:** artist, title and track length.
- **Card 2:** file type, bitrate, sample rate and channels, colour-coded by quality (green lossless, blue 256 kbps and up, amber 160 to 255, red below 160, violet for hi-res sample rates).
- Fades in and out, never takes focus, and ignores mouse input. Pause, resume and seeking do not trigger it.
- Plugin is now monitor aware will appear on same monitor where app is active. Option to manually select monitor of ur choice. (This is windows only function) 

**Settings** (Settings, Plugins, General, Track Overlay): display time, screen corner, opacity, file info card on/off, colour coding on/off, and a Preview button.

Limits: like any normal window, it will not appear over exclusive-fullscreen games or video.

## Building

A single-file plugin with no `moc` step. They are built inside the [audacious-plugins](https://github.com/audacious-media-player/audacious-plugins) source tree with Meson.

Plugins must be built against the same Audacious version you run.

## Known limitations

- **M4A files** are coloured by bitrate in the overlay, since AAC and ALAC cannot be told apart by extension.
