#include "speed_clock.h"

static bool near(double a,double b) {double d=a-b;return d>-.00001 && d<.00001;}
extern "C" int runSpeedClockTests() {
    if(!PracticeSpeedClock::validRate(.5f) || !PracticeSpeedClock::validRate(1.5f) ||
       PracticeSpeedClock::validRate(.49f) || PracticeSpeedClock::validRate(1.51f) ||
       PracticeSpeedClock::validRate(__builtin_nanf("")) || PracticeSpeedClock::validRate(__builtin_inff())) return 1;
    PracticeSpeedClock c;
    c.seek(100,10,true);c.change(.75f,10);
    if(!near(c.at(50),130)) return 2;
    c.change(1.5f,50);
    if(!near(c.at(50),130) || !near(c.at(60),145)) return 3;
    c.pause(60);
    if(!near(c.at(200),145)) return 4;
    c.change(.5f,100);
    if(!near(c.at(200),145)) return 5;
    c.seek(12.5,200,true);
    if(!near(c.at(202),13.5)) return 6;
    c.seek(2.5,202,true);
    if(!near(c.at(206),4.5)) return 7;
    c.seek(40,206,true);
    if(!near(c.at(210),42)) return 8;
    c.change(1,210);
    if(!near(c.at(210),42) || !near(c.at(220),52)) return 9;
    c.seek(0,300,true);c.change(.5f,299);
    if(!near(c.at(299.5),0) || !near(c.at(302),1)) return 10;
    c.seek(0,1000000,true);c.change(.75f,1000000);
    for(int i=0;i<=72000;++i)
        if(!near(c.at(1000000+i/120.),i/160.)) return 11;
    return 0;
}
