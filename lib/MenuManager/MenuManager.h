// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef MENU_MANAGER_H
#define MENU_MANAGER_H

#include <vector>
#include <string>

// Forward declarations or includes for UIElement/UITween:
#include "UIElement.h"
#include "UITween.h"
#include "HAL.h"          // So we can reference buttonManager, displayProxy, etc.
#include "AppManager.h"    // We need to launch apps from here
#include "ButtonManager.h" // For button callbacks
#include "LoadoutManifest.h" // ArrangeItem for reorder persistence

// Highlight shapes
enum HighlightShape {
    HIGHLIGHT_RECTANGLE,
    HIGHLIGHT_PARALLELOGRAM,
    HIGHLIGHT_SLANTED_CORNERS,
};

// Menu left/right animation transition states
enum CrossSlideState {
    CROSS_SLIDE_NONE,
    CROSS_SLIDE_FORWARD,  // going into child
    CROSS_SLIDE_BACK      // going to parent
};

/**
 * @brief A basic menu item. 
 * If isCategory=true, "children" hold sub-items.
 * If isCategory=false, it's a leaf item referencing a particular AppIndex.
 */
struct MenuItem {
    std::string  label;
    bool         isCategory;
    AppIndex     appIndex;        // used if isCategory=false && blobPath empty
    std::vector<MenuItem> children;
    // A ferried wasm app has no compile-time AppIndex; a non-empty blobPath
    // marks a blob leaf, launched via the WASM_HOST slot (T-183). blobLabel
    // is the app's own name (appIndex points at the shared WASM_HOST row).
    std::string  blobPath;
    std::string  blobLabel;
    int          blobAbi = 0;

    MenuItem(const std::string &lbl, bool cat, AppIndex idx)
        : label(lbl), isCategory(cat), appIndex(idx)
    {}
};

/**
 * @brief The MenuManager class is responsible for:
 *  - Storing a hierarchy of categories / sub-items
 *  - Tracking which category or sub-menu is active
 *  - Handling button presses for up/down/left/right/back/select
 *  - Smoothly animating the highlight UIElement
 *  - Launching an App when a user selects a menu item that is not a category
 *  - Providing a "returnToMenu()" callback for apps that want to hand back control
 */
class MenuManager
{
public:
    /**
     * @brief Singleton accessor (optional).
     * We often want a single global MenuManager.
     */
    static MenuManager &instance();

    /**
     * @brief Register an app in the menu system.
     * @param path The category path, e.g., "Tools/WiFi"
     * @param label The display name for the app, e.g., "WiFi Config."
     * @param index The AppIndex for this app.
     */ 
    void registerApp(const std::string &path,
                     const std::string &label,
                     AppIndex index);

    /**
     * @brief Register a ferried wasm app leaf (T-183). Launched through the
     * shared WASM_HOST slot, staging blobPath first.
     * @param path      category path, e.g. "Games"
     * @param label     the app's display name
     * @param blobPath  confined /apps/... path to the .wasm file
     */
    void registerBlobApp(const std::string &path,
                         const std::string &label,
                         const std::string &blobPath,
                         int blobAbi);

    /**
     * @brief Initialize the menu system. 
     * - Register button callbacks for navigation.
     * - Build the top-level items (Screensavers, Games, Tools) plus their children.
     */
    void begin();

    /**
     * @brief Call this repeatedly inside the main loop (e.g., from AppManager::loop())
     * so that animations can update and the menu can redraw if needed.
     */
    void update();

    /**
     * @brief Close the menu system
     *  - Unregister button callbacks for navigation.
     */
    void end();

    /**
     * @brief Called by an app to signal "I am done; go back to the menu."
     * This re-registers the menu's button callbacks and shows the last-known highlight.
     *  - Can also call a specific app to switch to, including the menu:
     *  - AppManager::instance().switchToApp(APP_MENU);
     */
    void returnToMenu();

    /**
     * @brief Utility to know if the menu is currently the "active" UI,
     * as opposed to an app being active.
     */
    bool isMenuActive() const { return menuActive; }

    /**
     * @brief Helper to set the highlight shape.
     */
    void setHighlightShape(HighlightShape shape) { highlightShape = shape; }

    /**
     * @brief Dump the built menu tree to Serial as machine-parseable
     * `[cmd] menutree.*` lines (T-183/T-191): one line per category and
     * leaf, with rendered label text, blob flag, and blob path. Read-only.
     */
    void dumpTree() const;

    /**
     * @brief Mark the menu stale after a loadout manifest change (T-183).
     * The next begin() rebuilds the tree once. Safe to call from the sync
     * path; the rebuild itself happens only on menu entry.
     */
    void markManifestDirty() { manifestDirty = true; }

private:
    // Private constructor: we use the singleton pattern above
    MenuManager();

    // ========== DATA STRUCTURES ==========

    // The "root" vector of top-level categories: Screensavers, Games, Tools
    std::vector<MenuItem> rootMenuItems;

