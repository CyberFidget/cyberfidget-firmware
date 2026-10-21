// SPDX-License-Identifier: GPL-3.0-or-later WITH Cyberfidget-HAL-exception
// Copyright (c) 2023-2026 Dismo Industries LLC

#include "SliderPosition.h"
#include "globals.h" 
#include "HAL.h"

/* 
 * Expert-Level Comment:
 * ---------------------
 * This file implements a Kalman filter for smoothing the slider position readings.
 * Additionally, it addresses the issue of percentage value jitter by:
 * - Using floating-point percentages to maintain higher resolution.
 * - Applying an Exponential Moving Average (EMA) filter to the percentage.
 * - Optionally introducing hysteresis to reduce sensitivity to minor fluctuations.
 *
 * Novice-Level Comment:
 * ---------------------
 * This code reads the slider position and filters the readings to get a smooth value.
 * It also calculates the position as a percentage and ensures that small changes
 * in the slider don't cause big jumps in the percentage value.
 */

// Declare the EMA filter state variable
static float percentageEMA = 0.0f;

// Structure to hold the Kalman filter state
struct KalmanState {
    float position;   // Estimated position
    float velocity;   // Estimated velocity
    float P[2][2];    // Error covariance matrix
};

// Kalman filter parameters (initialized once)
// These parameters can be fine-tuned to adjust the filter's responsiveness.
const struct KalmanParameters {
    float Q[2][2]; // Process noise covariance matrix
    float R;       // Measurement noise covariance
} params_12Bits = {
  {{1.0f, 0.0f}, {0.0f, 1.0f}}, // Q: CALIBRATION - Confidence in the model (higher values make the filter more responsive)
  1.0f                          // R: CALIBRATION - Confidence in the measurements (lower values trust measurements more)
};

// Initialize the KalmanState for the 12-bit signal
static KalmanState kalmanState_12Bits;

// Initialize the Kalman filter state
void sliderPositionKalmanFilterInit() {
    // Initial error covariance P
    kalmanState_12Bits.P[0][0] = 1.0f;
    kalmanState_12Bits.P[0][1] = 0.0f;
    kalmanState_12Bits.P[1][0] = 0.0f;
    kalmanState_12Bits.P[1][1] = 1.0f;

    // Initial position and velocity estimates
    kalmanState_12Bits.position = 0.0f;
    kalmanState_12Bits.velocity = 0.0f;
}

// Initialize the filtered values and the EMA filter state
void sliderPositionFilterInit() {
    // Initialize filtered slider positions
    sliderPosition_12Bits_Filtered = 0.0f;
    sliderPosition_12Bits_Inverted_Filtered = 0.0f;
    sliderPosition_8Bits_Filtered = 0;
    sliderPosition_8Bits_Inverted_Filtered = 0;
    sliderPosition_Percentage_Filtered = 0.0f;
    sliderPosition_Percentage_Filtered_Old = 0.0f;
    sliderPosition_Percentage_Inverted_Filtered = 0.0f;

    // Initialize the Kalman filter
    sliderPositionKalmanFilterInit();

    // Initialize the EMA filter state for percentage
    percentageEMA = 0.0f;
}

// Corrected Kalman filter update function
void updateKalmanFilter(KalmanState &state, float measurement, float &filteredOutput) {
    const float deltaTime = 0.02f; // Assuming 20ms task cycle

    // Predict state
    float predicted_position = state.position + state.velocity * deltaTime;
    float predicted_velocity = state.velocity;

    // Predict error covariance
    float P00 = state.P[0][0] + deltaTime * (state.P[1][0] + state.P[0][1]) + params_12Bits.Q[0][0];
    float P01 = state.P[0][1] + deltaTime * state.P[1][1] + params_12Bits.Q[0][1];
    float P10 = state.P[1][0] + deltaTime * state.P[1][1] + params_12Bits.Q[1][0];
    float P11 = state.P[1][1] + params_12Bits.Q[1][1];

    // Measurement update
    float S = P00 + params_12Bits.R;
    float K0 = P00 / S;
    float K1 = P10 / S;

    float y = measurement - predicted_position;

    // Update estimate
    state.position = predicted_position + K0 * y;
    state.velocity = predicted_velocity + K1 * y;

    // Update error covariance using predicted values
    state.P[0][0] = P00 - K0 * P00;
    state.P[0][1] = P01 - K0 * P01;
    state.P[1][0] = P10 - K1 * P00;
    state.P[1][1] = P11 - K1 * P01;

    // Output the filtered position
    filteredOutput = state.position;
}

// Clamp and convert a float value to an integer within the specified range
int clampAndConvertToInt(float value, int minVal, int maxVal) {
    if (value < minVal) value = minVal;
    if (value > maxVal) value = maxVal;
    return static_cast<int>(value);
}

// Apply a rate limit to the currentValue based on the newValue and rateLimit
void applyRateLimit(float &currentValue, float newValue, float rateLimit) {
    // Novice-Level Comment:
    // This function limits how fast the currentValue can change.
    // If the newValue is much higher or lower than currentValue,
    // it only allows it to change by rateLimit at a time.

    float delta = newValue - currentValue;
    if (fabs(delta) > rateLimit) {
        currentValue += copysign(rateLimit, delta);
    } else {
        currentValue = newValue;
    }
}

