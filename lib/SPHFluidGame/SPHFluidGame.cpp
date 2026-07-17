#include "SPHFluidGame.h"
#include "HAL.h"

SPHFluidGame sphFluidGame(HAL::buttonManager()); // App Manager Integration
SPHFluidGame* SPHFluidGame::instance = nullptr; // App Manager Integration

// Held duration (ms) required for Enter to open the settings menu.
static const unsigned long ENTER_HOLD_MS = 600;

//--------------------------------------------------------------------------------
// Constructor
//--------------------------------------------------------------------------------
SPHFluidGame::SPHFluidGame(ButtonManager& btnMgr)
    : display(HAL::displayProxy()),
    buttonManager(btnMgr) // AppManager Integration
{
    // Initialize simulation parameters
    currentCount     = 100;        // Default to 100 particles
    numParticles     = 100;        // Try small first to keep it real-time on MCU
    smoothingLength  = 5.0f;       // h
    restDensity      = 1.0f;       // ρ0
    stiffness        = 2.0f;       // k
    viscosity        = 1.6f;       // μ  (higher = thicker, more liquid-like)
    gravityX         = 0.0f;
    gravityY         = 9.8f;       // Earth-like gravity
    damping          = 0.99f;
    dt               = 0.1f;
    screenWidth      = 128;
    screenHeight     = 64;

    // Surface tension "stickiness" parameter — stronger pull keeps the fluid together
    cohesionStrength = 0.30f;  // Tweak to achieve desired "stickiness"

    // How big the particles are on screen, 
    particleRadius   = 2.0f; 

    // ---- Metaball rendering config ----
    blobThreshold    = 0.90f;
    renderStep       = 1;     // sample every pixel; raise to 2 for more speed

    // ---- UI state ----
    appScreen        = SCREEN_START_MENU;
    renderMode       = RENDER_BLOBS;
    startMenuIndex   = 0;
    settingsIndex    = 0;

    // ---- Enter long-hold tracking ----
    enterDown        = false;
    enterPressTime   = 0;
    enterHoldFired   = false;

    resetParticles();
    instance = this; // AppManager Integration   
}

//--------------------------------------------------------------------------------
// Main update function — dispatches based on active screen
//--------------------------------------------------------------------------------
void SPHFluidGame::update()
{
    // Detect an Enter long-hold here, based on how long the button has been down.
    // This avoids relying on any event.duration field.
    if (enterDown && !enterHoldFired &&
        (millis() - enterPressTime >= ENTER_HOLD_MS) &&
        appScreen != SCREEN_SETTINGS) {
        appScreen = SCREEN_SETTINGS;
        settingsIndex = 0;
        enterHoldFired = true;  // don't fire again for this same hold
    }

    switch (appScreen) {
        case SCREEN_START_MENU:
            renderStartMenu();
            break;

        case SCREEN_SIMULATION: {
            // Slider adjusts particle count live during the simulation.
            int targetCount = sliderPosition_Percentage_Inverted_Filtered;
            if (targetCount != currentCount) {
                setParticleCount(targetCount);
                currentCount = targetCount;
            }
            stepSimulation();
            render();
            break;
        }

        case SCREEN_SETTINGS:
            renderSettings();
            break;
    }
}

//--------------------------------------------------------------------------------
// Run one physics step (no rendering)
//--------------------------------------------------------------------------------
void SPHFluidGame::stepSimulation()
{
    // Combine tilt acceleration with gravity
    float ax = gravityX + accelX * -0.10f;
    float ay = gravityY + accelY * 0.10f;

    computeDensityPressure();
    computeForces(ax, ay);
    integrate();
    resolveParticleCollisions();
}

//--------------------------------------------------------------------------------
// Reset particles with random positions
//--------------------------------------------------------------------------------
void SPHFluidGame::resetParticles()
{
    particles.clear();
    particles.reserve(numParticles);

    for (int i = 0; i < numParticles; i++) {
        Particle p;
        p.x        = random(screenWidth);
        p.y        = random(screenHeight);
        p.vx       = 0.0f;
        p.vy       = 0.0f;
        p.density  = restDensity;
        p.pressure = 0.0f;
        particles.push_back(p);
    }
}

//--------------------------------------------------------------------------------
// Adjust the number of rendered particles
//--------------------------------------------------------------------------------
void SPHFluidGame::setParticleCount(int newCount) {
    if (newCount < 1) {
        newCount = 1;
    }
    numParticles = newCount;
    resetParticles();
}

