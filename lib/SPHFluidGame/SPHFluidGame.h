#pragma once

#include <vector>
#include <algorithm>
#include <math.h>
#include <Arduino.h>
#include "DisplayProxy.h" 
#include "ButtonManager.h" // For returning back to menu
#include "MenuManager.h" // For returning back to menu

class SPHFluidGame {
public:
    struct Particle {
        float x, y;
        float vx, vy;
        float density;
        float pressure;
    };

    // Which screen the app is currently showing.
    enum AppScreen {
        SCREEN_START_MENU,   // choose render style
        SCREEN_SIMULATION,   // running fluid sim
        SCREEN_SETTINGS      // parameter editor
    };

    // How the fluid is drawn.
    enum RenderMode {
        RENDER_BLOBS,        // metaball / connected liquid
        RENDER_PARTICLES     // classic distinct dots
    };

    SPHFluidGame(ButtonManager& btnMgr);

    // Updates the simulation by one step, taking external acceleration (e.g., from an IMU).
    void update();
    void begin(); // AppManager integration
    void end(); // AppManager integration

    // Resets/re-randomizes particles.
    void resetParticles();

    // Adjusts the number of particles rendered
    void setParticleCount(int newCount);

private:
    int currentCount; // For tracking the current number of particles

    // ---- SPH parameters ----
    int   numParticles;       // e.g., 100 or 200 for microcontroller
    float smoothingLength;    // h
    float restDensity;        // ρ0
    float stiffness;          // k
    float viscosity;          // μ
    float gravityX;           // external accel in X
    float gravityY;           // external accel in Y
    float damping;            // boundary damping factor
    float dt;                 // timestep
    float particleRadius;     // e.g. 2.0, how big the particle is on screen

    // -- Surface Tension (cohesion) parameter --
    float cohesionStrength;   // Additional force pulling particles together

    // ---- Display / screen ----
    DisplayProxy& display;
    int screenWidth;
    int screenHeight;

    // ---- Particle container ----
    std::vector<Particle> particles;

    // ---- Metaball / blobby rendering ----
    float blobThreshold;      // field value at which a pixel is considered "inside" the fluid
    int   renderStep;         // sampling stride for the field (1 = every pixel)
    std::vector<float> blobField; // scatter accumulator (screenWidth*screenHeight), allocated once

    // ---- Screen / UI state ----
    AppScreen  appScreen;     // which screen is active
    RenderMode renderMode;    // blobs vs particles
    int        startMenuIndex;  // highlighted item on the start menu
    int        settingsIndex;   // highlighted parameter in settings

    // ---- Enter long-hold tracking ----
    bool          enterDown;       // is Enter currently pressed?
    unsigned long enterPressTime;  // millis() when Enter went down
    bool          enterHoldFired;  // did this press already trigger the long-hold?

    // Number of editable parameters in the settings menu
    static const int SETTINGS_COUNT = 8;

    // Core SPH steps
    void computeDensityPressure();
    void computeForces(float ax, float ay);
    void resolveParticleCollisions();
    void integrate();
    void render();                 // dispatches to the active render style
    void renderBlobs();            // metaball rendering
    void renderParticles();        // classic dot rendering

    // Simulation stepping (physics only, no render)
    void stepSimulation();

    // Screen renderers
    void renderStartMenu();
    void renderSettings();

    // Settings helpers
    String   settingLabel(int index);
    float    settingValue(int index);   // returns the current value as float
    void     adjustSetting(int index, int direction); // +1 / -1 step

    // AppManager Integration
    ButtonManager& buttonManager;
    static void buttonPressedCallback(const ButtonEvent& event);
    static void onButtonBackPressed(const ButtonEvent& event);
    static void onButtonUpPressed(const ButtonEvent& event);
    static void onButtonDownPressed(const ButtonEvent& event);
    static void onButtonLeftPressed(const ButtonEvent& event);
    static void onButtonRightPressed(const ButtonEvent& event);
    static void onButtonEnter(const ButtonEvent& event);
    static SPHFluidGame* instance; // To allow static callbacks
};

extern SPHFluidGame sphFluidGame; // App Manager integration