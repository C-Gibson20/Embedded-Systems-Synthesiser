#pragma once
#include <U8g2lib.h>
#include "SysState.h"

constexpr int16_t GROUND_Y  = 28;
constexpr int16_t DINO_X    = 8;
constexpr int16_t GRAVITY   = 1;
constexpr int16_t JUMP_VEL  = -5;

enum DinoState { DINO_IDLE, DINO_PLAYING, DINO_DEAD };

class DinoGame {
public:
    void begin();
    void tick(JoyState joy);
    void render(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2);

private:
    DinoState state_    = DINO_IDLE;
    int16_t   dinoY_    = GROUND_Y - 12;
    int16_t   velY_     = 0;
    bool      onGround_ = true;

    int16_t   obsX_      = 128;
    uint8_t   score_     = 0;
    uint8_t   speed_     = 4;
    uint8_t   obsCount_  = 0;
    uint8_t   frameCount_= 0;
    JoyState  prevJoy_   = JOY_NEUTRAL;

    void updatePhysics(JoyState joy);
    void updateObstacle();
    bool checkCollision();
    void drawDino(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2);
    void drawObstacle(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2);
    void drawGround(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2);
    void drawScore(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2);
    void drawDino(bool ducking);    // stub — unused
    void drawObstacle();            // stub — unused
    void drawGround();              // stub — unused
    void drawScore();               // stub — unused
    void reset();
};

extern DinoGame dinoGame;