//--------------------------------------------------------------------------------
// 1) Compute density & pressure
//--------------------------------------------------------------------------------
void SPHFluidGame::computeDensityPressure()
{
    float h2      = smoothingLength * smoothingLength;
    float mass    = 1.0f;  // Uniform mass per particle
    float poly6K  = 315.0f / (64.0f * (float)M_PI * powf(smoothingLength, 9));

    for (int i = 0; i < numParticles; i++) {
        Particle &pi = particles[i];
        pi.density   = 0.0f;

        for (int j = 0; j < numParticles; j++) {
            Particle &pj = particles[j];

            float dx = pi.x - pj.x;
            float dy = pi.y - pj.y;
            float r2 = dx*dx + dy*dy;

            if (r2 < h2) {
                float t = (h2 - r2);
                pi.density += mass * poly6K * t * t * t;
            }
        }
        pi.pressure = stiffness * (pi.density - restDensity);
        if (pi.pressure < 0.0f) {
            pi.pressure = 0.0f;
        }
    }
}

//--------------------------------------------------------------------------------
// 2) Compute forces (pressure, viscosity, and surface tension / cohesion)
//--------------------------------------------------------------------------------
void SPHFluidGame::computeForces(float ax, float ay)
{
    float h      = smoothingLength;
    float h2     = h * h;
    float mass   = 1.0f;
    float spikyK = -45.0f / ((float)M_PI * powf(h, 6));
    float viscoK = 45.0f  / ((float)M_PI * powf(h, 6));

    for (int i = 0; i < numParticles; i++) {
        Particle &pi = particles[i];

        float fx = 0.0f;
        float fy = 0.0f;

        for (int j = 0; j < numParticles; j++) {
            if (i == j) continue;
            Particle &pj = particles[j];

            float dx  = pi.x - pj.x;
            float dy  = pi.y - pj.y;
            float r2  = dx*dx + dy*dy;

            if (r2 < h2 && r2 > 0.000001f) {
                float r    = sqrtf(r2);
                float invR = 1.0f / r;

                // Pressure force
                float avgP  = (pi.pressure + pj.pressure) / 2.0f;
                float hr    = h - r;
                float spiky = spikyK * hr * hr;
                float fPres = mass * avgP / pj.density * spiky; 

                fx += fPres * dx * invR;
                fy += fPres * dy * invR;

                // Viscosity force
                float vijx   = pj.vx - pi.vx;
                float vijy   = pj.vy - pi.vy;
                float lapVis = viscoK * (h - r);

                fx += viscosity * vijx * lapVis;
                fy += viscosity * vijy * lapVis;

                // Cohesion / surface tension
                float q    = r / h;
                float shape = q * (1.0f - q);
                float pull = cohesionStrength * shape * 4.0f;
                fx -= pull * (dx * invR);
                fy -= pull * (dy * invR);
            }
        }

        fx += ax * pi.density;
        fy += ay * pi.density;

        float invDen = 1.0f / pi.density;
        pi.vx += fx * invDen * dt;
        pi.vy += fy * invDen * dt;
    }
}

//--------------------------------------------------------------------------------
// 3) Integrate positions and apply boundary damping
//--------------------------------------------------------------------------------
void SPHFluidGame::integrate()
{
    for (int i = 0; i < numParticles; i++) {
        Particle &p = particles[i];

        p.x += p.vx * dt;
        p.y += p.vy * dt;

        if (p.x < 0) {
            p.x = 0;
            p.vx *= -damping;
        } else if (p.x >= screenWidth) {
            p.x = screenWidth - 1;
            p.vx *= -damping;
        }

        if (p.y < 0) {
            p.y = 0;
            p.vy *= -damping;
        } else if (p.y >= screenHeight) {
            p.y = screenHeight - 1;
            p.vy *= -damping;
        }
    }
}

//--------------------------------------------------------------------------------
// 3.5) Check for particle collisions
//--------------------------------------------------------------------------------
void SPHFluidGame::resolveParticleCollisions() {
    float minDist = 1.2f * particleRadius;
    float minDistSq = minDist * minDist;

    for (int i = 0; i < numParticles; i++) {
        for (int j = i + 1; j < numParticles; j++) {
            float dx = particles[j].x - particles[i].x;
            float dy = particles[j].y - particles[i].y;
            float distSq = dx * dx + dy * dy;

            if (distSq < minDistSq && distSq > 0.000001f) {
                float dist = sqrtf(distSq);
                float overlap = 0.5f * (minDist - dist);

                dx /= dist;
                dy /= dist;

                particles[i].x -= dx * overlap;
                particles[i].y -= dy * overlap;
                particles[j].x += dx * overlap;
                particles[j].y += dy * overlap;
            }
        }
    }
}

