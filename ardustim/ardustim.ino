/* vim: set syntax=c expandtab sw=2 softtabstop=2 autoindent smartindent smarttab : */
/*
 * Arbritrary wheel pattern generator
 *
 * copyright 2014 David J. Andruczyk
 * 
 * Ardu-Stim software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ArduStim software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with any FreeEMS software.  If not, see http://www.gnu.org/licenses/
 *
 */

#include "enums.h"
#include "wheel_defs.h"
#include <avr/pgmspace.h>
#include <SerialUI.h>
#include "serialmenu.h"

#define MAX_SWEEP_STEPS 12


/* Sensistive stuff used in ISR's */
volatile byte selected_wheel = TWENTY_FOUR_MINUS_TWO_WITH_SECOND_TRIGGER;
/* Setting rpm to any value over 0 will enabled sweeping by default */
volatile unsigned long wanted_rpm = 6000; 
/* Stuff for handling prescaler changes (small tooth wheels are low RPM) */
volatile byte reset_prescaler = 0;
volatile byte sweep_direction = ASCENDING;
volatile byte total_sweep_stages = 0;
volatile uint16_t sweep_step_counter = 0;
volatile int8_t sweep_stage = 0;
volatile byte sweep_reset_prescaler = 1; /* Force sweep to reset prescaler value */
volatile byte prescaler_bits = 0;
volatile byte mode = FIXED_RPM;
volatile byte last_prescale = 0;
volatile byte sweep_lock = 0;
volatile byte last_prescaler = 0;
volatile uint16_t new_OCR1A = 5000; /* sane default */
volatile uint16_t edge_counter = 0;
volatile byte state = 0;

/* Less sensitive globals */
uint16_t sweep_low_rpm = 0;
uint16_t sweep_high_rpm = 0;
uint16_t sweep_rate = 0;

SUI::SerialUI mySUI = SUI::SerialUI(greeting);                                  

/* Where to store our sweep pattern sets */
typedef struct _pattern_set pattern_set;
struct _pattern_set {
  uint16_t beginning_ocr;
  uint16_t ending_ocr;
  uint8_t prescaler_bits;
  uint16_t oc_step;
  uint16_t steps;
}SweepSteps[MAX_SWEEP_STEPS];

/* Tie things wheel related into one nicer structure ... */
typedef struct _wheels wheels;
struct _wheels {
  prog_char *decoder_name;
  prog_uchar *edge_states_ptr;
  const float rpm_scaler;
  const uint16_t wheel_max_edges;
}Wheels[MAX_WHEELS] = {
  /* Pointer to friendly name string, pointer to edge array, RPM Scaler, Number of edges in the array */
  { dizzy_four_cylinder_friendly_name, dizzy_four_cylinder, 0.03333, 4 },
  { dizzy_six_cylinder_friendly_name, dizzy_six_cylinder, 0.05, 6 },
  { dizzy_eight_cylinder_friendly_name, dizzy_eight_cylinder, 0.06667, 8 },
  { sixty_minus_two_friendly_name, sixty_minus_two, 1.0, 120 },
  { sixty_minus_two_with_cam_friendly_name, sixty_minus_two_with_cam, 1.0, 240 },
  { thirty_six_minus_one_friendly_name, thirty_six_minus_one, 0.6, 72 },
  { four_minus_one_with_cam_friendly_name, four_minus_one_with_cam, 0.06667, 16 },
  { eight_minus_one_friendly_name, eight_minus_one, 0.13333, 16 },
  { six_minus_one_with_cam_friendly_name, six_minus_one_with_cam, 0.15, 36 },
  { twelve_minus_one_with_cam_friendly_name, twelve_minus_one_with_cam, 0.6, 144 },
  { fourty_minus_one_friendly_name, fourty_minus_one, 0.66667, 80 },
  { dizzy_four_trigger_return_friendly_name, dizzy_four_trigger_return, 0.15, 9 },
  { oddfire_vr_friendly_name, oddfire_vr, 0.2, 24 },
  { optispark_lt1_friendly_name, optispark_lt1, 3.0, 720 },
  { twelve_minus_three_friendly_name, twelve_minus_three, 0.4, 48 },
  { thirty_six_minus_two_two_two_friendly_name, thirty_six_minus_two_two_two, 0.6, 72 },
  { thirty_six_minus_two_two_two_with_cam_friendly_name, thirty_six_minus_two_two_two_with_cam, 0.6, 144 },
  { fourty_two_hundred_wheel_friendly_name, fourty_two_hundred_wheel, 0.6, 72 },
  { thirty_six_minus_one_with_cam_fe3_friendly_name, thirty_six_minus_one_with_cam_fe3, 0.6, 144 },
  { six_g_seventy_two_with_cam_friendly_name, six_g_seventy_two_with_cam, 0.6, 144 },
  { buell_oddfire_cam_friendly_name, buell_oddfire_cam, 0.33333, 80 },
  { gm_ls1_crank_and_cam_friendly_name, gm_ls1_crank_and_cam, 6.0, 720 },
  { lotus_thirty_six_minus_one_one_one_one_friendly_name, lotus_thirty_six_minus_one_one_one_one, 0.6, 72 },
  { honda_rc51_with_cam_friendly_name, honda_rc51_with_cam, 0.2, 48 },
  { thirty_six_minus_one_with_second_trigger_friendly_name, thirty_six_minus_one_with_second_trigger, 0.6, 144 },
  { thirty_six_minus_one_plus_one_with_cam_ngc4_friendly_name, thirty_six_minus_one_plus_one_with_cam_ngc4, 3.0, 720 },
  { weber_iaw_with_cam_friendly_name, weber_iaw_with_cam, 1.2, 144 },
  { fiat_one_point_eight_sixteen_valve_with_cam_friendly_name, fiat_one_point_eight_sixteen_valve_with_cam, 3.0, 720 },
  { three_sixty_nissan_cas_friendly_name, three_sixty_nissan_cas, 3.0, 720 },
  { twenty_four_minus_two_with_second_trigger_friendly_name, twenty_four_minus_two_with_second_trigger, 0.3, 72 },
};