// Read and filter the slider position
void sliderPositionRead(int sliderVoltagePin) {
    // Read the raw slider position in millivolts and 12-bit ADC value
    sliderPosition_Millivolts = analogReadMilliVolts(sliderVoltagePin);
    sliderPosition_12Bits = analogRead(sliderVoltagePin);

    float filtered_12Bits;

    // Apply the corrected Kalman filter to the primary 12-bit variable
    updateKalmanFilter(kalmanState_12Bits, static_cast<float>(sliderPosition_12Bits), filtered_12Bits);

    // Clamp and convert the filtered 12-bit value to an integer between 0 and 4095
    sliderPosition_12Bits_Filtered = filtered_12Bits;
    if (sliderPosition_12Bits_Filtered < 0.0f) sliderPosition_12Bits_Filtered = 0.0f;
    if (sliderPosition_12Bits_Filtered > 4095.0f) sliderPosition_12Bits_Filtered = 4095.0f;
    sliderPosition_12Bits_Inverted_Filtered = 4095.0f - sliderPosition_12Bits_Filtered;

    // Calculate the percentage as a floating-point value to retain precision
    float percentage = (filtered_12Bits * 100.0f) / 4095.0f;

    // CALIBRATION
    // Apply an Exponential Moving Average (EMA) filter to the percentage
    // The EMA filter smooths out the percentage value to reduce the effect of small changes.
    // The smoothing factor alpha determines how much weight is given to new measurements.
    const float alpha = 0.38f; // Alpha between 0 (slow response) and 1 (fast response)

    // Update the EMA filter state
    percentageEMA = alpha * percentage + (1.0f - alpha) * percentageEMA;

    // CALIBRATION
    // Optionally, introduce hysteresis to prevent minor fluctuations
    // Only update the displayed percentage if the change exceeds a threshold
    static float lastDisplayedPercentage = 0.0f;
    const float hysteresisThreshold = 0.5f; // Update only if the change is greater than 0.5%

    if (fabs(percentageEMA - lastDisplayedPercentage) >= hysteresisThreshold) {
        sliderPosition_Percentage_Filtered = percentageEMA;
        lastDisplayedPercentage = percentageEMA;
    }

    // Ensure the percentage is within 0 to 100
    if (sliderPosition_Percentage_Filtered < 0.0f) sliderPosition_Percentage_Filtered = 0.0f;
    if (sliderPosition_Percentage_Filtered > 100.0f) sliderPosition_Percentage_Filtered = 100.0f;

    // Calculate the inverted percentage
    sliderPosition_Percentage_Inverted_Filtered = 100.0f - sliderPosition_Percentage_Filtered;

    // Similarly, calculate the 8-bit filtered values using the filtered 12-bit value
    float filtered_8Bits = (filtered_12Bits * 255.0f) / 4095.0f;

    // Clamp and convert to integers
    if (filtered_8Bits < 0.0f) filtered_8Bits = 0.0f;
    if (filtered_8Bits > 255.0f) filtered_8Bits = 255.0f;

    sliderPosition_8Bits_Filtered = static_cast<int>(filtered_8Bits);
    sliderPosition_8Bits_Inverted_Filtered = 255 - sliderPosition_8Bits_Filtered;

    // Check if we had a user interaction for keep-alives
    if (sliderPosition_Percentage_Filtered != sliderPosition_Percentage_Filtered_Old) {
        sliderPosition_Percentage_Filtered_Old = sliderPosition_Percentage_Filtered;
        millis_APP_LASTINTERACTION = millis_NOW; // Reset last interaction time
    }
}

// Optional function to update the EMA filter separately if needed
void sliderPositionFilterUpdate() {
    // This function can be used if the EMA filter needs to be updated separately
    // For now, it's implemented inside sliderPositionRead()
    // Left empty or can be removed if not needed
}

/*
 * Additional Notes:
 *
 * - The Kalman filter parameters (`Q` and `R`) can be adjusted to fine-tune the filter's responsiveness and noise rejection.
 * - The EMA filter smoothing factor (`alpha`) controls how quickly the percentage responds to changes.
 * - The hysteresis threshold helps to prevent the percentage output from updating due to very small changes, reducing jitter.
 *
 * Novice-Level Explanation:
 * -------------------------
 * In this code:
 * - We read the slider's raw position.
 * - We use a Kalman filter to smooth out the raw readings.
 * - We calculate the slider's position as a percentage, keeping the decimal part.
 * - We apply an EMA filter to the percentage to smooth out small changes.
 * - We only update the displayed percentage if the change is significant.
 * - This reduces the jitter caused by minor fluctuations in the slider readings.
 *
 * Expert-Level Explanation:
 * -------------------------
 * - The Kalman filter smooths the input signal by estimating the true position and velocity.
 * - The EMA filter provides additional smoothing to the percentage value, which can be sensitive to small changes due to quantization.
 * - Introducing hysteresis ensures that minor fluctuations do not cause the output to update unnecessarily, reducing perceived jitter.
 * - By maintaining higher resolution in the percentage calculation (using floats), we minimize quantization errors during mapping.
 * - Overall, these combined strategies improve the stability and responsiveness of the percentage output.
 */
