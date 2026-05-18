// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

// ReactionTimeGame.cpp
#include "ReactionTimeGame.h"
#include "HAL.h"

// Initialize static instance pointer
ReactionTimeGame* ReactionTimeGame::instance = nullptr;

/**
 * @brief Static callback for the reaction-game button.
 *        Only handles Pressed event; calls instance->handleButtonPress().
 */
void ReactionTimeGame::reactionButtonPressedCallback(const ButtonEvent& ev) {
    ESP_LOGI(TAG_MAIN, "ReactionTimeGame callback fired for button %d with event type %d", ev.buttonIndex, ev.eventType);

    // Handle only the Pressed event
    if (ev.eventType == ButtonEvent_Pressed) {
        if (ReactionTimeGame::instance) {
            ReactionTimeGame::instance->handleButtonPress();
        }
    }
}

/**
 * @brief Static callback for the "Back to menu" button
 */
void ReactionTimeGame::onButtonBackPressed(const ButtonEvent& ev) {
    if (ev.eventType == ButtonEvent_Released) {
        ESP_LOGI(TAG_MAIN, "[ReactionTimeGame] Back button pressed => returning to menu...");
        // Safely end the app
        if (ReactionTimeGame::instance) {
            ReactionTimeGame::instance->end();
        }
        // Go back to the main menu
        MenuManager::instance().returnToMenu();
    }
}

/**
 * @brief Constructor
 *        NOTE: The callback is not registered here; it's registered in begin().
 */
ReactionTimeGame::ReactionTimeGame(ButtonManager& btnManager)
    : display(HAL::displayProxy()),
      buttonManager(btnManager),
      gameStarted(false),
      waitingForReaction(false),
      delayActive(false),
      messageDisplayed(false) 
{
    // Store the instance pointer so static callbacks can reach 'this'
    ReactionTimeGame::instance = this;
}

/**
 * @brief AppManager integration: called when user launches the app
 */
void ReactionTimeGame::begin() {
    // Register the callback for the specific reaction-game button
    buttonManager.registerCallback(button_BottomRightIndex, ReactionTimeGame::reactionButtonPressedCallback);
    // Also register a "back" button to exit (commonly button_BottomLeftIndex)
    buttonManager.registerCallback(button_BottomLeftIndex, ReactionTimeGame::onButtonBackPressed);

    ESP_LOGI(TAG_MAIN, "[ReactionTimeGame] begin() => callbacks registered.");

    resetGame(); // Reset the game state
}

/**
 * @brief AppManager integration: called when user exits the app
 */
void ReactionTimeGame::end() {
    // Unregister both the reaction-game button and the back-to-menu button
    buttonManager.unregisterCallback(button_BottomRightIndex);
    buttonManager.unregisterCallback(button_BottomLeftIndex);

    ESP_LOGI(TAG_MAIN, "[ReactionTimeGame] end() => callbacks unregistered.");
}


/**
 * @brief Update function that loops the app
 * 
 */
void ReactionTimeGame::update() {
    // Initial screen prompt
    if (!gameStarted && !delayActive && !waitingForReaction && !messageDisplayed) {
        ESP_LOGI(TAG_MAIN, "Displaying 'Press to start...' screen.");
        display.clear();
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.setFont(ArialMT_Plain_16);
        display.drawString(64, 24, "Press to start");
        display.display();
        messageDisplayed = true;
    }

    // Handle the delay logic
    if (delayActive && millis_NOW >= randomDelayEnd) {
        ESP_LOGI(TAG_MAIN, "Displaying 'GO!' screen.");
        display.clear();
        display.drawString(64, 22, "GO!");
        display.display();

        startTime = millis_NOW;
        waitingForReaction = true;
        delayActive = false;
    }
}

/**
 * @brief Handle a button press event
 */
void ReactionTimeGame::handleButtonPress() {
    ESP_LOGI(TAG_MAIN, "handleButtonPress called.");
    if (!gameStarted) {
        ESP_LOGI(TAG_MAIN, "Starting game...");
        startGame(millis());
    } else if (waitingForReaction) {
        ESP_LOGI(TAG_MAIN, "Reaction time measured.");
        reactionTime = millis() - startTime;
        waitingForReaction = false;
        displayReactionTime();
    } else if (gameStarted && !waitingForReaction) {
        ESP_LOGI(TAG_MAIN, "Resetting game...");
        resetGame();
    } else {
        ESP_LOGI(TAG_MAIN, "Unexpected button press.");
    }
}

/**
 * @brief Start the reaction time game
 */
void ReactionTimeGame::startGame(unsigned long millis_NOW) {
    ESP_LOGI(TAG_MAIN, "Starting Reaction Time Game.");
    gameStarted = true;
    display.clear();
    display.drawString(64, 22, "Get Ready...");
    display.display();

    unsigned long randomDelay = random(1000, 5000); // Delay between 1s and 5s
    randomDelayEnd = millis_NOW + randomDelay;
    delayActive = true;
    messageDisplayed = false;
}

/**
 * @brief Display the reaction time to the user
 */
void ReactionTimeGame::displayReactionTime() {
    ESP_LOGI(TAG_MAIN, "Displaying reaction time.");
    display.clear();
    display.drawString(64, 22, "Time: " + String(reactionTime) + " ms");
    display.display();
}

/**
 * @brief Reset the game to its initial state
 */
void ReactionTimeGame::resetGame() {
    ESP_LOGI(TAG_MAIN, "Resetting Reaction Time Game.");
    gameStarted = false;
    waitingForReaction = false;
    delayActive = false;
    messageDisplayed = false;

    display.clear();
    display.drawString(64, 22, "Press to start");
    display.display();
}

// -------------------------------------------------------------------
// Define one global instance for the AppManager
// You can choose any hardware button index (example: button_TopRightIndex)
// -------------------------------------------------------------------
ReactionTimeGame reactionTimeGame(HAL::buttonManager());
