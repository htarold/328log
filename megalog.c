#include <stdint.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/boot.h>
#include <avr/fuse.h>
#include <util/delay.h>

#ifndef F_CPU
#error F_CPU not defined
#endif
#ifndef BOOTSTART                     /* byte address of bootloader */
#error BOOTSTART not defined
#endif

#define RECORD_SIZE 1                 /* ADC0, max 4 (ADC3) */

FUSES = {                             /* All are arduino defaults */
  .low = 0xFF,
  .high = 0xDA,
  .extended = 0x5,
};

#define DBG_LED_ON \
{ DDRB |= _BV(PB5); PORTB |= _BV(PB5); }

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
  do buf[i--] = "0123456789"[d%10]; while( (d /= 10) );
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

void page_addword(int16_t w)
{
  if( 0 == page_offset && page_address < BOOTSTART )
    boot_page_erase_safe(page_address);
  boot_page_fill_safe((uint32_t)page_address + page_offset, w);
  page_offset += 2;
  if( page_offset >= SPM_PAGESIZE )
    page_sync();
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
  OCR1A = 15625 * 2;                  /* every 2 seconds */
#else
  OCR1A = 3125;                       /* debug at speed */
#endif
  TCCR1A = 0;
  TCCR1B = _BV(WGM12)                 /* CTC mode 4 */
         | _BV(CS12) | _BV(CS10);     /* /1024 = 15625Hz */
}

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

void download(void)
{
  uint8_t i, empties;
  uint32_t addr;

  addr = 0;
  do{
    empties = 0;
    for(i = 0; i < RECORD_SIZE; i++){
      uint16_t lsb;
      lsb = pgm_read_word_near(addr);
      addr += 2;
      putd(lsb);
      putch(' ');
      if( 0xffff == lsb )empties++;
    }
    putstr("\r\n");
  }while( empties < RECORD_SIZE && addr < BOOTSTART );
  putstr("[EOF]\r\n");
}

void erase(void)
{
  putstr("Really erase? ");
  if( 'y' != getc() )return;
  boot_page_erase_safe(0);
  putstr("Erased\r\n");
}

/*
  ADC0-3 are available.
 */

void read_record(void)
{
  uint8_t i;
  uint16_t adc;

  for(i = 0; i < RECORD_SIZE; i++){
    adc = read_adc(i | _BV(REFS0) | _BV(REFS1));
    page_addword(adc);
    putstr("## Got value ");
    putd(adc);
    putstr("\r\n");
  }
}

int main(void)
{
  MCUCR = _BV(IVCE);
  MCUCR = _BV(IVSEL);

  usart_init();

  _delay_ms(3000);                    /* How we debounce */

  putstr("Starting\r\n");

  for( ; ; ){
    uint16_t w;
    char ch;
    w = pgm_read_word_near(0);
    putd(w);
    putstr("\r\n");
    if( 0xffff == w )break;
    putstr("[D]ownload/[e]rase? ");
    ch = getc();
    if( 'D' == ch )download();
    else if( 'e' == ch )erase();
  }

  putstr("Logging ");
  putd(RECORD_SIZE);
  putstr(" field[s] per record\r\n");
  page_address = 0;
  page_offset = 0;

  /*
    Turn on LED (PB5) for a while
   */

  DDRB |= _BV(PB5);
  PORTB |= _BV(PB5);
  _delay_ms(4000);
  PORTB &= ~_BV(PB5);
  DDRB &= ~_BV(PB5);

  timer_init();
  sei();

  set_sleep_mode(SLEEP_MODE_IDLE);

  for( ; ; ){
    sleep_mode();
    if( ! timer_flag )continue;
    timer_flag = 0;
    read_record();
  }
}
