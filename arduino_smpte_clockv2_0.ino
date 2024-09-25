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
#define LTC_BIT_DURATION 488  // Duration of each LTC bit in microseconds at 30fps
volatile bool clockStarted = false;  // SMPTE clock state (false until MIDI trigger or button press)
volatile bool clockPaused = false;   // True if the clock is paused

// MIDI settings
#define MIDI_BAUD 31250  // MIDI baud rate is 31.25 kbps
#define MIDI_INPUT_PIN PD0  // Define the MIDI input pin
#define MIDI_OUTPUT_PIN PD1 // Define the MIDI output pin (TX)

// Button configuration
#define START_BUTTON_PIN PD2  // Button to start/resume timecode
#define STOP_BUTTON_PIN PD3   // Button to stop/pause timecode

// Transport control states
#define TRANSPORT_STOPPED 0
#define TRANSPORT_RUNNING 1
#define TRANSPORT_PAUSED 2

volatile uint8_t transportState = TRANSPORT_STOPPED; // Track the transport state

// MIDI message types
#define MIDI_TIME_CODE_QUARTER_FRAME 0xF1
#define MIDI_START 0xFA
#define MIDI_STOP 0xFC

// Variables to track incoming and outgoing MIDI Timecode
volatile unsigned char mtcQuarterFrame[8];  // Store quarter-frame data
volatile unsigned char mtcFrameCounter = 0;
volatile uint8_t ppq = 24;  // Pulse Per Quarter default

// USART initialization for MIDI input and output (31.25 kbps)
void initUSART(void)
{
  // Set baud rate for MIDI (31.25 kbps)
  unsigned int ubrr = (F_CPU / (16UL * MIDI_BAUD)) - 1;
  UBRR0H = (unsigned char)(ubrr >> 8);
  UBRR0L = (unsigned char)ubrr;

  // Enable receiver, transmitter and RX complete interrupt
  UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);

  // Set frame format: 8 data bits, 1 stop bit
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

// USART Transmit function for sending MIDI bytes
void sendMIDIByte(unsigned char byte)
{
  while (!(UCSR0A & (1 << UDRE0)));  // Wait for the transmit buffer to be empty
  UDR0 = byte;  // Transmit the byte
}

// Transmit MTC Quarter-Frame messages
void sendMTCQuarterFrame()
{
  uint8_t frameNibble = (frameCount & 0x0F);
  uint8_t secondNibble = (secondCount & 0x0F);
  uint8_t minuteNibble = (minuteCount & 0x0F);
  uint8_t hourNibble = (hourCount & 0x1F);  // 5 bits for hours

  // Send quarter-frame messages (8 total, from 0 to 7)
  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x00);  // Frame LS nibble
  sendMIDIByte(frameNibble);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x01);  // Frame MS nibble
  sendMIDIByte(frameNibble >> 4);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x02);  // Seconds LS nibble
  sendMIDIByte(secondNibble);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x03);  // Seconds MS nibble
  sendMIDIByte(secondNibble >> 4);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x04);  // Minutes LS nibble
  sendMIDIByte(minuteNibble);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x05);  // Minutes MS nibble
  sendMIDIByte(minuteNibble >> 4);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x06);  // Hours LS nibble
  sendMIDIByte(hourNibble);

  sendMIDIByte(MIDI_TIME_CODE_QUARTER_FRAME | 0x07);  // Hours MS nibble
  sendMIDIByte(hourNibble >> 4);
}

// USART Receive Interrupt Service Routine (ISR) for MIDI
ISR(USART_RX_vect)
{
  unsigned char midiByte = UDR0;  // Read the incoming byte from the UART buffer

  // Check if the byte is a quarter-frame message (MTC)
  if ((midiByte & 0xF0) == MIDI_TIME_CODE_QUARTER_FRAME)
  {
    unsigned char messageType = midiByte & 0x0F;  // Get the quarter-frame type (0-7)
    unsigned char frameData = midiByte >> 4;      // Get the data
    mtcQuarterFrame[messageType] = frameData;     // Store the quarter-frame data

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

// Initialize the buttons for start/stop control
void initButtons(void)
{
  DDRD &= ~(1 << START_BUTTON_PIN);  // Set START_BUTTON_PIN as input
  PORTD |= (1 << START_BUTTON_PIN);  // Enable pull-up resistor on START_BUTTON_PIN

  DDRD &= ~(1 << STOP_BUTTON_PIN);   // Set STOP_BUTTON_PIN as input
  PORTD |= (1 << STOP_BUTTON_PIN);   // Enable pull-up resistor on STOP_BUTTON_PIN
}

void setup()
{
  DDRD = 0b00101000;  // Set pins for output

  // Initialize MIDI input and output (USART)
  initUSART();

  // Initialize transport control buttons
  initButtons();

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

void loop()
{
  // Check button states for transport control
  if (!(PIND & (1 << START_BUTTON_PIN)))  // Start/Resume button pressed
  {
    if (transportState == TRANSPORT_STOPPED || transportState == TRANSPORT_PAUSED)
    {
      transportState = TRANSPORT_RUNNING;
      clockStarted = true;
      clockPaused = false;
    }
  }

  if (!(PIND & (1 << STOP_BUTTON_PIN)))  // Stop/Pause button pressed
  {
    if (transportState == TRANSPORT_RUNNING)
    {
      transportState = TRANSPORT_PAUSED;
      clockPaused = true;
    }
    else
    {
      transportState = TRANSPORT_STOPPED;
      clockStarted = false;
      clockPaused = false;
    }
  }

  // Only proceed if the transport is running
  if (transportState == TRANSPORT_RUNNING)
  {
    // Handle SMPTE LTC output and MIDI Timecode transmission
    if (!clockPaused)
    {
      sendMTCQuarterFrame();  // Send the MIDI Timecode Quarter Frames
    }
  }
}

// Encode each SMPTE bit as LTC using Manchester encoding
void setLTCLevel(void)
{
  if (bitCount < 80)
  {
    // Calculate the current bit from SMPTE timecode (frames, seconds, minutes, hours)
    if (bitCount < 32) {
      currentBit = (frameCount >> (bitCount % 4)) & 1;
    } else if (bitCount < 40) {
      currentBit = (secondCount >> (bitCount - 32)) & 1;
    } else if (bitCount < 48) {
      currentBit = (minuteCount >> (bitCount - 40)) & 1;
    } else {
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
  if (frameCount < FPS - 1) {
    frameCount++;
  } else {
    frameCount = 0;
    if (secondCount < 59) {
      secondCount++;
    } else {
      secondCount = 0;
      if (minuteCount < 59) {
        minuteCount++;
      } else {
        minuteCount = 0;
        if (hourCount < 23) {
          hourCount++;
        } else {
          hourCount = 0;
        }
      }
    }
  }
}

// Timer1 interrupt handler: Trigger every LTC bit period
ISR(TIMER1_COMPA_vect)
{
  if (!clockPaused && clockStarted)
  {
    setLTCLevel();  // Output each LTC bit (Manchester encoding)
    if (bitCount == 80) {
      timeUpdate();  // After completing one SMPTE frame, update the timecode
    }
  }
}
