/*

Casio Keyboard ROM Pack Dumper
  by Ra_226.  October 16, 2021
  modified by whc2001

The ROM has a 4-bit bus and data is stored and addressed in 4-bit words.  There are 16-bits of address space, however only the top 12-bits are 
sent--the bottom 4 bits are implicitly zero, making data addressable only in 16-word blocks.  12 bits can address up to 4096 blocks.  The typical
6 kB (12k word) ROM has 768 addresses; the larger 24 kB (48k word) ROMs have 3072.

The ROM operates in one of two modes: ADDRESSING mode takes in an address and moves the pointer, while DATA OUT mode reads out to the bus.  For
an addressing operation, the CPU brings CE and OP Low, then sends 5 words for two clocks each: first the READ op code (0000), then three address
words (bits 4-7, bits 8-11, then bits 12-15), and finally the device code, which should be 0xC.  If the device code does not match, it's assumed
the CPU is communicating with another device and the ROM should not respond to any DATA OUT until a proper device code has been received via
addressing.  After addressing, CE and OP are brought high again and the ROM is given time to find the data (typically a dozen clocks or so).

For a DATA OUT operation, CE is brought low but OP remains high.  Assuming this follows a successful addressing op, the DATA lines switch from
high-impedance inputs to outputs and data is read out, one word every two clocks.  As long as CE is kept low, the ROM auto-increments the address
pointer, reading out data sequentially.  If CE is brought high, the ROM ceases outputting data, but the address pointer holds position.  If CE is
brought low again, it picks up where it left off unless a new addressing operation occurs.

A few more oddities of the Casio ROM:
- The power supply is negative, which means for power connections, GND is connected to 5V, and Vdd is 0V
- Since Vdd is considered HIGH, "negative logic" is in effect: 0V (really -5V) is HIGH and 5V (GND) is LOW (see comments below).
- The ROM uses a bi-phase clock where two clocks alternate back and forth, with a "dead" period between them when neither is active.
  - Due to negative logic, the clocks are considered active LOW (with a 25% duty cycle)
  - Signals from the keyboard (CE and OP changes, and data during ADDRESSING) occur on CK2
  - Signals from the ROM (data during DATA OUT) occur slightly after CK1, but always before the next CK2

I chose pins 12 and 13 for clock since I liked having the built-in LED on the clock signal.

Arduino Pin   Signal  ROM Pin     Notes
------------------------------------------------------------------------------------------------
PORTD
  Pin 2       CE      Pin 2       OP, CE High: ROM is idle
  Pin 3       OP      Pin 7       OP, CE Low: ADDRESSING mode.  Only CE Low: DATA OUT mode
  Pin 4       DATA1   Pin 8
  Pin 5       DATA2   Pin 9
  Pin 6       DATA3   Pin 10
  Pin 7       DATA4   Pin 11
PORTB    
  Pin 12      CK1     Pin 5       Outputs (Data) should appear on falling edge of CK1
  Pin 13      CK2     Pin 6       Inputs (CE and OP) should appear on falling edge of CK2
Power
  5V          GND     Pin 1       Connect to 5V (note the negative power supply)
  GND         VDD1    Pin 3       Connect to GND via 390 ohm resistor
  GND         VDD2    Pin 4       Connect to GND
------------------------------------------------------------------------------------------------

How to Use:
- Connect ROM Dumper Shield
- Compile and upload ROM Dumper to Arduino
- Disconnect power on Arduino
- Lay ROM on shield, lining up with guide lines.  You can use a rubber band, or just hold it in place with a finger over the left side
- When everything is aligned, connect power to Arduino and start the Serial Monitor, set baudrate to 230400 bps
- The Serial Monitor should report there will be a 1 second delay, then the read begins.
- Wait until the Serial Monitor reports the dump is finished. Take note of the total bytes and checksum.
- Press reset on the Arduino to dump again, the checksum should match, if not then check the ROM and connections.
- Copy the data from the serial monitor and paste (as Hex) directly into a Hex editor.
- A typical ROM is exactly 6 kB (6144 Bytes).  Delete anything past that
  - There are some larger ROMs: RO-302, for instance, is 24 kB.  Just trim the file down to the exact multiple of 6 kB.
- Run an MD5 or similar hash, and compare hashes to make sure they match
- If dumping RO-551 World Songs as a test, it should have length of 6144, checksum of 0x21134 and MD5 of ae6122817f62071fb6e8b9242e0fd0df

    ======/ Special Thanks to /====================================================================

    http://www.crumblenet.co.uk/keyb/rpacks.html
      Tim has studied the many ROM packs of several different Casio and other keyboards.

    http://www.pisi.com.pl/piotr433/hardware.htm
      Piotr has analyzed the SRAM of the Casio FX-700P a chip similar to Casio ROM

    https://seanriddle.com/m5268.html
      Sean dumped the contents of RO-251, starting me on this adventure.

    https://ra226.net/casio-rom-emulator-ro-226/
      This modified version is based on Ra226's original work, which is vastly helpful for beginning with.
  
    ===============================================================================================  
*/

