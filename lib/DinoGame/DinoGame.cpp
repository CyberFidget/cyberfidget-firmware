// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "DinoGame.h"
#include "HAL.h"
#include "MenuManager.h"

DinoGame dinoGame(HAL::buttonManager());

DinoGame* DinoGame::instance = nullptr;

namespace {

constexpr int kDinoX = 10;
constexpr int kGroundY = 48;
constexpr int kGroundTileCount = 4;

const cf::gfx::Animation* const kGroundAnimations[kGroundTileCount] = {
    cf::gfx::ground::anim_tile_a,
    cf::gfx::ground::anim_tile_b,
    cf::gfx::ground::anim_tile_c,
    cf::gfx::ground::anim_tile_d,
};

}  // namespace

DinoGame::DinoGame(ButtonManager& btnMgr)
  : display(HAL::displayProxy()),
    buttonManager(btnMgr),
    gameOver(false),
    dinoY(0),
    dinoVelocity(0),
    isJumping(false),
    isDucking(false),
    obstacleCount(0),
    obstaclesAvoidedCount(0),
    nextSpawnTime(0),
    obstacleSpeed(1.5f),
    minGapTime(800),    // minimal 0.8s between spawns
    maxGapTime(1800),   // up to 1.8s
    pterodactylUnlocked(false),
    pterodactylUnlockAt(5),
    groundScrollSpeed(1.0f)
{
    for(int i=0; i<MAX_OBSTACLES; i++){
        obstacles[i].x = -100;
        obstacles[i].isHigh = false;
        obstacles[i].passed = false;
    }

    dinoActor.setSheet(&cf::gfx::dino::sheet);
    dinoActor.play(cf::gfx::dino::anim_run, millis());
    dinoActor.setPos(kDinoX + 8, kGroundY);

    initGround();

    DinoGame::instance = this;
}

//---------------------------------------------------------------------------------
// Reset
//---------------------------------------------------------------------------------
void DinoGame::resetGame() {
    gameOver = false;
    dinoY = 0;
    dinoVelocity = 0;
    isJumping = false;
    isDucking = false;

    obstacleCount = 0;
    obstaclesAvoidedCount = 0;
    for(int i=0; i<MAX_OBSTACLES; i++){
        obstacles[i].x = -100;
        obstacles[i].isHigh = false;
        obstacles[i].passed = false;
        obstacles[i].actor = cf::gfx::Actor();
    }

    dinoActor = cf::gfx::Actor();
    dinoActor.setSheet(&cf::gfx::dino::sheet);
    dinoActor.setPos(kDinoX + 8, kGroundY);

    // Speed
    obstacleSpeed = 1.5f;
    groundScrollSpeed = 1.0f;

    // Next spawn
    unsigned long now = millis();
    dinoActor.play(cf::gfx::dino::anim_run, now);
    nextSpawnTime = now + random(minGapTime, maxGapTime);

    // Pterodactyl unlock logic
    int extra = random(0,4); // 0..3
    pterodactylUnlockAt = 5 + extra;
    pterodactylUnlocked = false;

    initGround();
}

void DinoGame::resetGame(uint32_t seed) {
    randomSeed(seed);
    resetGame();
}

//---------------------------------------------------------------------------------
// Ground initialization
//---------------------------------------------------------------------------------
void DinoGame::initGround() {
    // Fill groundSegs so they tile across [0..128]
    float xPos = 0;
    for(int i=0; i<GROUND_SEGMENTS; i++){
        groundSegs[i].x = xPos;
        groundSegs[i].tileIndex = random(0, kGroundTileCount);
        xPos += 16.0f; // each tile is 16 wide
    }
}

//---------------------------------------------------------------------------------
// Update
//---------------------------------------------------------------------------------
void DinoGame::update() {

    if(gameOver) return;

    setSpeedBySlider(sliderPosition_Percentage_Inverted_Filtered);
    updateDino();
    updateGround();
    updateObstacles();
    checkCollisions();
    draw();
}

//---------------------------------------------------------------------------------
// Draw
//---------------------------------------------------------------------------------
void DinoGame::draw() {
    display.clear();

    // 1) Draw ground first
    drawGround();

    // 2) Dino
    dinoActor.draw(display);

    // 3) Obstacles
    //    pterodactyl => y=24 (HIGH)
    //    cactus => y=32
    for(int i=0; i<obstacleCount; i++){
        obstacles[i].actor.draw(display);
    }

    // 4) Score or GameOver
    if(gameOver){
        display.drawString(35, 0, "GAME OVER!");
        display.drawString(30, 16, "Score: " + String(obstaclesAvoidedCount));
    } else {
        display.drawString(0, 0, "Score: " + String(obstaclesAvoidedCount));
    }

    display.display();
}

