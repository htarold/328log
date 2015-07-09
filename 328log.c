/*
  Turn an Arduino Pro Mini into a low-rate data logger.
  GPLv3.
  Harold Tay.
 */
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

FUSES = {                             /* All are arduino defaults */
  .low = 0xFF,
  .high = 0xDA,
  .extended = 0x5,
};

static uint8_t sample_interval;
static uint8_t nr_channels;
static uint8_t admux[4];
char EEMEM ee_config[80];

#define led_init() do { DDRB |= _BV(DDB5); }while( 0 )
#define led_on() do{ PORTB |= _BV(PORTB5); }while( 0 )
#define led_off() do{ PORTB &= ~_BV(PORTB5); }while( 0 )

volatile uint8_t timer_secs; ISR(TIMER1_COMPA_vect) { timer_secs++; }

static inline void timer_init(void)
{
  TIMSK1 = _BV(OCIE1A);
  OCR1A = 15624;                      /* every 1 second */
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

static void puteestr(char * s)
{
  uint8_t i;
  for(i = 0; ; i++){
    char ch;
    ch = eeprom_read_byte((uint8_t *)s+i);
    if( ! ch )break;
    putch(ch);
  }
}
#define PUTEESTR(lit) \
{ static char str[] EEMEM = lit; puteestr(str); }
static void putnl(void) { PUTEESTR("\r\n"); }

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
  boot_page_write_safe((uint32_t)page_address);
  boot_spm_busy_wait();
  boot_rww_enable_safe();
  page_address += SPM_PAGESIZE;
  page_offset = 0;
  PUTEESTR("Flushed\r\n");
}

static void page_addword(uint16_t w)
{
  if( page_address >= BOOTSTART )
    for( ; ; ){
      sleep_mode();
      PUTEESTR("Out of memory\r\n");
    }
  boot_page_fill_safe((uint32_t)page_address + page_offset, w);
  page_offset += 2;
  if( page_offset >= SPM_PAGESIZE )
    page_sync();
}

static inline void page_add(uint16_t w)
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

static inline uint16_t read_adc(uint8_t mux)
{
  uint16_t lsb;
  ADCSRA = _BV(ADEN)
         | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
  ADMUX = mux;
  ADCSRA |= _BV(ADSC);                /* start */
  while( ADCSRA & _BV(ADSC) );
  lsb = ADC;
  ADCSRA &= ~_BV(ADEN);
  return(lsb);
}

static void inline download(void)
{
  uint8_t i, field;
  uint16_t addr;

  putnl();
  puteestr(ee_config);
  putnl();

  addr = 0;
  field = 0;

  while( addr < BOOTSTART ){
    uint32_t buffer;
    buffer = pgm_read_dword_near(addr);
    if( 0xffffffff == buffer )break;
    addr += 4;
    for(i = 0; i < 3; i++){
      putd((buffer & 0x3ff00000)>>20);
      buffer <<= 10;
      field++;
      if( field >= nr_channels ){ field = 0; putnl(); }
      else PUTEESTR(" ");
    }
  }
  PUTEESTR("\r\n[EOF]\r\n");
}

static inline void erase(void)
{
  uint16_t addr;
  PUTEESTR("Really erase? ");
  if( 'y' != getc() )return;
  for(addr = 0; addr < BOOTSTART; addr += SPM_PAGESIZE)
    boot_page_erase_safe(addr);
  PUTEESTR("Erased\r\n");
}

/*
  ADC0-3 are available.
 */

static inline void read_record(void)
{
  uint8_t i;
  uint16_t adc;

  for(i = 0; i < nr_channels; i++)
    page_add(adc = read_adc(admux[i]));
}

static int8_t options_parse(void)
{
  char config[sizeof(ee_config)];
  char * s;
  uint8_t i;

  for(i = 0; i < sizeof(ee_config); i++){
    config[i] = eeprom_read_byte((uint8_t *)ee_config + i);
    if( config[i] > 0 )putch(config[i]);
  }
  putnl();

  s = config;

  /* [vV]+[0123456789]+#.*$ */
  nr_channels = 0;
  for(i = 0; i < 4; i++){
    if( *s == 'v' )
      admux[i] = VREF_1V1;
    else if( *s == 'V' )
      admux[i] = VREF_AVCC;
    else
      break;
    admux[i] |= i;
    s++;
    nr_channels++;
  }

  for(sample_interval = 0; *s >= '0' && *s <= '9'; s++){
    sample_interval *= 10;
    sample_interval += *s - '0';
  }
  if( *s != '#' )return(-1);
  if( ! sample_interval )return(-1);
  return(0);
}

static inline void options_read(void)
{
  char config[sizeof(ee_config)];
  uint8_t i;
  char ch;

  PUTEESTR("\r\nEnter options on one line: [vV]{1,4}[0-9]+#.*$\r\n"
           "  [vV]{1,4}: v = internal 1.1V reference or V = VCC\r\n"
           "  Repeat for up to 4 channels to log\r\n"
           "  [0-9]+: sampling interval, seconds (1 to 255)\r\n"
           "  #.*$: Free form log desription\r\n");
  PUTEESTR("  e.g. \"vV3# Trial #42<enter>\" means log ADC0 (1.1V reference)\r\n"
	   "and ADC1 (VCC as reference), every 3 seconds.\r\n"
	   "Logging 1 channel every 4s, memory will last 1 day.\r\n"
	   "Logging 4 channels every 1s, memory will last 1.5 hours.\r\n");

  for(i = 0; i < sizeof(config)-1; i++){
    ch = getc();
    if( ch > '\r' )
      putch(config[i] = ch);
    else
      break;
  }
  config[i] = '\0';
  putnl();
  eeprom_write_block(config, ee_config, sizeof(ee_config));
}

static inline int8_t have_data(void)
{
  return(pgm_read_dword_near(0) != 0xffffffff);
}

int main(void)
{
  MCUCR = _BV(IVCE);
  MCUCR = _BV(IVSEL);

  usart_init();
  timer_init();
  led_init();
  sei();


  for( ; ; ){
    int8_t options_are_bad;

    led_on();
    options_are_bad = options_parse();
    if( have_data() ){                /* assume options ok */
      char ch;
      PUTEESTR("[D]ownload/[e]rase? ");
      ch = getc();
      if( 'D' == ch )download();
      else if( 'e' == ch )erase();
      continue;
    }
    
    if( ! options_are_bad ){
      PUTEESTR("Start logging in 4 seconds unless key pressed...");
      led_off();
      for(timer_secs = 0; NO_CHAR_RECEIVED; )
        if( timer_secs >= 4 )goto begin_logging;
      (void)getc();
    }else
      PUTEESTR("Invalid options string.\r\n");
    options_read();
  }

begin_logging:
  led_on();
  PUTEESTR("\r\nLogging started (remove serial cable NOW).\r\n");
  set_sleep_mode(SLEEP_MODE_IDLE);
  led_off();

  for(timer_secs = 0; ; ){
    while( timer_secs < sample_interval )
      sleep_mode();
    timer_secs = 0;
    read_record();
  }
}
