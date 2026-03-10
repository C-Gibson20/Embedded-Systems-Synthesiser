// ================================================= //
// =================== Constants =================== //
// ================================================= //

constexpr uint32_t DISPLAY_INTERVAL = 100;
constexpr uint32_t SCAN_INTERVAL = 20;

//PCAL6408A Registers
constexpr uint8_t EXPANDER_ADDR = 0x21;
constexpr uint8_t REG_INPUT = 0x00;
constexpr uint8_t REG_PULL_EN = 0x43;
constexpr uint8_t REG_PULL_SEL = 0x44;
constexpr uint8_t REG_LAT_EN = 0x42;
constexpr uint8_t REG_INT_MASK = 0x45;

//Music Data
constexpr int DEFAULT_OCTAVE = 4;
constexpr double SAMPLING_RATE = 22'000.0;
constexpr uint64_t PHASE_MODULUS = 1ULL << 32;

constexpr const char* NOTE_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char* WAVE_NAMES[] = {"SQ","SW","TR","SI","SS","SF"};
constexpr float F_NOTES[] = {
    261.63f, 277.18f, 293.66f, 311.13f, // C, C#, D, D#
    329.63f, 349.23f, 369.99f, 392.00f, // E, F, F#, G
    415.30f, 440.00f, 466.16f, 493.88f  // G#, A, A#, B
};

constexpr uint32_t STEP_SIZES[] = {
    (uint32_t)(F_NOTES[0] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[1] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[2] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[3] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[4] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[5] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[6] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[7] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[8] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[9] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[10] * PHASE_MODULUS / SAMPLING_RATE),
    (uint32_t)(F_NOTES[11] * PHASE_MODULUS / SAMPLING_RATE)
};

//Knobs
enum class KnobIndex : uint8_t { PITCH = 0, WAVEFORM = 1, OCTAVE = 2, VOLUME = 3, MODE = 4 };

// Convenience aliases — remove when KnobManager is extracted (refactor step 4)
constexpr uint8_t pitchIdx        = static_cast<uint8_t>(KnobIndex::PITCH);
constexpr uint8_t waveIdx         = static_cast<uint8_t>(KnobIndex::WAVEFORM);
constexpr uint8_t octaveIdx       = static_cast<uint8_t>(KnobIndex::OCTAVE);
constexpr uint8_t volumeIdx       = static_cast<uint8_t>(KnobIndex::VOLUME);
constexpr uint8_t octaveOffsetIdx = static_cast<uint8_t>(KnobIndex::MODE);

//Sounds
constexpr uint8_t MAX_VOICES = 16;
constexpr int AUDIO_COMMAND_QUEUE_LENGTH = 32;
