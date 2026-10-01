#ifndef STOPWATCH_DEMO_H
#define STOPWATCH_DEMO_H
// Representative website-generated app for the device-compile demonstration:
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
