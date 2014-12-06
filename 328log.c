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

union {
  struct {
    uint8_t b[5];
  }bytes;
  struct {
    unsigned w0:10;
    unsigned w1:10;
    unsigned w2:10;
    unsigned w3:10;
  }words;
}u;

struct options {
  uint16_t cal1v1;  /* ever going to use this? */
  uint8_t nrchans;
  uint8_t vref;
  uint8_t nrseconds;
};
struct options EEMEM ee;
struct options opt;

#define DBG_LED_ON \
{ DDRB |= _BV(DDB5); PORTB |= _BV(PORTB5); }

volatile uint8_t timer_flag; ISR(TIMER1_COMPA_vect) { timer_flag = 1; }

static void timer_init(void)
{
  TIMSK1 = _BV(OCIE1A);
#if 1
  OCR1A = 15625;                      /* every 1 second */
#else
  OCR1A = 3125;                       /* debug at speed */
#endif
  TCCR1A = 0;
  TCCR1B = _BV(WGM12)                 /* CTC mode 4 */
         | _BV(CS12) | _BV(CS10);     /* /1024 = 15625Hz */
}

int8_t getc(void)
{
  while( ! (UCSR0A & _BV(RXC0)) )
    if( timer_flag )return(timer_flag = 0);
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
  char buf[8];
  int8_t i;
  i = sizeof(buf)-1;
  buf[i--] = '\0';
  do buf[i--] = '0' + (d%10); while( (d /= 10) );
  putstr(buf + i + 1);
}

void puteestr(char * s)  /* XXX May be very slow */
{
  uint8_t i;
  char ch;
  for(i = 0; (ch = eeprom_read_byte((uint8_t *)s+i)); i++)
    putch(ch);
}
#define PUTEESTR(lit) { static char str[] EEMEM = lit; puteestr(str); }
static inline void putnl(void) { PUTEESTR("\r\n"); }

void usart_init(void)
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

void page_sync(void)
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
  PUTEESTR("Synced\r\n");
}

static void page_addbyte(uint8_t b)
{
  if( 0 == page_offset && page_address < BOOTSTART )
    boot_page_erase_safe(page_address);
  boot_page_fill_safe((uint32_t)page_address + page_offset, b);
  page_offset++;
  if( page_offset >= SPM_PAGESIZE )
    page_sync();
}

static void page_add(uint16_t w)
{
  static uint8_t ndx;
  if( 0 == ndx )
    u.words.w0 = w;
  else if( 1 == ndx )
    u.words.w1 = w;
  else if( 2 == ndx )
    u.words.w2 = w;
  else if( 3 == ndx )
    u.words.w3 = w;
  ndx++;
  if( 4 == ndx ){
    uint8_t i;
    ndx = 0;
    for(i = 0; i < 5; i++)
      page_addbyte(u.bytes.b[i]);
  }
}


#define VREF_AVCC (_BV(REFS0))
#define VREF_1V1  (_BV(REFS1) | _BV(REFS0))

static INLINE uint16_t read_adc(uint8_t mux)
{
  uint16_t lsb;
  PRR &= ~_BV(PRADC);
  ADCSRB = 0;
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

  putd(opt.nrseconds);
  PUTEESTR(" between records.\r\n ");
  if( VREF_AVCC == opt.vref ){ PUTEESTR("5V");
  }else if( VREF_1V1 == opt.vref ){ PUTEESTR("1.1V");
  }else PUTEESTR("[unknown]");
  PUTEESTR(" reference\r\n");

  for(addr = 0; addr < SPM_PAGESIZE; addr++){
    char ch;
    ch = pgm_read_byte_near(addr);
    if( ! ch )break;
    putch(ch);
  }
  putnl();

  addr = SPM_PAGESIZE;
  empties = 0;
  field = 0;

  for( ; ; ){
    for(i = 0; i < 5; i++, addr++){
      if( addr >= BOOTSTART )goto out;
      u.bytes.b[i] = pgm_read_byte_near(addr);
      if( (addr % SPM_PAGESIZE) < 4 && u.bytes.b[i] == 0xff )
        if( ++empties > 1 )goto out;
    }
    if( empties > 1 )break;
    putd(u.words.w0);
    if( field++ >= opt.nrchans ){ field = 0; putnl(); }
    else putspace();
    putd(u.words.w1);
    if( field++ >= opt.nrchans ){ field = 0; putnl(); }
    else putspace();
    putd(u.words.w2);
    if( field++ >= opt.nrchans ){ field = 0; putnl(); }
    else putspace();
    putd(u.words.w3);
    if( field++ >= opt.nrchans ){ field = 0; putnl(); }
    else putspace();
  }
out:
  PUTEESTR("\r\n[EOF]\r\n");
}

