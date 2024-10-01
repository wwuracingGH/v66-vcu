//nicole swierstra - vcu code
//as of right now this is just a prototype there's a lot of debugging I'm gonna need to do
//and a lot of comments i'm going to need to remove when I have the documentation working lol

#include <stdint.h>
#ifndef STM32F042x6
#define STM32F042x6
#endif
#include "vendor/CMSIS/Device/ST/STM32F0/Include/stm32f0xx.h"
#include "canDefinitions.h"
#include "vendor/qfplib/qfplib.h"

#define ROLLING_ADC_FR_POW 5
#define ROLLING_ADC_FRAMES (1 << ROLLING_ADC_FR_POW) 
#define ROLLING_ADC_VALS  (ROLLING_ADC_FRAMES * 4)

#define MIN_TORQUE_REQ      0 //do not change this. car not legally allowed to go backwards.
#define MAX_TORQUE_REQ      1000

#define BRAKES_THREASHOLD   450 //change this in the future

#define CONSTINV(n)             (1.0f / (float)(n)) //TODO: change all of the devisors to precomputed const values
#define REMAP0_1(n, min, max)   ((float)(n - min) * CONSTINV(max - min))
#define REMAPm_M(n, min, max)   ((n) * (max - min) + (min))
#define FABS(x)                 ((x) > 0.0f ? (x) : -(x))

const uint32_t APPS1_MIN    = 1193;  
const uint32_t APPS1_MAX    = 1910;
const uint32_t APPS2_MIN    = 1810;
const uint32_t APPS2_MAX    = 2511;
const uint32_t FBPS_MIN     = 0;
const uint32_t FBPS_MAX     = 4092;
const uint32_t RBPS_MIN     = 0;
const uint32_t RBPS_MAX     = 4092;

//these values enable apps min and max to both be slightly inside pedal travel to produce a sort of "deadzone" effect.
const float SENSOR_MIN = -0.20f;
const float SENSOR_MAX =  1.25f;

const uint16_t controlReset = 5,  //how many milliseconds between control loop
               inputReset = 50,   //how many milliseconds between input parses
               recieveReset = 20, //how many milliseconds between can processes
               diagReset = 100;   //how many milliseconds between diagnostic can sends
               faultClearReset = 1000; //how many milliseconds between fault clear can messages

struct __attribute__((packed)) {
    volatile uint16_t APPS2;
    volatile uint16_t RBPS;
    volatile uint16_t FBPS;
    volatile uint16_t APPS1;
} ADC_Vars;

uint16_t ADC_RollingValues[ROLLING_ADC_VALS];

struct {
    volatile uint16_t ready_to_drive;
    volatile uint16_t torque_req;
    uint32_t buzzerTimer;
    uint16_t controlTimer, inputTimer, recieveTimer, diagTimer, faultClearTimer;
    uint8_t controlQue, inputQue, recieveQue, diagQue, faultClearQueue;
    uint16_t lastAPPSFault;
    struct{
        uint16_t apps1, apps2, torque, fault;
    } APPSCalib;
    union {
        MC_HighSpeed hs;
        uint64_t bits;
    } hsmessage;
    union {
        DL_WheelSpeed ws;
        uint64_t bits;
    } wheelspeed;
    union {
        DL_CarAcceleration ca;
        uint32_t bits;
    } acceleration;
    union {
        MC_FaultCodes fc;
        uint64_t bits;
    } faults;
    union {
        MC_InternalStates st;
        uint64_t bits;
    } MCstates;
} car_state;

typedef struct _canmsg{
    volatile uint32_t id;
    volatile uint32_t len;
    volatile uint64_t data;
} CAN_msg;

enum Pin_Mode {
    MODE_INPUT   = 0b00,
    MODE_OUTPUT  = 0b01,
    MODE_ALTFUNC = 0b10,
    MODE_ANALOG  = 0b11
};

