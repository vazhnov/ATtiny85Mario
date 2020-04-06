//#include <util/delay.h>

//#pragma GCC optimize ("-O3")

#include <avr\eeprom.h>
#include "mario.h"
#include "mario_tiles.h"
#include "bricks.h"
#include "level.h"
#include "font_digits.h"

//#define TEST_BUTTONS 1

#ifdef TEST_BUTTONS
#include "font_digits.h"
#endif

static byte oled_addr;
static void oledWriteCommand(unsigned char c);

struct music_table {
  uint16_t startpos;
  uint8_t len;
};

#define DIRECT_PORT
#define I2CPORT PORTB
// A bit set to 1 in the DDR is an output, 0 is an INPUT
#define I2CDDR DDRB

// Pin or port numbers for SDA and SCL
#define BB_SDA 0 //2
#define BB_SCL 2 //3

#define I2C_CLK_LOW() I2CPORT &= ~(1 << BB_SCL) //compiles to cbi instruction taking 2 clock cycles, extending the clock pulse

enum mario_state {idle, left, right, squash, dead};
enum mario_jumpstate {nojump, jumpup, jumpdown};
enum mario_direction {faceleft, faceright};
enum game_state {normal, paused, paused_ready, pause_return, mario_dying};

struct character {
  int16_t x, y;
  int16_t oldx[2], oldy[2];
  enum mario_state state;
  enum mario_jumpstate jumpstate;
  enum mario_direction dir;
  int8_t vx, vy;
  uint8_t frame;
  uint8_t collision;
  uint8_t coincollector;
  uint8_t mask=3; // 01b means 1 square cell, 0b11 (3) means 2 square cell
  uint8_t w=2;
};

struct character mario, turtle, fireball;

struct melody {
  uint16_t* melody; //+ 0
  uint8_t* noteDurations; // +2
  uint8_t numNotes; // +3
  uint8_t progmem; // + 4
};

struct toneController {
  uint8_t music_index = 0, music_pos = 0, music_note = 0, noteordelay = 0;
  uint16_t current_delay = 0;
  uint8_t soundeffect; // 0 = Music, 1=Sound effect (affects play priority and loop behaviour)
};

struct toneController MusicController;
struct toneController SoundEffectController;

/*
  const struct melody theme_music[]={{Melody_Intro, NoteDurations_Intro, sizeof(Melody_Intro)>>1,0},    // 0
                            {Melody_Refrain, NoteDurations_Refrain, sizeof(Melody_Refrain)>>1,1}, // 1
                            {Melody_Part2_1, NoteDurations_Part2_1, sizeof(Melody_Part2_1)>>1,0}, // 2
                            {Melody_Part2_2, NoteDurations_Part2_2, sizeof(Melody_Part2_2)>>1,0}, // 3
                            {Melody_Part2_3, NoteDurations_Part2_3, sizeof(Melody_Part2_3)>>1,0}, // 4
                            {Melody_Part3_1, NoteDurations_Part3_1, sizeof(Melody_Part3_1)>>1,1}, // 5
                            {CoinSound, CoinSound_Durations,sizeof(CoinSound)>>1,0}, // 6 - Coin sound effect
                            {JumpSound, JumpSound_Durations,sizeof(JumpSound)>>1,1},// 7 - Jump sound effect
                            {HitHeadSound, HitHead_Durations,sizeof(HitHeadSound)>>1,1}// 8 - Hit head sound effect
                            };*/

//const uint8_t *levelelements[] = {steps_up, steps_down, gap, biggap_platform, platform, platform2, flat, climb};
const uint8_t *levelelements[] = {flat, steps_up, gap, platform, flat, flat2, pipe, climb};

#define COIN_SOUND 6
#define JUMP_SOUND 7
#define HITHEAD_SOUND 8
#define PAUSE_SOUND 9

#define FIREBALL_SPEED 3

const uint8_t music_seq[] = {0, 1, 1, 2, 3, 2, 4, 2, 3, 2, 4, 5}; // 12 elements: 0-11. Go back to position 1 on overflow.

#define BASE 64-16-8 // height of mario, height of base bricks
#define SPEAKER 4 //3 // Can use hardware Timer1 OC1B
#define SPEAKER2 1
//#define WORLD_BUFFER 32
#define WORLD_MAX_LEN 63

uint8_t screen[WORLD_MAX_LEN + 1], soundeffectplaying = 0;

uint16_t viewx = 0, old_viewx = 0, lviewx_trigger, rviewx_trigger, MusicSpeed = 1000, mario_acc_timer = 0;
int delta_viewx = 0;

volatile long tone_timer1_toggle_count=0,mymicros=0,ISR_micro_period=0; 
volatile long tone_timer0_toggle_count = 0;
uint8_t dummy_register;
uint8_t coinframe = 1, cointimer = 0, coincount = 0;
uint32_t curr_seed = 0;
enum game_state gamestate = normal;

#define CLEAR_SCL asm("cbi 0x18, 2\n")   // 0x18 = PORTB, i.e. PORTB &= ~(1<<2);
#define CLEAR_SDA asm("cbi 0x18, 0\n")   // PORTB &= ~ (1<<0); 
#define SET_SCL asm("sbi 0x18, 2\n")    //
#define SET_SDA asm("sbi 0x18, 0\n")

#define OUTPUT_SCL asm("sbi 0x17, 2\n")   // 0x17 = DDRB, 2 = PB2 = SCL
#define OUTPUT_SDA asm("sbi 0x17, 0\n")   // 0x17 = DDRB, 0 = PB0 = SDA

#define INPUT_SCL asm("cbi 0x17, 2\n")    // DDRB &= ~(1<<2);
#define INPUT_SDA asm("cbi 0x17, 0\n")    // DDRB &= ~(1<<0);

/////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Transmit a byte and ack bit
//
static inline void i2cByteOut(byte b) // This is only called from i2Cbegin, before tones are started so don't need to worry about
{
  byte i;
  //byte bOld = I2CPORT & ~((1 << BB_SDA) | (1 << BB_SCL));

  // DUNCAN  I2CPORT &= ~(1<< BB_SCL); //Toggle clock - Off - this is redundant because i2cbegin

  // #define BB_SDA 0 // (1<<0) = 1
  ///#define BB_SCL 2   // (1<<2) = 4

  CLEAR_SCL;

  for (i = 0; i < 8; i++)
  {
    // I2CPORT &= ~(1 << BB_SDA);
    CLEAR_SDA;
    if (b & 0x80)
      SET_SDA;//I2CPORT |= (1 << BB_SDA);

    //I2CPORT = bOld

    SET_SCL;
    CLEAR_SCL;
    //I2CPORT |= (1 << BB_SCL); // ON
    //I2CPORT &= ~(1<< BB_SCL); //Toggle clock - Off

    b <<= 1;
  } // for i
  // ack bit

  CLEAR_SDA;
  SET_SCL;
  CLEAR_SCL;

  //I2CPORT &= ~(1 << BB_SDA); // set data low
  //I2CPORT |= (1 << BB_SCL); // toggle clock
  //I2CPORT &= ~(1 << BB_SCL); // toggle clock


} /* i2cByteOut() */

void i2cBegin(byte addr)
{
  /*I2CPORT |= ((1 << BB_SDA) + (1 << BB_SCL));
    I2CDDR |= ((1 << BB_SDA) + (1 << BB_SCL));
    I2CPORT &= ~(1 << BB_SDA); // data line low first
    I2CPORT &= ~(1 << BB_SCL); // then clock line low is a START signal
  */
  SET_SDA;
  SET_SCL;
  //I2CDDR |= ((1 << BB_SDA) + (1 << BB_SCL)); // ASM THIS...

  OUTPUT_SDA;
  OUTPUT_SCL;

  CLEAR_SDA;
  CLEAR_SCL;

  i2cByteOut(addr << 1); // send the slave address
} /* i2cBegin() */

