// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "MenuManager.h"
#include "HAL.h"         // So we can reach buttonManager, display, etc.
#include "AppManager.h"   // So we can set appActive, appPreviously, etc.
#include "globals.h"      // If you have global for 'millis_NOW', etc.
#include "AppDefs.h"      // For AppIndex enum

#include <sstream>        // For path parsing
#include <algorithm>  // for std::min/max if needed
#include <string>

// Aliases to hardware
static auto &display       = HAL::displayProxy();
static auto &buttonManager = HAL::buttonManager();

// Screen dimension (SSD1306 128×64 example)
static const int SCREEN_WIDTH  = 128; 
static const int SCREEN_HEIGHT = 64;

// Row spacing for text in the menu:
static const int MENU_ITEM_HEIGHT = 16;
static const int MENU_ITEM_Y_OFFSET = 2;

// We’ll offset the highlight bar a bit from the left edge:
static const int HIGHLIGHT_X_OFFSET = 4;
static const int HIGHLIGHT_Y_OFFSET = 4;
static const int HIGHLIGHT_WIDTH    = SCREEN_WIDTH - (HIGHLIGHT_X_OFFSET * 2); // just wide enough to look nice

// SCROLLING FIELDS
static int scrollOffset = 0;   // how many pixels the entire list is shifted up
static int oldScrollOffset = 0; // used if we want to animate from old→new
static const int VISIBLE_COUNT = 4; // how many items can fit on screen at once

// The transition duration in ms (tweak as you wish)
static int crossSlideDuration = 400;


// Helper to parse path like "Tools/WiFi" => ["Tools","WiFi"]
std::vector<std::string> MenuManager::parseCategoryPath(const std::string &path)
{
    std::vector<std::string> result;
    if (path.empty()) {
        return result; // no subcategories
    }
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            result.push_back(segment);
        }
    }
    return result;
}

/**
 * @brief Locates or creates a subcategory with name catName 
 * in currentLevel, returning a reference to that subcategory's children vector.
 */
std::vector<MenuItem>& MenuManager::findOrCreateCategory(std::vector<MenuItem> &currentLevel,
                                                         const std::string &catName)
{
    // see if catName already exists
    for (auto &mi : currentLevel) {
        if (mi.isCategory && mi.label == catName) {
            return mi.children; 
        }
    }
    // not found => create new category
    // must pass 3 args to constructor
    MenuItem newCat(catName, true, APP_COUNT); 
    // or APP_MENU or a "dummy" index for categories
    currentLevel.push_back(newCat);

    return currentLevel.back().children;
}

// This method is called from buildNestedMenu() to add a single app’s entry
static void addAppToMenu(const char* label, const char* path, int appIndex)
{
    MenuManager::instance().registerApp(path, label, (AppIndex)appIndex);
}


// Forward-declare a private helper (or just put it static inside .cpp):
static void drawHighlightShape(int x, int y, 
                               int w, int h,
                               HighlightShape shape)
{
    switch(shape)
    {
        case HIGHLIGHT_RECTANGLE:
        default:
        {
            // Standard rectangle
            display.drawRect(x, y, w, h);
            break;
        }

        case HIGHLIGHT_PARALLELOGRAM:
        {
            // Example: shift the top edge and bottom edge horizontally to create a parallelogram
            // Let's define a small horizontal offset:
            int offset = 8; // tweak as needed

            // We'll draw it using four drawLine calls:
            // top edge line (slanted):
            display.drawLine(x + offset,     y, x + w,          y);
            // right edge line (vertical):
            display.drawLine(x + w,          y, x + w - offset, y + h);
            // bottom edge line (slanted):
            display.drawLine(x + w - offset, y + h, x, y + h);
            // left edge line (vertical):
            display.drawLine(x, y + h, x + offset, y);
            break;
        }

        case HIGHLIGHT_SLANTED_CORNERS:
        {
            int corner = 4; // How many pixels to clip into the corner
        
            // We'll define rightX = x + w - 1, bottomY = y + h - 1
            // drawLine differs from drawRect by 1 pixel, must compensate to prevent overscan
            int rightX  = x + w - 1; 
            int bottomY = y + h - 1;
        
            // top-left diagonal
            display.drawLine(x,          y + corner, 
                             x + corner, y);
        
            // top edge
            display.drawLine(x + corner, y, 
                             rightX,     y);
        
            // right edge
            display.drawLine(rightX,     y, 
                             rightX,     bottomY - corner);
        
            // bottom-right diagonal
            display.drawLine(rightX,          bottomY - corner,
                             rightX - corner, bottomY);
        
            // bottom edge
            display.drawLine(rightX - corner, bottomY,
                             x,               bottomY);
        
            // left edge
            display.drawLine(x, bottomY, 
                             x, y + corner);
        
            break;
        }
    }
}

