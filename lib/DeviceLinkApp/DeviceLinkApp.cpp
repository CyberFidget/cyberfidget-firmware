// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "DeviceLinkApp.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "AppManager.h"
#include "CloudSync.h"
#include "DeviceIdentity.h"
#include "DisplayProxy.h"
#include "HAL.h"
#include "LinkSession.h"
#include "MenuManager.h"
#include "ModalPrompt.h"

namespace DeviceLinkApp {
namespace {

bool promptShown = false;
bool clearPromptShown = false;
bool linkPromptOpen = false;   // this app's account or Clear/Keep prompt is open
bool wasBusy = false;
bool hasLink = false;
char linkedAccount[40] = {0};
char fingerprint[5] = {0};

// Reads the stored link; called on entry and when a session ends, not per frame.
void refreshLink() { hasLink = CloudSync::linked(linkedAccount); }

void leaveWhileConfirming(int choice) {
    if (choice == 1) {
        CloudSync::requestCancel();
        MenuManager::instance().returnToMenu();
    }
}

void confirmDone(int choice) {
    linkPromptOpen = false;
    CloudSync::answerLink(choice == 0);
}
void clearDone(int choice) {
    linkPromptOpen = false;
    CloudSync::answerClearApps(choice == 0);
}

void unlinkDone(int choice) {
    if (choice == 0) CloudSync::startUnlink();
}

void optionsDone(int choice) {
    if (choice == 0) {
        promptShown = false;
        clearPromptShown = false;
        CloudSync::startLink();
    }
    if (choice == 1) {
        const char* const options[] = {"Unlink", "Keep link"};
        ModalPrompt::instance().open("Unlink this Fidget?", options, 2, unlinkDone);
    }
}

void onBack(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Released) return;
    if (CloudSync::linkSnapshot().state == CloudSync::LinkState::Confirming) {
        const char* const options[] = {"Stay", "Leave"};
        ModalPrompt::instance().open(
            "Still finishing - leaving now may leave the link half done",
            options, 2, leaveWhileConfirming);
        return;
    }
    CloudSync::requestCancel();
    MenuManager::instance().returnToMenu();
}

void onEnter(const ButtonEvent& event) {
    if (event.eventType != ButtonEvent_Released || CloudSync::busy()) return;
    char account[40];
    if (CloudSync::linked(account)) {
        const char* const options[] = {"Link again", "Unlink", "Cancel"};
        ModalPrompt::instance().open("Account link", options, 3, optionsDone);
    } else {
        CloudSync::startLink();
        promptShown = false;
        clearPromptShown = false;
    }
}

} // namespace

void refreshStoredLink() { refreshLink(); }

void begin() {
    CloudSync::resetLinkStatus();
    const DeviceIdentity::Fingerprint id = DeviceIdentity::readLive();
    memcpy(fingerprint, id.id + 8, 4);
    fingerprint[4] = '\0';
    auto& buttons = HAL::buttonManager();
    buttons.registerCallback(button_SelectIndex, onBack);
    buttons.registerCallback(button_EnterIndex, onEnter);
    promptShown = false;
    clearPromptShown = false;
    linkPromptOpen = false;
    refreshLink();
    if (!hasLink) CloudSync::startLink();
    wasBusy = CloudSync::busy();
}

void end() {
    auto& buttons = HAL::buttonManager();
    buttons.unregisterCallback(button_SelectIndex);
    buttons.unregisterCallback(button_EnterIndex);
    CloudSync::requestCancel();
    promptShown = false;
    clearPromptShown = false;
    linkPromptOpen = false;
}