void clock_init();
void ADC_DMA_Init(uint32_t *dest, uint32_t size);
void GPIO_Init();
void CAN_Init();

void default_handler();
void Control();
void Fault_Clear();
void MC_Init();
void Input();
void send_Diagnostics();

void APPS_RollingSmooth();
int  APPS_calc(uint16_t*, uint16_t);
void send_CAN(uint16_t, uint8_t, uint8_t*);
void process_CAN(CAN_msg);
void recieve_CAN();
void RTD_start();

uint32_t clz(uint32_t i){
    uint32_t j = 0, n = i;
    while((n = n >> 1)) j++;
    return j;
}

//shit function lol
uint32_t __aeabi_uidivmod(uint32_t u, uint32_t v){
    uint32_t div = u;
    while(div > v) div -= v;
    return div;
}

uint32_t __aeabi_uidiv(uint32_t u, uint32_t v) {
    uint32_t q = 0, k = clz(u) - clz(v);
    v = v << k;
    k = 1 << k;
    do {
        if(v >= u) continue;
        u -= v;
        q += k;
    }
    while(v = v >> 1, (k = k >> 1));
    return q;
}

//what gets sent to the motor controller
MC_Command canmsg = {0, 0, 1, 0, 0, 0, 0, 0};
MC_ParameterCommand faultClearMsg = {20, 1, 0, 0, 0};
MC_ParameterCommand shutup = {148, 1, 0, 0b0001110011100111, 0xFFFF};
//MC_ParameterCommand fastMsg = {227, 1, 0, 0xFFFE, 0}; //turn on high speed message
MC_ParameterCommand torqueLimitMsg = {129, 1, 0, MAX_TORQUE_REQ, 0};
MC_ParameterCommand cmdTimeoutMsg = {146, 1, 0, 1, 0};

int main(){
    //setup
    clock_init();

    for(int i = 0; i < ROLLING_ADC_VALS; i++){
        ADC_RollingValues[i] = 0;
    }

    car_state.ready_to_drive = 0;
    car_state.controlTimer = 0;
    car_state.inputTimer = 0;
    car_state.recieveTimer = 0;  
    car_state.buzzerTimer = 0;
    car_state.diagTimer = 0;
    car_state.faultClearTimer = 0;
    
    GPIO_Init(); //must be called first

    ADC_DMA_Init((uint32_t *)ADC_RollingValues, ROLLING_ADC_VALS);
    CAN_Init();

    SysTick_Config(48000); // 48MHZ / 48000 = 1 tick every ms
    __enable_irq(); //enable interrupts
   
    MC_Init();
    //non rt program bits
    for(;;){
        if(car_state.controlQue)      Control();
        if(car_state.inputQue)        Input();
        if(car_state.recieveQue)      recieve_CAN();
        if(car_state.diagQue)         send_Diagnostics();
        if(car_state.faultClearQueue) Fault_Clear();
    }
}

//runs every 1 ms
void systick_handler()
{   
    if(car_state.controlTimer == 0){
        car_state.controlQue = 1;
        car_state.controlTimer = controlReset;
    }
    if(car_state.faultClearTimer == 0){
        car_state.faultClearQueue = 1;
        car_state.faultClearTimer = faultClearReset;
    }
    if(car_state.inputTimer == 0){
        car_state.inputQue = 1;
        car_state.inputTimer = inputReset;
    }
    if(car_state.recieveTimer == 0){
        car_state.recieveQue = 1;
        car_state.recieveTimer = recieveReset;
    }
    if(car_state.diagTimer == 0){
        car_state.diagQue = 1;
        car_state.diagTimer = diagReset;
    }
    if(car_state.buzzerTimer == 1){
        GPIOB->ODR &= ~GPIO_ODR_5; //turn off that annoying ass buzzer
    }
    if (car_state.buzzerTimer > 0) car_state.buzzerTimer--;

    car_state.controlTimer--;
    car_state.recieveTimer--;
    car_state.inputTimer--;
    car_state.diagTimer--;
    car_state.faultClearTimer--;
}