// A helper that draws a given menu list at a horizontal offset “menuX”
void drawOneMenu(std::vector<MenuItem>* items,
                 int menuX,
                 UIElement &highlight,
                 HighlightShape shape,
                 int currentIndex,
                 int scrollOffset)
{
    if (!items) return;

    // Draw each item with horizontal offset
    for (int i = 0; i < (int)items->size(); i++)
    {
        int yPos = (i * MENU_ITEM_HEIGHT) - scrollOffset;
        if (yPos >= -MENU_ITEM_HEIGHT && yPos < SCREEN_HEIGHT)
        {
            int drawX = 10 + menuX;
            int drawY = yPos + 2; // offset
            
            display.setTextAlignment(TEXT_ALIGN_LEFT);
            display.setFont(ArialMT_Plain_10);
            display.drawString(drawX, drawY, (*items)[i].label.c_str());
        }
    }

    // Also draw highlight
    int hlX = highlight.getX() + menuX;
    int hlY = highlight.getY();
    drawHighlightShape(hlX, 
                       hlY,
                       highlight.getWidth(),
                       highlight.getHeight(),
                       shape);
}

//-------------------------------------------------------------------
// Singleton
MenuManager &MenuManager::instance()
{
    static MenuManager single;
    return single;
}

//-------------------------------------------------------------------
// Singleton constructor: build root items, set up highlight element, etc.
MenuManager::MenuManager()
{
    ESP_LOGI(TAG_MAIN, "MenuManager constructor start");
    // Our "current" location is the root and index=0
    scrollOffset = 0; // top
    currentItemList = &rootMenuItems;
    currentIndex    = 0;

    // Prepare highlight UIElement for row #0
    highlightElement.setX(HIGHLIGHT_X_OFFSET);
    highlightElement.setY(0); // We'll set correct row in begin()
    highlightElement.setWidth(HIGHLIGHT_WIDTH);
    highlightElement.setHeight(MENU_ITEM_HEIGHT);

    // Set the highlight shape
    highlightShape = HIGHLIGHT_SLANTED_CORNERS;

    // Clear itemYPositions (we fill them in drawMenu())
    itemYPositions.clear();
    ESP_LOGI(TAG_MAIN, "MenuManager constructor end");
}

void MenuManager::begin()
{
    // If we've already built the menu, do NOT rebuild
    if (!rootMenuItems.empty()) {
        ESP_LOGI(TAG_MAIN, "Menu already built; not rebuilding");
        registerMenuCallbacks();
        menuActive = true;
        return;
    }

    // // Register the menu callbacks:
    registerMenuCallbacks();

    // // Clear any existing root items if you want a fresh build
    rootMenuItems.clear();
    navigationStack.clear();
    currentIndex = 0;
    scrollOffset = 0;
    highlightElement.setY(0);

    // parse each line's path, build the categories
    ESP_LOGI(TAG_MAIN, "MenuManager.cpp - Pre buildNestedMenu");
    buildNestedMenu(); 
    ESP_LOGI(TAG_MAIN, "MenuManager.cpp - Post buildNestedMenu");
    // buildNestedMenu calls something like:
    // for i in [0..APP_COUNT-1]:
    //    addAppToMenu(appDefs[i].name, appDefs[i].categoryPath, i);

    menuActive = true;
}

void MenuManager::end()
{
    menuActive = false;
    unregisterMenuCallbacks();
}


// Called by the main loop to let the menu update animations, etc.
void MenuManager::update()
{
    if (!menuActive) return; // If an app is running, do nothing

    // Update (animate) highlight, etc. 
    animateAll();        // from your Animation.cpp
    updateTmp();         // if needed from your code
    handleCrossSlide();  // Process the cross slide transition if needed
    drawMenu();          // Redraw the menu each frame (or only if something changed).
}