    std::vector<MenuItem> *currentItemList = nullptr;
    // Which item in the current list is highlighted
    int currentIndex = 0;

    // Keep track of where each item is drawn on screen.
    // This example: each item is drawn in rows, so we store item Y positions.
    // That way we can animate the highlight from row to row.
    std::vector<int> itemYPositions; 

    // Our highlight UIElement for the current selected item
    // We'll animate its position whenever the user moves up/down.
    UIElement highlightElement;

    // Are we in the menu (true) or inside an app (false)?
    bool menuActive = true;
    // T-183: set when the loadout manifest changes (sync/lapply) so the next
    // menu entry rebuilds the tree once, surfacing ferried apps without a
    // reboot. Consumed in begin().
    bool manifestDirty = false;

    // If we navigate into a sub-menu, we push state here so we can go back
    struct MenuNavState {
        std::vector<MenuItem> *itemList;
        int index;
        int savedScrollOffset;
    };
    std::vector<MenuNavState> navigationStack;

    // ========== CROSS-SLIDE ==========

    CrossSlideState crossSlideState = CROSS_SLIDE_NONE;

    // These store pointers to old vs. new lists
    std::vector<MenuItem>* oldItemList = nullptr;
    std::vector<MenuItem>* newItemList = nullptr;

    // Offsets for old vs. new menu
    int oldMenuX = 0;
    int newMenuX = 0;

    // We'll store the highlight shape
    HighlightShape highlightShape = HIGHLIGHT_RECTANGLE;

    // Cross-slide transitions
    void crossSlideForward(const MenuItem &child);
    void crossSlideBackward();
    void handleCrossSlide(); // check if done, finalize

    // ========== SCROLLING / HIGHLIGHT ==========

    /**
     * @brief Helper to move the highlight up/down within the current item list.
     */
    void moveHighlightUp();
    void moveHighlightDown();

    /**
     * @brief Helper to adjust scrollOffset if the highlight goes off-screen
     */
    void updateScrollForCurrentIndex();

    /**
     * @brief Animate the highlight to the given screen-relative Y position
     */
    void animateHighlight(int targetY);

    // ========== NAVIGATION ==========

    /**
     * @brief Helper to go deeper into a category or launch an app if it’s not a category.
     */
    void selectCurrentItem();

    /**
     * @brief Helper to go back up one level in the navigation stack.
     */
    void goBack();

    // ========== MOVE (REORDER) MODE ==========
    //
    // Long-press (ButtonEvent_Held) on a leaf item "picks it up"; up/down
    // then reposition it within its current list (i.e. within its category
    // section); select commits — the new order is persisted to the loadout
    // manifest via AppManager::persistMenuArrangement so it survives
    // reboot — and back cancels, restoring the pre-move order.

    bool moveMode = false;                 // reorder mode active?
    bool swallowNextSelectRelease = false; // eat the Released that follows the Held
    std::vector<MenuItem> moveSnapshot;    // restore point for cancel
    HighlightShape savedHighlightShape = HIGHLIGHT_SLANTED_CORNERS;

    void enterMoveMode();
    void commitMove();
    void cancelMove();
    void moveItemUp();
    void moveItemDown();

    /**
     * @brief Walk the whole menu tree and emit the full display order as
     * id-anchored arrange items (leaves under a top-level category carry
     * that category's label — one-level flat model; root leaves carry "").
     */
    void collectArrangeOrder(std::vector<LoadoutManifest::ArrangeItem>& out) const;

    // ========== DRAWING ==========

    /**
     * @brief Re-draw the menu: items and highlight.
     * This is called from update().
     */
    void drawMenu();

    // Helpers for building the nested menu
    // parseCategoryPath => splits "Tools/WiFi" into ["Tools", "WiFi"]
    std::vector<std::string> parseCategoryPath(const std::string &path);
    
    // findOrCreateCategory => navigates or creates subcategories
    std::vector<MenuItem> &findOrCreateCategory(std::vector<MenuItem> &currentLevel,
                                                const std::string &categoryLabel);

    /**
     * @brief Register or unregister all six callbacks. 
     * We do this so that the app itself can take over the same hardware buttons.
     */
    void registerMenuCallbacks();
    void unregisterMenuCallbacks();

    // We will have one callback function for each button (some might share).
    static void onButtonLeftPressed(const ButtonEvent& event);
    static void onButtonRightPressed(const ButtonEvent& event);
    static void onButtonUpPressed(const ButtonEvent& event);
    static void onButtonDownPressed(const ButtonEvent& event);
    static void onButtonBackPressed(const ButtonEvent& event);
    static void onButtonSelectPressed(const ButtonEvent& event);

    int menuXOffset = 0; // X offset for menu transition animation
};

// Then declare the free functions if you want them visible:
extern void menuBegin();  // free function
extern void menuEnd(); 
extern void menuRun();

/**
 * @brief Helper to animate the highlight element to its new Y position.
 */
static void finalizeHighlightAnimation(UIElement* e);

#endif
