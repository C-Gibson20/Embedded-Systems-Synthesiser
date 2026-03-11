# Dino Game on the Synthesiser OLED

## Overview

A self-contained Chrome Dino clone running entirely on the STM32, rendered on the
128×32 OLED display. The joystick controls the dinosaur. No PC required.

```
Joystick Y-axis (JOYY_PIN A0)
        ↓
  scanKeysTask() — reads stick, writes JoyState to sysState
        ↓
  dinoTask() [~60ms loop] — game logic + U8g2 render
        ↓
  128×32 OLED display
```

---

## Display canvas

The SSD1305 display is **128 wide × 32 tall** pixels.

```
y=0  ┌────────────────────────────────────────────────────────────────┐
     │  score                                    sky / birds          │
     │                                                                │
     │   🦕                          🌵                              │
y=28 │_______________________________________________________________│  ← ground line
y=32 └────────────────────────────────────────────────────────────────┘
```

Layout:
| Element | Size (px) | Notes |
|---|---|---|
| Dino (standing) | 10 × 12 | Fixed x=8, y varies |
| Dino (ducking) | 14 × 8 | Shorter when ducking |
| Cactus | 6 × 14 | Single or triple variant |
| Ground line | 128 × 1 | y = 28 |
| Score | font 5×8 | top-right |

---

## Game mechanics

### Physics (frame-based, ~60ms tick)

```
GROUND_Y  = 28        // pixel row of ground
DINO_X    = 8         // fixed horizontal position
GRAVITY   = 3         // pixels/frame² downward acceleration
JUMP_VEL  = -10       // pixels/frame upward on jump
```

Jump arc at 60ms/frame:
- Frame 0: vel = -10 → moves up 10px
- Frame 1: vel = -7  → moves up 7px
- ...
- Lands after ~7 frames = ~420ms — feels right

### Obstacle system

- One obstacle active at a time (expandable)
- Spawns at x=128, moves left by `speed` pixels per frame
- `speed` starts at 4, increases by 0.5 every 10 obstacles
- Despawns when x < -8, increments score, spawns new one

### Collision detection

Axis-aligned bounding box (AABB):
```
dino_rect  = { x: DINO_X, y: dinoY,   w: 10, h: 12 }
cactus_rect= { x: obs.x,  y: GROUND_Y-14, w: 6, h: 14 }
overlap if: dino_rect.x < obs.x + obs.w  AND
            dino_rect.x + dino_rect.w > obs.x AND
            dino_rect.y < obs.y + obs.h  AND
            dino_rect.y + dino_rect.h > obs.y
```

Shrink collision boxes by 2px on each side (more forgiving feel).

### States

```cpp
enum DinoState { DINO_IDLE, DINO_PLAYING, DINO_DEAD };
```

- **IDLE**: "PUSH UP TO START" text, dino standing still
- **PLAYING**: game loop running
- **DEAD**: "GAME OVER" + score, push up to restart

---

## Input

Joystick already wired to `JOYY_PIN = A0`. Read in `scanKeysTask()` as before.
Add `joyState` to `sysState` so `dinoTask()` can read it.

```cpp
// In SysState.h
enum JoyState { JOY_NEUTRAL, JOY_UP, JOY_DOWN };

struct SysState {
    ...
    volatile JoyState joyState = JOY_NEUTRAL;  // written by scanKeysTask, read by dinoTask
};
```

In `scanKeysTask()`:
```cpp
uint16_t joyY = analogRead(JOYY_PIN);
if      (joyY > 3000) sysState.joyState = JOY_UP;
else if (joyY < 1000) sysState.joyState = JOY_DOWN;
else                  sysState.joyState = JOY_NEUTRAL;
```

`joyState` is a single enum (≤ 4 bytes) so the write is atomic — no mutex needed.

---

## New files

| File | Purpose |
|---|---|
| `src/ui/DinoGame.h` | `DinoGame` class declaration |
| `src/ui/DinoGame.cpp` | Game logic + U8g2 rendering |

### `DinoGame` class

```cpp
class DinoGame {
public:
    void begin();
    void tick(JoyState joy);   // call every ~60ms
    void render(U8G2 &u8g2);

private:
    DinoState  state_  = DINO_IDLE;
    int16_t    dinoY_  = GROUND_Y - 12;   // top of dino
    int16_t    velY_   = 0;
    bool       onGround_ = true;

    int16_t    obsX_   = 128;
    uint8_t    score_  = 0;
    uint8_t    speed_  = 4;
    uint8_t    obsCount_ = 0;

    void updatePhysics(JoyState joy);
    void updateObstacle();
    bool checkCollision();
    void drawDino(bool ducking);
    void drawObstacle();
    void drawGround();
    void drawScore();
    void reset();
};
```