// Called by an app to hand control back to the menu
void MenuManager::returnToMenu()
{
    AppManager::instance().switchToApp(APP_MENU);
}

// This is the function that actually registers an app in the data structure
// by parsing the path. e.g. "Tools/WiFi" => sub-cat "Tools", sub-cat "WiFi", then leaf.
void MenuManager::registerApp(const std::string &path,
                                const std::string &label,
                                AppIndex index)
{
    // If this is the menu app, skip adding a leaf item
    if (index == APP_MENU) return;

    // parse path, create subcategories
    auto categories = parseCategoryPath(path);
    // start at root
    std::vector<MenuItem> *level = &rootMenuItems;

    for (auto &catName : categories) {
        level = &findOrCreateCategory(*level, catName);
    }

    // now add a leaf item
    // we must pass 3 arguments: (label, isCategory=false, index)
    MenuItem leaf(label, false, index);
    level->push_back(leaf);
}   

// Private method: move highlight up
void MenuManager::moveHighlightUp()
{
    if (currentIndex > 0)
    {
        int oldIndex = currentIndex;
        currentIndex--;
        updateScrollForCurrentIndex(oldIndex);
    }
}

// Private method: move highlight down
void MenuManager::moveHighlightDown()
{
    if (currentIndex < (int)currentItemList->size() - 1)
    {
        int oldIndex = currentIndex;
        currentIndex++;
        updateScrollForCurrentIndex(oldIndex);
    }
}

/**
 * @brief This function checks whether the highlight has moved beyond
 *        the top or bottom of the visible region, and if so, it animates
 *        the entire list via scrollOffset. Otherwise, it just animates
 *        the highlight if the item is still on screen.
 */
void MenuManager::updateScrollForCurrentIndex(int oldIndex)
{
    // We'll define how many items fit “fully” on screen:
    // e.g. VISIBLE_COUNT=3 for 3 items (each 20 px tall => 60 px total).
    // The highlight’s “row index” on screen is currentIndex - scrollOffsetInItems
    // but we’re doing pixel-based “scrollOffset”. Let’s compute the item’s Y:
    int itemY = currentIndex * MENU_ITEM_HEIGHT - scrollOffset;
    int topY  = 0;
    int bottomY = SCREEN_HEIGHT - MENU_ITEM_HEIGHT;

    // kill any old scroll animation
    auto it = animationsInt.find(&scrollOffset);
    if (it != animationsInt.end()) {
        it->second->updateToCurrentValue();
        delete it->second;
        animationsInt.erase(it);
    }

    if (itemY < topY) {
        // We need to scroll upward so that the highlight is at or near top
        // e.g. new scrollOffset = currentIndex * MENU_ITEM_HEIGHT
        int newScroll = currentIndex * MENU_ITEM_HEIGHT;
        // We'll animate scrollOffset from oldScrollOffset to newScroll
        oldScrollOffset = scrollOffset;
        insertAnimation(
            new Animation(
                &scrollOffset,
                INDENT,
                newScroll,   // endVal
                250          // ms
            )
        );
    }
    else if (itemY > bottomY) {
        // We need to scroll downward so highlight is near the bottom row
        // e.g. new scrollOffset = currentIndex * MENU_ITEM_HEIGHT - bottomY
        int newScroll = currentIndex * MENU_ITEM_HEIGHT - bottomY;
        oldScrollOffset = scrollOffset;
        insertAnimation(
            new Animation(
                &scrollOffset,
                INDENT,
                newScroll,
                250
            )
        );
    }
    else {
        // The item is fully within the visible area. 
        // Animate the highlight’s Y only.
        animateHighlight(oldIndex);
    }
}

void MenuManager::animateHighlight(int oldIndex)
{
    // 1) Force any existing highlight animation to jump to its partial position
    finalizeHighlightAnimation(&highlightElement);

    // 2) Now read the highlight’s current Y (it’s updated by the partial step above)
    int curY = highlightElement.getY();

    // 3) Compute the new target Y based on currentIndex
    int newY = (currentIndex * MENU_ITEM_HEIGHT) - scrollOffset;

    // 4) Insert a brand-new animation from curY to newY (over 200ms, for instance)
    //    Because highlightElement’s .getY() is already curY, we only have to call
    //    insertAnimation() with the new final Y.
    insertAnimation(
        new Animation(
            &highlightElement,
            INDENT,        // or BOUNCE, etc.
            highlightElement.getX(),
            newY,
            highlightElement.getWidth(),
            highlightElement.getHeight(),
            250
        )
    );
}