//--------------------------------------------------------------------------------
// 4) Render dispatch
//--------------------------------------------------------------------------------
void SPHFluidGame::render()
{
    if (renderMode == RENDER_BLOBS) {
        renderBlobs();
    } else {
        renderParticles();
    }
}

//--------------------------------------------------------------------------------
// Blobby metaball rendering — connected liquid look
//--------------------------------------------------------------------------------
void SPHFluidGame::renderBlobs()
{
    display.clear();

    const float influence      = particleRadius * 2.6f;
    const float influenceSq     = influence * influence;
    const float invInfluenceSq  = 1.0f / influenceSq;
    const int   reach           = (int)ceilf(influence); // integer half-width of a particle's influence box
    const int   fieldSize       = screenWidth * screenHeight;

    // The field accumulator is allocated once — the screen size never changes.
    if ((int)blobField.size() != fieldSize) {
        blobField.assign(fieldSize, 0.0f);
    } else {
        std::fill(blobField.begin(), blobField.end(), 0.0f);
    }

    // Scatter: rather than testing every pixel against every particle, each
    // particle deposits its metaball contribution only into the small box of
    // pixels it can actually influence. This turns an O(pixels * particles)
    // pass into O(particles * boxArea) — the change that makes blobs mode
    // real-time. The set/unset outcome per pixel is identical to the old
    // gather (the previous early-break only saved work, it never flipped a
    // pixel's threshold result).
    for (int i = 0; i < numParticles; i++) {
        const float cx = particles[i].x;
        const float cy = particles[i].y;

        int x0 = (int)cx - reach; if (x0 < 0) x0 = 0;
        int x1 = (int)cx + reach; if (x1 >= screenWidth)  x1 = screenWidth  - 1;
        int y0 = (int)cy - reach; if (y0 < 0) y0 = 0;
        int y1 = (int)cy + reach; if (y1 >= screenHeight) y1 = screenHeight - 1;

        for (int py = y0; py <= y1; py++) {
            const float dy  = (float)py - cy;
            const float dy2 = dy * dy;
            float* row = &blobField[py * screenWidth];

            for (int px = x0; px <= x1; px++) {
                const float dx = (float)px - cx;
                const float d2 = dx * dx + dy2;
                if (d2 < influenceSq) {
                    const float t = 1.0f - d2 * invInfluenceSq;
                    row[px] += t * t;
                }
            }
        }
    }

    // Threshold the accumulated field into pixels, honoring renderStep block size.
    for (int py = 0; py < screenHeight; py += renderStep) {
        const float* row = &blobField[py * screenWidth];
        for (int px = 0; px < screenWidth; px += renderStep) {
            if (row[px] >= blobThreshold) {
                for (int oy = 0; oy < renderStep; oy++) {
                    for (int ox = 0; ox < renderStep; ox++) {
                        int fx = px + ox;
                        int fy = py + oy;
                        if (fx >= 0 && fx < screenWidth && fy >= 0 && fy < screenHeight) {
                            display.setPixel(fx, fy);
                        }
                    }
                }
            }
        }
    }

    display.display();
}

//--------------------------------------------------------------------------------
// Classic particle rendering — distinct dots
//--------------------------------------------------------------------------------
void SPHFluidGame::renderParticles()
{
    display.clear();

    int rad = (int)particleRadius;

    for (int i = 0; i < numParticles; i++) {
        int cx = (int)particles[i].x;
        int cy = (int)particles[i].y;

        for (int dy = -rad; dy <= rad; dy++) {
            for (int dx = -rad; dx <= rad; dx++) {
                int distSq = dx*dx + dy*dy;
                int rSq = rad * rad;
                if (distSq <= rSq) {
                    int px = cx + dx;
                    int py = cy + dy;
                    if (px >= 0 && px < screenWidth && py >= 0 && py < screenHeight) {
                        display.setPixel(px, py);
                    }
                }
            }
        }
    }

    display.display();
}