/******************* SETTINGS *******************/

const long serialBaudrate = 230400;            // Serial baudrate.  230400 is the fastest in theory to work reliably with USB to UART chips (CH340, FT232, etc.)
                                              // If you encounter errors, try slower like 115200. It's a great idea to also increase the clockPeriod below to not overrun the serial buffer.

const int clockPeriod = 10;                   // Quarter Clock period, in microseconds.  250 = 1 ms clock period.  24 seconds to complete a 6k ROM.
                                              // 2 clocks to read a Word of ROM = 2 ms and each word is sent as a single hex digit (one byte) over serial
                                              // 9600 baud = 1 byte per ms, so serial runs about twice read rate.  Don't go much faster or serial won't keep up
                                              // Actual piano runs at 100 kHz. Using 10 with baudrate 230400 should be much faster, tested without error but YMMV

const unsigned int blockLength = 6 * 1024;    // The length for one ROM block. Fixed to 6KB. Longer data should be a multiply of this.
const unsigned int maxLength = 24 * 1024;     // The max length to dump. Exceeding means error happened. Default to 24KB.

// The fixed tail data for determining when to stop dumping, generally no need to change
const unsigned char tail[] = {
  0x23, 0x83, 0x93, 0x06, 0x47, 0x83, 0xAB, 0x02, 0x63, 0x27, 0x4B, 0x27, 0x47, 0x93, 0x2B, 0x83, 
  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
};

/************************************************/

const unsigned char tailLength = sizeof(tail) / sizeof(tail[0]);

unsigned int dumpLength = 0;                  // Dump length counter
unsigned char tailIndex = 0;                  // Current index to compare tail data
unsigned long dumpChecksum = 0;                   // Simple checksum for quick compare of repeated dumps

// Read two 4-bit nibble word and return one byte
unsigned char readOne() {
  byte dataReadLOW = (~PIND & B11110000) >> 4;     // Inverse because of negative logic.  Zero the rest, store as lower half of byte.
  clockTick(2);                               // Read in next word, the upper half of byte.
  byte dataReadHIGH = (~PIND & B11110000) >> 4;    // Inverse because of negative logic.  Zero the rest, store as upper half of byte.
  clockTick(2);                               // Read in lower half of next byte...  
  return (dataReadHIGH << 4) | dataReadLOW;
}