void i2cWrite(byte *pData, byte bLen)
{
  byte i, b;
  //byte bOld = I2CPORT & ~((1 << BB_SDA) | (1 << BB_SCL)); // PORTB with SDA and SCL off

  while (bLen--)
  {
    b = *pData++;
    if (b == 0 || b == 0xff) // special case can save time
    {
      //I2CPORT &= ~(1 << BB_SDA); // switches off SDA in bOld
      CLEAR_SDA;

      if (b & 0x80)
        SET_SDA;//I2CPORT |= (1 << BB_SDA); // switches on SDA in bOld
      //I2CPORT = bOld; // sets PORTB to bOld (SDA is set)

      //I2CPORT &= ~(1<< BB_SCL); // Toggle clock OFF
      CLEAR_SCL;

      for (i = 0; i < 8; i++)
      {
        //I2CPORT |= (1 << BB_SCL); // SCL = ON, SDA stays the same
        //I2CPORT &= ~(1<< BB_SCL); // Toggle clock on and off
        SET_SCL;
        CLEAR_SCL;
      } // for i
    }
    else // normal byte needs every bit tested
    {
      //I2CPORT &= ~(1<< BB_SCL); // Toggle clock OFF
      CLEAR_SCL;

      for (i = 0; i < 8; i++)
      {

        //I2CPORT &= ~(-1 << BB_SDA);
        CLEAR_SDA;
        if (b & 0x80)
          SET_SDA; //I2CPORT |= (1 << BB_SDA);

        //I2CPORT = bOld;

        //I2CPORT |= (1 << BB_SCL);// Turn on
        //I2CPORT &= ~(1<< BB_SCL); // Toggle clock on and off
        SET_SCL;
        CLEAR_SCL;

        b <<= 1;
      } // for i

    }
    // ACK bit seems to need to be set to 0, but SDA line doesn't need to be tri-state
    //I2CPORT &= ~(1 << BB_SDA);
    //I2CPORT |= (1 << BB_SCL); // toggle clock
    //I2CPORT &= ~(1 << BB_SCL);

    CLEAR_SDA;
    SET_SCL;
    CLEAR_SCL;
  } // for each byte
} /* i2cWrite() */

//
// Send I2C STOP condition
//
void i2cEnd()
{
  /*I2CPORT &= ~(1 << BB_SDA);
    I2CPORT |= (1 << BB_SCL);
    I2CPORT |= (1 << BB_SDA);*/

  CLEAR_SDA;
  SET_SCL;
  SET_SDA;

  //I2CDDR &= ((1 << BB_SDA) | (1 << BB_SCL)); // let the lines float (tri-state)

  asm("cbi 0x17, 2\n");   // 0x17 = DDRB &= ~(1<<2);
  asm("cbi 0x18, 0\n");  // DDRB &= ~(1<<0);
} /* i2cEnd() */

// Wrapper function to write I2C data on Arduino
static void I2CWrite(int iAddr, unsigned char *pData, int iLen)
{
  i2cBegin(oled_addr);
  i2cWrite(pData, iLen);
  i2cEnd();
} /* I2CWrite() */


void oledInit(byte bAddr, int bFlip, int bInvert)
{
  unsigned char uc[4];
  unsigned char oled_initbuf[] = {0x00, 0xae, 0xa8, 0x3f, 0xd3, 0x00, 0x40, 0xa1, 0xc8,
                                  0xda, 0x12, 0x81, 0xff, 0xa4, 0xa6, 0xd5, 0x80, 0x8d, 0x14,
                                  0xaf, 0x20, 0x02
                                 };
  // unsigned char oled_initbuf[]={0x00, 0xA3, 0xC8,0xA1,0xA8, 0x3F,0xDA, 0x12, 0x8D, 0x14, 0xA5,0xAF};

  oled_addr = bAddr;
  I2CDDR &= ~(1 << BB_SDA);
  I2CDDR &= ~(1 << BB_SCL); // let them float high
  I2CPORT |= (1 << BB_SDA); // set both lines to get pulled up
  I2CPORT |= (1 << BB_SCL);

  I2CWrite(oled_addr, oled_initbuf, sizeof(oled_initbuf));
  if (bInvert)
  {
    uc[0] = 0; // command
    uc[1] = 0xa7; // invert command
    I2CWrite(oled_addr, uc, 2);
  }
  if (bFlip) // rotate display 180
  {
    uc[0] = 0; // command
    uc[1] = 0xa0;
    I2CWrite(oled_addr, uc, 2);
    uc[1] = 0xc0;
    I2CWrite(oled_addr, uc, 2);
  }
} /* oledInit() */

// Send a single byte command to the OLED controller
static void oledWriteCommand(unsigned char c)
{
  unsigned char buf[2];

  buf[0] = 0x00; // command introducer
  buf[1] = c;
  I2CWrite(oled_addr, buf, 2);
} /* oledWriteCommand() */

static void oledWriteCommand2(unsigned char c, unsigned char d)
{
  unsigned char buf[3];

  buf[0] = 0x00;
  buf[1] = c;
  buf[2] = d;
  I2CWrite(oled_addr, buf, 3);
} /* oledWriteCommand2() */

//
// Sets the brightness (0=off, 255=brightest)
//
void oledSetContrast(unsigned char ucContrast)
{
  oledWriteCommand2(0x81, ucContrast);
} /* oledSetContrast() */

//
// Send commands to position the "cursor" (aka memory write address)
// to the given row and column
//
static void oledSetPosition(int x, int y)
{

  ////ssd1306_send_command3(renderingFrame | (y >> 3), 0x10 | ((x & 0xf0) >> 4), x & 0x0f);
  /* oledWriteCommand(0xb0 | (y >> 3));  //x); // go to page Y
    oledWriteCommand(0x10 | ((x & 0xf)>>4)); // upper col addr
    oledWriteCommand(0x00 | (x  & 0xf)); // // lower col addr*/


  oledWriteCommand(0xb0 | y >> 3); // go to page Y
  oledWriteCommand(0x00 | (x & 0xf)); // // lower col addr
  oledWriteCommand(0x10 | ((x >> 4) & 0xf)); // upper col addr
  //iScreenOffset = (y*128)+x;
}

static void oledWriteDataBlock(unsigned char *ucBuf, int iLen)
{
  unsigned char ucTemp[17]; // 16 bytes max width + 1

  ucTemp[0] = 0x40; // data command
  memcpy(&ucTemp[1], ucBuf, iLen);

  I2CWrite(oled_addr, ucTemp, iLen + 1);
  // Keep a copy in local buffer
}

void oledFill(unsigned char ucData)
{
  int x, y;
  unsigned char temp[16] = {0};

  memset(temp, ucData, 16);
  for (y = 0; y < 8; y++)
  {
    oledSetPosition(0, y * 8); // set to (0,Y)
    for (x = 0; x < 8; x++)
    {
      oledWriteDataBlock(temp, 16);
    } // for x
  } // for y
} /* oledFill() */
/////////////////////////////////////////////////////////////////////////////////////////////////////////