//--------------------------------------------------------------------------------
// Start menu — pick the render style
//--------------------------------------------------------------------------------
void SPHFluidGame::renderStartMenu()
{
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 0, "Fluid Sim");

    display.setFont(ArialMT_Plain_10);

    const char* items[2] = { "Liquid Blobs", "Classic Dots" };
    for (int i = 0; i < 2; i++) {
        int y = 24 + i * 14;
        if (i == startMenuIndex) {
            // Highlight bar behind the selected item
            display.fillRect(10, y, 108, 12);
            display.setColor(BLACK);
            display.drawString(64, y + 1, items[i]);
            display.setColor(WHITE);
        } else {
            display.drawString(64, y + 1, items[i]);
        }
    }

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 54, "Enter: start");

    display.display();
}

//--------------------------------------------------------------------------------
// Settings menu — edit simulation parameters
//--------------------------------------------------------------------------------
String SPHFluidGame::settingLabel(int index)
{
    switch (index) {
        case 0: return "Particles";
        case 1: return "Smooth (h)";
        case 2: return "Stiffness";
        case 3: return "Viscosity";
        case 4: return "Cohesion";
        case 5: return "Gravity";
        case 6: return "Damping";
        case 7: return "Radius";
    }
    return "";
}

float SPHFluidGame::settingValue(int index)
{
    switch (index) {
        case 0: return (float)numParticles;
        case 1: return smoothingLength;
        case 2: return stiffness;
        case 3: return viscosity;
        case 4: return cohesionStrength;
        case 5: return gravityY;
        case 6: return damping;
        case 7: return particleRadius;
    }
    return 0.0f;
}

void SPHFluidGame::adjustSetting(int index, int direction)
{
    switch (index) {
        case 0: {
            int n = numParticles + direction * 10;
            if (n < 10) n = 10;
            if (n > 300) n = 300;
            setParticleCount(n);
            currentCount = n;
            break;
        }
        case 1:
            smoothingLength += direction * 0.5f;
            if (smoothingLength < 1.0f) smoothingLength = 1.0f;
            if (smoothingLength > 20.0f) smoothingLength = 20.0f;
            break;
        case 2:
            stiffness += direction * 0.5f;
            if (stiffness < 0.0f) stiffness = 0.0f;
            if (stiffness > 20.0f) stiffness = 20.0f;
            break;
        case 3:
            viscosity += direction * 0.2f;
            if (viscosity < 0.0f) viscosity = 0.0f;
            if (viscosity > 10.0f) viscosity = 10.0f;
            break;
        case 4:
            cohesionStrength += direction * 0.05f;
            if (cohesionStrength < 0.0f) cohesionStrength = 0.0f;
            if (cohesionStrength > 2.0f) cohesionStrength = 2.0f;
            break;
        case 5:
            gravityY += direction * 1.0f;
            if (gravityY < -20.0f) gravityY = -20.0f;
            if (gravityY > 20.0f) gravityY = 20.0f;
            break;
        case 6:
            damping += direction * 0.01f;
            if (damping < 0.0f) damping = 0.0f;
            if (damping > 1.0f) damping = 1.0f;
            break;
        case 7:
            particleRadius += direction * 0.5f;
            if (particleRadius < 1.0f) particleRadius = 1.0f;
            if (particleRadius > 6.0f) particleRadius = 6.0f;
            break;
    }
}

void SPHFluidGame::renderSettings()
{
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 0, "Settings");

    // Show a scrolling window of parameters around the selected one.
    const int visibleRows = 4;
    int start = settingsIndex - visibleRows / 2;
    if (start < 0) start = 0;
    if (start > SETTINGS_COUNT - visibleRows) start = SETTINGS_COUNT - visibleRows;
    if (start < 0) start = 0;

    for (int row = 0; row < visibleRows; row++) {
        int idx = start + row;
        if (idx >= SETTINGS_COUNT) break;

        int y = 14 + row * 12;

        String label = settingLabel(idx);
        float  val   = settingValue(idx);

        // Format value: whole number for particle count, else 2 decimals
        String valStr = (idx == 0) ? String((int)val) : String(val, 2);

        if (idx == settingsIndex) {
            display.fillRect(0, y, 128, 12);
            display.setColor(BLACK);
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(4, y + 1, label);
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(124, y + 1, valStr);
            display.setColor(WHITE);
        } else {
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.drawString(4, y + 1, label);
            display.setTextAlignment(TEXT_ALIGN_RIGHT);
            display.drawString(124, y + 1, valStr);
        }
    }

    display.display();
}

