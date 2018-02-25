#include "license.txt" // Oscar Steila ik1xpv
#include <stdio.h>
#include "mytypes.h"
#include "config.h"  // DbgPrintf
#include "BBRF103.h" // i2c stuff
#include "Si5351.h"


void Si5351init()
{
	// Set crystal load capacitance
    BBRF103.SendI2cbyte(SI5351_ADDR, SI5351_CRYSTAL_LOAD, SI5351_CRYSTAL_LOAD_6PF | 0b00010010);
	BBRF103.SendI2cbyte(SI5351_ADDR, SI_CLK0_CONTROL, 0x80); // clocks off
	BBRF103.SendI2cbyte(SI5351_ADDR, SI_CLK1_CONTROL, 0x80);
	BBRF103.SendI2cbyte(SI5351_ADDR, SI_CLK2_CONTROL, 0x80);
    if ((BBRF103.GetmodeRF() == HFMODE) || (BBRF103.GetmodeRF() == VLFMODE))
          si5351aSetFrequency(ADC_FREQ, R820T_ZERO);
    else
          si5351aSetFrequency(ADC_FREQ,R820T_FREQ);
    DbgPrintf("Si5351_init()\n");
}


//#define _PLLDEBUG_
//
// Set up specified PLL with mult, num and denom
// mult is 15..90
// num is 0..1,048,575 (0xFFFFF)
// denom is 0..1,048,575 (0xFFFFF)
//
void setupPLL(UINT8 pll, UINT8 mult, UINT32 num, UINT32 denom)
{
	UINT32 P1;					// PLL config register P1
	UINT32 P2;					// PLL config register P2
	UINT32 P3;					// PLL config register P3
	UINT8 data[8];

	// the actual multiplier is  mult + num / denom
#ifdef _PLLDEBUG_
    double fa;
    double x = 27000000.0;
    fa = x * ((double) mult + (double)num / (double)denom);
	DbgPrintf((char *) "pll = %d , mult = %d , num = %d , denom = %d\n", pll, mult, num, denom );
	DbgPrintf((char *) "pll target %e \n", fa);
#endif

	P1 = (UINT32)(128 * ((float)num / (float)denom));
	P1 = (UINT32)(128 * (UINT32)(mult) + P1 - 512);
	P2 = (UINT32)(128 * ((float)num / (float)denom));
	P2 = (UINT32)(128 * num - denom * P2);
	P3 = denom;

	data[0] = (P3 & 0x0000FF00) >> 8;
	data[1] = (P3 & 0x000000FF);
	data[2] = (P1 & 0x00030000) >> 16;
	data[3] = (P1 & 0x0000FF00) >> 8;
	data[4] = (P1 & 0x000000FF);
	data[5] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
	data[6] = (P2 & 0x0000FF00) >> 8;
	data[7] = (P2 & 0x000000FF);

	BBRF103.SendI2cbytes(SI5351_ADDR, pll, data, sizeof(data));
}
//
// Set up MultiSynth with integer divider and R divider
// R divider is the bit value which is OR'ed onto the appropriate register, it is a #define in si5351a.h
//
void setupMultisynth( UINT8 synth, UINT32 divider, UINT8 rDiv)
{
	UINT32 P1;	// Synth config register P1
	UINT32 P2;	// Synth config register P2
	UINT32 P3;	// Synth config register P3
	UINT8 data[8];

#ifdef _PLLDEBUG_
	DbgPrintf("setupMultisynth synth = %d  divider = %d  rDiv= %d \n", synth, divider, rDiv);
	DbgPrintf("output expected f = %e\n", fa / divider);
#endif

	P1 = 128 * divider - 512;
	P2 = 0;							// P2 = 0, P3 = 1 forces an integer value for the divider
	P3 = 1;

	data[0] = (P3 & 0x0000FF00) >> 8;
	data[1] = (P3 & 0x000000FF);
	data[2] = ((P1 & 0x00030000) >> 16) | rDiv ;
	data[3] = (P1 & 0x0000FF00) >> 8 ;
	data[4] = (P1 & 0x000000FF);
	data[5] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
	data[6] = (P2 & 0x0000FF00) >> 8;
	data[7] = (P2 & 0x000000FF);

	BBRF103.SendI2cbytes(SI5351_ADDR, synth, data, sizeof(data));
}
//
// Switches off Si5351a output
// Example: si5351aOutputOff(SI_CLK0_CONTROL);
// will switch off output CLK0
//
void si5351aOutputOff(UINT8 clk)
{
	BBRF103.SendI2cbyte(SI5351_ADDR, clk, 0x80);		// Refer to SiLabs AN619 to see bit values - 0x80 turns off the output stage
}