void finalizeHighlightAnimation(UIElement* e)
{
    // Look up the UIElement in the map of active animations:
    auto it = animationsUI.find(e);
    if (it != animationsUI.end())
    {
        Animation* oldAni = it->second;

        // Force it to update once to its "current" position:
        oldAni->updateToCurrentValue();

        // We then remove this animation from the map so it doesn’t keep running:
        delete oldAni;
        animationsUI.erase(it);
    }
}

// ========== MOVE (REORDER) MODE ==========

void MenuManager::enterMoveMode()
{
    if (moveMode) return;
    if (crossSlideState != CROSS_SLIDE_NONE) return; // not mid-transition
    if (!currentItemList || currentItemList->empty()) return;
    if ((*currentItemList)[currentIndex].isCategory) return; // leaves only

    moveMode = true;
    // The Held press still gets a Released when the finger lifts — don't
    // let it fall through to selectCurrentItem()/commitMove().
    swallowNextSelectRelease = true;
    moveSnapshot = *currentItemList; // restore point for cancel
    savedHighlightShape = highlightShape;
    highlightShape = HIGHLIGHT_PARALLELOGRAM; // visual cue: item picked up
    ESP_LOGI(TAG_MAIN, "Move mode: picked up item %d", currentIndex);
}

void MenuManager::moveItemUp()
{
    if (currentIndex <= 0) return;
    std::swap((*currentItemList)[currentIndex], (*currentItemList)[currentIndex - 1]);
    moveHighlightUp(); // highlight follows the moved item
}

void MenuManager::moveItemDown()
{
    if (currentIndex >= (int)currentItemList->size() - 1) return;
    std::swap((*currentItemList)[currentIndex], (*currentItemList)[currentIndex + 1]);
    moveHighlightDown();
}

void MenuManager::commitMove()
{
    moveMode = false;
    highlightShape = savedHighlightShape;
    moveSnapshot.clear();

    // Persist the FULL display order as one declarative arrange op so the
    // new arrangement survives reboot.
    std::vector<LoadoutManifest::ArrangeItem> order;
    collectArrangeOrder(order);
    AppManager::instance().persistMenuArrangement(order);
    ESP_LOGI(TAG_MAIN, "Move mode: committed (%d items)", (int)order.size());
}

void MenuManager::cancelMove()
{
    moveMode = false;
    highlightShape = savedHighlightShape;
    *currentItemList = moveSnapshot; // restore pre-move order
    moveSnapshot.clear();
    ESP_LOGI(TAG_MAIN, "Move mode: cancelled");
}

// Recursive helper: leaves under a top-level category carry that label
// (first path segment == the one-level flat category model); root leaves
// carry "".
static void collectLeaves(const std::vector<MenuItem>& items,
                          const std::string& topCategory,
                          std::vector<LoadoutManifest::ArrangeItem>& out)
{
    for (const auto& mi : items) {
        if (mi.isCategory) {
            collectLeaves(mi.children,
                          topCategory.empty() ? mi.label : topCategory,
                          out);
        } else {
            LoadoutManifest::ArrangeItem item;
            item.id          = appIds[mi.appIndex];
            item.category    = topCategory;
            item.hasCategory = true;
            out.push_back(item);
        }
    }
}

void MenuManager::collectArrangeOrder(std::vector<LoadoutManifest::ArrangeItem>& out) const
{
    out.clear();
    collectLeaves(rootMenuItems, std::string(), out);
}

// Called on "Select" button
// selectCurrentItem => if category => crossSlideForward
void MenuManager::selectCurrentItem()
{
    if (!currentItemList || currentItemList->empty()) return;
    
    const MenuItem &mi = (*currentItemList)[currentIndex];

    // If it’s a sub-category, do NOT immediately switch.
    // Instead, start the slide-out animation. handleMenuTransition() will do the actual switch.
    if (mi.isCategory)
    {
        if (crossSlideState == CROSS_SLIDE_NONE)
        {
            ESP_LOGI(TAG_MAIN, "MenuManager::selectCurrentItem - crossSlideForward");
            crossSlideForward(mi);
            ESP_LOGI(TAG_MAIN, "[selectCurrentItem] crossSlideState = %d", crossSlideState);
        }
    }
    else
    {
        // Leaf => launch
        menuActive = false;
        unregisterMenuCallbacks();
        AppManager::instance().switchToApp(mi.appIndex);
    }
}