void update() {
    const CloudSync::LinkSnapshot snap = CloudSync::linkSnapshot();
    const bool nowBusy = CloudSync::busy();
    if (wasBusy && !nowBusy) refreshLink();
    wasBusy = nowBusy;
    if (snap.state == CloudSync::LinkState::Confirm && !promptShown) {
        char title[62];
        snprintf(title, sizeof(title), "Link to @%s?", snap.account);
        const char* const options[] = {"OK", "Not me"};
        promptShown = ModalPrompt::instance().open(title, options, 2, confirmDone);
        if (promptShown) linkPromptOpen = true;
    }
    if (snap.state == CloudSync::LinkState::ClearApps && !clearPromptShown) {
        const char* const options[] = {"Clear", "Keep"};
        clearPromptShown = ModalPrompt::instance().open(
            "Clear the apps from the previous account?", options, 2, clearDone);
        if (clearPromptShown) linkPromptOpen = true;
    }
    auto& screen = HAL::displayProxy();
    screen.clear();
    screen.setColor(WHITE);
    screen.setTextAlignment(TEXT_ALIGN_CENTER);
    screen.setFont(ArialMT_Plain_10);
    if (snap.state == CloudSync::LinkState::Idle && hasLink) {
        screen.drawString(64, 4, "Linked account");
        char label[44]; snprintf(label, sizeof(label), "@%s", linkedAccount);
        screen.drawString(64, 20, label);
        screen.drawString(64, 45, "Enter: options");
    } else if (snap.state == CloudSync::LinkState::Code ||
               snap.state == CloudSync::LinkState::Confirm ||
               snap.state == CloudSync::LinkState::ClearApps) {
        screen.drawString(64, 0, "Type this code at");
        screen.setFont(ArialMT_Plain_16);
        screen.drawString(64, 12, snap.code);
        screen.setFont(ArialMT_Plain_10);
        screen.drawString(64, 32, "cyberfidget.com");
        screen.drawString(64, 43, "/my-fidget/link");
        char label[20]; snprintf(label, sizeof(label), "Fidget %s", fingerprint);
        screen.drawString(64, 54, label);
    } else {
        if (snap.state == CloudSync::LinkState::Unlinked) {
            screen.drawString(64, 2, "Unlinked on this Fidget");
            screen.drawString(64, 16, "Will finish next time");
            screen.drawString(64, 27, "you link or check");
            screen.drawString(64, 38, "for updates");
            screen.drawString(64, 52, "Enter: link again");
            screen.display();
            return;
        }
        if (snap.state == CloudSync::LinkState::Error) {
            // Says why, and what to do, where the reason is one the person
            // can fix (no saved WiFi, or WiFi out of reach).
            const CloudSync::LinkErrorText text = CloudSync::linkErrorText(snap.error);
            screen.drawString(64, 12, text.line1);
            screen.drawString(64, 26, text.line2);
            screen.drawString(64, 45, "Enter: try again");
            screen.display();
            return;
        }
        const char* message = snap.state == CloudSync::LinkState::Linked ?
                                  (snap.error[0] ? "Linked - apps remain" : "Linked") :
                              snap.state == CloudSync::LinkState::Declined ? "Link cancelled" :
                              snap.state == CloudSync::LinkState::Expired ? "Code expired" :
                              snap.state == CloudSync::LinkState::Idle ? "Not linked" :
                              snap.state == CloudSync::LinkState::Confirming ? "Finishing link..." :
                              "Connecting...";
        screen.drawString(64, 20, message);
        screen.drawString(64, 45, snap.state == CloudSync::LinkState::Linked ?
                          "Enter: options" : "Enter: try again");
    }
    screen.display();
}

// Closes only this app's own account or Clear/Keep prompt: the flag is set
// when that prompt opens and cleared by its done callback, so another
// feature's prompt is never closed here.
void closeStalePrompt() {
    if (!linkPromptOpen) return;
    const CloudSync::LinkState state = CloudSync::linkSnapshot().state;
    if (state == CloudSync::LinkState::Expired || state == CloudSync::LinkState::Declined ||
        state == CloudSync::LinkState::Error || state == CloudSync::LinkState::Unlinked) {
        if (ModalPrompt::instance().isOpen()) ModalPrompt::instance().closeForTeardown();
        linkPromptOpen = false;
        promptShown = false;
        clearPromptShown = false;
    }
}

} // namespace DeviceLinkApp