//--------------------------------------------------------------------------------
// AppManager lifecycle
//--------------------------------------------------------------------------------
void SPHFluidGame::begin() {
    // Register button callbacks for navigation + gameplay
    buttonManager.registerCallback(button_BottomLeftIndex, onButtonBackPressed);
    buttonManager.registerCallback(button_UpIndex,    onButtonUpPressed);
    buttonManager.registerCallback(button_DownIndex,  onButtonDownPressed);
    buttonManager.registerCallback(button_LeftIndex,  onButtonLeftPressed);
    buttonManager.registerCallback(button_RightIndex, onButtonRightPressed);
    buttonManager.registerCallback(button_EnterIndex, onButtonEnter);
}

void SPHFluidGame::end() {
    ESP_LOGI(TAG_MAIN, "end() => unregistering button callbacks...");
    buttonManager.unregisterCallback(button_BottomLeftIndex);
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
}

//--------------------------------------------------------------------------------
// Button handlers
//--------------------------------------------------------------------------------

// BACK (equals): from settings/sim -> back to a menu; from start menu -> exit app.
void SPHFluidGame::onButtonBackPressed(const ButtonEvent& event)
{
    if (event.eventType != ButtonEvent_Released) return;

    switch (instance->appScreen) {
        case SCREEN_SETTINGS:
            // Leave settings, resume the simulation.
            instance->appScreen = SCREEN_SIMULATION;
            break;

        case SCREEN_SIMULATION:
            // Return to the start menu.
            instance->appScreen = SCREEN_START_MENU;
            break;

        case SCREEN_START_MENU:
        default:
            // Exit the app entirely.
            ESP_LOGI(TAG_MAIN, "onButtonBackPressed => end() + returnToMenu...");
            instance->end();
            MenuManager::instance().returnToMenu();
            break;
    }
}

// UP (triangle up): move selection up.
void SPHFluidGame::onButtonUpPressed(const ButtonEvent& event)
{
    if (event.eventType != ButtonEvent_Released) return;

    if (instance->appScreen == SCREEN_START_MENU) {
        instance->startMenuIndex--;
        if (instance->startMenuIndex < 0) instance->startMenuIndex = 1;
    } else if (instance->appScreen == SCREEN_SETTINGS) {
        instance->settingsIndex--;
        if (instance->settingsIndex < 0) instance->settingsIndex = SETTINGS_COUNT - 1;
    }
}

// DOWN (triangle down): move selection down.
void SPHFluidGame::onButtonDownPressed(const ButtonEvent& event)
{
    if (event.eventType != ButtonEvent_Released) return;

    if (instance->appScreen == SCREEN_START_MENU) {
        instance->startMenuIndex++;
        if (instance->startMenuIndex > 1) instance->startMenuIndex = 0;
    } else if (instance->appScreen == SCREEN_SETTINGS) {
        instance->settingsIndex++;
        if (instance->settingsIndex >= SETTINGS_COUNT) instance->settingsIndex = 0;
    }
}

// LEFT (triangle left): decrease the selected setting.
void SPHFluidGame::onButtonLeftPressed(const ButtonEvent& event)
{
    if (event.eventType != ButtonEvent_Released) return;

    if (instance->appScreen == SCREEN_SETTINGS) {
        instance->adjustSetting(instance->settingsIndex, -1);
    }
}

// RIGHT (triangle right): increase the selected setting.
void SPHFluidGame::onButtonRightPressed(const ButtonEvent& event)
{
    if (event.eventType != ButtonEvent_Released) return;

    if (instance->appScreen == SCREEN_SETTINGS) {
        instance->adjustSetting(instance->settingsIndex, +1);
    }
}

// ENTER (circle):
//  - We time the press/release ourselves (no event.duration in this build).
//  - Held long enough -> update() opens settings from a live screen.
//  - Short press on start menu -> begin the simulation with the chosen style.
void SPHFluidGame::onButtonEnter(const ButtonEvent& event)
{
    if (event.eventType == ButtonEvent_Pressed) {
        // Start timing this hold.
        instance->enterDown       = true;
        instance->enterPressTime  = millis();
        instance->enterHoldFired  = false;
        return;
    }

    if (event.eventType == ButtonEvent_Released) {
        bool wasLongHold = instance->enterHoldFired;
        instance->enterDown = false;

        // If this press already triggered the settings long-hold, ignore the release.
        if (wasLongHold) return;

        // Short press behavior.
        if (instance->appScreen == SCREEN_START_MENU) {
            // Apply the chosen render style and start simulating.
            instance->renderMode =
                (instance->startMenuIndex == 0) ? RENDER_BLOBS : RENDER_PARTICLES;
            instance->resetParticles();
            instance->appScreen = SCREEN_SIMULATION;
        }
        // Short Enter in SIMULATION or SETTINGS does nothing.
    }
}