//---------------------------------------------------------------------------------
// Set speed via slider
//---------------------------------------------------------------------------------
void DinoGame::setSpeedBySlider(float sliderPercentage) {
    // Example: 1.0..5.0
    obstacleSpeed = 1.0f + (sliderPercentage/100.0f)*4.0f;
    // ground can scroll slightly slower for parallax
    groundScrollSpeed = 0.8f + (sliderPercentage/100.0f)*3.2f;
}

//---------------------------------------------------------------------------------
// Static button callbacks
//---------------------------------------------------------------------------------
void DinoGame::jumpButtonCallback(const ButtonEvent &ev) {
    if(ev.eventType == ButtonEvent_Pressed && instance) {
        instance->handleJump();
    }
}

void DinoGame::duckButtonCallback(const ButtonEvent &ev) {
    if(instance){
        instance->handleDuck(ev.eventType);
    }
}

void DinoGame::resetButtonCallback(const ButtonEvent &ev) {
    if(ev.eventType == ButtonEvent_Pressed && instance){
        instance->handleReset();
    }
}

void DinoGame::endButtonCallback(const ButtonEvent &ev) {
    if(ev.eventType == ButtonEvent_Released && instance){
        ESP_LOGI(TAG_MAIN, "onButtonBackPressed => calling end() + returning to menu...");
        instance->end();
        MenuManager::instance().returnToMenu();
    }
}

//---------------------------------------------------------------------------------
// Private event handlers
//---------------------------------------------------------------------------------
void DinoGame::handleJump() {
    if(!isJumping && !isDucking && dinoY == 0){
        isJumping = true;
        dinoVelocity = 5.0f;  // jump velocity
    }
}

void DinoGame::handleDuck(ButtonEventType evType) {
    if(evType == ButtonEvent_Pressed){
        if(!isJumping){
            isDucking = true;
        }
    } else if(evType == ButtonEvent_Released){
        isDucking = false;
    }
}

void DinoGame::handleReset() {
    resetGame();
}

//---------------------------------------------------------------------------------
// Dino Physics
//---------------------------------------------------------------------------------
void DinoGame::updateDino() {
    if(isJumping){
        dinoY += dinoVelocity;
        dinoVelocity -= 0.4f; // gravity
        if(dinoY < 0){
            dinoY = 0;
            isJumping = false;
            dinoVelocity = 0;
        }
    }

    unsigned long now = millis();
    const cf::gfx::Animation* animation = isJumping
        ? cf::gfx::dino::anim_jump
        : (isDucking ? cf::gfx::dino::anim_duck : cf::gfx::dino::anim_run);
    dinoActor.play(animation, now);
    dinoActor.update(now);
    dinoActor.setPos(kDinoX + 8, kGroundY - (int)dinoY);
}

//---------------------------------------------------------------------------------
// Ground
//---------------------------------------------------------------------------------
void DinoGame::updateGround() {
    // Scroll each segment left by groundScrollSpeed
    // If a segment goes off the left (x < -16), reposition it to the far right
    // with a new random tile, continuing the "endless" effect.
    float farRight = 0;
    for(int i=0; i<GROUND_SEGMENTS; i++){
        groundSegs[i].x -= groundScrollSpeed;
        if(groundSegs[i].x < -16){
            // find the current farthest right
            for(int j=0; j<GROUND_SEGMENTS; j++){
                if(groundSegs[j].x > farRight) {
                    farRight = groundSegs[j].x;
                }
            }
            // place this segment just beyond the farRight
            groundSegs[i].x = farRight + 16;
            groundSegs[i].tileIndex = random(0, kGroundTileCount);
        }
    }
}

void DinoGame::drawGround() {
    // Each segment is 16 wide, 8 high, drawn at y=56
    for(int i=0; i<GROUND_SEGMENTS; i++){
        int sx = (int)groundSegs[i].x;
        int tileIdx = groundSegs[i].tileIndex;
        const cf::gfx::Sprite* tile = kGroundAnimations[tileIdx]->frames[0];
        cf::gfx::drawSpritePivoted(display, *tile, sx + 8, 64);
    }
}

