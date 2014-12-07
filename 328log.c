#include <stdint.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/boot.h>
#include <avr/fuse.h>
#include <avr/eeprom.h>
#include <util/delay.h>

#ifndef F_CPU
#error F_CPU not defined
#endif
#ifndef BOOTSTART                     /* byte address of bootloader */
#error BOOTSTART not defined
#endif

#define INLINE /* nothing */

FUSES = {                             /* All are arduino defaults */
  .low = 0xFF,
  .high = 0xDA,
  .extended = 0x5,
};

struct options {
  uint8_t nrchans;
  uint8_t vref;
  uint8_t nrseconds;
};
struct options opt;
char EEMEM ee_config[88];

#define DBG_LED_ON \
{ DDRB |= _BV(DDB5); PORTB |= _BV(PORTB5); }

volatile uint8_t timer_secs; ISR(TIMER1_COMPA_vect) { timer_secs++; }

static inline void timer_init(void)
{
  TIMSK1 = _BV(OCIE1A);
#if 1
  OCR1A = 15625;                      /* every 1 second */
#else
  OCR1A = 3125;                       /* debug at speed */
#endif
  /* TCCR1A = 0; */
  TCCR1B = _BV(WGM12)                 /* CTC mode 4 */
         | _BV(CS12) | _BV(CS10);     /* /1024 = 15625Hz */
}

#define NO_CHAR_RECEIVED (! (UCSR0A & _BV(RXC0)))
int8_t getc(void)
{
  while( NO_CHAR_RECEIVED );
  return(UDR0);
}
void putch(char ch)
{
  while( !(UCSR0A & _BV(UDRE0)) );
  UDR0 = ch;
}
void putstr(char * s)
{
  while( *s )putch(*s++);
}
void putd(uint16_t d)
{
  static uint16_t powers[] = { 1000, 100, 10, 1 };
  char ch;
  uint8_t i;
  for(i = 0; i < 4; i++){
    ch = '0';
    while( d >= powers[i] ){
      d -= powers[i];
      ch++;
    }
    putch(ch);
  }
}

static void puteestr(char * s, uint8_t len)  /* XXX May be very slow */
{
  uint8_t i;
  for(i = 0; i < len; i++)
    putch(eeprom_read_byte((uint8_t *)s+i));
}
#define PUTEESTR(lit) \
{ static char str[] EEMEM = lit; puteestr(str, sizeof(str)-1); }
static inline void putnl(void) { PUTEESTR("\r\n"); }

static inline void usart_init(void)
{
  UBRR0H = 0;
  UBRR0L = 51;                        /* 19.2kbps */
  UCSR0B = _BV(RXEN0) | _BV(TXEN0);
}

/*
  page functions
 */

int16_t page_address;
uint8_t page_offset;

static inline void page_sync(void)
{
  if( page_address >= BOOTSTART ){
    for( ; ; ){
      sleep_mode();
      PUTEESTR("Out of memory\r\n");
    }
  }
  boot_page_write_safe((uint32_t)page_address);
  boot_spm_busy_wait();
  boot_rww_enable_safe();
  page_address += SPM_PAGESIZE;
  page_offset = 0;
  PUTEESTR("Flushed\r\n");
}

static void page_addword(uint8_t w)
{
  if( 0 == page_offset && page_address < BOOTSTART )
    boot_page_erase_safe(page_address);
  boot_page_fill_safe((uint32_t)page_address + page_offset, w);
  page_offset += 2;
  if( page_offset >= SPM_PAGESIZE )
    page_sync();
}

static void page_add(uint16_t w)
{
  static uint32_t buffer;
  static uint8_t ndx;
  buffer <<= 10;
  buffer |= w;
  if( ++ndx < 3 )return;
  ndx = 0;
  page_addword(buffer & 0xffff);
  page_addword(buffer >> 16);
}

#define VREF_AVCC (_BV(REFS0))
#define VREF_1V1  (_BV(REFS1) | _BV(REFS0))

static INLINE uint16_t read_adc(uint8_t mux)
{
  uint16_t lsb;
#if 0
  PRR &= ~_BV(PRADC);
  ADCSRB = 0;
#endif
  ADCSRA = _BV(ADEN)
         | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
  ADMUX = mux;
  ADCSRA |= _BV(ADSC);                /* start */
  while( ADCSRA & _BV(ADSC) );
  lsb = ADC;
  ADCSRA &= ~_BV(ADEN);
  return(lsb);
}