void drawSprite(int16_t x, int16_t y, uint8_t w, uint8_t h, const unsigned char *buf, uint8_t flip)
{
  // Do clipping:
  //if (x<0) x=0

  // Assume don't need clipping for now
  //ssd1306_send_command3(renderingFrame | (y >> 3), 0x10 | ((x & 0xf0) >> 4), x & 0x0f);
  //uint8_t offset_y = y & 0x07;

  int16_t sx, sy;
  unsigned char *pos = buf;
  uint8_t write_height = h / 8, xoff = 0, xw = w;
  // i.e. for write_higeht 16 means 2 lots of 8 bits.

  if (x >= 128 || x <= -w) return;
  if (y < 0) return;
  if (x < 0) {
    xoff = -x;
    x = 0;
  }
  if (x + w > 128) xw = 128 - x;

  uint8_t temp[16];

  for (uint8_t j = 0; j < write_height; j++) // goes from 0 to 1 (across y-axis)
  {
    sx = x ;
    sy = y + j * 8;

    // SetCursor
    //ssd1306_send_command3(renderingFrame | (sy >> 3), 0x10 | ((sx & 0xf0) >> 4), sx & 0x0f);
    oledSetPosition(sx, sy);

    //ssd1306_send_data_start();

    // Copy into local buffer forwards or backswards, then pass on :)

    for (uint8_t i = xoff; i < xw; i++) // i.e. i goes 0 to 15 (across x axis) if not clipped
    {
      if (flip == 0) pos = buf + j + (i * write_height);
      else pos = buf + j + ((w - 1 - i) * write_height);
      temp[i - xoff] =  pgm_read_byte(pos);
    }

    // cursor is incremented auto-magically to the right (x++) per write
    //     if (flip==0) pos=buf+j+(i*write_height);
    //   else pos=buf+j+((w-1-i)*write_height);

    oledWriteDataBlock(temp, xw - xoff); // DUNCAN - rewrite to allow whole row at once!
    //ssd1306_send_byte(pgm_read_byte(pos)); //DUNCAN

    //ssd1306_send_stop();
  }
}

void getWorld(uint8_t cx, uint8_t w)
{
  // vx is in units of bricks (i.e. vx/8), one screen is 16 wide
  uint8_t le;
  uint8_t *element_ptr;
  uint8_t i, j;

  for (i = cx; i < cx + w; i++) // cx is &WOLRD_MAX_LEN so cannot exceed 64.
  {
    le = curr_seed & 0x07;
    //curr_seed  = (curr_seed*1664525 + 1013904223) % 65536; // next seed

    element_ptr = levelelements[le];

    for (j = 0; j < 4; j++) // 4 = width of level element
    {
      if ((i + j) <= WORLD_MAX_LEN) screen[i + j] = *(element_ptr + j); //pgm_read_byte(element_ptr+j);
      else return;
    }

    i += 3; // width of level element - 1
  }
}

void drawScreen()
{
  uint8_t starti, stopi, wrapped_i, coin = 0, pipe = 0;
  int xoffset;

  starti = (viewx >> 3) & WORLD_MAX_LEN;  // wrap round (could be WORLD_BUFFER - 1)
  xoffset = -(viewx & 0x07);
  stopi = starti + 16;

  if (starti > 0) {
    starti--;  // Handles blanking blacking out edge when *just* disappeared off screen
    xoffset -= 8;
  }

  if (xoffset != 0) stopi++; // Need to add an extra brick on the right because we are not drawing left brick fully
  //if (stopi>WORLD_BUFFER) stopi=WORLD_BUFFER;

  for (uint8_t i = starti; i < stopi; i++) // limit of 256
  {
    if (delta_viewx < 0) wrapped_i = (i + 1) & WORLD_MAX_LEN; // wrap round (could be WORLD_BUFFER - 1)
    else if (delta_viewx > 0) wrapped_i = (i - 1) & WORLD_MAX_LEN;

    coin = 0;
    if (screen[i & WORLD_MAX_LEN] & (1 << 0)) coin = 1;

    if ((screen[i & WORLD_MAX_LEN] & 0xFE) == 0b11100000) pipe++; // A pipe column (first or second...)
    else pipe = 0;

    for (uint8_t y = 1; y < 8; y++)
    {
      if (screen[i & WORLD_MAX_LEN] & (1 << y)) // There is a block here
      {
        if (delta_viewx < 0) // screen is scrolling right and we are within array limit
          if (!(screen[wrapped_i] & (1 << y))) vblankout(xoffset + 8, y << 3, -delta_viewx); // View moving to the right, blocks scrolling to the left,
        // therefore blank out smearing trail to the right of block
        // but only if there isn't a block there.
        if (delta_viewx > 0)
          if (!(screen[wrapped_i] & (1 << y))) vblankout(xoffset - delta_viewx, y << 3, delta_viewx);


        if ((y == 5 || y == 6) && pipe != 0) // it's a pipe column
        {
          if (y == 5 && pipe == 2) drawSprite(xoffset - 8, y << 3, 16, 16, &mario_pipe2[0], 0);
          //drawSprite(xoffset, y<<3,8,8,&block8x8[0], 0);
        }
        else
        { // don't draw bricks on pipes
          if (y == 7) drawSprite(xoffset, y << 3, 8, 8, &block8x8[0], 0);
          else if ((screen[i & WORLD_MAX_LEN] & (1 << (y - 1))) == 0) // y>0 and nothing above, draw grassy brick
            drawSprite(xoffset, y << 3, 8, 8, &topbrick[0], 0);
          else drawSprite(xoffset, y << 3, 8, 8, &brick8x8[0], 0); // else brick is default
        }

        if (coin)
        {
          if (y == 1) drawCoin(xoffset, (y - 1) << 3);
          else drawCoin(xoffset, (y - 2) << 3);
          coin = 0;
        }
      }
      //else vblankout(xoffset,y<<3,8);
    }
    xoffset += 8;
  }
}

void playSoundEffect(uint8_t num)
{
  //if (soundeffectplaying) return; // only one at once
  // 6 - coin collect sound

  tone_timer0_toggle_count = 0; // stop current sound

  SoundEffectController.music_pos = 0;
  SoundEffectController.music_index = num;
  SoundEffectController.noteordelay = 1;

  soundeffectplaying = 1;
}

void initMusic(struct toneController *controller)
{
  controller->music_pos = 0; // First position in music sequence, i.e. 0,1,2,3 etc )
  controller->music_index = music_seq[controller->music_pos];//pgm_read_byte(&music_seq[0]+music_pos); // melody 0 (=intro
  controller->music_note = 0; // First note of music_index melody
  controller->noteordelay = 1; //simulate end of a delay to start the note
}

void handleMusic(struct toneController *controller)
{
  uint16_t *melody;
  uint8_t *noteDurations;
  uint8_t numNotes;
  uint8_t *entry_pos;

  if ((controller->soundeffect == 0 && tone_timer1_toggle_count == 0) || (controller->soundeffect == 1 && tone_timer0_toggle_count == 0 && soundeffectplaying == 1))
  {
    if (controller->noteordelay == 0) // 0 = NOTE just ended, therefore now delay
    {
      // Note ended, start a delay
      controller->current_delay = (controller->current_delay/3);

      if (controller->soundeffect == 0) // Music
      {
        mytone(0, controller->current_delay,1);
      }
      else mytone(0, controller->current_delay,0);

      // Advance to next note position
      controller->music_note++;
      numNotes = eeprom_read_byte((controller->music_index * sizeof(music_table)) + 2); // next position is numNotes

      if (controller->music_note >= numNotes) // exceeded notes in this melody part
      {
        controller->music_note = 0;
        controller->music_pos++; // advance sequence position

        if (controller->soundeffect == 1) // we are in the sound effect handler
        {
          soundeffectplaying = 0; // Global, control is passed back onto music handler, music will restart at next tone.
        }
        else if (controller->music_pos >= 12) // In music handler, and sequence has overflowed.
        {
          controller->music_pos = 1; // We are in the music handler
        }
        controller->music_index = music_seq[controller->music_pos];//pgm_read_byte(&music_seq[0]+music_pos);
      }
      controller->noteordelay = 1; //delay is next
    } // end note ended
    else if (controller->noteordelay == 1) // 1 = DELAY
    {
      // Delay ended, play next note
      entry_pos = controller->music_index * sizeof(music_table);
      melody = eeprom_read_word((uint16_t*)entry_pos); // EEPROM position in entry table
      entry_pos += 2;

      numNotes = eeprom_read_byte(entry_pos); // next position is numNotes
      noteDurations = (uint8_t*)(melody + (numNotes)); // noteDurations follows the melody in EEPROM

      uint16_t ms = 1000, freq;

      if (controller->soundeffect == 0) ms = MusicSpeed; // Music speed might change

      freq = eeprom_read_word(melody + controller->music_note);
      controller->current_delay = (ms / (uint16_t) eeprom_read_byte(noteDurations + controller->music_note));

      if (controller->soundeffect == 0) // Music
      {
        mytone(freq, controller->current_delay,1);
      }
      else  // Sound effects
      {
        mytone(freq, controller->current_delay,0);
      }

      controller->noteordelay = 0;
    }// end delay ended
  } // wnd time delay lapsed
}

