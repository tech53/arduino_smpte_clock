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

volatile bool clockStarted = false;  // SMPTE clock state (false until MIDI trigger or button press)

// MIDI definitions
#define MIDI_BAUD 31250  // MIDI baud rate is 31.25 kbps
#define MIDI_INPUT_PIN PD0  // Define the MIDI input pin

// Button configuration
#define BUTTON_PIN PD2  // Define the pin connected to the button

// MIDI message types
#define MIDI_TIME_CODE_QUARTER_FRAME 0xF1
#define MIDI_START 0xFA
#define MIDI_STOP 0xFC

// Variables to track incoming MIDI Timecode
volatile unsigned char mtcQuarterFrame[8];  // Store quarter-frame data
volatile unsigned char mtcFrameCounter = 0;

// USART initialization for MIDI input (31.25 kbps)
void initUSART(void)
{
  // Set baud rate for MIDI (31.25 kbps)
  unsigned int ubrr = (F_CPU / (16UL * MIDI_BAUD)) - 1;
  UBRR0H = (unsigned char)(ubrr >> 8);
  UBRR0L = (unsigned char)ubrr;

  // Enable receiver and RX complete interrupt
  UCSR0B = (1 << RXEN0) | (1 << RXCIE0);

  // Set frame format: 8 data bits, 1 stop bit
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

// USART Receive Interrupt Service Routine (ISR) for MIDI
ISR(USART_RX_vect)
{
  unsigned char midiByte = UDR0;  // Read the incoming byte from the UART buffer

  // Check if the byte is a quarter-frame message (MTC)
  if ((midiByte & 0xF0) == MIDI_TIME_CODE_QUARTER_FRAME)
  {
    // Extract the frame data from the message
    unsigned char messageType = midiByte & 0x0F;  // Get the quarter-frame type (0-7)
    unsigned char frameData = midiByte >> 4;      // Get the data

    // Store the quarter-frame data
    mtcQuarterFrame[messageType] = frameData;

    // When we receive the last quarter-frame (type 7), reconstruct the timecode
    if (messageType == 7)
    {
      frameCount = (mtcQuarterFrame[1] & 0x0F) + ((mtcQuarterFrame[0] & 0x01) << 4);  // Frames
      secondCount = (mtcQuarterFrame[3] & 0x0F) + ((mtcQuarterFrame[2] & 0x03) << 4); // Seconds
      minuteCount = (mtcQuarterFrame[5] & 0x0F) + ((mtcQuarterFrame[4] & 0x03) << 4); // Minutes
      hourCount = (mtcQuarterFrame[7] & 0x0F) + ((mtcQuarterFrame[6] & 0x01) << 4);   // Hours
      
      clockStarted = true;  // Start the SMPTE clock when we have a full timecode
    }
  }
}

// Initialize the button for timecode start
void initButton(void)
{
  // Set BUTTON_PIN as input and enable the internal pull-up resistor
  DDRD &= ~(1 << BUTTON_PIN);  // Set PD2 (BUTTON_PIN) as input
  PORTD |= (1 << BUTTON_PIN);  // Enable pull-up resistor on PD2
}

void setup()
{
  DDRD = 0b00101000;  // Set pins for output

  // Initialize MIDI input (USART)
  initUSART();

  // Initialize the start button
  initButton();

  // Configure Timer2 for fast PWM output
  TCCR2A = _BV(COM2A1) | _BV(WGM21) | _BV(WGM20); // Fast PWM mode, non-inverting output
  TCCR2B = _BV(CS21);  // Prescaler of 8

  // Configure Timer1 for SMPTE LTC bit timing
  TCCR1A = 0b00 << WGM10;
  TCCR1B = _BV(WGM12) | _BV(CS11); // CTC mode, prescaler 8
  OCR1A = LTC_BIT_DURATION;  // Set the duration of each bit
  TIMSK1 = _BV(OCIE1A);  // Enable interrupt on compare match

  sei();  // Enable global interrupts
}

// Add the loop function to handle the button press logic
void loop()
{
  // Wait for either a MIDI Timecode or button press to start the clock
  if (!clockStarted)
  {
    // If the button is pressed (BUTTON_PIN goes LOW)
    if (!(PIND & (1 << BUTTON_PIN)))
    {
      clockStarted = true;  // Start the clock when the button is pressed
    }
  }
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
