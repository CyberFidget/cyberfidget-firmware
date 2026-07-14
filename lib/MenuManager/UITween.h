// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#ifndef UITWEEN_H
#define UITWEEN_H

#include <Arduino.h>
#include <map>
#include <list>
#include <vector>
#include "UIElement.h"

/**
 * @brief The supported easing types:
 *   - LINEAR: Simple linear interpolation
 *   - INDENT: Exponential ease-out (like a “mouse wheel” feel)
 *   - INDENTINV: The inverse of INDENT
 *   - BOUNCE: Bouncy easing curve
 *   - PARALLELOGRAM: A new "cyberpunk skew" animation using an “easeInOutBack” style
 */
enum UIEaseType {
    LINEAR,
    INDENT,
    INDENTINV,
    BOUNCE,
    PARALLELOGRAM
};

class UITween {
public:
    /**
     * @brief Constructor for animating a UIElement’s position only (X,Y).
     *
     * @param targetUI The UIElement to animate
     * @param aniType  Which easing function (LINEAR, INDENT, etc.)
     * @param endX,endY  Target position (px)
     * @param totalTime   Duration in ms
     */
    UITween(UIElement* targetUI,
              UIEaseType aniType,
              int endX,
              int endY,
              int totalTime);

    /**
     * @brief Constructor for animating a UIElement’s position + size (X,Y,W,H).
     */
    UITween(UIElement* targetUI,
              UIEaseType aniType,
              int endX,
              int endY,
              int endWidth,
              int endHeight,
              int totalTime);

    /**
     * @brief Constructor for animating a single int* (e.g. brightness).
     */
    UITween(int* targetVal,
              UIEaseType aniType,
              int endVal,
              int totalTime);

    /**
     * @brief Overloaded versions that also include a delayTime before the animation starts.
     */
    UITween(UIElement* targetUI,
              UIEaseType aniType,
              int endX,
              int endY,
              int totalTime,
              int delayTime);

    UITween(UIElement* targetUI,
              UIEaseType aniType,
              int endX,
              int endY,
              int endWidth,
              int endHeight,
              int totalTime,
              int delayTime);

    UITween(int* targetVal,
              UIEaseType aniType,
              int endVal,
              int totalTime,
              int delayTime);

    /**
     * @brief NEW Constructor for animating position/size **plus** a "skew" property,
     *        if your UIElement supports setSkew()/getSkew().
     *
     * @param targetUI   The UIElement to animate
     * @param aniType    Typically PARALLELOGRAM if you want that “cyberpunk” skew effect
     * @param endX,endY,endWidth,endHeight   The usual geometry
     * @param endSkew    The final skew value
     * @param totalTime  Duration
     * @param delayTime  Optional delay before animation
     */
    UITween(UIElement* targetUI,
              UIEaseType aniType,
              int endX,
              int endY,
              int endWidth,
              int endHeight,
              float endSkew,
              int totalTime,
              int delayTime = 0);

    /**
     * @brief Initialize the animation (called once when the animation is inserted).
     */
    void init();

    /**
     * @brief Check if our delayTime has elapsed and we can start animating.
     */
    bool checkTime();

    /**
     * @brief Called once when we actually begin (after delayTime).
     */
    void initAni();

    /**
     * @brief Main function that updates the animation by one step, using our chosen easing.
     */
    void animate();

    /**
     * @brief Simple getters for flags
     */
    bool getIsStarted();
    bool getIsAnimating();
    bool getIsFinished();

    /**
     * @brief Accessors for the target object
     */
    UIElement* getTargetElement();
    int* getTargetVal();

    /**
     * @brief Forces a one-time update of this animation to whatever fraction
     *        of time has elapsed (0..1). This is useful if you want to get the
     *        "in-between" position before starting a brand-new animation.
     *
     *        After this call, the target’s X/Y (or int) is set to the correct
     *        partial position. If the animation has fully elapsed, it is marked
     *        finished and the target is snapped to the final.
     */
    void updateToCurrentValue();

private:
    // Internal helpers
    void updateTime();
    int getCurDuration();
    int getTotDuration();

    // Subroutines for each easing style
    void animateLinear();
    void animateIndent();
    void animateIndentInv();
    void animateBounce();

    /**
     * @brief Our new “PARALLELOGRAM” style,
     *        which uses a variant of “easeInOutBack” to animate X/Y/Width/Height + Skew.
     */
    void animateParallelogram();

private:
    /**
     * @brief Type: 0 => animating a whole UIElement, 1 => animating a single int*.
     */
    bool type;

    // If type=1 (int*), we store:
    int* targetVal;
    int  startVal, endVal;

    // If type=0 (UIElement*), we store geometry:
    UIElement* targetUI;

    int startX, startY, endX, endY;
    int startWidth, startHeight, endWidth, endHeight;

    // For optional skew:
    float startSkew = 0.0f;
    float endSkew   = 0.0f;
    bool  useSkew   = false; // true if this animation includes skew

    // Easing type
    UIEaseType aniType;

    // Timing
    int  prevTime;
    int  startTime;
    int  createTime;
    int  totalTime;
    int  delayTime;

    // Flags
    bool isStarted   = false;
    bool isFinished  = false;
    bool isAnimating = false;
};

// Global data structures to store active tweens:
extern std::map<UIElement*, UITween*> tweensUI;
extern std::map<int*, UITween*>       tweensInt;
extern std::list<UIElement*>          tmpTweenUI;

/**
 * @brief Insert a new UITween into the system.
 *        If there's an existing tween for the same target object, it’s replaced.
 */
void insertTween(UITween* animation);

/**
 * @brief Check whether an int* is currently being animated (i.e., not found => finished).
 */
bool isFinished(int* targetInt);

/**
 * @brief Check whether a UIElement* is currently being animated.
 */
bool isFinished(UIElement* targetUI);

/**
 * @brief Used to handle ephemeral UIElements that should be removed post-animation.
 *        Typically used in “aniOut()” calls or transitions.
 */
void insertTmpAnimationPointer(UIElement* tmpUI);

/**
 * @brief Called repeatedly (e.g. every frame or every ~10ms) to update all tweens.
 */
void tweenAll();

/**
 * @brief Cleans up items in tmpTweenUI that have finished animating
 */
void updateTmp();

#endif // UITWEEN_H
