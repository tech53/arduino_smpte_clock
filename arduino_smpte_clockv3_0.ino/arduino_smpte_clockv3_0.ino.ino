#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdbool.h>
#include <Wire.h>               // Include Wire library for I2C
#include <Adafruit_GFX.h>       // Include Adafruit graphics library
#include <Adafruit_SSD1306.h>   // Include Adafruit SSD1306 OLED library

#define F_CPU 16000000UL  // Set 16 MHz as the constant, prevent drift

// OLED Display Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1  // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// SMPTE timecode variables
volatile unsigned char hourCount = 0;
volatile unsigned char minuteCount = 0;
volatile unsigned char secondCount = 0;
volatile unsigned char frameCount = 0;  // Frame count (0 to 23 or 29 depending on FPS)

volatile unsigned char bitCount = 0;
volatile unsigned char currentBit = 0;
volatile unsigned char lastLevel = 0;

// SMPTE Resolutions
#define FPS_24 24
#define FPS_25 25
#define FPS_30 30
#define FPS_2997 29  // For drop-frame

// Default configuration
volatile uint8_t selectedFPS = FPS_30;
volatile uint8_t selectedPPQN = 24;  // Default PPQN
volatile bool displaySMPTE = true;   // Default display type

// MIDI settings
#define MIDI_BAUD 31250  // MIDI baud rate is 31.25 kbps
#define MIDI_INPUT_PIN PD0  // Define the MIDI input pin (RX)
#define MIDI_OUTPUT_PIN PD1 // Define the MIDI output pin (TX) for USART

// SMPTE Output Configuration
#define SMPTE_OUTPUT_PIN PD6  // Define the SMPTE output pin (OC2A)

// Transport Control Buttons
#define START_BUTTON_PIN PD2  // Button to start/resume timecode
#define STOP_BUTTON_PIN PD3   // Button to stop/pause timecode

// Menu Navigation Buttons
#define NEXT_BUTTON_PIN PC0      // Button to navigate to next or increment
#define PREVIOUS_BUTTON_PIN PC1  // Button to navigate to previous or decrement
#define SELECT_BUTTON_PIN PC2    // Button to select or confirm
#define BACK_BUTTON_PIN PC3      // Button to go back or exit menu

// Transport control states
#define TRANSPORT_STOPPED 0
#define TRANSPORT_RUNNING 1
#define TRANSPORT_PAUSED 2

volatile uint8_t transportState = TRANSPORT_STOPPED; // Track the transport state

// Menu states
#define MENU_MAIN 0
#define MENU_SMPTE_FPS 1
#define MENU_MIDI_PPQN 2
#define MENU_DISPLAY_TYPE 3

volatile uint8_t menuState = MENU_MAIN; // Main menu state
volatile uint8_t menuSelectedItem = 0;  // Current selected item in the menu
volatile bool menuActive = false;       // Flag to indicate if the menu is active

// SMPTE menu options
const uint8_t smpteFpsOptions[] = {FPS_24, FPS_25, FPS_30, FPS_2997};
const char *smpteFpsLabels[] = {"24 FPS", "25 FPS", "30 FPS", "29.97 FPS"};

// MIDI PPQN options for modern and retro gear
const uint8_t midiPpqnOptions[] = {24, 48, 96, 120, 192};
const char *midiPpqnLabels[] = {
    "24 PPQN (Default)",
    "48 PPQN",
    "96 PPQN",
    "120 PPQN",
    "192 PPQN"
};

// Display type options
const char *displayTypeOptions[] = {"Display SMPTE", "Display MIDI"};
volatile bool clockStarted = false;  // SMPTE clock state (false until MIDI trigger or button press)
volatile bool clockPaused = false;   // True if the clock is paused

// Function Prototypes
void initUSART(void);
void sendMIDIByte(unsigned char byte);
void sendMTCQuarterFrame(void);
void initTimer0(void);
void navigateMenu(void);
void selectMenuItem(void);
void backMenu(void);
void initButtons(void);
void setLTCLevel(void);
void timeUpdate(void);
void updateDisplay(void);
void displayMenu(void);
void displayTimecode(void);