static void options_read(void);
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
    adc = read_adc(i | VREF_1V1);
    page_add(adc);
    putstr("## Got value ");
    putd(adc);
    putnl();
  }
}

static INLINE void options_init(void)
{
  eeprom_read_block(&opt, &ee, sizeof(opt));
}

static INLINE void options_read(void)
{
  for( ; ; ){
    char ch;
    PUTEESTR("[I]nternal 1.1V vref\r\n"
           "[5]V vref\r\n"
  	 "[1234] # of channels\r\n"
  	 "[abc..] 1/2/3.. secs interval\r\n"
  	 "[Q]uit\r\n");
    ch = getc();
    if( 'I' == ch )opt.vref = VREF_1V1;
    else if( '5' == ch )opt.vref = VREF_AVCC;
    else if( ch < '5' && ch > '0' )opt.nrchans = ch - '0';
    else if( ch <= 'z' && ch >= 'a' )opt.nrseconds = 1 + (ch - 'a');
    /*else if( ch == 'X' )opt.cal1v1 = read_adc(0xe | VREF_AVCC);*/
    else if( 'Q' == ch )break;
  }
  eeprom_write_block(&opt, &ee, sizeof(opt));
}

static INLINE int8_t options_ok(void)
{
  /* if( opt.cal1v1 < 300 ) */
  if( opt.nrchans < 5 )
  if( opt.vref == VREF_1V1 || opt.vref == VREF_AVCC )
    return(1);
  return(0);
}

static INLINE int8_t have_data(void)
{
  return(pgm_read_word_near(SPM_PAGESIZE) != 0xffff);
}

int main(void)
{
  uint8_t seconds;
  MCUCR = _BV(IVCE);
  MCUCR = _BV(IVSEL);

  usart_init();

  options_init();

  _delay_ms(2000);                    /* How we debounce */

  PUTEESTR("Start\r\n");

  while( have_data() ){
    char ch;
    PUTEESTR("[D]ownload/[e]rase? ");
    ch = getc();
    if( 'D' == ch )download();
    else if( 'e' == ch )erase();
  }

  timer_init();
  sei();

  if( options_ok() ){
    /* before beginning to log, give usera chance to use menu */
    if( getc() )goto menu;  /* times out in 1 second */
    if( getc() )goto menu;
    if( getc() )goto menu;
    if( getc() )goto menu;
  }else{
menu:
    for( ; ; )
      options_read();
  }

#if 0
  PUTEESTR("Logging ");
  putd(opt.nrchans);
  PUTEESTR(" field[s] per record\r\n");
  page_address = SPM_PAGESIZE;
  page_offset = 0;

  /*
    Turn on LED (PB5) for a while
   */

  DDRB |= _BV(DDB5);
  PORTB |= _BV(PORTB5);
  _delay_ms(4000);
  PORTB &= ~_BV(PORTB5);
  DDRB &= ~_BV(DDB5);
#endif

  set_sleep_mode(SLEEP_MODE_IDLE);

  seconds = 0;
  for( ; ; ){
    while( ! timer_flag )
      sleep_mode();
    timer_flag = 0;
    seconds++;
    if( seconds < opt.nrseconds )continue;
    seconds = 0;
    read_record();
  }
}