---

## FreeRTOS task

Add a `dinoTask()` that replaces `displayUpdateTask()` when dino mode is active.
Use a `#define DINO_MODE` build flag in `platformio.ini` to switch between normal synth display and dino game.

```cpp
// In main.cpp
void dinoTask(void* pvParameters) {
    const TickType_t xFrequency = 60 / portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    dinoGame.begin();

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        JoyState joy = sysState.joyState;  // atomic read
        dinoGame.tick(joy);
    }
}
```

In `initialiseThreads()`:
```cpp
#ifdef DINO_MODE
    xTaskCreate(dinoTask, "dino", 256, NULL, 1, &dinoHandle);
#else
    xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
#endif
```

Priority 1 — same as the existing display task (lowest, non-critical).

---

## Rendering with U8g2

`dinoGame.render()` is called inside `tick()`. U8g2 calls needed:

```cpp
u8g2.clearBuffer();

drawGround();    // u8g2.drawHLine(0, GROUND_Y, 128)
drawDino();      // u8g2.drawBox(x, y, w, h) for body + legs
drawObstacle();  // u8g2.drawBox(obsX, GROUND_Y - 14, 6, 14)
drawScore();     // u8g2.setFont(...); u8g2.setCursor(90, 8); u8g2.print(score)

u8g2.sendBuffer();
```

Use `drawBox` and `drawLine` — no bitmap needed for a minimal version.
Add simple pixel-art sprites later using `drawXBMP` if desired.

---

## Files to change

| File | Change |
|---|---|
| `include/SysState.h` | Add `JoyState` enum + `joyState` field to `SysState` |
| `src/main.cpp` | Add joystick read in `scanKeysTask()`; add `dinoTask`; swap display task under `#ifdef DINO_MODE` |
| `src/ui/DinoGame.h` | New file — class declaration |
| `src/ui/DinoGame.cpp` | New file — game logic + rendering |
| `platformio.ini` | Add `DINO_MODE` to `build_flags` to enable |

No changes to audio, CAN, or Synth.

---

## Build flag

In `platformio.ini`, add to `build_flags`:
```ini
-D DINO_MODE
```

Remove it to go back to normal synth display.

---

## Pixel art sprites (optional upgrade)

U8g2 supports `drawXBMP(x, y, w, h, bitmap)` for custom bitmaps.
Small 10×12 dino sprite (XBM format, defined as `const uint8_t PROGMEM dino_bmp[]`):

```
  ██████
  ███ ██
  ██████
  ████
 ██████
 ██  ██
 ██
```

Can be drawn as a series of `drawBox` calls first, then replaced with a real bitmap sprite later.

---

## Implementation guide

Work through these steps in order. Each one is independently testable before moving on.

---

### Step 1 — Add `JoyState` to `SysState.h` and read the joystick

**File:** `include/SysState.h`

Add the enum above the struct, and one field inside it:

```cpp
enum JoyState { JOY_NEUTRAL, JOY_UP, JOY_DOWN };

struct SysState {
    ...
    volatile JoyState joyState = JOY_NEUTRAL;  // ← add this line
};
```

**File:** `src/main.cpp` — inside `scanKeysTask()`, after the existing key scan block:

```cpp
uint16_t joyY = analogRead(JOYY_PIN);
if      (joyY > 3000) sysState.joyState = JOY_UP;
else if (joyY < 1000) sysState.joyState = JOY_DOWN;
else                  sysState.joyState = JOY_NEUTRAL;
```

**Test:** Temporarily print `sysState.joyState` in `displayUpdateTask` to confirm values
change as you tilt the stick. Tune the 3000 / 1000 thresholds if needed — actual ADC
values depend on your hardware.

---

### Step 2 — Create `DinoGame.h` and `DinoGame.cpp` stubs

**File:** `src/ui/DinoGame.h` — create with the full class declaration (see class listing above).

**File:** `src/ui/DinoGame.cpp` — create with empty method bodies so it compiles:

