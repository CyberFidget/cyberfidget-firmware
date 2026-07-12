#ifndef STOPWATCH_DEMO_H
#define STOPWATCH_DEMO_H
// A representative website-generated app for the T-192 device-compile proof:
// a stopwatch using only device-safe HAL (display + buttons). Plain class
// with begin/update/end + a button callback - the generated-app shape.
class StopwatchDemo {
public:
    void begin();
    void update();
    void end();
    void onButton(const ButtonEvent& e);
private:
    unsigned long startMs = 0;
    unsigned long accumMs = 0;
    bool running = false;
};
extern StopwatchDemo stopwatchDemo;
#endif