/*
  #define ref_octave 4 // A4 = 440 Hz
  #define ref_note 9
  #define ref_freq 112640 // 256 * 440
  #define note_factor 271 // 256 * 1.0594630

  #define FIXED_MUL(a,b) {temp = a*b; a = temp + ((temp  & 1<<7)<<1); a>>=8;}

  uint16_t freq_shift[11] = {271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483};

  uint16_t calculate_freq(uint8_t coded_note)
  {
  uint8_t octave, note;
  uint32_t freq=ref_freq;
  uint32_t temp;

  octave=coded_note>>4;
  note=coded_note&0x0f;

  if (note>ref_note)
  {
    freq=freq*freq_shift[(note-ref_note)-1];
    freq>>=8;
  }
  else if (note<ref_note)
    freq=(freq<<8)/freq_shift[(ref_note-note)-1];

  if (octave>ref_octave)
    for (uint8_t o=ref_octave;o<octave;o++)
    {
      freq<<=1;
    }
  else if (octave<ref_octave)
    for (uint8_t o=octave;o<ref_octave;o++)
      freq>>=1;

  freq>>=8;
  return (uint16_t)freq;
  }*/

void setup() {
  //_delayms(50);
  oledInit(0x3c, 0, 0);
  oledFill(0);
  oledInit(0x3d, 0, 0);
  oledFill(0);

  //oledFill(0b00001111);

  mario.x = 5;
  mario.y = 0;
  mario.state = idle;
  mario.jumpstate = jumpdown;
  mario.vx = 0;
  mario.vy = 0;
  mario.frame = 0;
  mario.dir = faceright;
  mario.coincollector = 1;

  fireball.mask=1;
  fireball.w=1;
  fireball.state=dead;
  
  //analogReference(0); // 0 = Vcc as Aref (2 = 1.1v internal voltage reference)

  //pinMode(SPEAKER, OUTPUT);
  //pinMode(SPEAKER2, OUTPUT);

  DDRB |= (1 << SPEAKER); // OUTPUT
  DDRB |= (1 << SPEAKER2); // OUTPUT

  // Setup screen
  getWorld(0, WORLD_MAX_LEN + 1); //,16);

  lviewx_trigger = 0; //if viewx< trigger,
  rviewx_trigger = 12;// if viewx > trigger*8 getWorld element in certain position.
  //drawScreen();

  MusicController.soundeffect = 0;
  SoundEffectController.soundeffect = 1;
  soundeffectplaying = 0;
  initMusic(&MusicController);

  // Change music speaker to a hardware pin that Timer1 supports - i.e. pin4 = PB4
  TCCR0A = 0;//notone0A();
  TIMSK = 0;

  TCCR1 = 0 ;
  GTCCR = 0;
  TIFR = 0;
  
  turtle.x = 100; turtle.y = 0;
  turtle.vx = 1; turtle.state=dead;
}

void mytone(unsigned long frequency, unsigned long duration, uint8_t timer) // Hard coded for pin PB1 (OCR0A)
{
  uint32_t ocr;
  uint8_t prescalarbits = 0b001;
  uint8_t output = 1;

  // timer = 0 - use timer 0 and SPEAKER2 pin
  // timer = 1 - use timer 1 and SPEAKER pin

  if (frequency == 0)
  { // Means we want to switch off the pin and do a pause, but not output to the hardware pin
    output = 0; frequency = 300; // A dummy frequency - timings are calculated but no pin output.
  }

  ocr = F_CPU / (2 * frequency);
  
  if (timer==0)
  {
    if (ocr > 256)
    {
      ocr >>= 3; //divide by 8
      prescalarbits = 0b010;  // ck/8
      if (ocr > 256)
      {
        ocr >>= 3; //divide by a further 8
        prescalarbits = 0b011; //ck/64
        if (ocr > 256)
        {
          ocr >>= 2; //divide by a further 4
          prescalarbits = 0b100; //ck/256
          if (ocr > 256)
          {
            // can't do any better than /1024
            ocr >>= 2; //divide by a further 4
            prescalarbits = 0b101; //ck/1024
          } // ck/1024
        }// ck/256
      }// ck/64
    }//ck/8*/
  } //timer==0
  else
  {
    prescalarbits = 1;
    while (ocr > 0xff && prescalarbits < 15) {
          prescalarbits++;
          ocr>>=1;
    }
  }
   
  ocr -= 1;

  if (timer==0)
  {
    tone_timer0_toggle_count = (2 * frequency * duration) / 1000;
  }
  else
  {
    // timer1 scalar code here
    tone_timer1_toggle_count = (2 * frequency * duration) / 1000;
    // Music uses timer1 and is on continously so use as a crude timer
    ISR_micro_period = 500000L / frequency; // 1E6 / (2*frequency), because ISR is called 2 times every period.
  }

  if (output)
  {
    if (timer==0)
    {
    TCCR0A = (1 << COM0B0) | (1 << WGM01); // = CTC // Fast PWM | (1<<WGM01) | (1<<WGM00);
    }
    else 
    {
     GTCCR = (1 << COM1B0); // set the OC1B to toggle on match
    }
  }
  else // no output 
  {
    if (timer==0)
    {
      TCCR0A |= (1 << WGM01); // I guess this is always true
      TCCR0A &= ~(1 << COM0B0); // No hardware pin output 
      PORTB &= ~(1 << SPEAKER2); // Set pin LOW
    }
    else
    {
      // timer 1 code to turn off hardware support here
      GTCCR &= (1 <<COM1B0); // disconnect hardware pin
      PORTB &= ~(1 << SPEAKER); // Set pin LOW
    }
  }

  if (timer==0)
  {
    TCCR0B = prescalarbits << CS00; //(1<<CS01);// | (1<<CS00); // Scalar. //prescalarbits;//
    TCNT0 = 0; // Set timer0 counter to 0
    OCR0A = ocr; // set compare value
    TIMSK |= (1 << OCIE0A); // Activate timer0 COMPA ISR
  }
  else
  {
    TCCR1 = (1<<CTC1)| (prescalarbits<<CS10); // CTC1 : Clear Timer/Counter on Compare Match, after compare match with OCR1C value
    TCNT1 = 0; // timer 1 counter = 0
    OCR1C = ocr; // set compare value "OCR1C replaces OCR1B"
    TIMSK |= (1 << OCIE1B); // Activate timer1 COMPB ISR
  }
  
}