void Control() {
    car_state.controlQue = 0;
    APPS_RollingSmooth();
    car_state.lastAPPSFault = APPS_calc(&car_state.torque_req, car_state.lastAPPSFault);
    
    if(car_state.ready_to_drive)
        canmsg.torqueCommand = car_state.torque_req;
    send_CAN(MC_CANID_COMMAND, 8, (uint8_t*)&canmsg);        
    
    canmsg.inverterEnable = car_state.ready_to_drive;
    
    if(car_state.MCstates.st.vsmState == 6){ //only turn on RTD led when the wheels are ready to spin
        GPIOA->ODR |= (car_state.ready_to_drive > 0) << 9;
    } else {
        GPIOA->ODR &= ~GPIO_ODR_9;
    }
}

void Fault_Clear(){
    car_state.faultClearQueue = 0;
    if(car_state.faults.fc.postErrors & (1 << 21)){ //if there's an undervoltage fault
        send_CAN(MC_CANID_PARAMCOM, 8, (uint8_t*)&faultClearMsg);
    }
}

void MC_Init(){
    send_CAN(MC_CANID_PARAMCOM, 8, (uint8_t *)&shutup);
    //send_CAN(MC_CANID_PARAMCOM, 8, (uint8_t *)&fastMsg);
    //send_CAN(MC_CANID_PARAMCOM, 8, (uint8_t *)&torqueLimitMsg);
    //send_CAN(MC_CANID_PARAMCOM, 8, (uint8_t *)&cmdTimeoutMsg);
}

void Input(){
    car_state.inputQue = 0;
    if(!(GPIOB->IDR & GPIO_IDR_1)) {
        RTD_start();
    }
}

void send_Diagnostics(){
    car_state.diagQue = 0;
    send_CAN(VCU_CANID_APPS_RAW, 8, (uint8_t*)&ADC_Vars.APPS2);
    send_CAN(VCU_CANID_CALIBRATION, 8, (uint8_t *)&car_state.APPSCalib.apps1);
}

void RTD_start(){
    if (!((ADC_Vars.APPS1 <= APPS1_MIN) && (ADC_Vars.APPS2 <= APPS2_MIN))) return; 
    if (!(ADC_Vars.FBPS >= BRAKES_THREASHOLD)) return;
    if(car_state.faults.bits) return;
    if (car_state.ready_to_drive) {
        canmsg.inverterEnable = 0; //to disable the inverter lockout if RTD is already active and you rebooted the CM200
    } else {
        car_state.buzzerTimer = 1500; //buzzer timer in ms
        GPIOB->ODR |= GPIO_ODR_5; //buzzer
        car_state.ready_to_drive = 1;
    }
}

void clock_init() //turns on hsi48 and sets as system clock
{
    //wait one clock cycle before accessing flash memory @48MHZ
    FLASH->ACR |= 0b001 << FLASH_ACR_LATENCY_Pos;

    // Enables HSI48 oscillator
    RCC->CR2  |= RCC_CR2_HSI48ON;
    while (!(RCC->CR2 & RCC_CR2_HSI48RDY));
    
    //no peripheral prescaler div or hsi prescaler div
    RCC->CFGR &= ~(0b111 << RCC_CFGR_PPRE_Pos);
    RCC->CFGR &= ~(0b1111 << RCC_CFGR_HPRE_Pos);

    // sets system clock as HSI48 oscillator
    RCC->CFGR |= 0b11 << RCC_CFGR_SW_Pos;
    while (!(RCC->CFGR & (0b11 << RCC_CFGR_SWS_Pos)));
}