/* Initialization */
void setup() {
  serial_setup();

  cli(); // stop interrupts

  /* Configuring TIMER1 (pattern generator) */
  // Set timer1 to generate pulses
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 0;

  // Set compare register to sane default
  OCR1A = 1000;  /* 8000 RPM (60-2) */

  // Turn on CTC mode
  TCCR1B |= (1 << WGM12); // Normal mode (not PWM)
  // Set prescaler to 1
  TCCR1B |= (1 << CS10); /* Prescaler of 1 */
  // Enable output compare interrupt
  TIMSK1 |= (1 << OCIE1A);

  // Set timer2 to run sweeper routine
  TCCR2A = 0;
  TCCR2B = 0;
  TCNT2 = 0;

  // Set compare register to sane default
  OCR2A = 249;  /* With prescale of x64 gives 1ms tick */

  // Turn on CTC mode
  TCCR2A |= (1 << WGM21); // Normal mode (not PWM)
  // Set prescaler to x64
  TCCR2B |= (1 << CS22); /* Prescaler of 64 */
  // Enable output compare interrupt
  TIMSK2 |= (1 << OCIE2A);


  pinMode(7, OUTPUT);
  pinMode(8, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT); /* Knock signal for seank, ony on LS1 pattern */

  sei(); // Enable interrupts
} // End setup