ISR(TIMER1_COMPB_vect) {
  if (tone_timer1_toggle_count != 0)
  {
    if (tone_timer1_toggle_count > 0)
    {
      tone_timer1_toggle_count--;
      if (tone_timer1_toggle_count == 0)
      {
        // turn off tone
        GTCCR &= ~(1 << COM1B0); // Disconnect OC1B pin
       // TIMSK &= ~(1 << OCIE1B); // Turn off interrupt bit.
        return;
      }
    }
  }
  mymicros += ISR_micro_period;
}

ISR(TIMER0_COMPA_vect) {
  if (tone_timer0_toggle_count != 0)
  {
    if (tone_timer0_toggle_count > 0)
    {
      tone_timer0_toggle_count--;
      if (tone_timer0_toggle_count == 0)
      {
        // turn off tone
        //TCCR0A = 0;
        TCCR0A &= ~(1<<COM0B0);
        TIMSK &= ~(1 << OCIE0A); // Turn off interrupt bit.
        return;
      }
    }
  }
}

void loop() {
  //static uint16_t view_vel=1;
  static long loop_micros=0;
  
  curr_seed++;// = (curr_seed*1664525 + 1013904223) % 65536; // next seed

  readbuttons();

  if (gamestate == normal)
  {
    handlemap_collisions(&mario); // Check for collisions with Mario and the world
    
    if (fireball.state!=dead) 
    {
      handlemap_collisions(&fireball);
      if (fireball.y<=0 || fireball.y>=63) {fireball.y=0;fireball.frame=90;}
      
    // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT
      if (fireball.collision &1) fireball.vy=FIREBALL_SPEED;
      if (fireball.collision &4) fireball.vy=-FIREBALL_SPEED;
      if (fireball.collision &2) fireball.vx=-FIREBALL_SPEED;
      if (fireball.collision &8) fireball.vx=FIREBALL_SPEED;

      if (fireball.collision!=0) fireball.frame+=30;

      fireball.frame++;
      if (fireball.frame>100) fireball.state=dead;
    }

    if (turtle.state!=dead) 
    {
      handlemap_collisions(&turtle);
      animate_character(&turtle);
    }
   
    animate_character(&mario); // just applies gravity, could be done elsewhere (i.e. handlemap_collisions physics?)
    
    if (turtle.state!=dead) // Not dead. Alive you might say.
    {
      if (turtle.collision & 2) turtle.vx = -1;
      else if (turtle.collision & 8) turtle.vx = 1;

      if (turtle.x <= 0) {
        turtle.x = 0;
        turtle.vx = 1;
      }
      turtle.frame++;
      if (turtle.state==idle && turtle.frame >= 10) turtle.frame = 0;
      if (turtle.state==squash && turtle.frame>200) turtle.state=dead;
    } // end enemy not dead

    if (mario.frame++ >= 6) mario.frame = 0;

    cointimer++;  // Animate coin
    if (cointimer >= 5)
    {
      cointimer = 0;
      coinframe++;
      if (coinframe >= 5) coinframe = 1;
    }

    if (coincount >= 10 && coincount < 20) MusicSpeed = 500; else MusicSpeed = 1000;

    if (mario.x <= 192) viewx = 0;
    else if (viewx < mario.x - 192) viewx = mario.x - 192; // Do we need to change the viewport coordinate?

    uint16_t blockx = viewx >> 3; // viewx / 8;
    if (blockx >= rviewx_trigger) // convert viewx to blocks
    {
      getWorld((rviewx_trigger + 20 + 32)&WORLD_MAX_LEN, 4);

      rviewx_trigger += 4;
      lviewx_trigger = rviewx_trigger - 8; //4;
    }

  }

  /*
    if (blockx<=lviewx_trigger)
    {
      //curr_seed = prev_seed;
      // Going left, so put back the world that was replaced when we went right!
      //playSoundEffect(COIN_SOUND);
      getWorld(lviewx_trigger-8,(lviewx_trigger+24)&31,4);
      if (lviewx_trigger>=12) lviewx_trigger-=4;
      rviewx_trigger = lviewx_trigger + 8;//4;
    }*/

#ifndef TEST_BUTTONS
  updateDisplay(0); // Draw screen
  oledWriteInteger(10, 0, coincount);
//  oledWriteInteger(80, 0, mymicros/1000000L);
  drawCoin(0, 0);
  updateDisplay(1);
#endif

  // 1E6 / fps
  while (mymicros-loop_micros<=25000) // 40 fps
  {
    if (gamestate == normal) handleMusic(&MusicController);
  handleMusic(&SoundEffectController);
    }
  loop_micros = mymicros;
}

void handlemap_collisions(struct character *player)
{
  // Only apply velocities if no collisions in that direction, else clip to collision point.
  int newmario_x, newmario_y;
  uint8_t cellx, celly, ybitmask;
  uint16_t cellx_zone;

  if (player->vy < -7) player->vy = -7;
  newmario_x = player-> x + player->vx;
  newmario_y =  player->y + player->vy;
  player->collision = 0; // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT

  //if ((newmario_x-(int16_t)viewx)<0) newmario_x=viewx;

  cellx_zone = (newmario_x >> 9); // 0-31 = 0, 32-63=1, 64-95=2, 96-127=3. >>8 = 3 + (log2 WORLD_BUFFER);

  // 0-63 = 0, 64-127 = 1 ... therefore 64*8 = 2^6 * 2^3 = 2^9 = 512
  cellx = (newmario_x >> 3) & WORLD_MAX_LEN;
  celly = newmario_y >> 3; // generate bit-mask for byte describing y-column in screen data.
  uint8_t yoffset = newmario_y & 0x07, xoffset = newmario_x & 0x07;

  //ybitmask = 1 << celly;
  if (yoffset==0) ybitmask = (player->mask) << celly;
  else ybitmask = ((player->mask<<1)+1)<<celly; // add an extra cell 
  
  if (newmario_y < 0) {
    celly = 0;  // means that a block is never found and mario can jump through the top of the screen!
    ybitmask = 0;
  }

  //if (ybitmask == 1) {
//    ybitmask = 0; // means no collisions with row0
 // }
 ybitmask&=~(1<<0); // means no collisions with row0

  uint8_t check_cell, check_cell2;

  if (player->vx > 0) // Check Mario's righthand side if he tried to move to the right
  {                 
    check_cell = (cellx + player->w) & WORLD_MAX_LEN; // WORLD_BUFFER - 1
    
    //if (screen[check_cell]&ybitmask || screen[check_cell] & (ybitmask << 1) || (yoffset != 0 && screen[check_cell] & (ybitmask << 2)))
    if (screen[check_cell]&ybitmask) // that's all
    { // clip
      // Strictly we should update cellx_zone to be relevant to the zone of the block that caused the collision
      player->x = (cellx_zone << 9) + (cellx << 3);
      if (cellx == 0) cellx = WORLD_MAX_LEN; else cellx -= 1; // handle wrap
      player->collision |= 2; // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT
      player->vx = 0;
    }
    else
    {
      player->x = newmario_x;
    }
  }
  else if (player->vx < 0)
  {
    //player->x=newmario_x;
   // if (screen[cellx]&ybitmask || screen[cellx] & (ybitmask << 1) || (yoffset != 0 && screen[cellx] & (ybitmask << 2)))
   if (screen[cellx]&ybitmask) 
    { // clip
      ///cellx=(cellx+1) & 31; // if collided with left, push back to the right - handle wrap
      cellx++;
      if (cellx > WORLD_MAX_LEN) {
        cellx = 0;
        cellx_zone++;
      }
      player->x = (cellx_zone << 9) + (cellx << 3); // Strictly we should update cellx_zone to be relevant to the zone of the block that caused the collision
      player->collision |= 8; // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT
      player->vx = 0;
    }
    else
    {
      player->x = newmario_x;
    }

  }

  //
  // celly+2 should be valid at this point
  
  ybitmask = 1 << (celly+player->w);
  ybitmask&=~(1<<0); // means no collisions with row 0
  
  if (player->vy > 0 || player->vy == 0) // going down
  {
    check_cell = (cellx + 1) & WORLD_MAX_LEN;
    check_cell2 = (cellx + 2) & WORLD_MAX_LEN;
    //if ((celly + 2 >= 8) || screen[cellx] & (ybitmask << 2) || screen[check_cell] & (ybitmask << 2) || (xoffset != 0 && screen[check_cell2] & (ybitmask << 2)))
    if ((celly + 2 >= 8) || screen[cellx] & ybitmask  || screen[check_cell] & ybitmask  || (xoffset != 0 && screen[check_cell2] & ybitmask))
    { // clip
      player->vy = 0; player->jumpstate = nojump;
      if (celly + 2 >= 8) celly = 6;
      player->y = (celly << 3);
      player->collision |= 4; // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT
      player->vy = 0;
    }
    else
    {
      if (player->vy == 0 && player->jumpstate == nojump) {
        player->jumpstate = jumpdown;
      }
      player->y = newmario_y; // allow new vertical position
    }
  }
  else if (player->vy < 0) // Have we hit our head?
  {
    ybitmask = 1 << (celly);
    ybitmask&=~(1<<0); // means no collisions with row 0
  
    //player->y=newmario_y;// allow new vertical position
    check_cell = (cellx + 1) & WORLD_MAX_LEN; // WORLD_BUFFER -1
    check_cell2 = (cellx + 2) & WORLD_MAX_LEN; // WORLD_BUFFER -1

    if (screen[cellx] & (ybitmask) || screen[check_cell] & (ybitmask) || (xoffset != 0 && screen[check_cell2] & (ybitmask)))
    { // clip
      player->vy = 0; player->jumpstate = jumpdown;
      celly += 1;
      player->y = (celly << 3);
      if (player->coincollector) playSoundEffect(HITHEAD_SOUND);
      player->collision |= 1; // 1 = UP, 2=RIGHT, 4=DOWN, 8=LEFT
      player->vy = 0;
    }
    else
    {
      //if (mario.vy==0 && mario.jumpstate==nojump) {mario.jumpstate=jumpdown;}
      player->y = newmario_y; // allow new vertical position
    }
  }
  // Coin collision

  // mario is at: cellx, cellx+1 (and if xoffset!=0, cellx+2)
  // and at: celly, celly+1 (and if yoffset!=0, celly+2)

  if (player->coincollector)
  {
    collidecoins(cellx, celly, yoffset);
    collidecoins(cellx + 1, celly, yoffset);
    if (xoffset != 0) collidecoins(cellx + 2, celly, yoffset);
  }
}

