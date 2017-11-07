// this code is for EDP2 heart rate monitor - Team D;
#include "mbed.h"

#define max7219_reg_noop         0x00
#define max7219_reg_digit0       0x01
#define max7219_reg_digit1       0x02
#define max7219_reg_digit2       0x03
#define max7219_reg_digit3       0x04
#define max7219_reg_digit4       0x05
#define max7219_reg_digit5       0x06
#define max7219_reg_digit6       0x07
#define max7219_reg_digit7       0x08
#define max7219_reg_decodeMode   0x09
#define max7219_reg_intensity    0x0a
#define max7219_reg_scanLimit    0x0b
#define max7219_reg_shutdown     0x0c
#define max7219_reg_displayTest  0x0f

#define LOW 0
#define HIGH 1

SPI max72_spi(PTD2, NC, PTD1);
DigitalOut load(PTD0); //will provide the load signal

AnalogIn Ain(PTB1); //The photodiode signal
Serial pc(USBTX, USBRX);
DigitalIn enable(PTB0); //The input signal from the push button

Ticker DataIn;

#define dbSize  80
#define abSize  9

//Data initialisation
int dataLow = 0;
int dataHigh = 0;
int avgBuffer = 0;
int mode = 1; // mode = 0: shows graph of each pulse , mode = 1: shows the numberical heart rate
float dataBuffer[dbSize] = {}; // about int 80;
float averageBuffer[abSize] = {};  // about int 8-10;