void ADC_DMA_Init(uint32_t *dest, uint32_t size){
    RCC->APB2ENR |= RCC_APB2ENR_ADCEN;

    ADC1->CFGR1 &= ~(uint32_t)0b011000; // set 12 bit precision
    
    ADC1->CFGR1 |= ADC_CFGR1_CONT; //analog to digital converter to cont mode
    ADC1->CFGR1 &= ~ADC_CFGR1_ALIGN; //align bits to the right

    ADC1->CFGR1 |= ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG; //enable dma & make cont

    ADC1->SMPR |= 0b111;

    ADC1->CHSELR |= ADC_CHSELR_CHSEL1 | ADC_CHSELR_CHSEL5 | ADC_CHSELR_CHSEL6 | ADC_CHSELR_CHSEL8; //channels to scan

    RCC->CR2 |= RCC_CR2_HSI14ON;
    while ((RCC->CR2 & RCC_CR2_HSI14RDY) == 0);
    ADC1->CFGR2 &= (~ADC_CFGR2_CKMODE);

    if ((ADC1->ISR & ADC_ISR_ADRDY) != 0)
        ADC1->ISR |= ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    while ((ADC1->ISR & ADC_ISR_ADRDY) == 0);

    RCC->AHBENR |= RCC_AHBENR_DMAEN;

    DMA1_Channel1->CCR &= ~DMA_CCR_MEM2MEM; //peripheral to memory
    DMA1_Channel1->CCR |= DMA_CCR_CIRC | DMA_CCR_MINC | DMA_CCR_TEIE; //c
    DMA1_Channel1->CCR |= DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0; //set size of data to transfer
    
    DMA1_Channel1->CPAR = (uint32_t) (&(ADC1->DR)); //sets source of dma transfer
    DMA1_Channel1->CMAR = (uint32_t)dest; //sets destination of dma transfer
    DMA1_Channel1->CNDTR = size; //sets size of dma transfer
    
    DMA1_Channel1->CCR |= DMA_CCR_EN; //enables dma

	ADC1->CR |= ADC_CR_ADSTART; //starts adc

    NVIC_EnableIRQ(DMA1_Channel1_IRQn); /* (1) */
    NVIC_SetPriority(DMA1_Channel1_IRQn,0); /* (2) */
}

void CAN_Init (){
    RCC->APB1ENR |= RCC_APB1ENR_CANEN;
    CAN->MCR |= CAN_MCR_INRQ;

    while (!(CAN->MSR & CAN_MSR_INAK));
    
    CAN->MCR &= ~CAN_MCR_SLEEP;
    while (CAN->MSR & CAN_MSR_SLAK);

    CAN->BTR |= 23 << CAN_BTR_BRP_Pos | 1 << CAN_BTR_TS1_Pos | 0 << CAN_BTR_TS2_Pos;
    CAN->MCR &= ~CAN_MCR_INRQ;
    
    while (CAN->MSR & CAN_MSR_INAK);

    CAN->FMR |= CAN_FMR_FINIT;
    CAN->FA1R |= CAN_FA1R_FACT0;
    CAN->sFilterRegister[0].FR1 = 0; // Its like a filter, but doesn't filter anything!
    CAN->sFilterRegister[0].FR2 = 0;
    CAN->FMR &=~ CAN_FMR_FINIT;
    CAN->IER |= CAN_IER_FMPIE0;
}