/*
  void handlemap_collisions(struct character *player)
  {
  // Only apply velocities if no collisions in that direction, else clip to collision point.
  int newmario_x,newmario_y;
  uint8_t cellx, celly, ybitmask;
  uint16_t cellx_zone;

  if (mario.vy<-7) mario.vy=-7;
  newmario_x = player->x+mario.vx;
  newmario_y =  mario.y+mario.vy;


  if ((newmario_x-(int16_t)viewx)<0) newmario_x=viewx;

  cellx_zone = (newmario_x>>8); // 0-31 = 0, 32-63=1, 64-95=2, 96-127=3. >>8 = 3 + (log2 WORLD_BUFFER);
  cellx = (newmario_x>>3) & WORLD_MAX_LEN;
  celly = newmario_y>>3; // generate bit-mask for byte describing y-column in screen data.
  uint8_t yoffset = newmario_y & 0x07, xoffset=newmario_x & 0x07;

  ybitmask = 1<<celly;
  if (newmario_y<0) {celly=0;ybitmask=0;} // means that a block is never found and mario can jump through the top of the screen!

  if (ybitmask==1) {ybitmask=0;} // means no collisions with row0

  //if (celly+2>=8) {mario.y=celly<<3;mario}// In fact mario should die!

  //if (cellx+2>=WORLD_BUFFER) // overflow!
  //{
  //    mario.vx=0;
  //    mario.x=(WORLD_BUFFER-2)<<3;

  //} // Here we know that cell_x+2 is valid and hasn't overflowed.
  //else
  uint8_t check_cell,check_cell2;

  if (mario.vx>0) // Check Mario's righthand side if he tried to move to the right
  {
     check_cell = (cellx+2) & WORLD_MAX_LEN; // WORLD_BUFFER - 1

      if (screen[check_cell]&ybitmask || screen[check_cell]&(ybitmask<<1) || (yoffset!=0 && screen[check_cell]&(ybitmask<<2)))
      { // clip
        // Strictly we should update cellx_zone to be relevant to the zone of the block that caused the collision
      mario.x=(cellx_zone<<8) + (cellx<<3);
      if (cellx==0) cellx=WORLD_MAX_LEN; else cellx-=1; // handle wrap
      }
      else
      {
        mario.x=newmario_x;
      }
  }
  else if (mario.vx<0)
  {
    //mario.x=newmario_x;
    if (screen[cellx]&ybitmask || screen[cellx]&(ybitmask<<1) || (yoffset!=0 && screen[cellx]&(ybitmask<<2)))
      { // clip
        ///cellx=(cellx+1) & 31; // if collided with left, push back to the right - handle wrap
        cellx++;
        if (cellx>WORLD_MAX_LEN) {cellx=0; cellx_zone++;}
        mario.x=(cellx_zone<<8) + (cellx<<3); // Strictly we should update cellx_zone to be relevant to the zone of the block that caused the collision
      }
      else
      {
        mario.x=newmario_x;
      }

  }

  //
   // celly+2 should be valid at this point

  if (mario.vy>0 || mario.vy==0) // going down
  {
    check_cell = (cellx + 1) & WORLD_MAX_LEN;
    check_cell2 = (cellx + 2) & WORLD_MAX_LEN;
    if ((celly+2>=8) || screen[cellx]&(ybitmask<<2) || screen[check_cell]&(ybitmask<<2) || (xoffset!=0 && screen[check_cell2]&(ybitmask<<2)))
      { // clip
        mario.vy=0; mario.jumpstate=nojump;
        if (celly+2>=8) celly=6;
        mario.y=(celly<<3);
      }
      else
      {
        if (mario.vy==0 && mario.jumpstate==nojump) {mario.jumpstate=jumpdown;}
        mario.y=newmario_y;// allow new vertical position
      }
  }
  else if (mario.vy<0) // Have we hit our head?
  {
     //mario.y=newmario_y;// allow new vertical position
     check_cell = (cellx+1) & WORLD_MAX_LEN; // WORLD_BUFFER -1
     check_cell2 = (cellx+2) & WORLD_MAX_LEN; // WORLD_BUFFER -1

     if (screen[cellx]&(ybitmask) || screen[check_cell]&(ybitmask) || (xoffset!=0 && screen[check_cell2]&(ybitmask)))
      { // clip
        mario.vy=0;mario.jumpstate=jumpdown;
        celly+=1;
        mario.y=(celly<<3);
        playSoundEffect(HITHEAD_SOUND);
      }
      else
      {
        //if (mario.vy==0 && mario.jumpstate==nojump) {mario.jumpstate=jumpdown;}
        mario.y=newmario_y;// allow new vertical position
      }
  }
  // Coin collision

  // mario is at: cellx, cellx+1 (and if xoffset!=0, cellx+2)
  // and at: celly, celly+1 (and if yoffset!=0, celly+2)

  collidecoins(cellx,celly,yoffset);
  collidecoins(cellx+1,celly,yoffset);
  if (xoffset!=0) collidecoins(cellx+2,celly, yoffset);

  }

*/