//---------------------------------------------------------------------------------
// Obstacles
//---------------------------------------------------------------------------------
void DinoGame::updateObstacles() {
    unsigned long now = millis();
    // If it's time to spawn the next obstacle
    if(now >= nextSpawnTime) {
        spawnObstacle();
        // pick new spawn time
        nextSpawnTime = now + random(minGapTime, maxGapTime);
    }

    // Move obstacles, track passing
    for(int i=0; i<obstacleCount; i++){
        obstacles[i].x -= obstacleSpeed;
        const int obstacleX = (int)obstacles[i].x;
        obstacles[i].actor.setPos(
            obstacleX + (obstacles[i].isHigh ? 8 : 4),
            obstacles[i].isHigh ? 36 : kGroundY);
        obstacles[i].actor.update(now);
        if(!obstacles[i].passed && obstacles[i].x < kDinoX){
            obstacles[i].passed = true;
            obstaclesAvoidedCount++;
            if(!pterodactylUnlocked && obstaclesAvoidedCount >= pterodactylUnlockAt){
                pterodactylUnlocked = true;
            }
        }
    }

    // Remove offscreen
    int newCount=0;
    for(int i=0; i<obstacleCount; i++){
        if(obstacles[i].x > -20){
            obstacles[newCount++] = obstacles[i];
        }
    }
    obstacleCount = newCount;
}

void DinoGame::spawnObstacle() {
    if(obstacleCount < MAX_OBSTACLES) {
        Obstacle obs;
        obs.x = 128;     // start at the right edge
        obs.passed = false;
        // If not unlocked => always cactus
        // else random
        if(!pterodactylUnlocked){
            obs.isHigh = false; 
        } else {
            obs.isHigh = (random(0,2) == 1);
        }

        obstacles[obstacleCount++] = obs;
        Obstacle& spawned = obstacles[obstacleCount - 1];
        spawned.actor.setSheet(spawned.isHigh
            ? &cf::gfx::pterodactyl::sheet
            : &cf::gfx::cactus::sheet);
        spawned.actor.play(spawned.isHigh
            ? cf::gfx::pterodactyl::anim_fly
            : cf::gfx::cactus::anim_idle, millis());
        spawned.actor.setPos(
            (int)spawned.x + (spawned.isHigh ? 8 : 4),
            spawned.isHigh ? 36 : kGroundY);
    }
}

//---------------------------------------------------------------------------------
// Pixel-perfect collisions
//---------------------------------------------------------------------------------
void DinoGame::checkCollisions() {
    if(obstacleCount == 0) return;

    for(int i=0; i<obstacleCount; i++){
        if(cf::gfx::pixelCollides(dinoActor, obstacles[i].actor)) {
            gameOver=true;
            return;
        }
    }
}

//---------------------------------------------------------------------------------
// Register button callbacks
//---------------------------------------------------------------------------------
void DinoGame::registerButtonCallbacks() {
    buttonManager.registerCallback(button_MiddleLeftIndex, jumpButtonCallback);
    buttonManager.registerCallback(button_MiddleRightIndex, duckButtonCallback);
    buttonManager.registerCallback(button_BottomRightIndex, resetButtonCallback);

    // Exit App
    buttonManager.registerCallback(button_BottomLeftIndex, endButtonCallback);
}

//---------------------------------------------------------------------------------
// Unregister button callbacks
//---------------------------------------------------------------------------------
void DinoGame::unregisterButtonCallbacks() {
    // Unregister callbacks
    buttonManager.unregisterCallback(button_MiddleLeftIndex);
    buttonManager.unregisterCallback(button_MiddleRightIndex);
    buttonManager.unregisterCallback(button_BottomRightIndex);
    
    buttonManager.unregisterCallback(button_BottomLeftIndex);
}

//---------------------------------------------------------------------------------
// Initialize or begin the app
//---------------------------------------------------------------------------------
void DinoGame::begin() {
    ESP_LOGI(TAG_MAIN, "begin() => registering callbacks...");
    registerButtonCallbacks();
    resetGame();
}

//---------------------------------------------------------------------------------
// End the app
//---------------------------------------------------------------------------------
void DinoGame::end() {
    ESP_LOGI(TAG_MAIN, "end() => unregistering callbacks...");
    unregisterButtonCallbacks();
}