void GPIO_Init(){
    //turn on gpio clocks
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN; 
    RCC->AHBENR |= RCC_AHBENR_GPIOBEN; 

    GPIOA->MODER |= (MODE_INPUT     << GPIO_MODER_MODER0_Pos)   // PORTA_GPIO
                 |  (MODE_ANALOG    << GPIO_MODER_MODER1_Pos)   // APPS2
                 |  (MODE_INPUT     << GPIO_MODER_MODER3_Pos)   // PORTA_GPIO
                 |  (MODE_INPUT     << GPIO_MODER_MODER4_Pos)   // PORTA_GPIO
                 |  (MODE_ANALOG    << GPIO_MODER_MODER5_Pos)   // RBPS
                 |  (MODE_ANALOG    << GPIO_MODER_MODER6_Pos)   // FBPS
                 |  (MODE_OUTPUT    << GPIO_MODER_MODER9_Pos)   // RTD_LIGHT
                 |  (MODE_ALTFUNC   << GPIO_MODER_MODER11_Pos)  // CAN TX
                 |  (MODE_ALTFUNC   << GPIO_MODER_MODER12_Pos); // CAN RX

    // no pull up - pull down
    GPIOA->PUPDR  = 0 | (0b10 << GPIO_PUPDR_PUPDR1_Pos);
    GPIOB->PUPDR  = 0;
    GPIOA->AFR[1] = (4 << GPIO_AFRH_AFSEL11_Pos) | (4 << GPIO_AFRH_AFSEL12_Pos); //set up can

    GPIOB->MODER |= (MODE_ANALOG    << GPIO_MODER_MODER0_Pos)   // APPS1
                 |  (MODE_INPUT     << GPIO_MODER_MODER1_Pos)   // RTD_BUTTON
                 |  (MODE_INPUT     << GPIO_MODER_MODER4_Pos)   // PORTB_GPIO
                 |  (MODE_OUTPUT    << GPIO_MODER_MODER5_Pos)   // BUZZER
                 |  (MODE_OUTPUT    << GPIO_MODER_MODER6_Pos)   // LED1
                 |  (MODE_INPUT     << GPIO_MODER_MODER7_Pos);  // PORTB_GPIO
    
    GPIOB->PUPDR |= 0b01 << GPIO_PUPDR_PUPDR1_Pos; //button is pulled up
}

//very fast average
void APPS_RollingSmooth(){
    uint32_t APPS2 = 0,
             RBPS  = 0,
             FBPS  = 0,
             APPS1 = 0;
    for(int i = 0; i < ROLLING_ADC_VALS; i += 4){
        APPS2 += ADC_RollingValues[i + 0];
        RBPS  += ADC_RollingValues[i + 1];
        FBPS  += ADC_RollingValues[i + 2];
        APPS1 += ADC_RollingValues[i + 3];
    }

    ADC_Vars.APPS2 = APPS2 >> ROLLING_ADC_FR_POW;
    ADC_Vars.RBPS  = RBPS  >> ROLLING_ADC_FR_POW;
    ADC_Vars.FBPS  = FBPS  >> ROLLING_ADC_FR_POW;
    ADC_Vars.APPS1 = APPS1 >> ROLLING_ADC_FR_POW;
}


int GetTCMax(float coeff_friction){
    const float mass_KG = 240;
    const float rearAxel_Moment = 0.78f;
    const float gravity = 9.81f;
    const float cg_height = 22.86f;
    const float wheelbase = 1.54f;
    const float wheelbasediv = 1.0f/wheelbase;

    const float ellipseOblongConst = 1.2f; //reciprocal of how weak the side friction is compared to the long friction

    float rearAxel_downforce = ((2 * (wheelbase - rearAxel_Moment) * gravity) 
        + (cg_height * car_state.acceleration.ca.carAccel_X)) 
        * (0.5f * wheelbasediv)
        * mass_KG;
    float rearAxel_Force_Y = (wheelbase - rearAxel_Moment) * wheelbasediv * mass_KG 
        * car_state.acceleration.ca.carAccel_Y;

    float usable_ux = qfp_fsqrt(1 - (rearAxel_downforce * rearAxel_downforce)) * ellipseOblongConst;
    
    
}