void collidecoins(uint8_t cx, uint8_t celly, uint8_t yoffset)
{
  uint8_t coiny = 8, h;

  h = 2; if (yoffset != 0) h = 3;

  cx = cx & WORLD_MAX_LEN;

  if (screen[cx] & 1) // Is there a coin in this column?
  {
    coiny = findcoiny(cx);
    if (coiny != 8)
    {
      //if (collideMarioblock(cx, coiny))
      if (coiny >= celly && coiny < (celly + h))
      {
        screen[cx] &= 254;
        playSoundEffect(COIN_SOUND);
        coincount++;
        {
          turtle.x = viewx + 64;
          turtle.y = 0;
          turtle.vx = 1;
          turtle.state=idle;
        }
        //starti = (viewx >> 3) & 31;
        //vblankout((cx<<3)-(viewx & 127), coiny*8,16);

        // This function is only called when screen = 0, address 0x3D (left screen)
        if (mario.x > (viewx + 128)) oled_addr = 0x3C; // right hand side screen
        for (uint8_t x = 0; x < 8; x++) vblankout(x << 4, coiny * 8, 16);
        oled_addr = 0x3D; //set address back to be safe
      }
    }
  }

}
/*
  uint8_t collideMarioblock(uint8_t cx, uint8_t cy)
  {
  uint8_t cellx, celly,w,h;
  uint8_t yoffset, xoffset;

  cellx = (mario.x >>3) & WORLD_MAX_LEN; celly = mario.y >>3;
  xoffset = mario.x & 0x07;
  yoffset = mario.y & 0x07,

  w = 2; if (xoffset!=0) w = 3;
  h = 2; if (yoffset!=0) h = 3;

  //if (cx>=cellx && cx<(cellx+w) && cy>=celly && cy<(celly+h)) return 1;
  if (cy>=celly && cy<(celly+h)) return 1;
  return 0;
  }*/

uint8_t collideThings(int mx, int my, int mw, int mh, int tx, int ty, uint8_t w, uint8_t h)
{
  if ((mx + mw) < tx || mx > (tx + w)) return 0;
  if ((my + mh) < ty || my > (ty + h)) return 0;

  return 1;
}

uint8_t collideMario(int tx, int ty, uint8_t w, uint8_t h)
{
  if ((mario.x + 8 + 4) < tx || (mario.x + 4) > (tx + w)) return 0;
  if ((mario.y + 8 + 4) < ty || (mario.y + 4) > (ty + h)) return 0;

  return 1;
}

uint8_t findcoiny(uint8_t cx)
{
  for (uint8_t y = 1; y < 8; y++)
  {
    if (screen[cx & WORLD_MAX_LEN] & (1 << y)) // There is a block here
    {
      if (y == 1) return 0; //drawCoin(xoffset, (y-1)<<3);
      else return y - 2; //drawCoin(xoffset, (y-2)<<3);
    } // if found block
  } // for y
  return 8; // 8 means couldn't find a coin.
}

void animate_character(struct character *player)
{

  // Jumping
  if (player->jumpstate != nojump)
  {
    player->vy += 1; // Gravity
    if (player->vy >= 0 && player->jumpstate == jumpup) player->jumpstate = jumpdown; // flip state for animation
  }
  else // idle, left or right
  {
    //player->frame++;//1-mario.frame; // animate frame

  }
}

void draw_mario()
{
  // Drawing  ***************************************
  if (mario.jumpstate == nojump) // idle, left or right
  {
    if (mario.state == idle) drawSprite(mario.x - viewx, mario.y, 16, 16, &mario_idle[0], mario.dir);
    else if (mario.state == left || mario.state == right)
    {
      if (mario.frame < 3) drawSprite(mario.x - viewx, mario.y, 16, 16, &mario_walk1[0], mario.dir);
      else drawSprite(mario.x - viewx, mario.y, 16, 16, &mario_walk2[0], mario.dir);
    } // end: left or right walking
  } // end: not jumping
  else if (mario.jumpstate == jumpup) drawSprite(mario.x - viewx, mario.y, 16, 16, &mario_jump1[0], mario.dir);
  else if (mario.jumpstate == jumpdown) drawSprite(mario.x - viewx, mario.y, 16, 16, &mario_jump2[0], mario.dir);
}

void blank_character(uint8_t screen_id, struct character *player)
{
  //static int oldmario_x, oldmario_y; //initialised


  //if (screen_id==0) // left - for now do this, later will have to store for each screen
  //{

  if ((player->x - (int16_t)viewx) != player->oldx[screen_id] || (player->y != player->oldy[screen_id])) // Blankout old mario position sprite if we've moved
  {
    vblankout(player->oldx[screen_id], player->oldy[screen_id], 16);
    vblankout(player->oldx[screen_id], player->oldy[screen_id] + 8, 16);
  }

  //oldmario_x = mario.x-(int16_t)viewx; oldmario_y = mario.y;
  player->oldx[screen_id] = player->x - (int16_t)viewx;
  player->oldy[screen_id] = player->y;
  //}
}

void vblankout(int sx, int sy, uint8_t w)//, uint8_t h)
{
  // Set cursor @ top left corner

  if (sx + w <= 0 || sx >= 128) return; // No amount of vertical strip will be visible

  if (sx < 0) {
    w = sx + w;
    sx = 0;
  }
  if (sx + w > 128) {
    w = 128 - sx;
  }

  //ssd1306_send_command3(renderingFrame | (sy >> 3), 0x10 | ((sx & 0xf0) >> 4), sx & 0x0f);

  oledSetPosition(sx, sy);
  //ssd1306_send_data_start();

  uint8_t temp[16] = {0};

  oledWriteDataBlock(temp, w);

  /*
    for (uint8_t i=0; i<w; i++)// i.e. i goes 0 to 15 (across x axis)
    {
    //  ssd1306_send_byte(0x00); // DUNCAN
    }*/
  ////ssd1306_send_stop();

}

#define buttonBASE 21
#define button2BASE 30

