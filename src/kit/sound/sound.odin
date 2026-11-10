package sound

import ma "vendor:miniaudio"

// "New Notification" from Pixabay, trimmed and given a clean fade, embedded
// so nothing ships beside the exe.
@(private = "file")
ARRIVAL :: #load("sounds/arrival.wav")

@(private = "file")
chime: struct {
    ready:   bool,
    failed:  bool,
    engine:  ma.engine,
    decoder: ma.decoder,
    sound:   ma.sound,
}

@(private = "file")
init_chime :: proc() -> bool {
    if chime.ready do return true
    if chime.failed do return false
    if ma.engine_init(nil, &chime.engine) != .SUCCESS {
        chime.failed = true
        return false
    }
    data := ARRIVAL
    if ma.decoder_init_memory(raw_data(data), len(data), nil, &chime.decoder) != .SUCCESS {
        ma.engine_uninit(&chime.engine)
        chime.failed = true
        return false
    }
    if ma.sound_init_from_data_source(&chime.engine, cast(^ma.data_source)&chime.decoder, {}, nil, &chime.sound) != .SUCCESS {
        ma.decoder_uninit(&chime.decoder)
        ma.engine_uninit(&chime.engine)
        chime.failed = true
        return false
    }
    chime.ready = true
    return true
}

play :: proc() {
    if !init_chime() do return
    ma.sound_seek_to_pcm_frame(&chime.sound, 0)
    ma.sound_start(&chime.sound)
}

// Call once on exit.
shutdown :: proc() {
    if !chime.ready do return
    ma.sound_uninit(&chime.sound)
    ma.decoder_uninit(&chime.decoder)
    ma.engine_uninit(&chime.engine)
    chime.ready = false
}