ISR(TIMER2_COMPA_vect) {
  PORTD = (1 << 7);
  if ( mode != LINEAR_SWEPT_RPM)
  {
    PORTD = (0 << 7);
    return;
  }
  if (sweep_lock)  // semaphore to protect around changes/critical sections
  {  
    PORTD = (0 << 7);
    return;
  }
  sweep_lock = 1;
  if (sweep_reset_prescaler == 1)
  {
    reset_prescaler = 1;
    sweep_reset_prescaler = 0;
    prescaler_bits = SweepSteps[sweep_stage].prescaler_bits;
    last_prescaler = prescaler_bits;  
    new_OCR1A = SweepSteps[sweep_stage].beginning_ocr;  
  }
  /* Sweep code */
  /*
  struct _pattern_set {
    uint16_t beginning_ocr;
    uint16_t ending_ocr;
    uint8_t prescaler_bits;
    uint16_t oc_step;
    uint16_t steps;
  }SweepSteps[MAX_SWEEP_STEPS];
  */
  if (sweep_direction == ASCENDING)
  {
//    PORTD |= 1 << 7;  /* Debugginga, ascending */
    if (sweep_step_counter < SweepSteps[sweep_stage].steps)
    {
      new_OCR1A -= SweepSteps[sweep_stage].oc_step;
      sweep_step_counter++;
    }
    else /* END of the stage, find out where we are */
    {
      sweep_stage++;
      if (sweep_stage < total_sweep_stages)
      {
        new_OCR1A = SweepSteps[sweep_stage].beginning_ocr;
        sweep_step_counter = 0; /* we got the "0'th value in the previous line */
        if (SweepSteps[sweep_stage].prescaler_bits != last_prescaler)
        {
          reset_prescaler = 1;
          prescaler_bits = SweepSteps[sweep_stage].prescaler_bits;
          last_prescaler = prescaler_bits;
        }
      }
      else /* END of line, time to reverse direction */
      {
        sweep_stage--; /*Bring back within limits */
        sweep_direction = DESCENDING;
        new_OCR1A = SweepSteps[sweep_stage].ending_ocr;
        sweep_step_counter = SweepSteps[sweep_stage].steps; /* we got the last value in hte previous line */
      }
    }
  }
  else /* Descending */
  {
//      PORTD &= ~(1<<7);  /*Descending  turn pin off */
    if (sweep_step_counter > 0)
    {
      new_OCR1A += SweepSteps[sweep_stage].oc_step;
      sweep_step_counter--;
    }
    else /* End of stage */
    {
      sweep_stage--;
      if (sweep_stage >= 0)
      {
        new_OCR1A = SweepSteps[sweep_stage].ending_ocr;
        sweep_step_counter = SweepSteps[sweep_stage].steps;
        if (SweepSteps[sweep_stage].prescaler_bits != last_prescaler)
        {
          reset_prescaler = 1;
          prescaler_bits = SweepSteps[sweep_stage].prescaler_bits;
          last_prescaler = prescaler_bits;
        }
      }
      else /*End of the line */
      {
        sweep_stage++; /*Bring back within limits */
        sweep_direction = ASCENDING;
        new_OCR1A = SweepSteps[sweep_stage].beginning_ocr;
        sweep_step_counter = 0; /* we got the last value in hte previous line */
      }
    }
  }
  sweep_lock = 0;
  PORTD = (0 << 7);
}

/* Pumps the pattern out of flash to the port 
 * The rate at which this runs is dependent on what OCR1A is set to
 * the sweeper in timer2 alters this on the fly to alow changing of RPM
 * in a very nice way
 */
ISR(TIMER1_COMPA_vect) {
  /* This is VERY simple, just walk the array and wrap when we hit the limit */
  edge_counter++;
  if (edge_counter >= Wheels[selected_wheel].wheel_max_edges) {
    edge_counter = 0;
  }
  /* The tables are in flash so we need pgm_read_byte() */
  PORTB = pgm_read_byte(&Wheels[selected_wheel].edge_states_ptr[edge_counter]);   /* Write it to the port */

  /* Reset Prescaler only if flag is set */
  if (reset_prescaler)
  {
    TCCR1B &= ~((1 << CS10) | (1 << CS11) | (1 << CS12)); /* Clear CS10, CS11 and CS12 */
    TCCR1B |= prescaler_bits;
    reset_prescaler = 0;
  }
  /* Reset next compare value for RPM changes */
  OCR1A = new_OCR1A;  /* Apply new "RPM" from Timer2 ISR, i.e. speed up/down the virtual "wheel" */
}

void loop() {
  /* Just handle the Serial UI, everything else is in 
   * interrupt handlers or callbacks from SerialUI.
   */

  if (mySUI.checkForUserOnce())
  {
    // Someone connected!
    mySUI.enter();
    while (mySUI.userPresent()) 
    {
      mySUI.handleRequests();
    }
  }
}


/* 
 * Setup the initial menu structure and callbacks
 */
void serial_setup()
{
  mySUI.begin(9600);
  mySUI.setTimeout(20000);
  mySUI.setMaxIdleMs(30000);
  SUI::Menu *mainMenu = mySUI.topLevelMenu();
  /* Simple all on one menu... */
  mainMenu->setName(top_menu_title);
  mainMenu->addCommand(info_key,show_info,info_help);
  mainMenu->addCommand(next_key,select_next_wheel,next_help);
  mainMenu->addCommand(previous_key,select_previous_wheel,previous_help);
  mainMenu->addCommand(list_key,list_wheels,list_help);
  mainMenu->addCommand(choose_key,select_wheel,choose_help);
  mainMenu->addCommand(rpm_key,set_rpm,rpm_help);
  mainMenu->addCommand(sweep_key,sweep_rpm,sweep_help);
}