void readbuttons(void)
{
  static uint16_t sensor2Value = 260, mariosetvx = 0;
  static uint8_t pause_button = 0, pause_debounce = 0;
  int sensorValue;

  uint8_t buttons;
  uint8_t buttons2;

  sensorValue = analogRead(A3); // was A2 / A3
  sensor2Value = 1023 - analogRead(A1);
  sensor2Value = (99 * sensor2Value) + (1023 - analogRead(A1)); //0.9f*sensor2Value + 0.1f*(1023-analogRead(A1));
  sensor2Value /= 100; // Do some filtering on this in software

  //buttons2 = (sensor2Value+(button2BASE/2)-260)/button2BASE;
  //if (sensor2Value>450) buttons=7;
  buttons2 = 0;

#ifdef TEST_BUTTONS
  uint8_t buf[10];

  //oledFill(0);
  itoa(pause_button, buf, 10);
  oledWriteString(0, 0, buf);

  itoa(pause_debounce, buf, 10);
  oledWriteString(40, 0, buf);

  itoa(gamestate, buf, 10);
  oledWriteString(70, 0, buf);
#endif

  buttons = (sensorValue + (buttonBASE / 2)) / buttonBASE; //round(sensorValue/buttonBASE);

  if (gamestate == normal) // can only move if not paused, etc.
  {
    // Moving (independent of jumping)
    if (buttons & 1)
    {
      // if (mario.state!=right) {mario.vx++; mariosetvx=mario.vx;mario_acc_timer=30;}

      mario.state = right; mario.dir = faceright;
      //mario_acc_timer++;if (mario_acc_timer>30 && mario.vx<3) {mario_acc_timer=0;mario.vx++;mariosetvx=mario.vx;}
      //mario.vx=mariosetvx;
      mario.vx = 3;
    } // RIGHT
    else if (buttons & 4)
    {
      //if (mario.state!=left) {mario.vx--;mariosetvx=mario.vx;mario_acc_timer=0;}

      mario.state = left; mario.dir = faceleft;
      //mario_acc_timer++;if (mario_acc_timer>30 && mario.vx>-3) {mario_acc_timer=0;mario.vx--;mariosetvx=mario.vx;}
      mario.vx = -3; //mariosetvx;
    } // LEFT
    else
    {
      // De-accelerate
      if (mario.vx > 0) mario.vx -= 1;
      else if (mario.vx < 0) mario.vx += 1;
    }


    // Jumping
    if (buttons & 2 && mario.jumpstate == nojump) {
      mario.vy = -7;  // Only start jumping if not currently jumping
      mario.jumpstate = jumpup;
      playSoundEffect(JUMP_SOUND);
      fireball.frame=0;
      fireball.x=mario.x+16; fireball.y=mario.y; fireball.vy=FIREBALL_SPEED; fireball.vx=FIREBALL_SPEED; fireball.state=idle;
      if (mario.dir==faceleft) {fireball.vx=-FIREBALL_SPEED;fireball.x=mario.x;}
    }

    // Idle if not jumping and no keys pressed
    if (buttons == 0 && mario.jumpstate == nojump) mario.state = idle;
  }

  if (pause_button == 0)
  {
    if (buttons2 & 1) pause_debounce++; // button held
    else pause_debounce = 0; // off
  }
  else
  {
    if ((buttons2 & 1) == 0) pause_debounce++;
    else pause_debounce = 0;
  }

  if (pause_debounce > 10) {
    pause_button = 1 - pause_button;
    pause_debounce = 0;
  }

  if (pause_button)
  {
    if (gamestate == normal)
    {
      playSoundEffect(PAUSE_SOUND);
      oledWriteCommand2(0x81, 16);
      oled_addr = 0x3C;
      oledWriteCommand2(0x81, 16);
      oled_addr = 0x3D;
      gamestate = paused;
    }
    else if (gamestate == paused_ready) {
      gamestate = pause_return;
    }
  }
  else if (gamestate == paused)
  {
    gamestate = paused_ready;
  }
  else if (gamestate == pause_return)
  {
    gamestate = normal;
    oledWriteCommand2(0x81, 0x7F);
    oled_addr = 0x3C;
    oledWriteCommand2(0x81, 0x7F);
    oled_addr = 0x3D;
  }

}

void drawCoin(int sx, uint8_t sy)
{
  // Do coin(s)
  //  if (delta_viewx<0) vblankout(90-viewx+8,0,-delta_viewx);
  //  else if (delta_viewx>0) vblankout(90-viewx-delta_viewx,0,delta_viewx);

  if (delta_viewx < 0) vblankout(sx + 8, sy, -delta_viewx);
  else if (delta_viewx > 0) vblankout(sx - delta_viewx, sy, delta_viewx);

  if (coinframe == 1)
    drawSprite(sx, sy, 8, 8, &coin1[0], 0);
  else if (coinframe == 2 || coinframe == 4)
    drawSprite(sx, sy, 8, 8, &coin2[0], 0);
  else if (coinframe == 3)
    drawSprite(sx, sy, 8, 8, &coin3[0], 0);
}

void updateDisplay(uint8_t screen_id)
{
 // static int bally=0, ballvy=1;
  
  if (screen_id == 0) // Left screen
  {
    delta_viewx = old_viewx - viewx; // -ve means moving to right, +ve means moving to left.
    if (delta_viewx > 8) delta_viewx = 8; // Limit delta_viewx
    if (delta_viewx < -8) delta_viewx = -8;
    oled_addr = 0x3D;

   // vblankout(0,bally,8);
    //bally+=ballvy;
    //if (bally<0) {bally=0;ballvy=1;}
    //else if (bally>56) {bally=55;ballvy=-1;}
    
    //drawSprite(0,bally,8,8,&block8x8[0],0);
  }
  else // right hand screen  (0x3c)
  {
    viewx = viewx + 128;
    oled_addr = 0x3C;
  }

  drawScreen(); // Draws level scenery, coins, etc.

  if (fireball.state!=dead)
  {
    blank_character(screen_id, &fireball);
    if (fireball.frame<90) drawSprite(fireball.x-viewx, fireball.y,8,8,&fire[0],0); // this makes sure we blank before making dead
  }
  
  blank_character(screen_id, &mario);
  draw_mario();

  if (turtle.state!=dead) 
  {
    blank_character(screen_id, &turtle);
    if (turtle.state!=squash) drawSprite(turtle.x - viewx, turtle.y, 16, 16, &turtle1[0], (turtle.frame > 3));
    else if ((turtle.frame>>4)&1) drawSprite(turtle.x - viewx, turtle.y+8, 16, 8, &squashed[0], 0);
    else vblankout(turtle.x - viewx,turtle.y+8,16);
  }
  
  if (turtle.state==idle && collideMario(turtle.x, turtle.y, 16, 16))// || collideThings(turtle.x, turtle.y, 16,16,fireball.x, fireball.y,8,8)))
  {
    if (mario.vy > 0 && mario.jumpstate==jumpdown) {turtle.state = squash;turtle.frame=17;turtle.vx=0;turtle.x++;}//.x++ forces a blank replot
    else {
      turtle.x = viewx + 64;
      turtle.y = 0;
      turtle.vx = 1;
    }
  }

  if (screen_id == 0) old_viewx = viewx; // left screen
  else {
    viewx = viewx - 128;  // restore previous viewx
    oled_addr = 0x3D;
  }
}

void oledWriteInteger(uint8_t x, uint8_t y, uint8_t number)
{
  uint8_t n, n_div = 100;
  unsigned char ucTemp[16];


  oledSetPosition(x, y);

  for (uint8_t i = 0; i < 3; i++)
  {
    n = number / n_div; // 0-255, so n is never > 10
    oledWriteDataBlock(&ucSmallFont[n * 6], 6); // write character pattern
    number = number - n_div * n;
    n_div = n_div / 10; // 100 --> 10 --> 1
  }
  /*
    n = number/10;

    oledWriteDataBlock(&ucSmallFont[n*6], 6); // write character pattern
    number = number - 10*n;

    oledWriteDataBlock(&ucSmallFont[number*6], 6); // write character pattern
  */
}

/*
  int oledWriteString(int x, int y, char *szMsg)
  {
  int i, iLen, iFontOff;
  unsigned char c, *s, ucTemp[16];

    oledSetPosition(x, y);

  //       if (iLen*6 + x > 128) iLen = (128 - x)/6; // can't display it
  //       if (iLen < 0)return -1;
       iLen = 10; // max
       for (i=0; i<iLen; i++)
       {
         // we can't directly use the pointer to FLASH memory, so copy to a local buffer
         uint8_t c = szMsg[i];
         if (c=='\0') break;
         if (c>=45 && c<=57)
         {
            c = c-45;
            memcpy(ucTemp, &ucSmallFont[(unsigned char)c*6], 6);// was memcpy_P
            oledWriteDataBlock(ucTemp, 6); // write character pattern
         }
       }
  return 0;
  }*/
/* oledWriteString() */