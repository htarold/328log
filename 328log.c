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

int8_t getc(void)
{
  while( ! (UCSR0A & _BV(RXC0)) );
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
      putstr("Out of memory\r\n");
    }
  }
  boot_page_write_safe((uint32_t)page_address);
  boot_spm_busy_wait();
  boot_rww_enable_safe();
  page_address += SPM_PAGESIZE;
  page_offset = 0;
  putstr("Synced\r\n");
}

void page_addbyte(uint8_t b)
{
  if( 0 == page_offset && page_address < BOOTSTART )
    boot_page_erase_safe(page_address);
  boot_page_fill_safe((uint32_t)page_address + page_offset, b);
  page_offset++;
  if( page_offset >= SPM_PAGESIZE )
    page_sync();
}

void page_add(uint16_t w)
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

volatile uint8_t timer_flag;
ISR(TIMER1_COMPA_vect)
{
  timer_flag = 1;
}

void timer_init(void)
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

#define VREF_AVCC (_BV(REFS0))
#define VREF_1V1  (_BV(REFS1) | _BV(REFS0))

uint16_t read_adc(uint8_t mux)
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

void inline download(void)
{
  uint8_t i, empties;
  uint32_t addr;

  /*
  uint32_t factor;
    mV = (1000*lsb/1024) * vref
    if vref is 5V, then
    mV = 250*lsb/256
    if vref is 1v1, then
    vref = (opt.cal1v1/1024) * 5
    mV = (1000*lsb * opt.cal1v1*5)/(1024*1024)
    mV = (125*lsb * opt.cal1v1*5)/(128*1024)
    mV = 0.61...(lsb*opt.cal1v1)/128
    mV = (625*lsb*opt.cal1v1/1024)/128
    mV = (625*lsb*opt.cal1v1/512)/256

  if( opt.vref == VREF_1V1 )
    factor = (opt.cal1v1 * 125 * 5)/512;
  else
    factor = 250;
   */

  putd(opt.nrseconds);
  putstr("between records.\r\n");
  if( VREF_AVCC == opt.vref )putstr("5V");
  else if( VREF_1V1 == opt.vref )putstr("1.1V");
  else putstr("[unknown]");
  putstr("reference\r\n");

  addr = 0;
  empties = 0;

  for( ; ; ){
    for(i = 0; i < 5; i++, addr++){
      if( addr >= BOOTSTART )goto out;
      u.bytes.b[i] = pgm_read_byte_near(addr);
      if( (addr % SPM_PAGESIZE) < 4 && u.bytes.b[i] == 0xff )
        if( ++empties > 1 )goto out;
    }
    if( empties > 1 )break;
    putd(u.words.w0); putstr("\r\n");
    putd(u.words.w1); putstr("\r\n");
    putd(u.words.w2); putstr("\r\n");
    putd(u.words.w3); putstr("\r\n");
  }
out:
  putstr("[EOF]\r\n");
}

void options_read(void);
void inline erase(void)
{
  putstr("Really erase? ");
  if( 'y' != getc() )return;
  boot_page_erase_safe(0);
  putstr("Erased\r\n");
  options_read();
}

/*
  ADC0-3 are available.
 */

void read_record(void)
{
  uint8_t i;
  uint16_t adc;

  for(i = 0; i < opt.nrchans; i++){
    adc = read_adc(i | VREF_1V1);
    page_add(adc);
    putstr("## Got value ");
    putd(adc);
    putstr("\r\n");
  }
}

static inline void options_init(void)
{
  eeprom_read_block(&opt, &ee, sizeof(opt));
}
void options_read(void)
{
  for( ; ; ){
    char ch;
    putstr("[I]nternal 1.1V vref\r\n"
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
inline int8_t options_ok(void)
{
  /* if( opt.cal1v1 < 300 ) */
  if( opt.nrchans < 5 )
  if( opt.vref == VREF_1V1 || opt.vref == VREF_AVCC )
    return(1);
  return(0);
}

inline int8_t have_data(void)
{
  return(pgm_read_word_near(0) != 0xffff);
}

int main(void)
{
  uint8_t seconds;
  MCUCR = _BV(IVCE);
  MCUCR = _BV(IVSEL);

  usart_init();

  options_init();

  _delay_ms(2000);                    /* How we debounce */

  putstr("Start\r\n");

  while( have_data() ){
    char ch;
    putstr("[D]ownload/[e]rase? ");
    ch = getc();
    if( 'D' == ch )download();
    else if( 'e' == ch )erase();
  }

  while( ! options_ok() )options_read();

  putstr("Logging ");
  putd(opt.nrchans);
  putstr(" field[s] per record\r\n");
  page_address = 0;
  page_offset = 0;

  /*
    Turn on LED (PB5) for a while
   */

  DDRB |= _BV(DDB5);
  PORTB |= _BV(PORTB5);
  _delay_ms(4000);
  PORTB &= ~_BV(PORTB5);
  DDRB &= ~_BV(DDB5);

  timer_init();
  sei();

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