/* Helper function to spit out amount of ram remainig */
int freeRam () {                                                                
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}


/* SerialUI Callbacks */
void show_info()
{
  mySUI.println_P(info_title);
  mySUI.print_P(free_ram);
  mySUI.print(freeRam());
  mySUI.println_P(bytes);
  mySUI.print_P(current_pattern);
  mySUI.print(selected_wheel+1);
  mySUI.print_P(colon_space);
  mySUI.println_P(Wheels[selected_wheel].decoder_name);
  if (mode == FIXED_RPM) {
    mySUI.print_P(fixed_current_rpm);
    mySUI.println(wanted_rpm);
  } 
  if (mode == LINEAR_SWEPT_RPM) {
    mySUI.print_P(swept_rpm_from);
    mySUI.print(sweep_low_rpm);
    mySUI.print_P(space_to_colon_space);
    mySUI.print(sweep_high_rpm);
    mySUI.print_P(space_at_colon_space);
    mySUI.print(sweep_rate);
    mySUI.println_P(rpm_per_second);
  }
}

void select_wheel()
{
  mySUI.showEnterNumericDataPrompt();
  byte newWheel = mySUI.parseInt();
  if ((newWheel < 1) || (newWheel > (MAX_WHEELS+1))) {
    mySUI.returnError("Wheel ID out of range");
  }
  selected_wheel = newWheel - 1; /* use 1-MAX_WHEELS range */
  reset_new_OCR1A(wanted_rpm);

  mySUI.println_P(new_wheel_chosen);
  mySUI.print(selected_wheel+1);
  mySUI.print_P(colon_space);
  mySUI.print_P(Wheels[selected_wheel].decoder_name);
  mySUI.print_P(space_at_space);
  mySUI.print(wanted_rpm);
  mySUI.println_P(space_RPM);
  mySUI.returnOK();
  edge_counter = 0;
}

void set_rpm()
{
  mySUI.showEnterNumericDataPrompt();
  uint32_t newRPM = mySUI.parseULong();
  if (newRPM < 100)  {
    mySUI.returnError("Invalid RPM, RPM too low");
    return;
  }
  mode = FIXED_RPM;
  reset_new_OCR1A(newRPM);
  wanted_rpm = newRPM;

  mySUI.print_P(new_rpm_chosen);
  mySUI.println(wanted_rpm);
  mySUI.returnOK();
}

void list_wheels()
{
  byte i = 0;
  for (i=0;i<MAX_WHEELS;i++)
  {
    mySUI.print(i+1);
    mySUI.print_P(colon_space);
    mySUI.println_P((Wheels[i].decoder_name));
  }
  mySUI.returnOK();
}

void select_next_wheel()
{
  if (selected_wheel == (MAX_WHEELS-1))
    selected_wheel = 0;
  else 
    selected_wheel++;
  edge_counter = 0;
  reset_new_OCR1A(wanted_rpm);
  
  mySUI.println_P(new_wheel_chosen);
  mySUI.print(selected_wheel+1);
  mySUI.print_P(colon_space);
  mySUI.print_P(Wheels[selected_wheel].decoder_name);
  mySUI.print_P(space_at_space);
  mySUI.print(wanted_rpm);
  mySUI.println_P(space_RPM);
  mySUI.returnOK();
}

void select_previous_wheel()
{
  if (selected_wheel == 0)
    selected_wheel = MAX_WHEELS-1;
  else 
    selected_wheel--;
  edge_counter = 0;
  reset_new_OCR1A(wanted_rpm);
  
  mySUI.println_P(new_wheel_chosen);
  mySUI.print(selected_wheel+1);
  mySUI.print_P(colon_space);
  mySUI.print_P(Wheels[selected_wheel].decoder_name);
  mySUI.print_P(space_at_space);
  mySUI.print(wanted_rpm);
  mySUI.println_P(space_RPM);
  mySUI.returnOK();
}