// Called on "Back" button
void MenuManager::goBack()
{
    // If there's no parent, do nothing
    if (navigationStack.empty()) return;
    // If we’re mid-transition, do nothing
    if (crossSlideState != CROSS_SLIDE_NONE) return;

    // Start the backward 2-step slide
    crossSlideBackward();
}

// The CROSS-SLIDE methods
void MenuManager::crossSlideForward(const MenuItem &child)
{
    // 1) Store old pointer, new pointer
    oldItemList = currentItemList;
    newItemList = const_cast<std::vector<MenuItem>*>(&child.children);

    // 2) push old nav state so we can come back
    MenuNavState s;
    s.itemList          = currentItemList;
    s.index             = currentIndex;
    s.savedScrollOffset = scrollOffset;  // if you have vertical scrolling
    navigationStack.push_back(s);

    // We want the child highlight at row 0 from the start:
    highlightElement.setY(0); 

    // 3) We start with old at x=0, new at x=+SCREEN_WIDTH (off right)
    oldMenuX = 0;
    newMenuX = SCREEN_WIDTH;

    // 4) Animate old to -SCREEN_WIDTH, new to 0
    insertAnimation(new Animation((int*)&oldMenuX,
                                  INDENT,
                                  -SCREEN_WIDTH,  // end
                                  crossSlideDuration)); // Set type to true
    insertAnimation(new Animation((int*)&newMenuX,
                                  INDENT,
                                  0,              // end
                                  crossSlideDuration)); // Set type to true

    // 5) set crossSlideState
    crossSlideState = CROSS_SLIDE_FORWARD;
    ESP_LOGI(TAG_MAIN, "[crossSlideForward] crossSlideState = %d", crossSlideState);
}

void MenuManager::crossSlideBackward()
{
    // If no parent
    if (navigationStack.empty()) return;

    // old is the current sub-menu, new is the parent
    oldItemList = currentItemList;

    MenuNavState s = navigationStack.back();
    navigationStack.pop_back();

    newItemList = s.itemList;
    currentIndex = s.index;
    scrollOffset = s.savedScrollOffset;
    highlightElement.setY((currentIndex * MENU_ITEM_HEIGHT) - scrollOffset);

    // Start old at x=0, new at x=-SCREEN_WIDTH
    oldMenuX = 0;
    newMenuX = -SCREEN_WIDTH;

    // Animate old to +SCREEN_WIDTH, new to 0
    insertAnimation(new Animation((int*)&oldMenuX,
                                  INDENT,
                                  SCREEN_WIDTH,
                                  crossSlideDuration));
    insertAnimation(new Animation((int*)&newMenuX,
                                  INDENT,
                                  0,
                                  crossSlideDuration));

    crossSlideState = CROSS_SLIDE_BACK;
}

//---------------------------------
// Each frame, we check if the cross-slide is done
void MenuManager::handleCrossSlide()
{
    if (crossSlideState == CROSS_SLIDE_NONE) return;

    bool oldDone = isFinished(&oldMenuX);
    bool newDone = isFinished(&newMenuX);

    if (oldDone && newDone)
    {
        // The cross-slide is complete
        if (crossSlideState == CROSS_SLIDE_FORWARD)
        {
            // We are now inside the child
            currentItemList = newItemList;
            currentIndex = 0;
            scrollOffset = 0;
            highlightElement.setY(0);
        }
        else if (crossSlideState == CROSS_SLIDE_BACK)
        {
            // We are now in the parent (already set in crossSlideBackward)
            currentItemList = newItemList;
        }

        // Stop referencing old/new so we only show current
        oldItemList = nullptr;
        newItemList = nullptr;

        crossSlideState = CROSS_SLIDE_NONE;
    }
}

/**
 * @brief Redraw all items, factoring in scrollOffset for each.
 *        Also draw highlight if we want a separate highlight rect.
 */
