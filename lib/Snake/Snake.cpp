#include "Snake.h"
#include "globals.h"
#include "HAL.h"
#include "MenuManager.h"
#include "RGBController.h"

Snake snakeApp(HAL::buttonManager());
Snake* Snake::instance = nullptr;

Snake::Snake(ButtonManager& btnMgr)
    : buttonManager(btnMgr), display(HAL::displayProxy()) {
    instance = this;
}

void Snake::begin() {
    registerButtonCallbacks();
    restartGame();
}

void Snake::restartGame() {
    // Place snake in the middle, length 3, heading right
    headIndex = 2;
    length = 3;
    body[0] = {5, 4};
    body[1] = {6, 4};
    body[2] = {7, 4};

    dir = DIR_RIGHT;
    nextDir = DIR_RIGHT;
    state = STATE_PLAYING;
    score = 0;
    moveInterval = 200;
    lastMoveTime = millis();
    dirChanged = false;

    rippleActive = false;
    rippleStep = 0;
    rippleStepTime = 0;

    spawnFood();
}

// Place food at a random cell not occupied by the snake
void Snake::spawnFood() {
    bool occupied = true;
    while (occupied) {
        food.x = (int8_t)(random(0, SNAKE_COLS));
        food.y = (int8_t)(random(0, SNAKE_ROWS));
        occupied = false;
        for (int i = 0; i < length; i++) {
            int idx = ((headIndex - i) + SNAKE_MAX_LENGTH) % SNAKE_MAX_LENGTH;
            if (body[idx].x == food.x && body[idx].y == food.y) {
                occupied = true;
                break;
            }
        }
    }
}

void Snake::moveSnake() {
    dir = nextDir;
    dirChanged = false;

    // Compute new head position
    SnakeCell newHead = body[headIndex];
    switch (dir) {
        case DIR_UP:    newHead.y -= 1; break;
        case DIR_DOWN:  newHead.y += 1; break;
        case DIR_LEFT:  newHead.x -= 1; break;
        case DIR_RIGHT: newHead.x += 1; break;
    }

    // Wrap around walls
    newHead.x = (int8_t)(((int)newHead.x + SNAKE_COLS) % SNAKE_COLS);
    newHead.y = (int8_t)(((int)newHead.y + SNAKE_ROWS) % SNAKE_ROWS);

    // Advance head in ring buffer
    headIndex = (headIndex + 1) % SNAKE_MAX_LENGTH;
    body[headIndex] = newHead;

    bool ate = (newHead.x == food.x && newHead.y == food.y);
    if (ate) {
        length++;
        score++;
        // Speed up slightly each food eaten
        if (moveInterval > 80) moveInterval -= 8;
        if (length >= SNAKE_MAX_LENGTH) {
            state = STATE_WIN;
            return;
        }
        spawnFood();
        triggerRipple(); // kick off the top-to-bottom LED ripple
    }

    // Check self-collision (new head vs rest of body)
    for (int i = 1; i < length; i++) {
        int idx = ((headIndex - i) + SNAKE_MAX_LENGTH) % SNAKE_MAX_LENGTH;
        if (body[idx].x == newHead.x && body[idx].y == newHead.y) {
            state = STATE_DEAD;
            deathTime = millis();
            return;
        }
    }
}

void Snake::drawGrid() {
    display.setColor(WHITE);

    // Draw food as a filled 6x6 square centered in its cell
    int fx = food.x * SNAKE_CELL + 1;
    int fy = food.y * SNAKE_CELL + 1;
    display.fillRect(fx, fy, 6, 6);

    // Draw snake body cells
    for (int i = 0; i < length; i++) {
        int idx = ((headIndex - i) + SNAKE_MAX_LENGTH) % SNAKE_MAX_LENGTH;
        int px = body[idx].x * SNAKE_CELL;
        int py = body[idx].y * SNAKE_CELL;
        if (i == 0) {
            // Head: solid fill
            display.fillRect(px + 1, py + 1, 6, 6);
            // Eyes on head depending on direction
            display.setColor(BLACK);
            if (dir == DIR_RIGHT) {
                display.setPixel(px + 5, py + 2);
                display.setPixel(px + 5, py + 5);
            } else if (dir == DIR_LEFT) {
                display.setPixel(px + 2, py + 2);
                display.setPixel(px + 2, py + 5);
            } else if (dir == DIR_UP) {
                display.setPixel(px + 2, py + 2);
                display.setPixel(px + 5, py + 2);
            } else {
                display.setPixel(px + 2, py + 5);
                display.setPixel(px + 5, py + 5);
            }
            display.setColor(WHITE);
        } else {
            // Body: filled with a 1px inner border to show segments
            display.fillRect(px + 1, py + 1, 6, 6);
            display.setColor(BLACK);
            display.drawRect(px + 1, py + 1, 6, 6);
            display.setColor(WHITE);
        }
    }

    // Score top-right
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(127, 0, String(score));
}

void Snake::drawDeadScreen() {
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 12, "GAME OVER");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 32, "Score: " + String(score));
    display.drawString(64, 44, "Restart");
}