// USART initialization for MIDI input and output (31.25 kbps)
void initUSART(void)
{
  unsigned int ubrr = (F_CPU / (16UL * MIDI_BAUD)) - 1;
  UBRR0H = (unsigned char)(ubrr >> 8);
  UBRR0L = (unsigned char)ubrr;

  UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

// USART Transmit function for sending MIDI bytes
void sendMIDIByte(unsigned char byte)
{
  while (!(UCSR0A & (1 << UDRE0)));
  UDR0 = byte;
}

// Transmit MTC Quarter-Frame messages
void sendMTCQuarterFrame()
{
  uint8_t frameNibble = (frameCount & 0x0F);
  uint8_t secondNibble = (secondCount & 0x0F);
  uint8_t minuteNibble = (minuteCount & 0x0F);
  uint8_t hourNibble = (hourCount & 0x1F);  // 5 bits for hours

  sendMIDIByte(0xF1 | 0x00);  // Frame LS nibble
  sendMIDIByte(frameNibble);
  
  sendMIDIByte(0xF1 | 0x01);  // Frame MS nibble
  sendMIDIByte(frameNibble >> 4);
  
  sendMIDIByte(0xF1 | 0x02);  // Seconds LS nibble
  sendMIDIByte(secondNibble);
  
  sendMIDIByte(0xF1 | 0x03);  // Seconds MS nibble
  sendMIDIByte(secondNibble >> 4);
  
  sendMIDIByte(0xF1 | 0x04);  // Minutes LS nibble
  sendMIDIByte(minuteNibble);
  
  sendMIDIByte(0xF1 | 0x05);  // Minutes MS nibble
  sendMIDIByte(minuteNibble >> 4);
  
  sendMIDIByte(0xF1 | 0x06);  // Hours LS nibble
  sendMIDIByte(hourNibble);
  
  sendMIDIByte(0xF1 | 0x07);  // Hours MS nibble
  sendMIDIByte(hourNibble >> 4);
}

// Timer0 configuration for menu handling
void initTimer0(void)
{
  TCCR0A = (1 << WGM01); // CTC mode
  TCCR0B = (1 << CS02) | (1 << CS00); // Prescaler of 1024
  OCR0A = 156; // 10 ms interrupt
  TIMSK0 = (1 << OCIE0A); // Enable compare match interrupt
}

// Menu navigation logic
void navigateMenu()
{
  // Use Next and Previous buttons to navigate
  if (!(PINC & (1 << NEXT_BUTTON_PIN))) // Next/Increment button pressed
  {
    menuSelectedItem = (menuSelectedItem + 1) % 4; // Cycle through menu items
  }

  if (!(PINC & (1 << PREVIOUS_BUTTON_PIN))) // Previous/Decrement button pressed
  {
    menuSelectedItem = (menuSelectedItem == 0) ? 3 : (menuSelectedItem - 1); // Cycle backwards through items
  }
}

// Select or confirm the current menu item
void selectMenuItem()
{
  if (menuState == MENU_MAIN)
  {
    switch (menuSelectedItem)
    {
      case 0:
        menuState = MENU_SMPTE_FPS;
        menuSelectedItem = 0;
        break;
      case 1:
        menuState = MENU_MIDI_PPQN;
        menuSelectedItem = 0;
        break;
      case 2:
        menuState = MENU_DISPLAY_TYPE;
        menuSelectedItem = 0;
        break;
      default:
        menuState = MENU_MAIN;
        break;
    }
  }
  else if (menuState == MENU_SMPTE_FPS)
  {
    selectedFPS = smpteFpsOptions[menuSelectedItem];
    menuState = MENU_MAIN;
  }
  else if (menuState == MENU_MIDI_PPQN)
  {
    selectedPPQN = midiPpqnOptions[menuSelectedItem];
    menuState = MENU_MAIN;
  }
  else if (menuState == MENU_DISPLAY_TYPE)
  {
    displaySMPTE = (menuSelectedItem == 0);
    menuState = MENU_MAIN;
  }
}

// Go back in the menu
void backMenu()
{
  if (menuState != MENU_MAIN)
  {
    menuState = MENU_MAIN;
    menuSelectedItem = 0; // Reset to the main menu
  }
}

// Timer0 interrupt for menu system handling
ISR(TIMER0_COMPA_vect)
{
  if (menuActive)
  {
    navigateMenu();

    if (!(PINC & (1 << SELECT_BUTTON_PIN))) // Select/OK button pressed
    {
      selectMenuItem();
    }

    if (!(PINC & (1 << BACK_BUTTON_PIN))) // Back button pressed
    {
      backMenu();
    }
  }
  else
  {
    // Check if the select button is pressed to activate the menu
    if (!(PINC & (1 << SELECT_BUTTON_PIN))) 
    {
      menuActive = true;
    }
  }
}

// Initialize buttons
void initButtons(void)
{
  // Set buttons as inputs and enable pull-ups
  DDRC &= ~((1 << NEXT_BUTTON_PIN) | (1 << PREVIOUS_BUTTON_PIN) | (1 << SELECT_BUTTON_PIN) | (1 << BACK_BUTTON_PIN));
  PORTC |= (1 << NEXT_BUTTON_PIN) | (1 << PREVIOUS_BUTTON_PIN) | (1 << SELECT_BUTTON_PIN) | (1 << BACK_BUTTON_PIN);
  
  DDRD &= ~((1 << START_BUTTON_PIN) | (1 << STOP_BUTTON_PIN));
  PORTD |= (1 << START_BUTTON_PIN) | (1 << STOP_BUTTON_PIN); // Enable pull-ups
}

// Initialize OLED display
void initDisplay() {
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
    // Display connection failed
    for(;;); // Don't proceed, loop forever
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

// Main setup
void setup()
{
  DDRD = 0b00101000;  // Set PD2, PD3, and PD5 as outputs for other operations
  initUSART();         // Initialize the USART for MIDI communication
  initButtons();       // Initialize the buttons with pull-ups for menu control
  initTimer0();        // Initialize Timer0 for menu handling
  initDisplay();       // Initialize the OLED display
  
  // Timer2 for PWM output on OC2A, used for SMPTE output
  TCCR2A = _BV(COM2A1) | _BV(WGM21) | _BV(WGM20); // Fast PWM mode, non-inverting output
  TCCR2B = _BV(CS21);  // Prescaler of 8

  // Timer1 for LTC bit timing, outputting bits at the correct intervals
  TCCR1A = 0b00 << WGM10; // Normal mode
  TCCR1B = _BV(WGM12) | _BV(CS11); // CTC mode, prescaler 8
  OCR1A = 488;  // Set duration for each LTC bit
  TIMSK1 = _BV(OCIE1A); // Enable Timer1 compare match interrupt

  sei();  // Enable global interrupts
}

// Main loop
void loop()
{
  if (!menuActive)  // Only allow transport control if menu is not active
  {
    if (!(PIND & (1 << START_BUTTON_PIN)))
    {
      if (transportState == TRANSPORT_STOPPED || transportState == TRANSPORT_PAUSED)
      {
        transportState = TRANSPORT_RUNNING;
        clockStarted = true;
        clockPaused = false;
      }
    }

    if (!(PIND & (1 << STOP_BUTTON_PIN)))
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

    if (transportState == TRANSPORT_RUNNING && !clockPaused)
    {
      sendMTCQuarterFrame();
    }
    displayTimecode(); // Display the selected timecode (MIDI or SMPTE)
  }
  else
  {
    displayMenu(); // Show menu when active
  }
}

// Encode each SMPTE bit as LTC using Manchester encoding
void setLTCLevel(void)
{
  if (bitCount < 80)
  {
    if (bitCount < 32) {
      currentBit = (frameCount >> (bitCount % 4)) & 1;
    } else if (bitCount < 40) {
      currentBit = (secondCount >> (bitCount - 32)) & 1;
    } else if (bitCount < 48) {
      currentBit = (minuteCount >> (bitCount - 40)) & 1;
    } else {
      currentBit = (hourCount >> (bitCount - 48)) & 1;
    }

    if (currentBit == 0) {
      OCR2A = 44;  // Low level
    } else {
      OCR2A = 82; // High level
    }
    lastLevel = currentBit;
    bitCount++;
  }
  else
  {
    bitCount = 0;
    OCR2A = 63; // Reset to ground level
  }
}

// Update SMPTE timecode values (increment frames, seconds, minutes, hours)
void timeUpdate(void)
{
  if (frameCount < selectedFPS - 1) {
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

// ISR for SMPTE LTC bit output
ISR(TIMER1_COMPA_vect)
{
  if (!clockPaused && clockStarted)
  {
    setLTCLevel();
    if (bitCount == 80)
    {
      timeUpdate();
    }
  }
}

// Update OLED Display with current menu items
void displayMenu() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Menu:");
  
  switch (menuState) {
    case MENU_MAIN:
      display.println(menuSelectedItem == 0 ? "> SMPTE FPS" : "  SMPTE FPS");
      display.println(menuSelectedItem == 1 ? "> MIDI PPQN" : "  MIDI PPQN");
      display.println(menuSelectedItem == 2 ? "> Display Type" : "  Display Type");
      break;
    case MENU_SMPTE_FPS:
      for (int i = 0; i < 4; i++) {
        display.println(menuSelectedItem == i ? ("> " + String(smpteFpsLabels[i])).c_str() : ("  " + String(smpteFpsLabels[i])).c_str());
      }
      break;
    case MENU_MIDI_PPQN:
      for (int i = 0; i < 5; i++) {
        display.println(menuSelectedItem == i ? ("> " + String(midiPpqnLabels[i])).c_str() : ("  " + String(midiPpqnLabels[i])).c_str());
      }
      break;
    case MENU_DISPLAY_TYPE:
      display.println(menuSelectedItem == 0 ? "> Display SMPTE" : "  Display SMPTE");
      display.println(menuSelectedItem == 1 ? "> Display MIDI" : "  Display MIDI");
      break;
  }
  display.display();
}

// Display the selected timecode on the OLED
void displayTimecode() {
  display.clearDisplay();
  display.setCursor(0, 0);
  char buffer[20]; // Buffer for formatting the timecode string
  if (displaySMPTE) {
    sprintf(buffer, "SMPTE: %02d:%02d:%02d:%02d", hourCount, minuteCount, secondCount, frameCount);
    display.print(buffer);
  } else {
    display.print("MIDI Timecode");
  }
  display.display();
}