void reset_new_OCR1A(uint16_t new_rpm)
{
  long tmpl = 0;
  tmpl = (long)(8000000.0/(Wheels[selected_wheel].rpm_scaler * (float)new_rpm));
  prescaler_bits = check_and_adjust_tcnt_limits(&tmpl, &tmpl);
  new_OCR1A = (uint16_t)tmpl; 
  reset_prescaler = 1; 
}


void sweep_rpm()
{
  byte count = 0;
  uint16_t tmp_min = 0;
  uint16_t tmp_max = 0;
  uint16_t tmp_rpm_per_sec = 0;
  uint16_t end_tcnt = 0;
  long low_tcnt = 0;
  uint16_t low_rpm = 0;
  long high_tcnt = 0;
  uint16_t high_rpm = 0;
  int i = 0;

  char sweep_buffer[20];
  mySUI.showEnterDataPrompt();
  count = mySUI.readBytesToEOL(sweep_buffer,20);
  mySUI.print(F("Read: "));
  mySUI.print(count);
  mySUI.println(F(" characters from the user...")); 
  count = sscanf(sweep_buffer,"%i,%i,%i",&tmp_min,&tmp_max,&tmp_rpm_per_sec);
  mySUI.print(F("Number of successfull matches (should be 3): "));
  mySUI.println(count);
  mySUI.print(F("min: "));
  mySUI.println(tmp_min);
  mySUI.print(F("max: "));
  mySUI.println(tmp_max);
  mySUI.print(F("RPM/sec: "));
  mySUI.println(tmp_rpm_per_sec);
  if ((count == 3) && 
    (tmp_min >= 50) &&
    (tmp_max < 51200) &&
    (tmp_rpm_per_sec > 0) &&
    (tmp_rpm_per_sec < 51200) &&
    (tmp_min < tmp_max))
  {
    sweep_low_rpm = tmp_min;
    sweep_high_rpm = tmp_max;
    sweep_rate = tmp_rpm_per_sec;
    //struct pattern_set {
    //  uint16_t beginning_ocr
    //  bool reset_prescale;
    //  byte prescaler_bits;
    //  uint16_t oc_step;
    //  uint16_t steps;
    //}SweepSteps[max_sweep_steps];
    sweep_lock = 1;
    low_tcnt = (long)(8000000.0/(((float)tmp_min)*Wheels[selected_wheel].rpm_scaler));
    high_tcnt = low_tcnt >> 1; /* divide by two */
    low_rpm = tmp_min;
    end_tcnt = 8000000/(tmp_max*Wheels[selected_wheel].rpm_scaler);

    while((i < MAX_SWEEP_STEPS) && (high_rpm < tmp_max))
    {
      SweepSteps[i].prescaler_bits = check_and_adjust_tcnt_limits(&low_tcnt,&high_tcnt);
      if (high_tcnt < end_tcnt) /* Prevent overshoot */
        high_tcnt = end_tcnt;
      SweepSteps[i].oc_step = (((1.0/low_rpm)*high_tcnt)*(tmp_rpm_per_sec/1000.0));
      if (SweepSteps[i].oc_step == 0)
        SweepSteps[i].oc_step = 1;
      SweepSteps[i].steps = (low_tcnt-high_tcnt)/SweepSteps[i].oc_step;
      if (SweepSteps[i].prescaler_bits == 4) {
        SweepSteps[i].oc_step /= 256;  /* Divide by 256 */
        SweepSteps[i].beginning_ocr = low_tcnt/256;  /* Divide by 256 */
        SweepSteps[i].ending_ocr = high_tcnt/256;  /* Divide by 256 */
      } else if (SweepSteps[i].prescaler_bits == 3) {
        SweepSteps[i].oc_step /= 64;  /* Divide by 64 */
        SweepSteps[i].beginning_ocr = low_tcnt/64;  /* Divide by 64 */
        SweepSteps[i].ending_ocr = high_tcnt/64;  /* Divide by 64 */
      } else if (SweepSteps[i].prescaler_bits == 2) {
        SweepSteps[i].oc_step /= 8;  /* Divide by 8 */
        SweepSteps[i].beginning_ocr = low_tcnt/8;  /* Divide by 8 */
        SweepSteps[i].ending_ocr = high_tcnt/8;  /* Divide by 8 */
      } else {
        SweepSteps[i].beginning_ocr = low_tcnt;  /* Divide by 1 */
        SweepSteps[i].ending_ocr = high_tcnt;  /* Divide by 1 */
      }
      mySUI.print(F("sweep step: "));
      mySUI.println(i);
      mySUI.print(F("Beginning tcnt: "));
      mySUI.println(low_tcnt);
      mySUI.print(F("ending tcnt: "));
      mySUI.println(high_tcnt);
      mySUI.print(F("Prescaled beginning tcnt: "));
      mySUI.println(SweepSteps[i].beginning_ocr);
      mySUI.print(F("Prescaled ending tcnt: "));
      mySUI.println(SweepSteps[i].ending_ocr);
      mySUI.print(F("prescaler: "));
      mySUI.println(SweepSteps[i].prescaler_bits);
      mySUI.print(F("steps: "));
      mySUI.println(SweepSteps[i].steps);
      mySUI.print(F("OC_Step: "));
      mySUI.println(SweepSteps[i].oc_step);
      mySUI.print(F("End of step: "));
      mySUI.print(i);
      mySUI.print(F(" High RPM at end: "));
      high_rpm = (8000000/(Wheels[selected_wheel].rpm_scaler*high_tcnt));
      mySUI.println(high_rpm);
      high_tcnt >>= 1; //  SweepSteps[i].oc_step;eset for next round.

      low_tcnt = 8000000/((high_rpm + (tmp_rpm_per_sec/1000))*Wheels[selected_wheel].rpm_scaler);
      low_rpm =  (uint16_t)((float)(8000000.0/low_tcnt)/Wheels[selected_wheel].rpm_scaler);
      mySUI.print(F("Low RPM for next step: "));
      mySUI.println(low_rpm);
      i++;
    }
    total_sweep_stages = i;
    mySUI.print(F("Total sweep stages: "));
    mySUI.println(total_sweep_stages);
      i++;
  }
  else {
    mySUI.returnError("Range error !(50-50000)!");
  } 
  mySUI.returnOK();
  /* Reset params for Timer2 ISR */
  sweep_stage = 0;
  sweep_step_counter = 0;
  sweep_direction = ASCENDING;
  sweep_reset_prescaler = 1;
  mode = LINEAR_SWEPT_RPM;
  sweep_lock = 0;
}

