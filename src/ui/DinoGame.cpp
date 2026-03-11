#include "DinoGame.h"

DinoGame dinoGame;

// ================================================= //
// =================== Sprites ==================== //
// ================================================= //

// Dino: 10w x 12h, 2 bytes per row (LSB = leftmost pixel)
// Frame 1: left foot down, Frame 2: right foot down
static const uint8_t dino_run1[] = {
    0x78, 0x00,  //  ....####..  head top
    0xFC, 0x00,  //  ..######..  head
    0xEC, 0x00,  //  ..##.###..  head + eye
    0xFC, 0x00,  //  ..######..  head
    0xFE, 0x01,  //  .#########  upper body
    0xFF, 0x03,  //  ##########  body
    0xFF, 0x03,  //  ##########  body
    0x7E, 0x00,  //  .######...  lower body
    0x00, 0x00,  //  ..........  gap
    0xC6, 0x00,  //  .##...##..  legs
    0xC6, 0x00,  //  .##...##..  legs
    0x06, 0x00,  //  .##.......  left foot down
};

static const uint8_t dino_run2[] = {
    0x78, 0x00,
    0xFC, 0x00,
    0xEC, 0x00,
    0xFC, 0x00,
    0xFE, 0x01,
    0xFF, 0x03,
    0xFF, 0x03,
    0x7E, 0x00,
    0x00, 0x00,
    0xC6, 0x00,
    0xC6, 0x00,
    0xC0, 0x00,  //  .......##.  right foot down
};

// Dino dead: eyes X
static const uint8_t dino_dead[] = {
    0x78, 0x00,
    0xFC, 0x00,
    0xAC, 0x00,  //  ..#.#.##..  X eyes
    0xFC, 0x00,
    0xFE, 0x01,
    0xFF, 0x03,
    0xFF, 0x03,
    0x7E, 0x00,
    0x00, 0x00,
    0xC6, 0x00,
    0xC6, 0x00,
    0xC6, 0x00,
};

// Cactus: 8w x 10h, 1 byte per row (LSB = leftmost pixel)
static const uint8_t cactus_bmp[] = {
    0x18,  //  ...##...  trunk
    0x78,  //  ...####.  trunk + right arm
    0x78,  //  ...####.
    0x7B,  //  ##.####.  left arm + trunk + right arm
    0x1B,  //  ##.##...  left arm + trunk
    0x1F,  //  #####...  left arm top merges trunk
    0x18,  //  ...##...  trunk
    0x18,
    0x18,
    0x18,
};

// ================================================= //
// ================== Game logic ================== //
// ================================================= //

void DinoGame::begin() {}

void DinoGame::tick(JoyState joy) {
    bool justPressedUp = (joy == JOY_UP && prevJoy_ != JOY_UP);

    if (state_ == DINO_IDLE   && justPressedUp) state_ = DINO_PLAYING;
    if (state_ == DINO_PLAYING) {
        updatePhysics(joy);
        updateObstacle();
        frameCount_++;
        if (checkCollision()) state_ = DINO_DEAD;
    }
    if (state_ == DINO_DEAD   && justPressedUp) reset();

    prevJoy_ = joy;
}

void DinoGame::updatePhysics(JoyState joy) {
    if (joy == JOY_UP && onGround_) {
        velY_ = JUMP_VEL;
        onGround_ = false;
    }
    velY_ += GRAVITY;
    dinoY_ += velY_;

    if (dinoY_ >= GROUND_Y - 12) {
        dinoY_ = GROUND_Y - 12;
        velY_ = 0;
        onGround_ = true;
    }
}

void DinoGame::updateObstacle() {
    obsX_ -= speed_;
    if (obsX_ < -8) {
        obsX_ = 128;
        score_++;
        obsCount_++;
        if (obsCount_ % 10 == 0) speed_++;
    }
}

bool DinoGame::checkCollision() {
    // 2px shrink on each side for forgiveness
    return (DINO_X + 2      < obsX_ + 8 - 2)        &&
           (DINO_X + 10 - 2 > obsX_  + 2)           &&
           (dinoY_ + 2      < GROUND_Y - 2)          &&
           (dinoY_ + 10     > GROUND_Y - 10 + 2);   // cactus height 10
}

void DinoGame::reset() {
    state_     = DINO_PLAYING;
    dinoY_     = GROUND_Y - 12;
    velY_      = 0;
    onGround_  = true;
    obsX_      = 128;
    score_     = 0;
    speed_     = 4;
    obsCount_  = 0;
    frameCount_= 0;
    prevJoy_   = JOY_NEUTRAL;
}

// ================================================= //
// ================== Rendering =================== //
// ================================================= //

void DinoGame::drawGround(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    u8g2.drawHLine(0, GROUND_Y,     128);
    u8g2.drawHLine(0, GROUND_Y + 1, 128);
}

void DinoGame::drawDino(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    const uint8_t *bmp;
    if (state_ == DINO_DEAD) {
        bmp = dino_dead;
    } else if (!onGround_) {
        bmp = dino_run1;  // fixed pose in the air
    } else {
        bmp = ((frameCount_ / 4) % 2 == 0) ? dino_run1 : dino_run2;
    }
    u8g2.drawXBMP(DINO_X, dinoY_, 10, 12, bmp);
}

void DinoGame::drawObstacle(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    u8g2.drawXBMP(obsX_, GROUND_Y - 10, 8, 10, cactus_bmp);
}

void DinoGame::drawScore(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    u8g2.setCursor(96, 8);
    u8g2.print(score_);
}

void DinoGame::render(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_4x6_tr);

    if (state_ == DINO_IDLE) {
        u8g2.setCursor(14, 20);
        u8g2.print("PUSH UP TO START");
    } else {
        drawGround(u8g2);
        drawDino(u8g2);
        drawObstacle(u8g2);
        drawScore(u8g2);

        if (state_ == DINO_DEAD) {
            u8g2.setCursor(34, 10);
            u8g2.print("GAME OVER");
        }
    }

    u8g2.sendBuffer();
}

void DinoGame::drawDino(bool ducking) {}
void DinoGame::drawObstacle() {}
void DinoGame::drawGround() {}
void DinoGame::drawScore() {}