void setup() {
  
  Serial.begin(serialBaudrate);                         // Initialize Serial communications
  
  DDRB |= B00110000;                          // Set pins to OUTPUTs: Two-phase Clock: CK1, CK2 (pins 12-13)
  DDRD |= B11111100;                          // Set pins to OUTPUTs: OP, CE, DATA1 - DATA4 (pins 2-7)
                                              //
                                              // To make SPI pins avaialble for possible SD card use, move clocks to pins 8 and 9 
                                              // Note: this breaks compatibility with my ROM Dump shield and also I like having an LED on the clock signal
  
// Initialize pins.  Pins 0 and 1 reserved for Serial communication.
// PORTD
  digitalWrite(2,HIGH);                       // OP       ROM Pin 7
  digitalWrite(3,HIGH);                       // CE       ROM Pin 2
  digitalWrite(4,HIGH);                       // DATA1    ROM Pin 8
  digitalWrite(5,HIGH);                       // DATA2    ROM Pin 9
  digitalWrite(6,HIGH);                       // DATA3    ROM Pin 10
  digitalWrite(7,HIGH);                       // DATA4    ROM Pin 11
// PORTB
  digitalWrite(12,HIGH);                      // CK1      ROM Pin 5
  digitalWrite(13,HIGH);                      // CK2      ROM Pin 6

                                              // Other ROM chip connections (note the power connections--they are correct, VDD is -5V):
                                              //   GND    ROM Pin 1   Connect to 5V
                                              //   VDD1   ROM Pin 3   Connect to GND via 390 ohm resistor
                                              //   VDD2   ROM Pin 4   Connect to GND
  
  Serial.println("Beginning ROM dump in 1 seconds");
  delay(1000);

  clockTick(30);                              // 30 ticks before read.  May not be necessary but Sean's code does this.

  Serial.println("Beginning READ operation...");
  PORTD &= B11110011;                         // CE and OP LOW
  clockTick(2);                               // Send READ Op code 0000 (the pins are already in this state)
  
// Send 16-bit address zzzz yyyy xxxx 0000 (bits 0-3 are implicitly 0000)
  Serial.println("Sending Address...");
  clockTick(2);                               // Send bits 4-7 (0000)
  clockTick(2);                               // Send bits 8-11 (0000)
  clockTick(2);                               // Send bits 12-15 (0000)

// Send Device Code.  Anything other than 0xC (1100) puts pins in high-Z state, freeing bus for another device
  PORTD &= B00111111;                         
  Serial.println("Sending Device Code...");
  clockTick(2);                               // Send 0xC (1100)
  
// End Read command
  PORTD |= B00001100;                         // Set CE and OP HIGH
  DDRD &= B00001111;                          // Set D4-D7 as INPUT pins
  PORTD &= B00001111;                         // Disable pull-ups on D4-D7
  Serial.println("Waiting for ROM to respond...");
  clockTick(18);                              // Give time for ROM to respond

  PORTD &= B11110111;                         // Set CE LOW
  clockTick(1);                               // Wait one tick just to make sure DATA is present (should arrive after half a tick)

  Serial.println();
}


void loop() {
  unsigned char r = readOne();
  Serial.print((r & 0xF0) >> 4, HEX);
  Serial.print(r & 0x0F, HEX);
  dumpChecksum += r;
  ++dumpLength;

  // Tail judgement
  if(r == tail[tailIndex]) {                  // Compare the current byte with the predefined bytes sequentially
    ++tailIndex;
    if(tailIndex >= tailLength) {             // If all bytes are the same then we have reached the end
      Serial.println("\n");
      Serial.print("Dump finished, total ");
      Serial.print(dumpLength);
      Serial.print(" bytes, checksum is ");
      Serial.println(dumpChecksum, HEX);
      if(dumpLength % blockLength != 0) {            // Length should be a multiple of 6KB
        Serial.print("ERROR: The length is not a multiple of ");
        Serial.print(blockLength);
        Serial.println(" bytes");
      }
      while(1);
    }
  }
  else {
    tailIndex = 0;
  }

  // Length judgement
  if(dumpLength > maxLength) {
      Serial.println("\n");
      Serial.print("ERROR: The dumped data exceeds ");
      Serial.print(maxLength);
      Serial.println(" and the tail is still not found");
      while(1);
  }
}


void clockTick(int x) {                       // "Bi-phase" clock, two clock signals, 180 degrees apart, 75% duty cycle (inverted 25%)
  for (int i = 0; i < x; i++) {
    PORTB &= B11011111;                       // CK2 LOW (OP and CE should change with CK2 falling edge)
    delayMicroseconds(clockPeriod);
    PORTB |= B00100000;                       // CK2 HIGH
    delayMicroseconds(clockPeriod);
    PORTB &= B11101111;                       // CK1 LOW (Data should appear on the bus "half" clock later, on CK1 falling edge)
    delayMicroseconds(clockPeriod);
    PORTB |= B00010000;                       // CK1 HIGH
    delayMicroseconds(clockPeriod);
  }
}
