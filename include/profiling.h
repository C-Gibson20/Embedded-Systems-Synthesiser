#pragma once

// Comment out to disable profiling mode
#define PROFILING_MODE

#ifdef PROFILING_MODE
    #define DISABLE_THREADS
    #define DISABLE_ISRS

    #define PROFILE_SCANKEYS
    #define PROFILE_DISPLAY
    #define PROFILE_DECODE
    #define PROFILE_CAN_TX

    #define PROFILE_SAMPLE_ISR
    #define PROFILE_CAN_RX_ISR
    #define PROFILE_CAN_TX_ISR

    #ifdef I2C_EXPANDER_KNOBS
        #define PROFILE_KNOB
        #define PROFILE_KNOB_ISR
    #endif
#endif