```cpp
#include "DinoGame.h"

void DinoGame::begin()  {}
void DinoGame::tick(JoyState joy) {}
// ... one empty stub per method
```

**Test:** Just confirm it compiles with no errors before writing any logic.

---

### Step 3 — Draw the static scene

Implement `render()` in `DinoGame.cpp` with hardcoded positions — no movement yet:

```cpp
void DinoGame::render(U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C &u8g2) {
    u8g2.clearBuffer();
    u8g2.drawHLine(0, 28, 128);          // ground
    u8g2.drawBox(8, 16, 10, 12);         // dino (static rectangle)
    u8g2.drawBox(110, 14, 6, 14);        // cactus (static)
    u8g2.sendBuffer();
}
```

Hook it up temporarily inside `displayUpdateTask` in `Display.cpp` to see it on screen.
**Get pixels on the display before worrying about any logic.**

---

### Step 4 — Add the `dinoTask` and build flag

**File:** `platformio.ini` — add to `build_flags`:

```ini
-D DINO_MODE
```

**File:** `src/main.cpp` — add the task function (before `setup()`):

```cpp
#ifdef DINO_MODE
void dinoTask(void* pvParameters) {
    const TickType_t xFrequency = 60 / portTICK_PERIOD_MS;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    dinoGame.begin();
    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        JoyState joy = sysState.joyState;
        dinoGame.tick(joy);
    }
}
#endif
```

Inside `initialiseThreads()`, swap the display task:

```cpp
#ifdef DINO_MODE
    xTaskCreate(dinoTask, "dino", 256, NULL, 1, &dinoHandle);
#else
    xTaskCreate(displayUpdateTask, "displayUpdate", 256, NULL, 1, &displayUpdateHandle);
#endif
```

Also declare `DinoGame dinoGame;` as a global alongside `SysState sysState;`.

**Test:** Confirm the static scene still renders, now driven by `dinoTask`.

---

### Step 5 — Add jump physics

Implement `updatePhysics()` in `DinoGame.cpp`:

```cpp
void DinoGame::updatePhysics(JoyState joy) {
    if (joy == JOY_UP && onGround_) {
        velY_ = -10;
        onGround_ = false;
    }
    velY_ += 3;           // gravity
    dinoY_ += velY_;

    if (dinoY_ >= GROUND_Y - 12) {
        dinoY_ = GROUND_Y - 12;
        velY_ = 0;
        onGround_ = true;
    }
}
```

Call it from `tick()`, then use `dinoY_` in `render()` instead of the hardcoded 16.

**Test:** Tilt stick up — dino should jump and land. Tune `GRAVITY` (3) and `JUMP_VEL` (-10)
until the arc feels right on the 32px tall screen.

---

### Step 6 — Moving obstacle, score, and collision

Only once the jump feels right, add the rest:

**Obstacle movement** in `updateObstacle()`:
```cpp
obsX_ -= speed_;
if (obsX_ < -8) {
    obsX_ = 128;
    score_++;
    obsCount_++;
    if (obsCount_ % 10 == 0) speed_++;  // speed up every 10 obstacles
}
```

**Collision** in `checkCollision()` — AABB with 2px shrink for forgiveness:
```cpp
bool DinoGame::checkCollision() {
    return (DINO_X + 2      < obsX_ + 6 - 2) &&
           (DINO_X + 10 - 2 > obsX_ + 2)     &&
           (dinoY_ + 2      < GROUND_Y - 14 + 14 - 2) &&
           (dinoY_ + 12 - 2 > GROUND_Y - 14 + 2);
}
```

**State transitions** in `tick()`:
```cpp
void DinoGame::tick(JoyState joy) {
    if (state_ == DINO_IDLE && joy == JOY_UP) state_ = DINO_PLAYING;
    if (state_ == DINO_PLAYING) {
        updatePhysics(joy);
        updateObstacle();
        if (checkCollision()) state_ = DINO_DEAD;
    }
    if (state_ == DINO_DEAD && joy == JOY_UP) reset();
    render();
}
```

---

### Testing steps

1. After Step 1: serial/display shows correct `joyState` on stick tilt.
2. After Step 3: static dino + cactus visible on OLED.
3. After Step 4: same scene, now from `dinoTask` at 60ms.
4. After Step 5: dino jumps and lands — tune physics constants.
5. After Step 6: obstacle moves, score increments, collision kills dino.
6. Remove `-D DINO_MODE` from `platformio.ini` to restore normal synth display.