//returns 1 if there's an issue
int APPS_calc(uint16_t *torque, uint16_t lastFault){
    uint16_t fault = 0, t_req = 0;
    
    const float apps1div = 1.0f / (APPS1_MAX - APPS1_MIN);
    const float apps2div = 1.0f / (APPS2_MAX - APPS2_MIN);

    float apps1 = ((float)ADC_Vars.APPS1 - APPS1_MIN) * apps1div,
          apps2 = ((float)ADC_Vars.APPS2 - APPS2_MIN) * apps2div; 

    if(apps1 < SENSOR_MIN || apps2 < SENSOR_MIN || apps1 > SENSOR_MAX || apps2 > SENSOR_MAX)
        fault = 1;
    else {
        apps1 = (apps1 > 1.0f) ? 1.0f : apps1;
        apps1 = (apps1 < 0.0f) ? 0.0f : apps1;
        apps2 = (apps2 > 1.0f) ? 1.0f : apps2;
        apps2 = (apps2 < 0.0f) ? 0.0f : apps2;
    }

    float c_app = (apps1 + apps2) * 0.5f;

    if      (lastFault == 3 && c_app > 0.05f) //if there was APPS/BrakePlausability and apps is still > 5%
        fault = 3;
    else if (fault == 1)
                 ;
    else if (FABS(apps1 - apps2) > 0.1f)
        fault = 2;
    else if (c_app > 0.25f && (ADC_Vars.FBPS > BRAKES_THREASHOLD))
        fault = 3;
    else if (!car_state.ready_to_drive);
    else //if there are no faults
        t_req = REMAPm_M(c_app, MIN_TORQUE_REQ, MAX_TORQUE_REQ);
 
    car_state.APPSCalib.apps1  = (uint16_t)(apps1 * 1000);
    car_state.APPSCalib.apps2  = (uint16_t)(apps2 * 1000);
    car_state.APPSCalib.torque = (uint16_t)t_req;
    car_state.APPSCalib.fault  = (uint16_t)fault;
    
    *torque = t_req;
    return fault;
}

void send_CAN(uint16_t id, uint8_t length, uint8_t* data){
    //find first empty mailbox
    int j = (CAN->TSR & CAN_TSR_CODE_Msk) >> CAN_TSR_CODE_Pos;

    CAN->sTxMailBox[j].TDTR = length;
    CAN->sTxMailBox[j].TDLR = 0;
    CAN->sTxMailBox[j].TDHR = 0;
    for(int i = 0; i < length && i < 4; i++)
        CAN->sTxMailBox[j].TDLR |= ((data[i] & 0xFF) << i * 8);
    for(int i = 0; i < length - 4; i++)
        CAN->sTxMailBox[j].TDHR |= ((data[i+4] & 0xFF) << i * 8);
    CAN->sTxMailBox[j].TIR = (uint32_t)((id << CAN_TI0R_STID_Pos) | CAN_TI0R_TXRQ);
}

void recieve_CAN(){
    car_state.recieveQue = 0;
    while ((CAN->RF0R & CAN_RF0R_FMP0) != 0) {
        uint8_t  can_len    = CAN->sFIFOMailBox[0].RDTR & 0xF;
        uint64_t can_data   = CAN->sFIFOMailBox[0].RDLR + ((uint64_t)CAN->sFIFOMailBox[0].RDHR << 32);
        uint16_t can_id     = CAN->sFIFOMailBox[0].RIR >> CAN_RI0R_STID_Pos;
        CAN->RF0R |= CAN_RF0R_RFOM0; //release mailbox

        CAN_msg canrx = {can_id, can_len, can_data};
        process_CAN(canrx);
    }
}

void process_CAN(CAN_msg cm){
    switch (cm.id){
        case MC_CANID_HIGHSPEEDMESSAGE:
            car_state.hsmessage.bits = cm.data;
        case MC_CANID_FAULTCODES:
            car_state.faults.bits = cm.data;
        case MC_CANID_ANALOGINVOLTAGES:
            MC_Init(); //shuts the CM200 up in case it's been reset
        case MC_CANID_INTERNALSTATES:
            car_state.MCstates.bits = cm.data;
        break;
    }
}