//Basic display pattern which will be modified during the program's operantion
char  pattern_display[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

//Static patterns used for outputing the stationary patterns.
char  pattern_blank[8] = { 0x00, 0x00, 0x00,0x00,0x00,0x00,0x00,0x00};
char  pattern_heart[8] = { 0x00, 0x38, 0x44,0x42,0x21,0x42,0x44,0x38};
char  pattern_cross[8] = { 0x81, 0x42, 0x24,0x18,0x18,0x24,0x42,0x81};


// Function prototype for interrupt service routine (80 Hz)
void interrupt(void);

/*
    A simple bit-map font for the digits 0-9. Data is stored bitwise in rows,
    grouped by columns. 5 rows and 4 columns per digit.
    Storage format:
    bit0: RRRRR000RRRRR000RRRRR000RRRRR000 :bit31
    col:    0       1       2       3
    row:  43210   43210   43210   43210
    where the standard top-left co-ordinate system is used
*/
const uint32_t char5x4bitmap[10] = {
    0x1F111F00, 0x1F000000, 0x9151300, 0x1F151100, 0x1F041C00,
    0x17151D00, 0x17151F00, 0x1F101000, 0x1F151F00, 0x1F141C00
};

/*
*********************** START OF DISPLAY FUNCTIONS ****************************
*/
/*
    write_to_max writes to a specific register on the MAX7221 display driver
*/
void write_to_max( int reg, int col)
{
    load = LOW;            // begin
    max72_spi.write(reg);  // specify register
    max72_spi.write(col);  // put data
    load = HIGH;           // make sure data is loaded (on rising edge of LOAD/CS)
}

/*
    pattern_to_display sends an array of 8 digits to the MAX7221, left to right
*/
void pattern_to_display(const char *testdata)
{
    int cdata;
    for(int idx = 0; idx <= 7; idx++) {
        cdata = testdata[idx];
        write_to_max(idx+1,cdata);
    }
}

/*
   displayNumber prints the passed integer to the display. Input must be
   between 0 and 299.
*/
void displayNumber(int k) {
    // Separate each digit which is to be written to the display
    int left = k % 10;
    k /= 10;
    int middle = k % 10;
    k /= 10;
    int right = k % 10;

    // Print digits column by column
    char pattern[8];
    for (int i = 0; i < 4; i++) {
        pattern[i] = char5x4bitmap[middle] >> (8 * i) & 0xff;
    }
    for (int i = 4; i < 8; i++) {
        pattern[i] = char5x4bitmap[left] >> (8 * (i - 4)) & 0xff;
    }
    if (right > 0) {
        pattern[0] |= 0xE0;
    }
    pattern_to_display(pattern);
}

/*
    setup_dot_matrix configures the display for use
*/
void setup_dot_matrix () {
    // initiation of the max 7219
    // SPI setup: 8 bits, mode 0
    max72_spi.format(8, 0);
    max72_spi.frequency(100000); //down to 100khx easier to scope 
    write_to_max(max7219_reg_scanLimit, 0x07);
    write_to_max(max7219_reg_decodeMode, 0x00);  // using an led matrix (not digits)
    write_to_max(max7219_reg_shutdown, 0x01);    // not in shutdown mode
    write_to_max(max7219_reg_displayTest, 0x00); // no display test
    for (int e=1; e<=8; e++) {    // empty registers, turn all LEDs off
        write_to_max(e,0);
    }
    write_to_max(max7219_reg_intensity,  0x08);
}

/*
    clear turns all LEDs to off
*/
void clear() {
    for (int e=1; e<=8; e++) {    // empty registers, turn all LEDs off
        write_to_max(e,0);
    }
}

/*
******************** START OF DATA PROCESSING FUNCTIONS ************************
*/

/*
    Adds the new char into the first 1/8th of the display after shifting each existing 1/8th forward one position and deleting the last
*/
void update(char pattern, char newChar)
{
    for(int i = 0; i <= 6; i++) {
        pattern[i] = pattern[i+1];
    }
    pattern[7] = newChar;
}

/*
    Returns the maximum value inside a given array.
*/
float maxFloat(const float* array, int arraySize)
{
    float max = -INFINITY;
    for (int i = 0; i < arraySize; i++) {
        if (array[i] > max) max = array[i];
    }
    return max;
}

/*
    Returns the minimum value inside a given array.
*/
float minFloat(const float* array, int arraySize)
{
    float min = +INFINITY;
    for (int i = 0; i < arraySize; i++) {
        if (array[i] < min) min = array[i];
    }
    return min;
}

int main() {
    // Variable declerations:
    float min = 1.0; // minimum value in average buffer
    float max = 0.0; // maximum value in average buffer
    float avgSum = 0.0; // sum of values in average buffer
    int peak = 0; // stores 1 if last input was a peak
    int peakCount = 0; // stores the number of peaks within a 10 second period
    int peakCycles = 0; // number of cycles elapsed since pulse was last calc

    float heartRate = 0;; // the last recorded pulse rate

    setup_dot_matrix ();      /* setup matric */

    //Displays a side scrolling heart on the display for the first several seconds of the program's operation.

    for(int i; i < 20; i++) {
        pattern_to_display(pattern_heart);
        wait(0.2);
        char temp = pattern_heart[7];
        for(int i = 7; i > 0; i--) {
            pattern_heart[i] = pattern_heart[i-1];
        }
        pattern_heart[0] = temp;
    }

    DataIn.attach(&interrupt, 0.0125);

    while(1) {
        if ((dataLow - dataHigh >= 8 )
                || (dataLow < dataHigh && (dataLow + dbSize - dataHigh >= 8))) {
            /*
               This code calculates an average input value after 10 (or more)
               readings have been taken.
            */
            float sum = 0;
            for (int n = 0; n < 8; n++) {
                sum += dataBuffer[dataHigh];
                dataHigh = (dataHigh + 1) % dbSize;
            }
            float currentAvg = sum/8;

            /*
                Update average buffer with new value amd recalculate rolling
                average, minimum and maximum value.
            */
            float trash_data = averageBuffer[avgBuffer];
            avgSum = avgSum - trash_data + currentAvg; // update sum

            // Update average buffer
            averageBuffer[avgBuffer] = currentAvg;
            avgBuffer = (avgBuffer + 1) % abSize; // increase buffer pointer

            // Update maximum and minimum only if neccesary (either the new
            // value is a max/min or the value deleted was the previous max/min)
            if (currentAvg > max) max = currentAvg;
            else {
                if (trash_data == max) max = maxFloat(averageBuffer, abSize);
            }
            if (currentAvg < min) min = currentAvg;
            else {
                if (trash_data == min) min = minFloat(averageBuffer, abSize);
            }
            //Recalculate the rolling average

            // Normalise reading so that is lies in the range between 0 and 1
            float range = max - min;
            if (range<0.009) {
                pattern_to_display(pattern_cross);
                wait(0.2);
                pattern_to_display(pattern_blank);
                wait(0.2);
            } else {
                currentAvg = (currentAvg - min) / range;

                // Detect a peak if normalised reading greater than a threshold
                if (peak == 0 && currentAvg > 0.55) {
                    peak = 1;
                    peakCount++;
                } else if (peak == 1 && currentAvg < 0.45) {
                    peak = 0;
                }

                // Increment cycle count and recompute heartRate if 15s elapsed
                peakCycles++;
                if (peakCycles >= 15.0 / 0.1) {
                    heartRate = 4 * (float) peakCount;

                    peakCycles = 0;
                    peakCount = 0;
                }

                // Scale reading so that it lies between 0 and 9
                currentAvg = currentAvg * 9;

                // Determine what to update display with
                if (mode != 0) {
                    char outputChar;
                    if (currentAvg <= 1) {
                        outputChar = 0x00;
                    } else if (currentAvg <= 2) {
                        outputChar = 0x01;
                    } else if (currentAvg <= 3) {
                        outputChar = 0x03;
                    } else if (currentAvg <= 4) {
                        outputChar = 0x07;
                    } else if (currentAvg <= 5) {
                        outputChar = 0x0f;
                    } else if (currentAvg <= 6) {
                        outputChar = 0x1f;
                    } else if (currentAvg <= 7) {
                        outputChar = 0x3f;
                    } else if (currentAvg <= 8) {
                        outputChar = 0x7f;
                    } else if (currentAvg > 8) {
                        outputChar = 0xff;
                    }
                    // update display
                    update(pattern_display, outputChar);
                    pattern_to_display(pattern_display);
                } else {
                    displayNumber( (int) heartRate);
                }
            }
        }
    }
}
/* Stores the past states of the switch in bits */
int switch_states = 0xffffffff;
int hasToggled = 0;
/*
    This ISR is attached to a ticker. It stores a value from the analog input
    into the circular data buffer.
*/
void interrupt()
{
    // Data collection:
    dataBuffer[dataLow] = Ain;
    dataLow = (dataLow+1) % dbSize;

    // Switch debouncing:
    switch_states = (switch_states << 1) | enable.read();
    if (hasToggled == 0 && (switch_states & 0x1f) == 0x00) {
        mode = !mode;
        hasToggled = 1;
    } else if (hasToggled == 1 && (switch_states & 0x1f) == 0x1f) {
        hasToggled = 0;
    }
}