// Start a new ripple animation from the top LED
void Snake::triggerRipple() {
    rippleActive = true;
    rippleStep = 0;
    rippleStepTime = millis();
}

void Snake::updateLeds() {
    if (state == STATE_DEAD) {
        // All front LEDs dim, back LED red
        HAL::setRgbLed(pixel_Front_Top,    0, 30, 0, 0);
        HAL::setRgbLed(pixel_Front_Middle, 0, 30, 0, 0);
        HAL::setRgbLed(pixel_Front_Bottom, 0, 30, 0, 0);
        HAL::setRgbLed(pixel_Back,         0, 80, 0, 0);
        rippleActive = false;
        return;
    }

    // Baseline green glow grows with score
    uint8_t baseGreen = (uint8_t)min(40 + score * 8, 200);

    if (rippleActive) {
        unsigned long now = millis();

        // Advance ripple step when enough time has passed
        if (now - rippleStepTime >= RIPPLE_STEP_MS) {
            rippleStep++;
            rippleStepTime = now;
            if (rippleStep >= RIPPLE_STEPS) {
                rippleActive = false;
            }
        }

        if (rippleActive) {
            // The active step lights up bright white; others show baseline green
            uint8_t topG    = (rippleStep == 0) ? 0       : baseGreen;
            uint8_t topW    = (rippleStep == 0) ? 220     : 0;
            uint8_t midG    = (rippleStep == 1) ? 0       : baseGreen;
            uint8_t midW    = (rippleStep == 1) ? 220     : 0;
            uint8_t botG    = (rippleStep == 2) ? 0       : baseGreen;
            uint8_t botW    = (rippleStep == 2) ? 220     : 0;

            HAL::setRgbLed(pixel_Front_Top,    topG, 0, 0, topW);
            HAL::setRgbLed(pixel_Front_Middle, midG, 0, 0, midW);
            HAL::setRgbLed(pixel_Front_Bottom, botG, 0, 0, botW);
            HAL::setRgbLed(pixel_Back,         0,    0, 0, 0);
            return;
        }
    }

    // Default: all three front LEDs glow green based on score
    HAL::setRgbLed(pixel_Front_Top,    baseGreen, 0, 0, 0);
    HAL::setRgbLed(pixel_Front_Middle, baseGreen, 0, 0, 0);
    HAL::setRgbLed(pixel_Front_Bottom, baseGreen, 0, 0, 0);
    HAL::setRgbLed(pixel_Back,         0,         0, 0, 0);
}

void Snake::update() {
    display.clear();

    if (state == STATE_PLAYING) {
        unsigned long now = millis();
        if (now - lastMoveTime >= moveInterval) {
            lastMoveTime = now;
            moveSnake();
        }
        drawGrid();
    } else if (state == STATE_DEAD || state == STATE_WIN) {
        if (state == STATE_WIN) {
            display.setFont(ArialMT_Plain_16);
            display.setTextAlignment(TEXT_ALIGN_CENTER);
            display.drawString(64, 12, "YOU WIN!");
            display.setFont(ArialMT_Plain_10);
            display.drawString(64, 32, "Score: " + String(score));
            display.drawString(64, 44, "[●] Restart");
        } else {
            drawDeadScreen();
        }
    }

    display.display();
    updateLeds();
}

void Snake::end() {
    unregisterButtonCallbacks();
    setColorsOff();
}

// --- Button callbacks ---

void Snake::onButtonUp(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (!instance->dirChanged && instance->dir != DIR_DOWN) {
        instance->nextDir = DIR_UP;
        instance->dirChanged = true;
    }
}

void Snake::onButtonDown(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (!instance->dirChanged && instance->dir != DIR_UP) {
        instance->nextDir = DIR_DOWN;
        instance->dirChanged = true;
    }
}

void Snake::onButtonLeft(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (!instance->dirChanged && instance->dir != DIR_RIGHT) {
        instance->nextDir = DIR_LEFT;
        instance->dirChanged = true;
    }
}

void Snake::onButtonRight(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (!instance->dirChanged && instance->dir != DIR_LEFT) {
        instance->nextDir = DIR_RIGHT;
        instance->dirChanged = true;
    }
}

void Snake::onButtonEnter(const ButtonEvent& e) {
    if (e.eventType != ButtonEvent_Pressed) return;
    if (instance->state == STATE_DEAD || instance->state == STATE_WIN) {
        instance->restartGame();
    }
}

void Snake::onButtonBack(const ButtonEvent& e) {
    if (e.eventType == ButtonEvent_Released) {
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

void Snake::registerButtonCallbacks() {
    buttonManager.registerCallback(button_UpIndex,     onButtonUp);
    buttonManager.registerCallback(button_DownIndex,   onButtonDown);
    buttonManager.registerCallback(button_LeftIndex,   onButtonLeft);
    buttonManager.registerCallback(button_RightIndex,  onButtonRight);
    buttonManager.registerCallback(button_EnterIndex,  onButtonEnter);
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBack);
}

void Snake::unregisterButtonCallbacks() {
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}