void si5351aSetFrequency(UINT32 freq, UINT32 freq2)
{
    UINT32 frequency;
	UINT32 pllFreq;
	UINT32 xtalFreq = SI5351_FREQ;
	UINT32 l;
	double f;
	UINT8 mult;
	UINT32 num;
	UINT32 denom;
	UINT32 divider;
	UINT32 rdiv;

	double corr = 0.9999314;
	//double corr = 1.0;

	DbgPrintf("BBRF103 si5351 SetFreq ADC sampling:%d R820T reference:%d\n", freq, freq2);

	rdiv = (UINT32) SI_R_DIV_1;

	frequency = (UINT32)((double) freq * corr);

	while (frequency < 1000000)
	{
		frequency = frequency * 2;
		rdiv += SI_R_DIV_2;
	}
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "\nCLK0 frequency %d \n", frequency);
#endif
	divider = 900000000UL / frequency;// Calculate the division ratio. 900,000,000 is the maximum internal
									// PLL frequency: 900MHz
	if (divider % 2) divider--;		// Ensure an even integer division ratio

	pllFreq = divider * frequency;	// Calculate the pllFrequency: the divider * desired output frequency
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "pllA Freq  %d \n", pllFreq);
#endif
	mult = pllFreq / xtalFreq;		// Determine the multiplier to get to the required pllFrequency
	l = pllFreq % xtalFreq;			// It has three parts:
	f =(double) l;					// mult is an integer that must be in the range 15..90
	f *= 1048575;					// num and denom are the fractional parts, the numerator and denominator
	f /= xtalFreq;					// each is 20 bits (range 0..1048575)
	num = (UINT32) f;				// the actual multiplier is  mult + num / denom
	denom = 1048575;				// For simplicity we set the denominator to the maximum 1048575
									// Set up PLL A with the calculated multiplication ratio
	setupPLL(SI_SYNTH_PLL_A, mult, num, denom);
									// Set up MultiSynth divider 0, with the calculated divider.
									// The final R division stage can divide by a power of two, from 1..128.
									// represented by constants SI_R_DIV1 to SI_R_DIV128 (see si5351a.h header file)
									// If you want to output frequencies below 1MHz, you have to use the
									// final R division stage
	setupMultisynth(SI_SYNTH_MS_0, divider, rdiv);
									// Reset the PLL. This causes a glitch in the output. For small changes to
									// the parameters, you don't need to reset the PLL, and there is no glitch
	BBRF103.SendI2cbyte((UINT8)SI5351_ADDR, (UINT8)SI_PLL_RESET, (UINT8) 0x20); //pllA
									// Finally switch on the CLK0 output (0x4F)
									// and set the MultiSynth0 input to be PLL A
	BBRF103.SendI2cbyte(SI5351_ADDR, SI_CLK0_CONTROL, 0x4F | SI_CLK_SRC_PLL_A);

	if (freq2 > 0)
	{
		// calculate clk2
		frequency = (UINT32)((double)freq2 * corr);
		rdiv = SI_R_DIV_1;
		xtalFreq = SI5351_FREQ;
		while (frequency <= 1000000)
		{
			frequency = frequency * 2;
			rdiv += SI_R_DIV_2;
		}
#ifdef _PLLDEBUG_
		DbgPrintf((char *) "\nCLK2 frequency %d \n", frequency);
#endif
		divider = 900000000UL / frequency;// Calculate the division ratio. 900,000,000 is the maximum internal
										  // PLL frequency: 900MHz
		if (divider % 2) divider--;		// Ensure an even integer division ratio

		pllFreq = divider * frequency;	// Calculate the pllFrequency: the divider * desired output frequency
#ifdef _PLLDEBUG_
		DbgPrintf((char *) "pllB Freq  %d \n", pllFreq);
#endif
		mult = pllFreq / xtalFreq;		// Determine the multiplier to get to the required pllFrequency
		l = pllFreq % xtalFreq;			// It has three parts:
		f = (double)l;							// mult is an integer that must be in the range 15..90
		f *= 1048575;					// num and denom are the fractional parts, the numerator and denominator
		f /= xtalFreq;					// each is 20 bits (range 0..1048575)
		num = (UINT32)f;				// the actual multiplier is  mult + num / denom
		denom = 1048575;				// For simplicity we set the denominator to the maximum 1048575

										// Set up PLL B with the calculated multiplication ratio
		setupPLL(SI_SYNTH_PLL_B, mult, num, denom);
		// Set up MultiSynth divider 0, with the calculated divider.
		// The final R division stage can divide by a power of two, from 1..128.
		// represented by constants SI_R_DIV1 to SI_R_DIV128 (see si5351a.h header file)
		// If you want to output frequencies below 1MHz, you have to use the
		// final R division stage

		setupMultisynth(SI_SYNTH_MS_2, divider, rdiv);
		// Reset the PLL. This causes a glitch in the output. For small changes to
		// the parameters, you don't need to reset the PLL, and there is no glitch
		BBRF103.SendI2cbyte((UINT8)SI5351_ADDR, (UINT8)SI_PLL_RESET, (UINT8)0x80); //pllB
		// Finally switch on the CLK2 output (0x4C)
		// and set the MultiSynth0 input to be PLL A
		BBRF103.SendI2cbyte(SI5351_ADDR, SI_CLK2_CONTROL, 0x4C | SI_CLK_SRC_PLL_B);  // select PLLB
		// calculate clk2
        BBRF103.UptLedYellow(!BBRF103.ledY);  //blink  gadget Yellow
	}

}