static void putspace(void) { PUTEESTR(" "); }
static void INLINE download(void)
{
  uint8_t i, empties, field;
  uint32_t addr;

  puteestr(ee_config, sizeof(ee_config)-1);
  putnl();

  addr = SPM_PAGESIZE;
  empties = 0;
  field = 0;

  while( addr < BOOTSTART ){
    uint32_t buffer;
    buffer = pgm_read_word_near(addr += 2);
    buffer <<= 16;
    buffer |= pgm_read_word_near(addr += 2);
    for(i = 0; i < 3; i++){
      putd((buffer & (0x3f<<20))>>20);
      buffer <<= 10;
      if( ++field == opt.nrchans ){ field = 0; putnl(); }
      else putspace();
    }
  }
  PUTEESTR("\r\n[EOF]\r\n");
}

static INLINE void erase(void)
{
  PUTEESTR("Really erase? ");
  if( 'y' != getc() )return;
  boot_page_erase_safe(0);
  PUTEESTR("Erased\r\n");
}

/*
  ADC0-3 are available.
 */

static INLINE void read_record(void)
{
  uint8_t i;
  uint16_t adc;

  for(i = 0; i < opt.nrchans; i++){
    page_add(adc = read_adc(i | opt.vref));
#if 0
    putstr("## Got value ");
    putd(adc);
    putnl();
#endif
  }
}

static int8_t options_parse(void)
{
  char config[sizeof(ee_config)];
  char * s;

  eeprom_read_block(config, ee_config, sizeof(ee_config));
  s = config;

  /* parse vref */
  if( 'I' == *s )opt.vref = VREF_1V1;
  else if( 'E' == *s )opt.vref = VREF_AVCC;
  else return(-1);

  /* parse nrchans */
  s++;
  if( *s < '1' || *s > '4' )return(-1);
  opt.nrchans = *s - '0';

  /* parse interval */
  s++;
  opt.nrseconds = 0;
  while( *s >= '0' && *s <= '9' ){
    opt.nrseconds *= 10;
    opt.nrseconds += (*s - '0');
    s++;
  }

  if( *s != 'L' )return(-1);
  return(0);
}
static INLINE void options_read(void)
{
  char s[sizeof(ee_config)];
  uint8_t i;
  char nl;

  PUTEESTR("Enter option string on one line: [IE][1234][0-9]+L.*$\r\n"
           "  [IE]: I = internal 1.1V reference, E = AVCC\r\n"
           "  [1234]: number of channels to log\r\n"
           "  [0-9]+: sampling interval, seconds (max 255)\r\n"
           "  L.*$: Free form log desription\r\n");
  nl = ' ' + 1;
  for(i = 0; i < sizeof(s); i++){
    if( nl >= ' ' )
      putch(nl = s[i] = getc());
    else
      s[i] = ' ';
  }
  eeprom_write_block(s, ee_config, sizeof(ee_config));
}

static inline int8_t have_data(void)
{
  return(pgm_read_word_near(0) != 0xffff);
}

int main(void)
{
  MCUCR = _BV(IVCE);
  MCUCR = _BV(IVSEL);

  usart_init();

  while( have_data() ){
    char ch;
    PUTEESTR("[D]ownload/[e]rase? ");
    ch = getc();
    if( 'D' == ch )download();
    else if( 'e' == ch )erase();
  }

  timer_init();
  sei();

  for( ; ; ){
    if( 0 == options_parse() ){
      /* wait 4 seconds to begin logging */
      for(timer_secs = 0; NO_CHAR_RECEIVED; )
        if( timer_secs >= 4 )goto begin_logging;
      (void)getc();
    }else
      PUTEESTR("Invalid options string.\r\n");
    options_read();
  }

begin_logging:
  PUTEESTR("Logging start.\r\n");
  set_sleep_mode(SLEEP_MODE_IDLE);

  for(timer_secs = 0; ; ){
    while( timer_secs < opt.nrseconds )
      sleep_mode();
    timer_secs = 0;
    read_record();
  }
}
