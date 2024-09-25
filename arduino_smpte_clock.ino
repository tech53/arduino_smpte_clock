#include <avr/io.h>
#include <avr/interrupt.h>

#define highLevel 82
#define lowLevel 44
#define groundLevel 63

// SMPTE timecode variables
volatile unsigned char hourCount = 0;
volatile unsigned char minuteCount = 0;
volatile unsigned char secondCount = 0;
volatile unsigned char frameCount = 0; // Frame count (0 to 23 or 29 depending on FPS)

volatile unsigned char bitCount = 0;
volatile unsigned char currentBit = 0;
volatile unsigned char lastLevel = 0;

#define FPS 30  // Frames per second (24, 25, or 30 FPS depending on your setup)

// LTC settings
#define LTC_BIT_DURATION 488  // Duration of each LTC bit in microseconds at 30fps

void setup()
{
  DDRD = 0b00101000;  // Set pin as output
  
  // Configure Timer2 for fast PWM output
  TCCR2A = _BV(COM2A1) | _BV(WGM21) | _BV(WGM20); // Fast PWM mode, non-inverting output
  TCCR2B = _BV(CS21);  // Prescaler of 8

  // Configure Timer1 for SMPTE LTC bit timing
  TCCR1A = 0b00 << WGM10;
  TCCR1B = _BV(WGM12) | _BV(CS11); // CTC mode, prescaler 8
  OCR1A = LTC_BIT_DURATION;  // Set the duration of each bit
  TIMSK1 = _BV(OCIE1A);  // Enable interrupt on compare match
  
  sei();  // Enable global interrupts

  while (1)
    ;
}

// Encode each SMPTE bit as LTC using Manchester encoding
void setLTCLevel(void)
{
  // This function is responsible for outputting each bit using Manchester encoding
  if (bitCount < 80)
  {
    // Calculate the current bit from SMPTE timecode (frames, seconds, minutes, hours)
    if (bitCount < 32) {
      // Frames
      currentBit = (frameCount >> (bitCount % 4)) & 1;
    } else if (bitCount < 40) {
      // Seconds
      currentBit = (secondCount >> (bitCount - 32)) & 1;
    } else if (bitCount < 48) {
      // Minutes
      currentBit = (minuteCount >> (bitCount - 40)) & 1;
    } else {
      // Hours
      currentBit = (hourCount >> (bitCount - 48)) & 1;
    }

    // Manchester encoding: A '0' is a low-to-high transition, a '1' is a high-to-low transition
    if (currentBit == 0) {
      OCR2A = lowLevel;  // First half of '0' bit
    } else {
      OCR2A = highLevel; // First half of '1' bit
    }
    lastLevel = currentBit;
    bitCount++;
  }
  else
  {
    // After sending 80 bits, reset the bit counter
    bitCount = 0;
    OCR2A = groundLevel; // Reset to ground level after completing one frame
  }
}

// Update SMPTE timecode values (increment frames, seconds, minutes, hours)
void timeUpdate(void)
{
  // Update frame count and overflow to seconds, minutes, hours
  if (frameCount < FPS - 1)
  {
    frameCount++;
  }
  else
  {
    frameCount = 0;
    if (secondCount < 59)
    {
      secondCount++;
    }
    else
    {
      secondCount = 0;
      if (minuteCount < 59)
      {
        minuteCount++;
      }
      else
      {
        minuteCount = 0;
        if (hourCount < 23)
        {
          hourCount++;
        }
        else
        {
          hourCount = 0;
        }
      }
    }
  }
}

// Timer1 interrupt handler: Trigger every LTC bit period
ISR(TIMER1_COMPA_vect)
{
  // Output each LTC bit (Manchester encoding) and update timecode when necessary
  setLTCLevel();

  // After completing one SMPTE frame, update the timecode
  if (bitCount == 80) {
    timeUpdate();
  }
}