void MenuManager::drawMenu()
{
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);

    if (crossSlideState == CROSS_SLIDE_NONE)
    {
        // Normal drawing: just draw the current menu
        drawOneMenu(currentItemList,
                    0,
                    highlightElement,
                    highlightShape,
                    currentIndex,
                    scrollOffset);
    }
    else
    {
        // CROSS-SLIDE: we have oldItemList and newItemList
        // Draw old first so new is on top
        if (newItemList)
        {
            drawOneMenu(newItemList,
                        newMenuX,
                        highlightElement,
                        highlightShape,
                        // For forward, the new starts at index=0
                        // For backward, we carried over the parent's old highlight
                        currentIndex,
                        scrollOffset);
        }
        if (oldItemList)
        {
            drawOneMenu(oldItemList,
                        oldMenuX,
                        highlightElement,
                        highlightShape,
                        currentIndex,  // or you may store oldIndex if you prefer
                        scrollOffset);
        }
    }

    display.display();
}

// Register our button callbacks:
void MenuManager::registerMenuCallbacks()
{
    // For each hardware button, call the static method we set up:
    buttonManager.registerCallback(button_LeftIndex, onButtonLeftPressed);
    buttonManager.registerCallback(button_RightIndex, onButtonRightPressed);
    buttonManager.registerCallback(button_UpIndex, onButtonUpPressed);
    buttonManager.registerCallback(button_DownIndex, onButtonDownPressed);
    buttonManager.registerCallback(button_SelectIndex, onButtonBackPressed);
    buttonManager.registerCallback(button_EnterIndex, onButtonSelectPressed);
}

// Unregister them:
void MenuManager::unregisterMenuCallbacks()
{
    buttonManager.unregisterCallback(button_LeftIndex);
    buttonManager.unregisterCallback(button_RightIndex);
    buttonManager.unregisterCallback(button_UpIndex);
    buttonManager.unregisterCallback(button_DownIndex);
    buttonManager.unregisterCallback(button_SelectIndex);
    buttonManager.unregisterCallback(button_EnterIndex);
}

// Static callback handlers:
void MenuManager::onButtonLeftPressed(const ButtonEvent& event)
{
    // Example: do nothing or implement "move to previous root category"? 
    // Since it's a nested menu, left/right might not do anything if you only have
    // vertical list navigation. Feel free to define your own logic.
}
void MenuManager::onButtonRightPressed(const ButtonEvent& event)
{
    // Press
    if (event.eventType == ButtonEvent_Pressed){
        // Same as above
    }    
}
void MenuManager::onButtonUpPressed(const ButtonEvent& event)
{
    // Press
    if (event.eventType == ButtonEvent_Pressed){
        if (instance().moveMode) instance().moveItemUp();
        else                     instance().moveHighlightUp();
    }
}
void MenuManager::onButtonDownPressed(const ButtonEvent& event)
{
    // Press
    if (event.eventType == ButtonEvent_Pressed){
        if (instance().moveMode) instance().moveItemDown();
        else                     instance().moveHighlightDown();
    }
}
void MenuManager::onButtonBackPressed(const ButtonEvent& event)
{    // Press
    if (event.eventType == ButtonEvent_Pressed){
        if (instance().moveMode) instance().cancelMove();
        else                     instance().goBack();
    }
}
void MenuManager::onButtonSelectPressed(const ButtonEvent& event)
{
    // Long-press picks up the current leaf for reordering (T-115).
    if (event.eventType == ButtonEvent_Held){
        instance().enterMoveMode();
        return;
    }
    // Press
    if (event.eventType == ButtonEvent_Released){  //Released intentionally to avoid carrying into event
        if (instance().swallowNextSelectRelease){
            // This release belongs to the long-press that entered move
            // mode — not a select.
            instance().swallowNextSelectRelease = false;
            return;
        }
        if (instance().moveMode) instance().commitMove();
        else                     instance().selectCurrentItem();
    }
}


// these are free (standalone) functions, not in the class
extern void menuBegin() __attribute__((used));
extern void menuBegin() {
    ESP_LOGI(TAG_MAIN, "menuBegin start");
    MenuManager::instance().begin();
}

extern void menuEnd() __attribute__((used));
extern void menuEnd() {
    ESP_LOGI(TAG_MAIN, "menuEnd start");
    MenuManager::instance().end();
}

extern void menuRun() __attribute__((used));
extern void menuRun() {
    // logs so we see if it's actually called
    ESP_LOGI(TAG_MAIN, "menuRun start");
    MenuManager::instance().update();
}//