int check_and_adjust_tcnt_limits(long *low_tcnt, long *high_tcnt) 
{

  if ((*low_tcnt >= 16777216) && (*high_tcnt >= 16777216))
  {
    return PRESCALE_256; /* Very low RPM condition wiht low edge pattern */
  }
  else if ((*low_tcnt >= 16777216) && (*high_tcnt >= 524288) && (*high_tcnt < 16777216))
  {
    *high_tcnt = 1677215;
    return PRESCALE_256;
  }
  else if ((*low_tcnt >= 524288) && (*low_tcnt < 16777216) && (*high_tcnt >= 1677216))
  {
    *low_tcnt = 1677215;
    return PRESCALE_256;
  }
  else if ((*low_tcnt >= 524288) && (*low_tcnt < 16777216) && (*high_tcnt >= 524288) && (*high_tcnt < 16777216))
  {
    return PRESCALE_64; 
  }
  else if ((*low_tcnt >= 524288) && (*low_tcnt < 16777216) && (*high_tcnt >= 65536) && (*high_tcnt < 524288))
  {
    *high_tcnt = 524287;
    return PRESCALE_64; 
  }
  else if ((*low_tcnt >= 65536) && (*low_tcnt < 524288) && (*high_tcnt >= 524288) && (*high_tcnt < 1677216))
  {
    *low_tcnt = 524287;
    return PRESCALE_64; 
  }
  else if ((*low_tcnt >= 65536) && (*low_tcnt < 524288) && (*high_tcnt >= 65536) && (*high_tcnt < 524288))
  {
    return PRESCALE_8; 
  }
  else if ((*low_tcnt >= 65536) && (*low_tcnt < 524288) && (*high_tcnt < 65536))
  {
    *high_tcnt = 65535;
    return PRESCALE_8; 
  }
  else if ((*low_tcnt < 65536) && (*high_tcnt >= 65536) && (*high_tcnt < 524288))
  {
    *low_tcnt = 65535;
    return PRESCALE_8; 
  }
  else
    return PRESCALE_1;
  return PRESCALE_1;
}

/* In the passed low/high params, one of them will cause a prescaler overflow
 * so we need to determine a new limit that stays inside the prescaler limits
 */

